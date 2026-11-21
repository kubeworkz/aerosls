//! linux_run.rs — step 1 of the tenant Linux path: a payload that lives in
//! the tenant's own VFS, uploaded at runtime, spawned through the real kernel.
//!
//! The gate is deliberately one-shot and static: `linux run PATH` reads the
//! file with VFS task 0, requires a static ET_EXEC with no PT_INTERP, and only
//! after `linux_spawn` has succeeded allocates the child's VFS task. The
//! ordering matters because `Vfs::add_task` has no removal: allocating first
//! would leak a task slot on every refused payload, and a task allocated
//! before the spawn could hand its fd table to a child the kernel never made.
//! The image is read into the top of the sidecar's budget region, the same
//! tail e7_firstuser uses; the caller's heap must still have that tail free,
//! so `poll` is given `HEAP.remaining()` and the spawn refuses unless the bump
//! cursor is at least `RESERVE + 0x1000` below the end.
//!
//! The applet and the event loop are in the same thread, so the request is a
//! small static record rather than a channel: the applet posts a path, the
//! event loop's `poll` performs the spawn once, and the applet yields until
//! `result()` returns the code. No profile, registration or checkpoint slot
//! is added here — the default tenant's record names tasks 0/1 only, and the
//! child's name stays outside it.

use aerosls_proto::kabi::Kernel;
use aerosls_proto::manifest::{
    pack_manifest, ArgEnv, Bootstrap, Budget, Cpu, Image, Limits, Manifest,
    ManifestCap, MAX_MANIFEST_CAPS,
};
use aerosls_vfs::{BufferAlloc, Vfs};

use crate::linux::LINUX_DEFAULT_ENV;

/// The child's kernel name. It is also its argv[0].
pub const TASK_NAME: &str = "linux.run.0";

/// The top of the budget region reserved for the staged image. The kernel
/// copies the image synchronously inside `linux_spawn`, so the bytes only
/// need to be valid until that syscall returns.
pub const RESERVE: u64 = 1024 * 1024;

/// Step 1's static-image cap. A tenant upload larger than this is refused
/// before the kernel sees it, keeping the reserve and the heap tail bounded.
pub const MAX_IMAGE: u64 = 64 * 1024;

/// The path buffer in the applet→event-loop request.
pub const MAX_PATH: usize = 128;

pub const ERR_TAIL: i32 = -20;
pub const ERR_BUDGET: i32 = -21;
pub const ERR_EMPTY: i32 = -22;
pub const ERR_NOT_STATIC: i32 = -23;
pub const ERR_DYNAMIC: i32 = -24;
pub const ERR_VFS: i32 = -25;
pub const ERR_TRACK: i32 = -26;
pub const ERR_PATH: i32 = -27;
pub const ERR_ALREADY: i32 = -28;
pub const ERR_BUSY: i32 = -29;

const IMAGE_VBASE: u64 = 0x4000_0000_0000; /* USER_PROC_CODE_BASE */
const HEAP_GUARD_BYTES: u64 = 4096;        /* cap.c: one guard page */
const MEM_BUDGET_BYTES: u64 = 256 * 1024;
const STACK_BUDGET_BYTES: u64 = 64 * 1024;

const REQ_IDLE: u8 = 0;
const REQ_PENDING: u8 = 1;
const REQ_DONE: u8 = 2;

struct Request {
    state: u8,
    path_len: u8,
    code: i32,
    path: [u8; MAX_PATH],
}

static mut REQUEST: Request = Request {
    state: REQ_IDLE,
    path_len: 0,
    code: 0,
    path: [0; MAX_PATH],
};

/// One kernel-created child per sidecar. Set at the `linux_spawn` boundary,
/// not on full success: `Vfs::add_task` has no removal, so a retry after a
/// partial wiring failure would leak a task slot or a child.
static mut SPAWNED: bool = false;

/// Post one spawn request. The applet calls this once; the event loop picks
/// it up on its next pass. An already-created child is final: the one-shot is
/// what keeps `Vfs::add_task`'s missing removal from becoming a
/// tenant-controlled task-slot leak.
pub fn request(path: &str) -> Result<(), i32> {
    if path.is_empty() || path.len() > MAX_PATH {
        return Err(ERR_PATH);
    }
    unsafe {
        if SPAWNED {
            return Err(ERR_ALREADY);
        }
        let r = &mut *core::ptr::addr_of_mut!(REQUEST);
        if r.state != REQ_IDLE {
            return Err(ERR_BUSY);
        }
        r.path[..path.len()].copy_from_slice(path.as_bytes());
        r.path_len = path.len() as u8;
        r.state = REQ_PENDING;
        r.code = 0;
    }
    Ok(())
}

