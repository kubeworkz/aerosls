//! linux.rs — the sidecar core's Linux-syscall answers (E7 increment 3,
//! AeroSLS-Linux-ABI-Shim-Design-v0.1.md §3.3 step 6).
//!
//! A Linux-personality task's syscalls arrive here as the kernel's 72-byte
//! `SLSLinuxSyscallRequest` on the messenger channel that task was spawned
//! with (`Kernel::linux_spawn`'s CHAN_R). Answering is the whole of D1's
//! point: the sidecar performs — or refuses — the operation through what it
//! already has (its own policy, its own `Kernel` syscalls, its own authority),
//! and replies with the same struct carrying `ret`/`status`; the kernel maps
//! `status` through the §5.7 errno table on the way back, so a CAP code never
//! reaches the task as anything but a Linux errno.
//!
//! The answers this increment knows (§5.6's census, answered honestly):
//!   - `set_tid_address` (218): store-and-ignore — single-threaded, so the
//!     register echo is the complete POSIX answer;
//!   - `exit_group` (231): the sidecar REAPS (§5.6's exit class) — the task is
//!     parked awaiting this exchange, so a `proc_kill` is an immediate
//!     teardown and the frames return to the partition. No reply is owed:
//!     the task never runs again. If the pid cannot be resolved the task is
//!     answered rather than left parked forever;
//!   - `arch_prctl` (158, §5.3): the POLICY half of the one kernel semantic —
//!     SET_FS is GRANTED only for a user-canonical address (the kernel
//!     re-checks that bound at the wrmsr), everything else — SET_GS (no GSR
//!     in this kernel), GET_FS/GET_GS (the value lives in the kernel's field,
//!     not here, and neither is in the census), unknown cmd words — is
//!     answered ENOSYS with its number logged, honest by the rule below;
//!   - `write` (1, the fd class's byte crossing — E7 increment 5): the
//!     bytes live in the CHILD's address space (a separate cr3 by §3.5),
//!     so the KERNEL copied them out into the message behind this
//!     72-byte header (kernel/linux_abi.c's plan + copy); this side owns
//!     the POLICY half — the task's fd table (fds 0/1/2 are its console,
//!     POSIX Sidecar Phase 2 §3-§4), an unknown descriptor is EBADF, and
//!     the attached count must equal args[2] exactly or the request is
//!     malformed;
//!   - `brk` (12, the mem class's bump pointer — E7 increment 6, §5.4):
//!     the BUDGET SPAN is this side's ledger (it authored the manifest's
//!     BUDGET record; the kernel stamped its own copy of the span at
//!     create), so a growth inside the span is granted and echoed as the
//!     new break, and everything else answers the UNCHANGED current
//!     break — Linux's own brk failure shape, never an errno;
//!   - `mprotect` (10, §5.4's W^X refusal): write+execute together is
//!     refused EPERM — the refusal H2's Node exclusion IS, and the twin
//!     of the kernel's first-trap refusal (both ends exist so neither
//!     alone is load-bearing);
//!   - everything else — `open`/`close`/`lseek` and the rest of the fd
//!     class (Phase 2 §3-§4), `mmap`/`munmap` (§5.4's sub-allocation
//!     half, the increment after 6), any mprotect whose prot is not
//!     W|X (real permission changes are not landed), the sig class
//!     (accepted-never-delivered), `clock_gettime` (needs the clock plus
//!     a child-buffer write) and any number outside the census —
//!     is answered `ENOSYS` with its number logged, §11 Q5's recommended
//!     policy: visible and cheap, never a silent success.
//!
//! Nothing here interprets a Linux syscall beyond choosing that answer: the
//! kernel's forward carries argument POSITIONS only, and the sidecar owns
//! every semantic decision (the §3.3 step-6 contract — there is exactly one
//! POSIX implementation and it is not in the kernel).

use aerosls_proto::kabi::{GrantedCap, Kernel};
use aerosls_vfs::CharNode;   /* E7 increment 5: the fd table's console slot —
                              * service() holds the handle because only the
                              * entry loop has the CharNode */

/// The wire size of `kernel/linux_abi.h`'s struct, pinned as literals in
/// `tests/linux_personality_host_test.c` (72 = num 8 + args 48 + ret 8 +
/// status 4 + pad 4) — this module and that test are the two ends of one
/// ABI.
pub const LINUX_SYSCALL_REQ_BYTES: usize = 72;

