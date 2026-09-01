//! The sidecar entry point over the real kernel ABI (cargo feature
//! `target`). Bootstrap per Phase 2 §6.2:
//!
//! 1. crt0 hands us the BootInfo pointer (a0/rdi).
//! 2. Parse the BIB; resolve budget/console/ramdisk caps by name.
//! 3. Init the heap over the budget region; bind `BudgetAlloc` to the
//!    budget cap for request buffers.
//! 4. `boot()`: connect the block cache to the ramdisk channel, mount
//!    `/`, `/dev`, `/tmp`, install the applet registry, spawn init (the
//!    boot script runner — it executes `/etc/init.rc`) with console stdio.
//! 5. Run the scheduler; when it quiesces, park on the console channel
//!    (the event loop — typed input arrives there as messages; the ChanDev
//!    adapter that feeds the in-memory console is future work, so v1 just
//!    waits).
//!
//! Like the ramdisk driver's entry, this is forward-declared: it compiles
//! against the not-yet-existing kernel (`k_chan_*` externs in
//! `aerosls_proto::kabi`); the rootfs image (with `/etc/init.rc` and the
//! applet script files) is the image-build's business.

use crate::allocator::BudgetAlloc;
use crate::boot::{boot, BootCaps};
use crate::heap::Bump;
use aerosls_proto::bootinfo::BootInfo;
use aerosls_proto::kabi::{CAP_CHAN_W, Kernel, RealKernel, TIMEOUT_NONE};

extern "C" {
    fn k_chan_send(chan: u32, tag: u32, flags: u16, payload: *const u8,
                   payload_len: u32, caps: *const core::ffi::c_void,
                   n_caps: u32, timeout_ns: u64) -> i32;
    fn k_chan_recv(chan: u32, buf: *mut u8, buf_len: u32,
                   slots: *mut core::ffi::c_void, n_slots: u32,
                   out: *mut core::ffi::c_void) -> i32;
    fn k_yield();
}

/// Result of the NET_INFO handshake, stored here so the linker cannot
/// eliminate the function call (the write is to a `#[used]` static).
#[used]
static mut NET_INFO_RESULT: u32 = 0;

/// Nettest result buffer: the applet writes here; the event loop drains
/// via serial_write_raw (syscall 165 survives LTO in this binary crate).
#[used]
static mut NETTEST_RESULT: [u8; 512] = [0u8; 512];
#[used]
static mut NETTEST_RESULT_LEN: u32 = 0;

/// Write a string into the nettest result buffer (called from lib crate
/// nettest).  # Safety: single-threaded, only nettest writes.
#[no_mangle]
pub unsafe extern "C" fn nettest_push_result(data: *const u8, len: u32) {
    let n = (len as usize).min(512 - NETTEST_RESULT_LEN as usize);
    core::ptr::copy_nonoverlapping(
        data,
        NETTEST_RESULT.as_mut_ptr().add(NETTEST_RESULT_LEN as usize),
        n,
    );
    NETTEST_RESULT_LEN += n as u32;
}

/// Network channel slot numbers, exported for fork children (nettest applet)
/// that can't access the BIB.
#[used]
pub static mut NET_W_SLOT: u32 = 0xFF;
#[used]
pub static mut NET_R_SLOT: u32 = 0xFF;