/// The result the applet polls for: `None` while the event loop has not run
/// the spawn, `Some(0)` on success, `Some(rc)` on a refusal. Reading the
/// result consumes it: the applet is about to exit, and the NEXT `linux run`
/// must be able to post a fresh request. Without this reset every refusal left
/// the record DONE and the next invocation answered `busy` instead of being
/// judged by the spawner. `SPAWNED` is separate and still bounds the step to
/// one kernel-created child per sidecar.
pub fn result() -> Option<i32> {
    unsafe {
        let r = &mut *core::ptr::addr_of_mut!(REQUEST);
        if r.state == REQ_DONE {
            let code = r.code;
            r.state = REQ_IDLE;
            Some(code)
        } else {
            None
        }
    }
}

/// Perform a pending request once. Called from the sidecar's event loop for
/// an environment sidecar only (`caps.identity.is_some()`), before
/// `linux::service`, so the child's first forward is serviced in the same
/// pass that created it.
pub fn poll<K: Kernel, A: BufferAlloc>(
    k: &K,
    vfs: &mut Vfs<K, A>,
    budget_base: u64,
    budget_len: u64,
    heap_remaining: usize,
) {
    let mut path = [0u8; MAX_PATH];
    let path_len: usize;
    unsafe {
        let r = &*core::ptr::addr_of!(REQUEST);
        if r.state != REQ_PENDING {
            return;
        }
        path_len = r.path_len as usize;
        if path_len == 0 || path_len > MAX_PATH {
            let r = &mut *core::ptr::addr_of_mut!(REQUEST);
            r.state = REQ_DONE;
            r.code = ERR_PATH;
            return;
        }
        path[..path_len].copy_from_slice(&r.path[..path_len]);
    }

    let path = unsafe { core::str::from_utf8_unchecked(&path[..path_len]) };
    let rc = match spawn(k, vfs, path, budget_base, budget_len, heap_remaining) {
        Ok(()) => 0,
        Err(rc) => rc,
    };

    unsafe {
        let r = &mut *core::ptr::addr_of_mut!(REQUEST);
        r.state = REQ_DONE;
        r.code = rc;
    }
}