/// The environment this environment's Linux children cross on the spawn
/// (§5.2's environment-crossing record, `SIDECAR_TAG_ARGENV`): the POSIX
/// sidecar's own default shell environment — `PATH=/bin` (applets'
/// bare-command search path) and `HOME=/root` — the same two pairs
/// `applets.rs`'s `sh` initializes on its first prompt. The three e7 spawn
/// sites author their record's envp from THIS list so the twins cannot
/// drift, and the answer is honest about time: these spawns run at boot,
/// before any shell exists, so a later `export` lives in the shell task's
/// own env region and never crossed (or pretended to). Clause O7 pins the
/// list and its three users.
pub const LINUX_DEFAULT_ENV: &[&str] = &["PATH=/bin", "HOME=/root"];

/// CAP_E* (kernel/cap.h, negatives): the family this sidecar answers with,
/// because linux_errno_from_cap maps every one of them into musl's errno
/// window. Values pinned by tools/aeroidl/tests/cross_lang_constants.rs.
const E_NOSYS: i32 = -10;      /* CAP_ENOSYS  -> -ENOSYS  (§5.7) */
const E_CONFLICT: i32 = -11;   /* CAP_ECONFLICT -> -EBUSY: the reaper could not
                                * resolve its own child — a state conflict, not
                                * a permission or an absence of policy */
const E_INVAL: i32 = -1;      /* CAP_EINVAL -> -EINVAL (§5.7): the §5.3 address
                                * bound — a non-canonical FS base is a bad
                                * VALUE, not an unimplemented command */
const E_BADF: i32 = -2;       /* CAP_EBADF -> -EBADF (§5.7): the fd table has
                                * no slot for this descriptor — the fd
                                * class's own policy answer (increment 5),
                                * pinned at cap.h's -2 so the map delivers
                                * 9, not an invented number */
const E_REVOKED: i32 = 2;     /* CAP_ERR_REVOKED -> -EIO (§5.7): the console
                                * channel died under the write — cap.h's
                                * literal 2, a POSITIVE CAP_ERR_ status */
const E_PERM: i32 = -12;      /* CAP_EPERM -> -EPERM (§5.7): §5.4's W|X
                                * refusal — pinned at cap.h's -12 so the
                                * map delivers 1, never an invented number */
const STATUS_OK: i32 = 0;      /* CAP_ERR_OK: the reply carries a real `ret` */

/// E7 increment 5 (the fd class): the number that may carry attached
/// bytes, and the bound the kernel's LINUX_FD_WRITE_CHUNK (1024) mirrors
/// — a request whose count is larger than the chunk, or whose payload
/// length is not exactly args[2], is malformed by this side's own twin
/// arithmetic rather than trusted because it arrived.
const LINUX_WRITE: u64 = 1;
const FD_PAYLOAD_CAP: usize = 1024;

/// The reply wire format: `kernel/linux_abi.h`'s SLSLinuxSyscallRequest,
/// field order C-guaranteed by `repr(C)`. Only num/ret/status are read by
/// the kernel's apply (`linux_syscall_apply_reply`), and args ride zeroed —
/// the request's own copy already lives on the kernel side.
#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct LinuxSyscallReq {
    pub num: u64,
    pub args: [u64; 6],
    pub ret: u64,
    pub status: i32,
    pub _pad: i32,
}

/* The one size that matters, checked at compile time: a layout drift from
 * the kernel's struct would make every reply unparseable (and the kernel's
 * size check would refuse it — the failure is loud, but it is nicer to not
 * build it at all). */
const _: () = assert!(core::mem::size_of::<LinuxSyscallReq>() == LINUX_SYSCALL_REQ_BYTES);

/// What the answer to one forwarded syscall IS.
enum Answer {
    /// Reply with this ret/status (status 0 = ret is the value).
    Reply(u64, i32),
    /// write(2) — the fd class's perform half (increment 5): the fd
    /// lookup already succeeded, so `service` performs these `len`
    /// payload bytes on the task's console-backed slot and replies with
    /// the count actually taken. Only `service` touches the console — it
    /// holds the handle; this stays pure over the request and the table.
    Write(usize),
    /// exit_group: reap the task, reply nothing.
    Exit,
}