/// NET_INFO handshake — called unconditionally from `rust_entry` via a
/// `#[no_mangle]` symbol so the compiler cannot prove it dead and
/// eliminate it.  Pass 0xFF for either handle to skip.
///
/// Uses a timeout + retry loop to avoid hanging if the network sidecar
/// hasn't adopted the POSIX channel yet.
#[no_mangle]
pub extern "C" fn posix_net_info_handshake(net_w: u32, net_r: u32) -> u32 {
    if net_w == 0xFF || net_r == 0xFF {
        return 0;
    }
    // 1. Build NET_INFO payload (NetFrame: 16-byte header)
    //    magic[8] = "AEROSNT\x01", version=1, ty=NET_INFO=1, flags=0, pad=0
    let mut payload = [0u8; 16];
    payload[0..8].copy_from_slice(b"AEROSNT\x01");
    payload[8..10].copy_from_slice(&[1, 0]); // version=1
    payload[10..12].copy_from_slice(&[1, 0]); // ty=NET_INFO=1
    payload[12..16].fill(0); // flags=0, pad=0

    // Retry loop: send NET_INFO, recv reply with 200ms timeout.
    // The network sidecar may not have adopted the POSIX channel yet
    // when we first send, so we retry up to 10 times (2s total).
    const NET_INFO_TIMEOUT_NS: u64 = 200_000_000; // 200ms
    const MAX_RETRIES: u32 = 10;

    // ── IMPORTANT: send NET_INFO exactly ONCE ───────────────────────────
    // The network server replies to every NET_INFO it receives.  If we
    // re-sent on each retry (old behavior), the queue would accumulate
    // stale tag=1 replies, and the NetClient (whose tag counter also
    // starts at 1) would consume a stale reply for `socket()`, then get
    // the *socket* reply for `bind()` — a tag mismatch Protocol error.
    // So: send once, and only retry the RECV (the server processes the
    // single queued message as soon as it adopts our channel).
    let send_rc = unsafe {
        k_chan_send(
            net_w,              // CHAN_W slot
            1,                  // tag=1
            0,                  // flags
            payload.as_ptr(),
            16,                 // payload_len
            core::ptr::null(),  // no caps
            0,                  // n_caps=0
            NET_INFO_TIMEOUT_NS, // timeout_ns (non-zero = finite deadline)
        )
    };
    if send_rc != 0 {
        klog(b"[POSIX] NETBOOT FAILED: NET_INFO send rc=", send_rc as u32, b"\n");
        return 0;
    }

    // 2. Recv reply on chan_r — retried (with yield) until it arrives.
    //    The recv is non-blocking; the server may not have adopted our
    //    channel yet, so poll until the single reply lands.
    let mut buf = [0u8; 256];
    #[repr(C)]
    struct RecvOut {
        kind: u16,
        flags: u16,
        tag: u32,
        len: u32,
        n_caps: u16,
        needed: u16,
    }
    let mut out = RecvOut { kind: 0, flags: 0, tag: 0, len: 0, n_caps: 0, needed: 0 };
    for _attempt in 0..MAX_RETRIES {
        out = RecvOut { kind: 0, flags: 0, tag: 0, len: 0, n_caps: 0, needed: 0 };
        let recv_rc = unsafe {
            k_chan_recv(
                net_r,                    // CHAN_R slot
                buf.as_mut_ptr(),
                buf.len() as u32,
                core::ptr::null_mut(),    // no slot caps
                0,                        // n_slots=0
                &mut out as *mut RecvOut as *mut core::ffi::c_void,
            )
        };
        if recv_rc == 0 {
            unsafe { crate::applets::NETBOOT_OK = 1; }
            return 1;
        }
        // Recv failed or timed out — the server hasn't replied yet.
        // Yield so the network sidecar can run and process our request.
        unsafe { k_yield(); }
    }
    klog(b"[POSIX] NETBOOT FAILED: no NET_INFO reply after ", MAX_RETRIES, b" recv retries\n");
    0
}

/* The crt0 (crt0.S) is assembled by rustc's LLVM integrated assembler
 * through global_asm — no external cross-GCC — and linked at address 0 by
 * posix.ld (ENTRY(_start)). It saves rdi (the BIB pointer) and switches
 * to the sidecar's own boot stack before calling rust_entry. */
#[cfg(all(feature = "target", target_arch = "x86_64", target_os = "none"))]
core::arch::global_asm!(include_str!("crt0.S"), options(att_syntax));

/// Reserved heap over the budget region (single-threaded sidecar; access is
/// confined to `rust_entry`).
static mut HEAP: Bump = Bump::new();

/* Global allocator: bump over the budget region. The heap is
 * initialized in rust_entry (after the BIB is parsed). Until then,
 * alloc returns null (no heap allocs happen before init). */
