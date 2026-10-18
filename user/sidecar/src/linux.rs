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
//!   - everything else — the fd class (its bytes live in the CHILD's address
//!     space, a separate cr3 by §3.5, so `write` is a kernel copy + the
//!     sidecar's fd table: POSIX Sidecar Phase 2 §3-§4), the mem class
//!     (§5.4, increment 5), `arch_prctl` (§5.3, increment 4 — kernel-side),
//!     the sig class (accepted-never-delivered), `clock_gettime` (needs the
//!     clock plus a child-buffer write) and any number outside the census —
//!     is answered `ENOSYS` with its number logged, §11 Q5's recommended
//!     policy: visible and cheap, never a silent success.
//!
//! Nothing here interprets a Linux syscall beyond choosing that answer: the
//! kernel's forward carries argument POSITIONS only, and the sidecar owns
//! every semantic decision (the §3.3 step-6 contract — there is exactly one
//! POSIX implementation and it is not in the kernel).

use aerosls_proto::kabi::{GrantedCap, Kernel};

/// The wire size of `kernel/linux_abi.h`'s struct, pinned as literals in
/// `tests/linux_personality_host_test.c` (72 = num 8 + args 48 + ret 8 +
/// status 4 + pad 4) — this module and that test are the two ends of one
/// ABI.
pub const LINUX_SYSCALL_REQ_BYTES: usize = 72;

/// CAP_E* (kernel/cap.h, negatives): the family this sidecar answers with,
/// because linux_errno_from_cap maps every one of them into musl's errno
/// window. Values pinned by tools/aeroidl/tests/cross_lang_constants.rs.
const E_NOSYS: i32 = -10;      /* CAP_ENOSYS  -> -ENOSYS  (§5.7) */
const E_CONFLICT: i32 = -11;   /* CAP_ECONFLICT -> -EBUSY: the reaper could not
                                * resolve its own child — a state conflict, not
                                * a permission or an absence of policy */
const STATUS_OK: i32 = 0;      /* CAP_ERR_OK: the reply carries a real `ret` */

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
    /// exit_group: reap the task, reply nothing.
    Exit,
}

/// The census's first answers (see the module header for the honest
/// refusals). Pure over the request's registers — pid/name resolution
/// belongs to the caller (`service`), because it needs `Kernel`.
fn answer(num: u64, args: &[u64; 6]) -> Answer {
    match num {
        /* set_tid_address (218, LSC_LOCAL): single-threaded — the "address
         * of the cleared TID" can never be written, so the complete and
         * truthful answer is the address itself (§5.6: stored, ignored). */
        218 => Answer::Reply(args[0], STATUS_OK),
        /* exit_group (231, LSC_EXIT): the sidecar reaps, the frames return
         * (§5.6) — handled by service(), which owns the pid lookup. */
        231 => Answer::Exit,
        /* Everything else, named in the module header: ENOSYS + a logged
         * number, never a silent success and never a fabricated value. */
        _ => Answer::Reply(0, E_NOSYS),
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
pub fn track(ch_r: u32, ch_w: u32, name: &str, partition: u32) -> bool {
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
        enosys_log(0xF1F1, b"[POSIX] linux reply send failed\n");
    }
}

/// One ENOSYS/protocol log line, host-build tolerant (entry.rs's klog is
/// the target-feature serial path; on host fakes the answers still run,
/// they just stay quiet).
fn enosys_log(tag_num: u32, extra: &[u8]) {
    #[cfg(feature = "target")]
    crate::entry::klog(b"[E7] linux: num=", tag_num, extra);
    #[cfg(not(feature = "target"))]
    {
        let _ = (tag_num, extra);
    }
}

/// Drain every tracked child's channel: answer what is queued, reap what
/// exited. One non-blocking recv per child per pass — the same poll shape
/// as the console arm of the entry loop — so it is safe to call from the
/// loop's hot path, and the quiesce wait set (append_wait) is what turns a
/// parked sidecar into a woken one.
pub fn service<K: Kernel>(k: &K) {
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

        let mut buf = [0u8; LINUX_SYSCALL_REQ_BYTES];
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

        if rr.len != LINUX_SYSCALL_REQ_BYTES {
            /* The kernel's forward always posts exactly 72 bytes, so this
             * cannot be a parseable request. Answer with a well-formed
             * ENOSYS reply whose num=0: it CANNOT echo the in-flight
             * request's number, so the kernel's apply refuses it -EPROTO —
             * the task errors out loud instead of hanging on a reply that
             * never fits the contract. */
            enosys_log(0xBAD0, b"[POSIX] malformed request -> -EPROTO\n");
            reply(k, ch_w, rr.tag, 0, 0, E_NOSYS);
            continue;
        }

        let num = read_u64(&buf, 0);
        let mut args = [0u64; 6];
        for (ai, a) in args.iter_mut().enumerate() {
            *a = read_u64(&buf, 8 + 8 * ai);
        }

        match answer(num, &args) {
            Answer::Reply(ret, status) => {
                if status == E_NOSYS {
                    enosys_log(num as u32, b" -> ENOSYS\n");
                } else {
                    /* An answered syscall must be visible too: this line
                     * is the serial-log half of E7's round-trip proof
                     * (a real task's real request arrived, and its reply
                     * is on its way back through §5.7). */
                    enosys_log(num as u32, b" ok\n");
                }
                reply(k, ch_w, rr.tag, num, ret, status);
            }
            Answer::Exit => {
                /* Visible like every other answer: exit_group is the one
                 * forward that gets NO reply, so without this line the
                 * reap would be invisible in the log. */
                enosys_log(num as u32, b" exit_group\n");
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
                    enosys_log(num as u32, b" exit_group: NOT-FOUND (reap refused)\n");
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