/// One Linux task's §5.4 heap ledger: the budget span the create stamped
/// (the kernel's ProcessDescriptor carries the same three numbers as its
/// own mechanism-side copy — kernel/cap.c derives them from the manifest
/// this side authored; guard clause N2 pins both halves) and the bump
/// pointer this side moves. Pure over a LOCAL copy — `service` writes
/// `cur` back after the answer, like the fd table's reads — so the arm
/// below is assertable without a table or a task.
struct MemBrk {
    base: u64,
    limit: u64,
    cur: u64,
}

/// The census's first answers (see the module header for the honest
/// refusals). Pure over the request's registers, the task's fd table and
/// a local copy of its brk ledger — pid/name resolution and the console
/// handle belong to the caller (`service`), because they need `Kernel`
/// and the entry loop's objects.
fn answer(num: u64, args: &[u64; 6], fds: &[FdSlot], brk: &mut MemBrk) -> Answer {
    match num {
        /* set_tid_address (218, LSC_LOCAL): single-threaded — the "address
         * of the cleared TID" can never be written, so the complete and
         * truthful answer is the address itself (§5.6: stored, ignored). */
        218 => Answer::Reply(args[0], STATUS_OK),
        /* write (1, LSC_FD — the fd class, E7 increment 5): the fd table
         * decides, exactly as Phase 2 §3 puts the core between a
         * descriptor and its destination. 0/1/2 are the task's console
         * (a tty is bidirectional — Linux does not make write(0) an
         * EBADF); every other descriptor has no slot, and since
         * open/close are still ENOSYS (the default arm below) no other
         * slot can exist yet — so EBADF here is the whole truth, not a
         * shortcut. The BYTES are the payload the KERNEL copied out
         * (§3.5: they live in the child's cr3, which this side cannot
         * reach); the count is args[2], clamped by the kernel's plan and
         * cross-checked against the payload length by `service`. */
        1 => match fd_kind(fds, args[0]) {
            Some(FD_KIND_CONSOLE) => Answer::Write(args[2] as usize),
            /* A kind this increment does not perform — unreachable today
             * (only the console is ever registered); ENOSYS, not a fake. */
            Some(_) => Answer::Reply(0, E_NOSYS),
            None => Answer::Reply(0, E_BADF),
        },
        /* arch_prctl (158, LSC_MSR, §5.3): the POLICY side of the one
         * kernel semantic — mirror of kernel/linux_abi.h's bound (the Rust
         * side cannot include that header, so the literals are repeated,
         * commented as twins): GRANT `SET_FS` (0x1002) only for a
         * user-canonical address; a bad address is E_INVAL (a bad VALUE,
         * not an unimplemented command); SET_GS has no GSR in this kernel,
         * GET_FS/GET_GS return a value only the kernel's field holds (and
         * are not in the census), any other cmd word is ENOSYS. The kernel
         * re-checks canonicality at the wrmsr — the grant says WHETHER, the
         * mechanism still refuses a value it cannot write. */
        158 => match (args[0], args[1]) {
            (0x1002, a) if a <= 0x0000_7FFF_FFFF_FFFF => {
                Answer::Reply(0, STATUS_OK)
            }
            (0x1002, _) => Answer::Reply(0, E_INVAL),
            _ => Answer::Reply(0, E_NOSYS),
        },
        /* brk (12, LSC_MEM — §5.4, E7 increment 6): the bump pointer
         * inside the budget span, decided HERE because the span is this
         * side's policy (it authored the BUDGET record). args[0] == 0 is
         * the query and falls into the else below as a no-op: the
         * current break IS the answer. A growth inside the span is
         * granted and echoed as the new break; EVERYTHING else — past
         * the limit, at or below the current break (v1 GROWS ONLY: a
         * shrink is a refusal, not a crash), any nonsense low ask —
         * answers the UNCHANGED current break, which is Linux's own brk
         * failure shape: the syscall returns the current break rather
         * than an errno, and libc compares it against the request. The
         * KERNEL re-checks a grant against its own stamped span at the
         * reply (kernel/linux_abi.c's verdict, over the request the
         * first trap staged) — this arm decides WHETHER, the mechanism
         * still refuses a value it cannot map. */
        12 => {
            let want = args[0];
            if want > brk.cur && want <= brk.limit {
                brk.cur = want;
                Answer::Reply(want, STATUS_OK)
            } else {
                Answer::Reply(brk.cur, STATUS_OK)
            }
        }
        /* mprotect (10, LSC_MEM — §5.4's W^X): only the refusal exists
         * in v1 — simultaneous write+execute (prot & 0x2 & 0x4) is
         * refused EPERM, the twin of linux_mprotect_refuses_wx (the
         * kernel refuses it first-trap, before any post; both ends
         * exist so neither alone is the load-bearing refusal — guard
         * clause N3 pins both). Any other prot is ENOSYS with the
         * number logged: real permission changes, mmap and munmap are
         * the mem class's next increment, and a fake grant here would
         * be a page the kernel never mapped. */
        10 => {
            if args[2] & 0x2 != 0 && args[2] & 0x4 != 0 {
                Answer::Reply(0, E_PERM)
            } else {
                Answer::Reply(0, E_NOSYS)
            }
        }
        /* exit_group (231, LSC_EXIT): the sidecar reaps, the frames return
         * (§5.6) — handled by service(), which owns the pid lookup. */
        231 => Answer::Exit,
        /* Everything else, named in the module header: ENOSYS + a logged
         * number, never a silent success and never a fabricated value. */
        _ => Answer::Reply(0, E_NOSYS),
    }
}