#[cfg(all(feature = "target", target_os = "none"))]
struct HeapAlloc;

#[cfg(all(feature = "target", target_os = "none"))]
unsafe impl core::alloc::GlobalAlloc for HeapAlloc {
    #[allow(static_mut_refs)]
    unsafe fn alloc(&self, layout: core::alloc::Layout) -> *mut u8 {
        match unsafe { HEAP.alloc(layout.size(), layout.align()) } {
            Some(addr) => addr as *mut u8,
            None => core::ptr::null_mut(),
        }
    }
    unsafe fn dealloc(&self, _ptr: *mut u8, _layout: core::alloc::Layout) {
        // bump allocator: no dealloc
    }
}

#[cfg(all(feature = "target", target_os = "none"))]
#[global_allocator]
static GLOBAL_ALLOC: HeapAlloc = HeapAlloc;

/* Panic handler: write to kernel serial log (SYS_SLS_SERIAL_WRITE = 165). */
#[cfg(all(feature = "target", target_os = "none"))]
struct PanicBuf { buf: [u8; 256], pos: usize }

#[cfg(all(feature = "target", target_os = "none"))]
impl core::fmt::Write for PanicBuf {
    fn write_str(&mut self, s: &str) -> core::fmt::Result {
        let room = &mut self.buf[self.pos..];
        let n = s.len().min(room.len());
        room[..n].copy_from_slice(&s.as_bytes()[..n]);
        self.pos += n;
        Ok(())
    }
}

#[cfg(all(feature = "target", target_os = "none"))]
#[panic_handler]
fn panic(info: &core::panic::PanicInfo) -> ! {
    use core::fmt::Write as _;
    let mut b = PanicBuf { buf: [0u8; 256], pos: 0 };
    let _ = core::write!(&mut b, "[POSIX PANIC] {info}");
    unsafe {
        core::arch::asm!(
            "syscall",
            inlateout("rax") 165u64 => _,  // SYS_SLS_SERIAL_WRITE
            inlateout("rdi") b.buf.as_ptr() => _,
            // The kernel's syscall path uses every caller-saved register
            // (do_syscall's args) and never restores them — declare ALL of
            // them clobbered so the compiler cannot reuse a stale value.
            lateout("rcx") _, lateout("r11") _, lateout("rsi") _, lateout("rdx") _, lateout("r8") _, lateout("r9") _, lateout("r10") _,
            options(nostack),
        );
    }
    loop { core::hint::spin_loop(); }
}

/// Kernel-serial log (SYS_SLS_SERIAL_WRITE = 165): `tag`, then the value
/// as 8 hex digits, then `extra`. NUL-terminated like the kernel's
/// `kernel_serial_print` expects. Lives in the binary crate so the call
/// survives release LTO.
/// LTO-proof: write the last syscall return value here so the
/// compiler cannot eliminate the preceding asm block.
/// The read_volatile in `touch_cookie` forces the compiler to
/// keep the write, and `touch_cookie` is called from rust_entry
/// so the linker cannot eliminate it.
#[used]
static mut SYSCALL_COOKIE: u64 = 0;

/// Force the compiler to keep SYSCALL_COOKIE alive.
#[inline(never)]
fn touch_cookie() {
    unsafe { core::ptr::read_volatile(&SYSCALL_COOKIE); }
}