/// Stage `path` at the budget tail and spawn it. The image is read through
/// VFS task 0 (init's table is alive in every tenant: the default init parks
/// in its supervisor loop), and the child's VFS task is allocated only after
/// the kernel has returned the channel pair. `heap_remaining` is the sidecar
/// bump heap's free tail at the moment of the spawn; the tail reserve must
/// still be entirely below the heap cursor.
pub fn spawn<K: Kernel, A: BufferAlloc>(
    k: &K,
    vfs: &mut Vfs<K, A>,
    path: &str,
    budget_base: u64,
    budget_len: u64,
    heap_remaining: usize,
) -> Result<(), i32> {
    if budget_len < RESERVE + 0x1000 {
        return Err(ERR_TAIL);
    }
    if heap_remaining < (RESERVE + 0x1000) as usize {
        return Err(ERR_TAIL);
    }
    let end = budget_base.checked_add(budget_len).ok_or(ERR_BUDGET)?;
    let main_kaddr = end.checked_sub(RESERVE).ok_or(ERR_BUDGET)?;

    /* Task 0, deliberately: no VFS task is allocated until the kernel has
     * accepted the image. A failed open/read/refusal thus leaks nothing. */
    let image_len = read_vfs(vfs, 0, path, main_kaddr, MAX_IMAGE)?;
    if image_len == 0 {
        return Err(ERR_EMPTY);
    }
    let image = unsafe {
        core::slice::from_raw_parts(main_kaddr as *const u8, image_len as usize)
    };
    if !is_elf_exec(image) {
        return Err(ERR_NOT_STATIC);
    }
    if has_interp(image) {
        return Err(ERR_DYNAMIC);
    }

    let caps: [Option<ManifestCap<'_>>; MAX_MANIFEST_CAPS] = [None; MAX_MANIFEST_CAPS];
    let m = Manifest {
        version_major: 1,
        version_minor: 0,
        /* No FLAG_TENANT_LINUX / gate bit: step 1's child is an ordinary
         * tenant Linux task, outside the checkpoint registry. */
        flags: 0,
        name: Some(TASK_NAME),
        personality: Some("linux.run.v1"),
        image: Some(Image {
            offset: 0,
            size: image_len as u32,
            entry: 0, /* ignored for an ELF; valid because 0 < size */
        }),
        budget: Some(Budget {
            mem_bytes: MEM_BUDGET_BYTES,
            stack_bytes: STACK_BUDGET_BYTES as u32,
            heap_initial: 0,
        }),
        cpu: Some(Cpu { share: 10, preemptible: true }),
        limits: Some(Limits {
            max_tasks: 1,
            max_fds: 0,
            max_channels: 2,
            max_open_files: 0,
            chan_queue_depth: 4,
        }),
        caps,
        n_caps: 0,
        bootstrap: Some(Bootstrap {
            console: None,
            debug: None,
            log_level: 1,
        }),
        flags_value: Some(0),
        argenv: Some(ArgEnv::new(&[TASK_NAME], LINUX_DEFAULT_ENV)),
        interp: None,
        signature: None,
    };
    let blob = pack_manifest(&m, main_kaddr);

    /* target_partition 0 = inherit this tenant's partition. */
    let (ch_r, ch_w) = match k.linux_spawn(&blob, 0) {
        Ok(pair) => pair,
        Err(rc) => return Err(rc),
    };

    /* The kernel created the child, so the one-spawn limit is spent even if
     * the VFS task or the linux track table cannot be wired below. Only now
     * does the VFS task exist: a refusal above could not have leaked a slot. */
    unsafe { SPAWNED = true; }
    let vt = match vfs.add_task() {
        Ok(vt) => vt,
        Err(_) => {
            k.close(ch_r, 0, 0).ok();
            k.close(ch_w, 0, 0).ok();
            return Err(ERR_VFS);
        }
    };

    let pid = k.sidecar_pid(TASK_NAME, 0) as u64;
    let n_img_pages = (image_len + 4095) / 4096;
    let stack_base = IMAGE_VBASE + n_img_pages * 4096 + HEAP_GUARD_BYTES;
    let stk_pages = (STACK_BUDGET_BYTES + 4095) / 4096;
    let heap_base = stack_base + stk_pages * 4096;
    let heap_limit = heap_base + ((MEM_BUDGET_BYTES + 4095) / 4096) * 4096;

    if !crate::linux::track(
        ch_r,
        ch_w,
        TASK_NAME,
        0,
        heap_base,
        heap_limit,
        (0, 0),
        vt,
        pid,
        false,
    ) {
        k.close(ch_r, 0, 0).ok();
        k.close(ch_w, 0, 0).ok();
        return Err(ERR_TRACK);
    }

    Ok(())
}

/// Read at most `cap` bytes of `path` into the identity-mapped tail at `dst`.
/// Returns the image length; -5 is an open failure (a missing payload), -6 is
/// "larger than the cap", -7 is a read failure. These are the same codes
/// m2_dyn's read_vfs uses, so the applet's error mapping is shared.
fn read_vfs<K: Kernel, A: BufferAlloc>(
    vfs: &mut Vfs<K, A>,
    task: u32,
    path: &str,
    dst: u64,
    cap: u64,
) -> Result<u64, i32> {
    let fd = vfs.open(task, path, 0, 0).map_err(|_| -5)?;
    let mut chunk = [0u8; 4096];
    let mut off = 0u64;
    loop {
        if off == cap {
            /* The cap is inclusive: one extra byte distinguishes an exact-cap
             * image from one the tenant tried to grow past it. */
            let mut extra = [0u8; 1];
            match vfs.read_at(task, fd, off, &mut extra) {
                Ok(0) => break,
                Ok(_) => {
                    vfs.close(task, fd).ok();
                    return Err(-6);
                }
                Err(_) => {
                    vfs.close(task, fd).ok();
                    return Err(-7);
                }
            }
        }
        let want = (cap - off).min(chunk.len() as u64) as usize;
        match vfs.read_at(task, fd, off, &mut chunk[..want]) {
            Ok(0) => break,
            Ok(n) => {
                unsafe {
                    core::ptr::copy_nonoverlapping(
                        chunk.as_ptr(),
                        (dst + off) as *mut u8,
                        n,
                    );
                }
                off += n as u64;
            }
            Err(_) => {
                vfs.close(task, fd).ok();
                return Err(-7);
            }
        }
    }
    vfs.close(task, fd).ok();
    Ok(off)
}

fn is_elf_exec(b: &[u8]) -> bool {
    b.len() >= 18
        && &b[0..4] == &b"\x7FELF"[..]
        && b[4] == 2                 /* ELFCLASS64 */
        && b[5] == 1                 /* little-endian */
        && u16::from_le_bytes([b[16], b[17]]) == 2 /* ET_EXEC */
}

/// Walk the program headers for PT_INTERP. A dynamic ET_EXEC is step 2's
/// material; step 1 accepts a static image only.
fn has_interp(b: &[u8]) -> bool {
    if b.len() < 64 {
        return false;
    }
    let phoff = u64::from_le_bytes([
        b[32], b[33], b[34], b[35], b[36], b[37], b[38], b[39],
    ]) as usize;
    let phentsize = u16::from_le_bytes([b[54], b[55]]) as usize;
    let phnum = u16::from_le_bytes([b[56], b[57]]) as usize;
    if phentsize < 56 {
        return false;
    }
    for i in 0..phnum {
        let Some(off) = phoff.checked_add(i.saturating_mul(phentsize)) else {
            return false;
        };
        let Some(hdr) = b.get(off..off.saturating_add(56)) else {
            return false;
        };
        if u32::from_le_bytes([hdr[0], hdr[1], hdr[2], hdr[3]]) == 3 {
            return true;
        }
    }
    false
}