/// One slot of a Linux task's fd table (E7 increment 5 — the fd class's
/// sidecar half: Phase 2 §3's table, at the granularity §3.5 allows — this
/// registry entry IS the task). `kind` grows with the class: only the
/// console exists while open() is ENOSYS, and an unknown kind answers
/// ENOSYS rather than pretending to perform something it cannot.
#[derive(Clone, Copy)]
struct FdSlot {
    used: bool,
    kind: u8,
}

const FD_KIND_CONSOLE: u8 = 0;
const LINUX_MAX_FDS: usize = 16;

/// Resolve a descriptor against a task's table — pure over the slice, so
/// every branch (slot present, slot empty, descriptor out of range) is
/// guard clause M4's to hold rather than an observation from one boot.
fn fd_kind(fds: &[FdSlot], fd: u64) -> Option<u8> {
    if fd >= LINUX_MAX_FDS as u64 {
        return None;
    }
    let s = &fds[fd as usize];
    if s.used {
        Some(s.kind)
    } else {
        None
    }
}

/// One Linux-personality child whose forwards this sidecar answers. The
/// registry is the spawn path's bookkeeping, not the kernel's: syscall 328
/// hands back only the CALLER's channel handles, and the reaper needs a
/// name to resolve a pid (`sidecar_pid`) — the kernel keeps no process-name
/// authority decision here that its own gates do not already own.
#[derive(Clone, Copy)]   /* Copy so the table can be a const-initialized
                         * static array ([..; N] repeat needs it) */
struct LinuxTask {
    used: bool,
    ch_r: u32,   /* the child's forwards arrive here (our recv handle)   */
    ch_w: u32,   /* replies leave here (our send handle)                 */
    name: [u8; 48],
    name_len: usize,
    partition: u32,
    fds: [FdSlot; LINUX_MAX_FDS],   /* increment 5: the task's fd table  */
    brk_base: u64,  /* increment 6: the §5.4 ledger — the span this side */
    brk_limit: u64, /* authored into the manifest and the kernel stamped */
    brk_cur: u64,   /* into its own descriptor, and the bump pointer     */
                    /* this side moves (the kernel's brk_cur is the      */
                    /* mechanism's twin — the probe proves they agree)   */
}

const MAX_LINUX_TASKS: usize = 8;

#[allow(static_mut_refs)]   /* single-threaded sidecar, entry-loop confined */
fn tasks() -> &'static mut [LinuxTask; MAX_LINUX_TASKS] {
    static mut TABLE: [LinuxTask; MAX_LINUX_TASKS] = [LinuxTask {
        used: false,
        ch_r: 0,
        ch_w: 0,
        name: [0; 48],
        name_len: 0,
        partition: 0,
        fds: [FdSlot { used: false, kind: 0 }; LINUX_MAX_FDS],
        brk_base: 0,
        brk_limit: 0,
        brk_cur: 0,
    }; MAX_LINUX_TASKS];
    unsafe { &mut TABLE }
}