fn klog(tag: &[u8], val: u32, extra: &[u8]) {
    let mut msg = [0u8; 96];
    let n = tag.len().min(24);
    msg[..n].copy_from_slice(&tag[..n]);
    let mut off = n;
    let hex = b"0123456789ABCDEF";
    for i in (0..8).rev() {
        msg[off] = hex[((val >> (i * 4)) & 0xF) as usize];
        off += 1;
    }
    msg[off] = b' ';
    off += 1;
    let ne = extra.len().min(95 - off);
    msg[off..off + ne].copy_from_slice(&extra[..ne]);
    off += ne;
    msg[off] = b'\n';
    unsafe {
        let mut rax: u64;
        core::arch::asm!(
            "syscall",
            inlateout("rax") 165u64 => rax,
            inlateout("rdi") msg.as_ptr() => _,
            // The kernel's syscall path uses every caller-saved register
            // (do_syscall's args) and never restores them — declare ALL of
            // them clobbered so the compiler cannot reuse a stale value
            // after the syscall (caught live: the msg-pointer survived in
            // rsi, the kernel clobbered rsi, and the post-syscall msg
            // tail-zeroing wrote through the garbage → #PF at 0x430).
            lateout("rcx") _, lateout("r11") _, lateout("rsi") _, lateout("rdx") _, lateout("r8") _, lateout("r9") _, lateout("r10") _,
            options(nostack),
        );
        SYSCALL_COOKIE = rax;
    }
    touch_cookie();
}

/// Raw serial write: send `data` directly to kernel serial via
/// SYS_SLS_SERIAL_WRITE (165).  Same syscall as klog but without the
/// tag/hex/extra framing - just raw bytes.
fn serial_write_raw(data: &[u8]) {
    let mut buf = [0u8; 512];
    let n = data.len().min(512);
    buf[..n].copy_from_slice(&data[..n]);
    unsafe {
        let mut rax: u64;
        core::arch::asm!(
            "syscall",
            inlateout("rax") 165u64 => rax,
            inlateout("rdi") buf.as_ptr() => _,
            lateout("rcx") _, lateout("r11") _, lateout("rsi") _,
            lateout("rdx") _, lateout("r8") _, lateout("r9") _, lateout("r10") _,
            options(nostack),
        );
        SYSCALL_COOKIE = rax;
    }
    touch_cookie();
}