/// Register a spawned Linux child so `service()` answers it. The spawn
/// caller (`Kernel::linux_spawn`) records the returned handles here — before
/// or after the child first runs, both safe: the child's forward parks
/// until a reply, and its message simply waits in the channel queue until
/// the next `service()` pass drains it. Returns false when the table is
/// full (the child then parks unanswered, which its first forward's
/// -EAGAIN/… never happens — it stays parked; a spawn path SHOULD check).
pub fn track(
    ch_r: u32,
    ch_w: u32,
    name: &str,
    partition: u32,
    heap_base: u64,
    heap_limit: u64,
) -> bool {
    let nlen = name.len().min(48);
    for t in tasks().iter_mut() {
        if t.used {
            continue;
        }
        t.used = true;
        t.ch_r = ch_r;
        t.ch_w = ch_w;
        t.name[..nlen].copy_from_slice(&name.as_bytes()[..nlen]);
        t.name_len = nlen;
        t.partition = partition;
        /* The §5.4 ledger is born with the task: the span the spawn
         * caller derived from the manifest it authored (the kernel's
         * create stamped its own copy from the same record — the two
         * are one truth, mirrored because the Rust side cannot include
         * kernel/linux_abi.h). The break starts at the span's base:
         * nothing is mapped above the stack until a brk grants it. */
        t.brk_base = heap_base;
        t.brk_limit = heap_limit;
        t.brk_cur = heap_base;
        /* The fd table is born with the task: fds 0, 1, 2 — its console,
         * registered at spawn like every process's stdio. All three
         * accept writes (a tty is bidirectional; write(0) is not EBADF
         * on Linux), and the slots are what later increments grow by
         * open()'s hand. */
        t.fds = [FdSlot { used: false, kind: 0 }; LINUX_MAX_FDS];
        for slot in t.fds.iter_mut().take(3) {
            slot.used = true;
            slot.kind = FD_KIND_CONSOLE;
        }
        return true;
    }
    false
}

/// Append every tracked child's recv channel to a wait set (the entry
/// loop's quiesce park): a child's forward must WAKE the sidecar, not spin
/// it. Silent no-op when `chans` is full — the service() poll in the loop
/// above still runs every pass, so a full wait set costs latency, never
/// correctness.
pub fn append_wait(chans: &mut [u32], n: &mut usize) {
    for t in tasks().iter() {
        if t.used && *n < chans.len() {
            chans[*n] = t.ch_r;
            *n += 1;
        }
    }
}

/// Little-endian read — the wire is the kernel's x86-64 struct, so LE is
/// native, but reading it byte-wise keeps the parse honest about that
/// instead of aliasing a packed buffer as a struct.
fn read_u64(buf: &[u8], off: usize) -> u64 {
    u64::from_le_bytes([
        buf[off],
        buf[off + 1],
        buf[off + 2],
        buf[off + 3],
        buf[off + 4],
        buf[off + 5],
        buf[off + 6],
        buf[off + 7],
    ])
}

/// The reply is the SAME struct back — num echoed, ret/status filled, args
/// zeroed — which is exactly what kernel/chan.c's `k_linux_deliver`
/// validates (72 bytes, num echo enforced by `linux_syscall_apply_reply`)
/// before mapping `status` through the §5.7 table. A send failure here can
/// only mean the child's end died mid-call: its own death woke its waiters,
/// so dropping the reply costs nothing but the log line.
fn reply<K: Kernel>(k: &K, ch_w: u32, tag: u32, num: u64, ret: u64, status: i32) {
    let mut payload = [0u8; LINUX_SYSCALL_REQ_BYTES];
    payload[0..8].copy_from_slice(&num.to_le_bytes());
    payload[56..64].copy_from_slice(&ret.to_le_bytes());
    payload[64..68].copy_from_slice(&status.to_le_bytes());
    if k.send(ch_w, tag, 0, &payload, &[], aerosls_proto::kabi::TIMEOUT_NONE).is_err() {
        /* No task context here: reply() is called from every arm of
         * service(), so the name is not in scope and this line says so
         * by name (`child=?`) rather than by silence. */
        enosys_log(0xF1F1, b"", b"[POSIX] linux reply send failed\n");
    }
}

/// One ENOSYS/protocol log line, host-build tolerant (entry.rs's klog is
/// the target-feature serial path; on host fakes the answers still run,
/// they just stay quiet).
///
/// The CHILD'S NAME rides in the line — the design doc's §10.13 step 0.5,
/// and the difference between a measurement and an inference. Several
/// Linux tasks share this one service loop, `num=` names only the
/// NUMBER, and without a name on the line a refusal belongs to any of
/// them: the E3 log's per-child counts could only be read off the
/// arithmetic that "fits three of each", never off the line that proves
/// it. `child=?` is what a line with NO task context says (reply's own
/// send-failure arm) — never an empty field, so a missing name looks
/// missing rather than like a parsing accident. The caps below keep a
/// 24-character name plus the longest suffix inside klog's own bound.
fn enosys_log(tag_num: u32, child: &[u8], extra: &[u8]) {
    #[cfg(feature = "target")]
    {
        let mut line = [0u8; 72];
        let mut off = 0;
        line[off..off + 6].copy_from_slice(b"child=");
        off += 6;
        let name: &[u8] = if child.is_empty() { b"?" } else { child };
        let n = name.len().min(24);
        line[off..off + n].copy_from_slice(&name[..n]);
        off += n;
        let e = extra.len().min(line.len() - off);
        line[off..off + e].copy_from_slice(&extra[..e]);
        off += e;
        crate::entry::klog(b"[E7] linux: num=", tag_num, &line[..off]);
    }
    #[cfg(not(feature = "target"))]
    {
        let _ = (tag_num, child, extra);
    }
}