#[no_mangle]
// The `static mut` heap is deliberate on a bare-metal single-threaded
// target; the lint fires at this use site.
#[allow(static_mut_refs)]
pub extern "C" fn rust_entry(bib_ptr: *const u8) -> ! {
    let bib = unsafe { BootInfo::from_raw(bib_ptr) }.expect("[POSIX] corrupt boot info");
    let caps = BootCaps::from_bib(&bib).expect("missing initial caps");

    // Budget → heap and the request-buffer arena. The budget MEM cap is the
    // whole region; the main bump heap and BudgetAlloc must NOT both claim
    // [base, base+len) from the same cursor — the block cache's RD_READ /
    // RD_WRITE grant buffers (carved by BudgetAlloc, 4 KiB each) would then
    // alias the heap's first allocations (the console Arc, the VFS), and
    // the ramdisk driver's reply write would overwrite them. Caught live:
    // the rootfs superblock reply (block 0, 4096 bytes) landed on the
    // console CharNode's ConsoleIo, smashing the output RefCell's borrow
    // field to a nonzero value, and the console pump's first drain panicked
    // "RefCell already borrowed". Split the region: BudgetAlloc owns the
    // first MiB (256 x 4 KiB buffers — ample for the mount + demo reads),
    // the main heap owns the rest.
    const REQ_BUF_ARENA: u64 = 1024 * 1024;   /* grant-buffer arena within the budget */
    unsafe {
        HEAP.init(
            (caps.budget_base + REQ_BUF_ARENA) as usize,
            caps.budget_len.saturating_sub(REQ_BUF_ARENA) as usize,
        );
    }
    let alloc = BudgetAlloc::new(caps.budget_slot, caps.budget_base, REQ_BUF_ARENA);

    // Debug: log ramdisk caps found in BIB.
    {
        let rw = caps.ramdisk_chan_w.map_or(0xFFu32, |v| v);
        let rr = caps.ramdisk_chan_r.map_or(0xFFu32, |v| v);
        let cw = caps.console_chan.map_or(0xFFu32, |v| v);
        let nw = caps.net_chan_w.map_or(0xFFu32, |v| v);
        let mut msg = [0u8; 80];
        let s = b"[POSIX] boot: rw=";
        msg[..s.len()].copy_from_slice(s);
        let mut off = s.len();
        let hex = b"0123456789ABCDEF";
        msg[off] = hex[(rw >> 4) as usize & 0xF]; off += 1;
        msg[off] = hex[(rw & 0xF) as usize]; off += 1;
        let s2 = b" rr=";
        msg[off..off+s2.len()].copy_from_slice(s2); off += s2.len();
        msg[off] = hex[(rr >> 4) as usize & 0xF]; off += 1;
        msg[off] = hex[(rr & 0xF) as usize]; off += 1;
        let s3 = b" cw=";
        msg[off..off+s3.len()].copy_from_slice(s3); off += s3.len();
        msg[off] = hex[(cw >> 4) as usize & 0xF]; off += 1;
        msg[off] = hex[(cw & 0xF) as usize]; off += 1;
        let s4 = b" nw=";
        msg[off..off+s4.len()].copy_from_slice(s4); off += s4.len();
        msg[off] = hex[(nw >> 4) as usize & 0xF]; off += 1;
        msg[off] = hex[(nw & 0xF) as usize]; off += 1;
        msg[off] = b'\n'; off += 1;
        unsafe {
            core::arch::asm!("syscall",
                inlateout("rax") 165u64 => _,
                inlateout("rdi") msg.as_ptr() => _,
                lateout("rcx") _, lateout("r11") _, lateout("rsi") _, lateout("rdx") _, lateout("r8") _, lateout("r9") _, lateout("r10") _,
                options(nostack),
            );
        }
    }

    let console = alloc::sync::Arc::new(aerosls_vfs::CharNode::console());
    let mut booted = boot(RealKernel, &caps, console, alloc)
        .unwrap_or_else(|e| panic!("sidecar boot failed: {e:?}"));

    // NET_INFO handshake: #[no_mangle] function called unconditionally
    // so the compiler cannot eliminate it.  Pass 0xFF sentinel for absent caps.
    {
        let net_w = caps.net_chan_w.map_or(0xFFu32, |v| v);
        let net_r = caps.net_chan_r.map_or(0xFFu32, |v| v);
        // Export channel slots for fork children (nettest applet).
        unsafe {
            NET_W_SLOT = net_w;
            NET_R_SLOT = net_r;
        }
        let rc = unsafe { posix_net_info_handshake(net_w, net_r) };
        unsafe { NET_INFO_RESULT = rc; }
    }
    // Install the network client into the ProcManager so applets can
    // call ctx.net() for socket I/O.
    if let Some(nc) = booted.net.take() {
        booted.proc.set_net(alloc::boxed::Box::new(nc));
    }

    // The console channel's WRITE endpoint (CHAN_R is `caps.console_chan`).
    // Applet/init stdout lands in the in-memory console (CharNode); the
    // pump below forwards it here, and the kernel console service
    // (console_service.c) prints every message it receives to serial — so
    // the sidecar's output becomes visible in the QEMU log.
    let console_w = bib.find_cap(CAP_CHAN_W, "console").map(|c| c.slot);

    // NOTE: nettest is now run via init.rc (applet ctx.net() path),
    // not via raw syscalls in the event loop.  The set_net() call
    // above installs the NetClient into the ProcManager so applets
    // can call ctx.net() for socket I/O.

    let mut boot_stats_sent = false;
    loop {
        // ── Console input: recv from kernel, push into ConsoleIo ──
        // The kernel's console_service_tick polls serial input and sends
        // bytes on the console CHAN_W. We recv from our CHAN_R and push
        // into the in-memory ConsoleIo buffer for shell stdin.
        if let Some(cr) = caps.console_chan {
            let mut input_buf = [0u8; 256];
            let mut input_caps = [aerosls_proto::kabi::GrantedCap::default(); 1];
            match RealKernel.recv(cr, &mut input_buf, &mut input_caps) {
                Ok(rr) if rr.kind == aerosls_proto::CH_KIND_MSG && rr.len > 0 => {
                    booted.console.console_io().push_input(&input_buf[..rr.len]);
                }
                _ => {}
            }
        }
        // Drain console output to the kernel console channel, chunked
        // under the transport's payload bound (CAP_MSG_MAX_PAYLOAD). A
        // send parks while the queue is full; the kernel console service
        // drains every tick, so this is simple flow control, not a stall.
        if let Some(cw) = console_w {
            let out = booted.console.console_io().drain_output();
            for chunk in out.chunks(1024) {
                let _ = RealKernel.send(cw, 0, 0, chunk, &[], TIMEOUT_NONE);
            }
        }
        booted.proc.drain_wakes();
        // One-shot boot diagnostic: report what task 0 (init) did and how
        // many tasks are live, so a quiet boot can be told apart from a
        // crashed one. Runs ONCE after boot — the per-iteration repetition
        // interleaved with the shell prompt and re-read /etc/init.rc on
        // every idle pass.
        if !boot_stats_sent {
            boot_stats_sent = true;
            let code = booted.proc.exit_code(0);
            let live = booted.proc.task_count() as u32;
            // Show the exit code if the init task has exited, else
            // report 0xFEEDFACE as a "still running" sentinel (avoids
            // the confusing 0xFFFFFFFF from unwrap_or(-1)).
            let code_u32 = code.map(|c| c as u32).unwrap_or(0xFEEDu32);
            klog(b"[POSIX] loop exit0=", code_u32, b"");
            klog(b"[POSIX] loop tasks=", live, b"");
            // Probe the rootfs directly: open /etc/init.rc through the
            // VFS and report the errno (0 = ok). If the open succeeds,
            // report the script length. This isolates a data/transport
            // problem (corrupted block 0) from an init-applet logic one.
            match booted.proc.vfs.open(0, "/etc/init.rc", aerosls_vfs::O_RDONLY, 0) {
                Ok(fd) => {
                    let mut b = [0u8; 128];
                    match booted.proc.vfs.read(0, fd, &mut b) {
                        Ok(n) => klog(b"[POSIX] rc read=", n as u32, b""),
                        Err(e) => klog(b"[POSIX] rc read err=", e.code() as u32, b""),
                    }
                    booted.proc.vfs.close(0, fd).ok();
                }
                Err(e) => klog(b"[POSIX] rc open err=", e.code() as u32, b""),
            }
            // Report the root aerofs cache's device state (0 = Live;
            // otherwise the stale-reason code) so an EIO can be attributed
            // to a stale device vs. a parse failure.
            klog(b"[POSIX] aero state=", booted.proc.vfs.aerofs_state(), b"");
        }
        match booted.proc.run_next() {
            Some(_) => {
                // Drain any nettest result written by the lib-crate applet
                // into the static buffer (the LTO-proof output path).
                unsafe {
                    let len = NETTEST_RESULT_LEN as usize;
                    if len > 0 {
                        serial_write_raw(&NETTEST_RESULT[..len]);
                        NETTEST_RESULT_LEN = 0;
                    }
                }
            }
            None => {
                // Quiesced — nothing runnable, nothing blocked. Park until
                // the kernel delivers work: a wake on ANY wired channel
                // (the console for typed input, the ramdisk for a blocked
                // task's device reply). Parking on the console channel
                // ONLY is wrong: a task blocked on a ramdisk read would
                // never see its reply — the message sits in the channel
                // queue while the sidecar sleeps (caught live: init
                // quiesced with its /etc/init.rc data never delivered).
                // The kernel wakes the park on any listed channel; a
                // spurious wake just re-runs the loop (drain_wakes + a
                // re-poll re-check everything).
                let mut chans = [u32::MAX; 4];
                let mut n = 0usize;
                if let Some(c) = caps.console_chan {
                    chans[n] = c;
                    n += 1;
                }
                if let Some(r) = caps.ramdisk_chan_r {
                    chans[n] = r;
                    n += 1;
                }
                if n == 0 {
                    // No wired channels: spin (v1 debug only).
                    continue;
                }
                let _ = RealKernel.wait(&chans[..n], TIMEOUT_NONE);
            }
        }
    }
}