/// Drain every tracked child's channel: answer what is queued, reap what
/// exited. One non-blocking recv per child per pass — the same poll shape
/// as the console arm of the entry loop — so it is safe to call from the
/// loop's hot path, and the quiesce wait set (append_wait) is what turns a
/// parked sidecar into a woken one.
///
/// `console` is the entry loop's CharNode — the fd table's console slots
/// name it, this function performs on it (increment 5), and the loop's own
/// pump drains what lands there to the kernel console channel on the next
/// pass, so a write becomes serial output without a second path.
pub fn service<K: Kernel>(k: &K, console: &CharNode) {
    for i in 0..MAX_LINUX_TASKS {
        if !tasks()[i].used {
            continue;
        }
        let ch_r = tasks()[i].ch_r;
        let ch_w = tasks()[i].ch_w;
        let partition = tasks()[i].partition;
        let mut name_buf = [0u8; 48];
        let nlen = tasks()[i].name_len;
        name_buf[..nlen].copy_from_slice(&tasks()[i].name[..nlen]);

        /* The recv buffer now carries the fd class's attached bytes
         * behind the header: 72 + the mirror of LINUX_FD_WRITE_CHUNK.
         * Undersize would be ERR_BUFSZ — which does NOT consume the
         * message (transport spec §3.4) — so this capacity is what stops
         * a write from wedging its own channel. */
        let mut buf = [0u8; LINUX_SYSCALL_REQ_BYTES + FD_PAYLOAD_CAP];
        let mut slots = [GrantedCap::default(); 1];
        let rr = match k.recv(ch_r, &mut buf, &mut slots) {
            Ok(rr) => rr,
            Err(_) => continue,   /* nothing queued (EAGAIN): next pass re-polls */
        };

        match rr.kind {
            /* The child died or closed its end: its forwards can no longer
             * come, and its channel slot is spent. Drop the registry entry
             * so a respawned child re-registers fresh. */
            aerosls_proto::CH_KIND_CLOSE => {
                tasks()[i].used = false;
                continue;
            }
            aerosls_proto::CH_KIND_MSG => {}
            _ => continue,
        }

        if rr.len < LINUX_SYSCALL_REQ_BYTES {
            /* Shorter than a header can never be a parseable request.
             * Answer with a well-formed reply whose num=0: it CANNOT echo
             * the in-flight request's number, so the kernel's apply
             * refuses it -EPROTO — the task errors out loud instead of
             * hanging on a reply that never fits the contract. */
            enosys_log(0xBAD0, &name_buf[..nlen], b"[POSIX] malformed request -> -EPROTO\n");
            reply(k, ch_w, rr.tag, 0, 0, E_NOSYS);
            continue;
        }

        let num = read_u64(&buf, 0);
        let mut args = [0u64; 6];
        for (ai, a) in args.iter_mut().enumerate() {
            *a = read_u64(&buf, 8 + 8 * ai);
        }

        /* E7 increment 5 (the fd class): exactly ONE number may carry
         * bytes behind the header, and only under the kernel's own
         * arithmetic — attached == args[2] (the clamp rides IN the
         * description) and attached ≤ FD_PAYLOAD_CAP (the twin of
         * LINUX_FD_WRITE_CHUNK). Every other number must be exactly 72.
         * A violation is the malformed arm's answer: num=0, which the
         * kernel's echo check refuses -EPROTO — loud, never a silent
         * success and never bytes this task did not write. */
        let payload: &[u8] = if num == LINUX_WRITE {
            let want = LINUX_SYSCALL_REQ_BYTES + args[2] as usize;
            if args[2] as usize > FD_PAYLOAD_CAP || rr.len != want {
                enosys_log(num as u32, &name_buf[..nlen], b"[POSIX] write payload mismatch -> -EPROTO\n");
                reply(k, ch_w, rr.tag, 0, 0, E_NOSYS);
                continue;
            }
            &buf[LINUX_SYSCALL_REQ_BYTES..rr.len]
        } else {
            if rr.len != LINUX_SYSCALL_REQ_BYTES {
                enosys_log(0xBAD0, &name_buf[..nlen], b"[POSIX] malformed request -> -EPROTO\n");
                reply(k, ch_w, rr.tag, 0, 0, E_NOSYS);
                continue;
            }
            &buf[..0]
        };

        let fds = tasks()[i].fds;
        let mut mem = MemBrk {
            base: tasks()[i].brk_base,
            limit: tasks()[i].brk_limit,
            cur: tasks()[i].brk_cur,
        };

        let ans = answer(num, &args, &fds, &mut mem);
        /* The ledger survives the reply: only a granted growth moves it
         * (the arm's own rule), and the KERNEL's brk_cur is its own
         * twin — the probe's denial-echo check is what proves the two
         * never drift. The base and limit are never written here: they
         * are the span, decided once, at spawn. */
        tasks()[i].brk_cur = mem.cur;

        match ans {
            Answer::Reply(ret, status) => {
                if status == E_NOSYS {
                    enosys_log(num as u32, &name_buf[..nlen], b" -> ENOSYS\n");
                } else {
                    /* An answered syscall must be visible too: this line
                     * is the serial-log half of E7's round-trip proof
                     * (a real task's real request arrived, and its reply
                     * is on its way back through §5.7). */
                    enosys_log(num as u32, &name_buf[..nlen], b" ok\n");
                }
                reply(k, ch_w, rr.tag, num, ret, status);
            }
            Answer::Write(len) => {
                /* The perform half of step 6 (increment 5): the fd
                 * lookup said this descriptor names the console, and the
                 * payload is the kernel's copy out of the child — both
                 * halves of \"the kernel copy plus the sidecar's fd
                 * table\" meet here. The count the task sees is what the
                 * console took (a short write is POSIX-legal; the clamp
                 * above already made args[2] the payload's own length). */
                match console.write(&payload[..len]) {
                    Ok(n) => {
                        enosys_log(num as u32, &name_buf[..nlen], b" write ok\n");
                        reply(k, ch_w, rr.tag, num, n as u64, STATUS_OK);
                    }
                    Err(_) => {
                        /* The console channel is dead: the write did not
                         * happen, and that is an I/O error — CAP_ERR_
                         * REVOKED's -EIO through §5.7 — never a success
                         * and never ENOSYS. */
                        enosys_log(num as u32, &name_buf[..nlen], b" write FAILED (console closed)\n");
                        reply(k, ch_w, rr.tag, num, 0, E_REVOKED);
                    }
                }
            }
            Answer::Exit => {
                /* Visible like every other answer: exit_group is the one
                 * forward that gets NO reply, so without this line the
                 * reap would be invisible in the log. */
                enosys_log(num as u32, &name_buf[..nlen], b" exit_group\n");
                /* exit_group: reap through the same name->pid lookup the
                 * environment destroy path uses (sidecar_pid is the
                 * liveness test — a name that no longer resolves is a
                 * task whose teardown already ran, so there is nothing
                 * left to kill and nothing owed). */
                let name = core::str::from_utf8(&name_buf[..nlen]).unwrap_or("");
                let pid = k.sidecar_pid(name, partition);
                if pid == 0 {
                    /* Cannot find it: answer rather than strand the task
                     * parked forever on a reply nobody will send. */
                    enosys_log(num as u32, &name_buf[..nlen], b" exit_group: NOT-FOUND (reap refused)\n");
                    reply(k, ch_w, rr.tag, num, 0, E_CONFLICT);
                } else {
                    /* The task is PROC_BLOCKED awaiting this very exchange
                     * (never RUNNING), so the kill is an immediate teardown
                     * — frames return to the partition (§5.6's exit class).
                     * No reply: the task will never run again, and its
                     * channel goes with its table. */
                    k.proc_kill(pid);
                    tasks()[i].used = false;
                }
            }
        }
    }
}
