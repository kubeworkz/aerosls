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
//!   - `rt_sigaction` (13) / `rt_sigprocmask` (14) / `tkill` (200) — the
//!     signal seam (§10.13's step 2): the act/set bytes arrive ATTACHED
//!     (the kernel copied them out of the child; this side never reads
//!     the child's memory), dispositions record DFL/IGN only — a handler
//!     install is ENOSYS by name BEFORE any mutation (no signal frames in
//!     v1), the mask records BLOCK/UNBLOCK/SETMASK exactly, and delivery
//!     is REAL: a tkill to the task's own tid whose signal is unblocked
//!     with a default-terminate action kills the task through exit_group's
//!     own reap path (no reply — it never runs again), while a blocked
//!     signal pends and is delivered when a later rt_sigprocmask's new
//!     mask reveals it — which is exactly how musl's `raise()` dies on
//!     real Linux (block → tkill → unblock → the default action runs).
//!     Refused by name with the number logged: handler invocation (no
//!     frames/sigreturn), stop-by-default delivery, and a tid that is not
//!     this task's own (ESRCH — cross-task signals are unscoped, never
//!     silently re-targeted at the sender). The oldact/oldset out-pointer
//!     is NO LONGER refused: E7 increment 12 landed §10.10's copy-IN gap —
//!     the old disposition/mask travels back as this reply's PAYLOAD
//!     (Answer::ReplyBytes), the kernel copies it into the span its own
//!     first trap validated, and service() bounds the bytes against the
//!     forwarded out-pointer before anything is sent;
//!   - `mmap` (9, the mem class's anonymous-map half — E7 increment 16,
//!     §5.4): the ledger's cursor decides the address for an anonymous
//!     private map inside the budget span (or the caller's own FIXED
//!     address echoes back when it is inside the span), PROT_NONE maps
//!     no frames, W|X is EPERM and every un-served shape is refused by
//!     name with the request's words on the line — the kernel's
//!     first-trap ladder repeated as this side's twin, and the kernel's
//!     verdict re-checks every grant against its own span and its own
//!     cursors, so neither end is load-bearing alone;
//!   - everything else — `open`/`close`/`lseek` and the rest of the fd
//!     class (Phase 2 §3-§4), `munmap` (frames return through the
//!     partition at the task's teardown; unmapping through the channel
//!     is its own seam), any mprotect whose prot is not
//!     W|X (real permission changes are not landed), `clock_gettime`
//!     (needs the clock plus
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
const E_NOMEM: i32 = -7;      /* CAP_ENOMEM -> -ENOMEM (§5.7): the mem
                                * class's budget shape (increment 16) — a
                                * FIXED ask outside the span, or a cursor
                                * that cannot fit the request */
const STATUS_OK: i32 = 0;      /* CAP_ERR_OK: the reply carries a real `ret` */

/// E7 increment 10 (the signal seam, §10.13's step 2): CAP_ERR_TARGET
/// (kernel/cap.h's literal 7) pinned so tkill's "no such thread" reaches
/// the task as -ESRCH through §5.7, never as an invented number.
const E_TARGET: i32 = 7; /* CAP_ERR_TARGET -> -ESRCH (§5.7) */

/// The sig seam's numbers and sizes — twins of kernel/linux_abi.h's
/// LINUX_RT_SIGACTION / LINUX_RT_SIGPROCMASK / LINUX_SIGACT_BYTES /
/// LINUX_SIGSET_BYTES (the Rust side cannot include that header, so the
/// literals repeat, commented as twins; guard clause R pins both halves
/// and the kernel's plan derives the SAME attach length from these
/// same words: num and args[1] != 0).
const LINUX_RT_SIGACTION: u64 = 13;
const LINUX_RT_SIGPROCMASK: u64 = 14;
const LINUX_TKILL: u64 = 200;
/// mmap(9) — armed since E7 increment 16 (§5.4's anonymous-map half):
/// the 9 arm below answers the measured allocator's shapes from the
/// ledger, and every refusal's LOG line carries the request's own
/// numbers (addr/len/prot/flags) so "the allocator asked for X and this
/// shim answered Y" is read off the line, never reconstructed.
const LINUX_MMAP: u64 = 9;
const SIGACT_BYTES: usize = 32; /* kernel-ABI struct sigaction: four u64s */
const SIGSET_BYTES: u64 = 8;    /* the sigset this seam crosses            */

/// How far a signal number may run: Linux x86-64 signals are 1..=64
/// (1..31 standard, 32..64 real-time; 0 is tkill's existence probe and
/// rt_sigaction's EINVAL case, never an index).
const SIG_MAX: usize = 64;

/// Dispositions this v1 can honestly hold: SIG_DFL and SIG_IGN. A handler
/// address is never stored — installation is refused before any mutation
/// (no signal frames exist to invoke it with, so recording it would make
/// every later delivery a coin flip between a silent kill and a silent
/// nothing).
const DISPO_DFL: u8 = 0;
const DISPO_IGN: u8 = 1;

/// A signal's DEFAULT action as POSIX defines it per signal number. The
/// three v1 outcomes are each honest: IGNORE — the default has no
/// observable effect, so answering 0 IS the complete delivery;
/// TERMINATE — the kill arm runs (the CORE class kills without writing a
/// dump: this kernel has no core-file path, named in §10.13 rather than
/// faked); STOP — not landed, refused by name: silently killing a
/// stop-default signal would be a different signal than the caller asked
/// for.
const ACT_IGNORE: u8 = 0;
const ACT_TERMINATE: u8 = 1;
const ACT_STOP: u8 = 2;

fn default_action(sig: u64) -> u8 {
    match sig {
        17 | 18 | 23 | 28 => ACT_IGNORE, /* CHLD CONT URG WINCH */
        19 | 20 | 21 | 22 => ACT_STOP,   /* STOP TSTP TTIN TTOU  */
        1..=64 => ACT_TERMINATE, /* the rest, incl. the CORE class and RT 32..64 */
        _ => ACT_STOP, /* unreachable: every arm range-checks first */
    }
}

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

/// What the answer to one forwarded syscall IS. Debug exists for the unit
/// tests' failure messages only (cfg_attr — it never builds into the
/// target binary).
#[cfg_attr(test, derive(Debug))]
enum Answer {
    /// Reply with this ret/status (status 0 = ret is the value).
    Reply(u64, i32),
    /// write(2) — the fd class's perform half (increment 5): the fd
    /// lookup already succeeded, so `service` performs these `len`
    /// payload bytes on the task's console-backed slot and replies with
    /// the count actually taken. Only `service` touches the console — it
    /// holds the handle; this stays pure over the request and the table.
    Write(usize),
    /// E7 increment 12 (the reply payload, §10.13's step 3): the reply
    /// carries PAYLOAD bytes BACK into the child — rt_sigaction's oldact
    /// and rt_sigprocmask's oldset. Only `service` sends them (it owns the
    /// bound: the bytes must equal the clamp the forwarded out-pointer
    /// implies, or the answer goes out as num=0 → -EPROTO — the twin of
    /// the deliver's staged-len check, neither end load-bearing alone).
    /// The fixed buffer is the largest payload any current number owes
    /// (SIGACT_BYTES = 32); `len` is the exact count that crosses.
    ReplyBytes(u64, usize, [u8; SIGACT_BYTES]),
    /// exit_group: reap the task, reply nothing.
    Exit,
    /// E7 increment 10 (the signal seam): a signal's DEFAULT ACTION ran —
    /// reap the task through exit_group's own path and reply nothing. On
    /// real Linux the process dies inside the delivering syscall and never
    /// returns to its caller, so "no reply" is the faithful shape; the
    /// variant is distinct from `Exit` only so the serial log names WHICH
    /// decision killed the task — the boot's attribution reads the line,
    /// not an inference. The payload is the SIGNAL NUMBER itself (increment
    /// 14): the kill line prints `sig=<N>`, so WHICH signal killed the task
    /// is read off the line rather than reconstructed from which arms ran.
    SignalKill(u64),
}

/// One Linux task's §5.4 heap ledger: the budget span the create stamped
/// (the kernel's ProcessDescriptor carries the same four numbers as its
/// own mechanism-side copy — kernel/cap.c derives them from the manifest
/// this side authored; guard clause N2 pins both halves) and the two
/// cursors this side moves: brk's bump pointer `cur` growing up from the
/// base, and `mtop` — the anonymous-map cursor (increment 16) — born at
/// the limit and descending by every granted non-FIXED mmap, so brk's
/// ceiling is min(limit, mtop) here and at the kernel's verdict: a page
/// one side granted is a page the other may never map over. Pure over a
/// LOCAL copy — `service` writes `cur` and `mtop` back after the answer,
/// like the fd table's reads — so the arm below is assertable without a
/// table or a task.
struct MemBrk {
    base: u64,
    limit: u64,
    cur: u64,
    mtop: u64,
}

/// One Linux task's signal STATE (E7 increment 10, §10.13's step 2) — the
/// sidecar's half of D1: the dispositions, the blocked mask, the pending
/// bits, and the tid `tkill` is matched against. Pure over itself: every
/// arm below mutates it through `&mut`, `service` writes it back beside
/// the brk ledger, and the unit tests at this file's end drive the whole
/// seam without a table, a task or a boot.
///
/// `tid` deserves its own note because the seam's correctness rests on it:
/// this shim's tid IS `set_tid_address`'s answer (the arm below echoes
/// args[0]), and musl stores that return into `pt->tid` as an int32 — so
/// `tkill` hands back the sign-extended LOW 32 BITS of exactly this value.
/// The match therefore compares at u32 width, which is the only width at
/// which the two ends can be the same number.
#[derive(Clone, Copy)]
struct SigState {
    tid: u64,
    mask: u64,                /* blocked signals, bit (sig - 1), signals 1..=64 */
    pending: u64,             /* blocked signals that have arrived              */
    dispo: [u8; SIG_MAX + 1], /* indexed by signal number: DFL / IGN only       */
}

impl SigState {
    const fn new() -> Self {
        SigState {
            tid: 0,
            mask: 0,
            pending: 0,
            dispo: [DISPO_DFL; SIG_MAX + 1],
        }
    }
}

/// The bytes an oldact reply payload carries (E7 increment 12, §10.13's
/// step 3) — the kernel-ABI struct sigaction as THIS state records it:
/// the handler word at offset 0 (SIG_DFL 0 / SIG_IGN 1 — the only two
/// dispositions v1 stores; `no` is range-checked by every caller), and
/// every other field ZERO — sa_flags, sa_restorer and sa_mask were never
/// stored (a handler install is refused before mutation, so nothing may
/// depend on bytes this side declined to keep), and zero IS the honest
/// answer rather than a default invented for the reply. Twin of the
/// kernel's staged clamp (32 bytes — LINUX_SIGACT_BYTES), which the
/// deliver bounds the copy against.
fn old_act_bytes(sig: &SigState, no: u64) -> [u8; SIGACT_BYTES] {
    let mut b = [0u8; SIGACT_BYTES];
    let handler: u64 = if sig.dispo[no as usize] == DISPO_IGN { 1 } else { 0 };
    b[0..8].copy_from_slice(&handler.to_le_bytes());
    b
}

/// A SUCCESSFUL sig answer, with the out-pointer's payload attached iff
/// the forwarded args[2] (the oldact/oldset pointer) was non-NULL — the
/// arm-side half of service()'s bound: a NULL out-pointer owes no bytes
/// and must carry none (the deliver refuses payload on a request that
/// staged none), a non-NULL one owes exactly the struct's size. The two
/// refusal paths above never reach here — an errno reply carries no
/// payload, which is the deliver's shape-1 rule on the other end.
fn sig_ok(args: &[u64; 6], old: [u8; SIGACT_BYTES]) -> Answer {
    if args[2] != 0 {
        Answer::ReplyBytes(0, SIGACT_BYTES, old)
    } else {
        Answer::Reply(0, STATUS_OK)
    }
}

/// The bytes an oldset reply payload carries (increment 12): the 8-byte
/// mask, little-endian, in the same fixed buffer ReplyBytes owns — `len`
/// (SIGSET_BYTES) is what crosses and what service()'s bound checks, so
/// the buffer's upper bytes never leave this side. Twin of the kernel's
/// staged 8-byte clamp (LINUX_SIGSET_BYTES).
fn sigset_payload(mask: u64) -> [u8; SIGACT_BYTES] {
    let mut b = [0u8; SIGACT_BYTES];
    b[..SIGSET_BYTES as usize].copy_from_slice(&mask.to_le_bytes());
    b
}

/// The census's first answers (see the module header for the honest
/// refusals). Pure over the request's registers, the task's fd table, a
/// local copy of its brk ledger AND its signal state (E7 increment 10 —
/// `service` writes both back, exactly as each is only ever changed by
/// the arm that owns it), and the payload bytes the kernel attached (the
/// act/set structs — this side never reads the child's memory itself).
/// Pid/name resolution and the console
/// handle belong to the caller (`service`), because they need `Kernel`
/// and the entry loop's objects.
fn answer(
    num: u64,
    args: &[u64; 6],
    fds: &[FdSlot],
    brk: &mut MemBrk,
    sig: &mut SigState,
    payload: &[u8],
) -> Answer {
    match num {
        /* set_tid_address (218, LSC_LOCAL): single-threaded — the "address
         * of the cleared TID" can never be written, so the complete and
         * truthful answer is the address itself (§5.6: stored, ignored).
         * E7 increment 10: that echo IS this shim's tid — musl stores the
         * return into pt->tid as an int32 and tkill hands its low 32 bits
         * back — so the answer is also RECORDED here: the seam's whole
         * notion of "this task" for tkill's match (SigState's note). */
        218 => {
            sig.tid = args[0];
            Answer::Reply(args[0], STATUS_OK)
        }
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
        /* poll (7, LSC_FD — E7 increment 11, §10.13's step-2 coda): the
         * firstuser's LAST ENOSYS answer, and the run's line that §10.13
         * once annotated "munmap (7)" — the mislabel this increment
         * corrects (munmap is 11 and never fires before the kill;
         * num=00000007 is Rust std's lang_start_internal polling fds
         * {0,1,2} with a zero timeout before any handler exists, and an
         * errno outside its tolerated set aborts the task). The timeout
         * word is Linux's int in args[2], so the low 32 bits decide.
         *
         * timeout 0 — the probe — answers 0 EVENTS: this shim has no
         * console-input readiness state (§10.13's named input class), so
         * "nothing ready" is the whole truth it can tell, and
         * UNDER-reporting readiness can only send a caller back to its
         * own loop — never into an action built on readiness nobody
         * claimed. The array is never read and revents never written: a
         * count of 0 promises neither (POSIX defines revents only for
         * ready fds), so no copy crosses in either direction — the same
         * non-fault posture as write(fd, NULL, 0), and the copy-IN that
         * would carry revents back stays increment 8's named gap.
         *
         * a WAITING poll (ms > 0 or the -1 of forever) is ENOSYS by
         * name — the TWIN of the kernel's first-trap refusal (guard
         * clause S; mprotect's both-ends posture over a timeout word):
         * waiting needs a wake against input state that does not exist,
         * and answering 0 to a 5000 ms poll would make the caller spin
         * its five seconds inside its own loop on a lie. */
        7 => {
            if (args[2] as i32) == 0 {
                Answer::Reply(0, STATUS_OK)
            } else {
                Answer::Reply(0, E_NOSYS)
            }
        }
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
            /* The ceiling is the span's limit CAPPED by the anonymous-map
             * cursor (increment 16): an arena a granted mmap backed is a
             * page this growth may never map over — a re-map would leak
             * the frame and zero the arena's bytes. The kernel's brk
             * verdict caps by its OWN linux_mmap_top, and each side's
             * cap is what makes the other's grant safe (guard V5). */
            if want > brk.cur && want <= brk.mtop {
                brk.cur = want;
                Answer::Reply(want, STATUS_OK)
            } else {
                Answer::Reply(brk.cur, STATUS_OK)
            }
        }
        /* mmap (9, LSC_MEM — E7 increment 16, §5.4's anonymous-map half):
         * the ledger's POLICY half decides, the kernel's verdict
         * re-checks every grant against its own stamped span and its own
         * two cursors at the reply (neither end load-bearing). The
         * word-shape ladder is the kernel's first-trap refusal repeated
         * as this side's own twin (guard clause V pins both): W|X is
         * EPERM (mprotect's §5.4 rule over a prot word), PROT_EXEC alone
         * and un-served shapes are ENOSYS by name, a malformed word is
         * EINVAL, a FIXED address outside the span is ENOMEM — the
         * budget shape §5.4 names. A pass either echoes the caller's own
         * FIXED address (inside the span — last map wins, as Linux
         * itself resolves it; the cursor does not move) or takes a page
         * from the cursor descending from the span's top: strictly above
         * the break, at or below the cursor, which then descends past
         * it so no later grant re-hands this range. PROT_NONE backs no
         * frames at the reply (a reservation whose every access faults)
         * but consumes the same span. MAP_NORESERVE and MAP_STACK are
         * accepted and ignored — documented no-op hints on x86-64, so
         * ignoring them IS Linux's behavior, not a shortcut. */
        9 => {
            const PROT_WRITE: u64 = 0x2;
            const PROT_EXEC: u64 = 0x4;
            const PROT_MASK: u64 = 0x7;
            const MAP_PRIVATE: u64 = 0x02;
            const MAP_FIXED: u64 = 0x10;
            const MAP_ANONYMOUS: u64 = 0x20;
            /* The closed allowlist: PRIVATE|FIXED|ANON plus the two
             * hints. SHARED (0x1) and every unknown bit are simply NOT
             * in it — refused by name, the flags on the log line. */
            const MAP_ALLOW: u64 = MAP_PRIVATE | MAP_FIXED | MAP_ANONYMOUS
                                 | 0x1000  /* MAP_NORESERVE: hint, ignored */
                                 | 0x20000; /* MAP_STACK:    hint, ignored */
            let (addr, len, prot, flags) =
                (args[0], args[1], args[2], args[3]);
            let len_r = match len.checked_add(0xFFF) {
                Some(v) if len != 0 => v & !0xFFFu64,
                _ => return Answer::Reply(0, E_INVAL),
            };
            if prot & !PROT_MASK != 0 {
                Answer::Reply(0, E_INVAL)
            } else if prot & PROT_WRITE != 0 && prot & PROT_EXEC != 0 {
                Answer::Reply(0, E_PERM)
            } else if prot & PROT_EXEC != 0 || flags & MAP_ANONYMOUS == 0
                   || flags & MAP_PRIVATE == 0 || flags & !MAP_ALLOW != 0 {
                Answer::Reply(0, E_NOSYS)
            } else if flags & MAP_FIXED != 0 {
                if addr & 0xFFF != 0 {
                    Answer::Reply(0, E_INVAL)
                } else if addr < brk.base || addr > brk.limit
                       || len_r > brk.limit - addr {
                    Answer::Reply(0, E_NOMEM)
                } else {
                    Answer::Reply(addr, STATUS_OK)
                }
            } else {
                match brk.mtop.checked_sub(len_r) {
                    Some(top) if top >= brk.cur => {
                        brk.mtop = top;
                        Answer::Reply(top, STATUS_OK)
                    }
                    _ => Answer::Reply(0, E_NOMEM),
                }
            }
        }
        /* mprotect (10, LSC_MEM — §5.4's W^X): only the refusal exists
         * in v1 — simultaneous write+execute (prot & 0x2 & 0x4) is
         * refused EPERM, the twin of linux_mprotect_refuses_wx (the
         * kernel refuses it first-trap, before any post; both ends
         * exist so neither alone is the load-bearing refusal — guard
         * clause N3 pins both). Any other prot is ENOSYS with the
         * number logged: real permission changes are not landed, and
         * munmap's channel-side seam is its own increment (frames
         * return through the partition at the task's teardown) — a
         * fake grant here would be a page the kernel never mapped. */
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
        /* rt_sigaction (13, LSC_SIG — E7 increment 10, §10.13's step 2;
         * the out-pointer half landed in increment 12): the KERNEL copied
         * `act` out of the child (32 bytes behind the header), so nothing
         * here reads the child's memory. The kernel's first-trap refusals
         * are checked here as this side's OWN twins — the W|X posture one
         * level down: both ends, so neither alone is load-bearing. A foreign
         * sigsetsize is EINVAL (Linux's own number). act NULL with no
         * oldact is the no-op both Linux and this seam answer 0.
         *
         * The OUT-POINTER (increment 12): when oldact was passed, a
         * successful answer carries the OLD disposition back as this
         * reply's payload — captured BEFORE any mutation below (oldact is
         * the disposition as it WAS, and a refusal arm returns with the
         * plain reply, no bytes: the deliver only copies on success). The
         * DISPOSITION records DFL/IGN only: a handler address is ENOSYS by
         * name BEFORE any mutation — v1 has no signal frames, so
         * installing a handler this shim can never invoke would make every
         * later delivery a coin flip; sa_flags and sa_mask are
         * deliberately NOT stored (they can only ride with a handler
         * install, which was just refused — nothing may depend on bytes
         * this side declined to keep — and zero in the oldact payload IS
         * that truth). */
        LINUX_RT_SIGACTION => {
            if args[3] != SIGSET_BYTES {
                Answer::Reply(0, E_INVAL)
            } else if args[1] == 0 && args[2] == 0 {
                Answer::Reply(0, STATUS_OK) /* nothing in, nothing back */
            } else if args[0] == 0 || args[0] > SIG_MAX as u64 {
                Answer::Reply(0, E_INVAL)
            } else if args[1] == 0 {
                /* QUERY — act NULL, oldact set: the OLD disposition is the
                 * whole answer, carried back as the reply payload (this
                 * side never touches the child's memory — the kernel
                 * copies the bytes into the span its first trap validated).
                 * Legal for EVERY signal, SIGKILL/SIGSTOP included: only
                 * CHANGING those is EINVAL, and a range-checked query is
                 * also what keeps the dispo[] index below inside the
                 * table. */
                let old = old_act_bytes(sig, args[0]);
                Answer::ReplyBytes(0, SIGACT_BYTES, old)
            } else if args[0] == 9 || args[0] == 20 {
                /* SIGKILL / SIGSTOP: Linux itself refuses any change to
                 * these dispositions (EINVAL), whatever the act says. */
                Answer::Reply(0, E_INVAL)
            } else if payload.len() < SIGACT_BYTES {
                Answer::Reply(0, E_NOSYS) /* service's length check refuses these too */
            } else {
                /* oldact captured BEFORE the record moves: the caller must
                 * see the disposition as it was at entry. */
                let old = old_act_bytes(sig, args[0]);
                match read_u64(payload, 0) {
                    0 => {
                        sig.dispo[args[0] as usize] = DISPO_DFL;
                        sig_ok(args, old)
                    }
                    1 => {
                        sig.dispo[args[0] as usize] = DISPO_IGN;
                        sig_ok(args, old)
                    }
                    _ => Answer::Reply(0, E_NOSYS), /* handler install not landed */
                }
            }
        }
        /* rt_sigprocmask (14, LSC_SIG — the seam's other recorded half):
         * the same sigsetsize twin as 13, and — since increment 12 — the
         * same OLDSET-AS-PAYLOAD answer (the out-pointer's 8 bytes cross
         * back on success, captured before the mask moves; a refusal
         * carries none). The mask itself is
         * POSIX arithmetic — BLOCK / UNBLOCK / SETMASK — and then the
         * DELIVERY: what does the new mask REVEAL? A revealed pending
         * signal whose disposition is IGN (or whose default is ignore) is
         * discarded — that IS its delivery; a default-terminate signal
         * runs the kill arm RIGHT HERE with no reply, which is exactly
         * where musl's raise() dies on real Linux (block → tkill pends →
         * restore unmasks → the default action runs inside this syscall);
         * a default-stop signal refuses the call by name BEFORE any
         * mutation — nothing moved, not even the mask. */
        LINUX_RT_SIGPROCMASK => {
            if args[3] != SIGSET_BYTES {
                Answer::Reply(0, E_INVAL)
            } else if args[1] == 0 && args[2] == 0 {
                Answer::Reply(0, STATUS_OK) /* nothing in, nothing back */
            } else if args[1] == 0 {
                /* QUERY — set NULL, oldset set: the CURRENT mask back as
                 * the reply payload (increment 12; Linux ignores `how`
                 * when the set pointer is NULL, and so does this arm — the
                 * query happens before the how match). */
                Answer::ReplyBytes(0, SIGSET_BYTES as usize,
                                   sigset_payload(sig.mask))
            } else if payload.len() < SIGSET_BYTES as usize {
                Answer::Reply(0, E_NOSYS) /* service's length check refuses these too */
            } else {
                let set = read_u64(payload, 0);
                let old = sig.mask;
                let old_bytes = sigset_payload(old); /* the oldset payload,
                                                    * captured BEFORE the
                                                    * mask moves */
                match args[0] {
                    0 | 1 | 2 => {
                        let new_mask = match args[0] {
                            0 => old | set,  /* SIG_BLOCK    */
                            1 => old & !set, /* SIG_UNBLOCK  */
                            _ => set,        /* SIG_SETMASK  */
                        };
                        /* Pass 1: what does the new mask REVEAL? Discards
                         * (an IGN disposition, or an ignore-by-default
                         * signal) are invisible either way; a
                         * default-terminate signal KILLS right here — no
                         * reply, this syscall never returns to its caller;
                         * a default-stop signal refuses the CALL before
                         * anything moves (not even the mask). */
                        let mut revealed = sig.pending & !new_mask;
                        let mut verdict = 0u8; /* 0 proceed, 1 kill, 2 refuse */
                        let mut kill_sig = 0u64; /* WHICH revealed signal earns
                                                  * the kill — carried on the
                                                  * Answer so the line names it
                                                  * (increment 14) */
                        while revealed != 0 && verdict == 0 {
                            let bit = revealed.trailing_zeros();
                            revealed &= !(1u64 << bit);
                            let sig_no = bit + 1;
                            if sig.dispo[sig_no as usize] == DISPO_IGN
                                || default_action(sig_no as u64) == ACT_IGNORE
                            {
                                continue; /* discard — its delivery is a no-op */
                            } else if default_action(sig_no as u64) == ACT_TERMINATE {
                                verdict = 1;
                                kill_sig = sig_no as u64;
                            } else {
                                verdict = 2;
                            }
                        }
                        match verdict {
                            1 => Answer::SignalKill(kill_sig),
                            2 => Answer::Reply(0, E_NOSYS),
                            _ => {
                                /* Pass 2: apply — the revealed discards
                                 * leave the pending set (pending &= mask
                                 * keeps only what is STILL blocked), then
                                 * the mask moves. Both refusals above
                                 * return before this line: a failed call
                                 * never half-changes the mask. */
                                sig.pending &= new_mask;
                                sig.mask = new_mask;
                                if args[2] != 0 {
                                    Answer::ReplyBytes(0,
                                                       SIGSET_BYTES as usize,
                                                       old_bytes)
                                } else {
                                    Answer::Reply(0, STATUS_OK)
                                }
                            }
                        }
                    }
                    _ => Answer::Reply(0, E_INVAL), /* a `how` Linux does not have */
                }
            }
        }
        /* tkill (200 — the seam's DELIVERY): the tid must be the one this
         * task's own set_tid_address answer declared, compared at u32
         * width (musl stores that echo into pt->tid as an int32 and tkill
         * sign-extends it back — SigState's note holds the arithmetic).
         * A tid that matches nothing answers ESRCH — Linux's own answer
         * for a thread that does not exist; cross-task signals stay
         * unscoped, never silently re-targeted at the sender. sig 0 is
         * Linux's existence probe: validate the tid, deliver nothing. The
         * delivery decision is then exactly two questions: is it
         * blocked (it PENDS — raise() observes 0 and the action runs at
         * the next unblock, which is the arm above), and is its
         * disposition/default one this v1 can honestly perform? */
        LINUX_TKILL => {
            if sig.tid == 0 || args[0] as u32 != sig.tid as u32 {
                Answer::Reply(0, E_TARGET)
            } else if args[1] == 0 {
                Answer::Reply(0, STATUS_OK) /* existence probe */
            } else if args[1] > SIG_MAX as u64 {
                Answer::Reply(0, E_INVAL)
            } else if sig.dispo[args[1] as usize] == DISPO_IGN {
                Answer::Reply(0, STATUS_OK) /* ignored: 0 IS the delivery */
            } else if sig.mask & (1u64 << (args[1] - 1)) != 0 {
                sig.pending |= 1u64 << (args[1] - 1);
                Answer::Reply(0, STATUS_OK) /* blocked: arrives as pending */
            } else {
                match default_action(args[1]) {
                    ACT_TERMINATE => Answer::SignalKill(args[1]),
                    ACT_IGNORE => Answer::Reply(0, STATUS_OK),
                    _ => Answer::Reply(0, E_NOSYS), /* stop-default: not landed */
                }
            }
        }
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

/// The message length one request must arrive with — pure over (num,
/// args), the TWIN of the kernel's attach decision: write carries
/// `args[2]` bytes behind the header (the clamp that rode IN the
/// description), rt_sigaction/rt_sigprocmask carry their fixed-size
/// struct/set when an act/set pointer was actually passed, everything
/// else is exactly the 72-byte description it has always been. Both ends
/// deriving this from the same words is what makes a drifted attach
/// detectable as a wrong LENGTH (the malformed arm, num=0 → -EPROTO)
/// instead of silently dropping or misaligning bytes. Unit-tested at this
/// file's end; guard clause R pins it against the kernel's plan.
fn expected_req_len(num: u64, args: &[u64; 6]) -> usize {
    match num {
        LINUX_WRITE => LINUX_SYSCALL_REQ_BYTES + args[2] as usize,
        LINUX_RT_SIGACTION if args[1] != 0 => LINUX_SYSCALL_REQ_BYTES + SIGACT_BYTES,
        LINUX_RT_SIGPROCMASK if args[1] != 0 => LINUX_SYSCALL_REQ_BYTES + SIGSET_BYTES as usize,
        _ => LINUX_SYSCALL_REQ_BYTES,
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
    brk_mtop: u64,  /* increment 16: the anonymous-map cursor — born at  */
                    /* the limit, descending by every granted non-FIXED  */
                    /* mmap (the kernel's linux_mmap_top is its twin)    */
    sig: SigState,  /* increment 10: the signal seam's per-task state —  */
                    /* dispositions, mask, pending, and the tid tkill    */
                    /* matches (recorded from this task's OWN 218 echo)  */
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
        brk_mtop: 0,
        sig: SigState::new(),
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
        /* The anonymous-map cursor is born at the span's top (increment
         * 16): brk grows up from the base, a granted mmap descends from
         * the limit, and the two can never meet — the kernel stamps
         * linux_mmap_top from the same manifest record. */
        t.brk_mtop = heap_limit;
        /* The signal state is born with the task, like the fd table and
         * the ledger below: a reused registry slot must not inherit its
         * predecessor's dispositions, mask, pending bits or TID — an
         * inherited tid would answer ESRCH-vs-deliver wrongly for the
         * next child, which is the stale-state failure this reset exists
         * to refuse (proc_clear_resume_state's discipline, sidecar side). */
        t.sig = SigState::new();
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

/// The reply WITH a payload behind the header (E7 increment 12): the same
/// little-endian field writes as `reply()` — num at 0, ret at 56, status
/// left zeroed at 64 — then the payload bytes appended at 72, sent as one
/// message: exactly the shape kernel/chan.c's deliver validates (72 + n,
/// n bounded by the staged clamp, ret's shape checked against the
/// number). A send failure is reply()'s own (the child's end died
/// mid-call; dropping the reply costs nothing but the log line).
fn reply_bytes<K: Kernel>(k: &K, ch_w: u32, tag: u32, num: u64, ret: u64,
                          bytes: &[u8]) {
    let mut payload = [0u8; LINUX_SYSCALL_REQ_BYTES + SIGACT_BYTES];
    debug_assert!(bytes.len() <= SIGACT_BYTES);
    payload[0..8].copy_from_slice(&num.to_le_bytes());
    payload[56..64].copy_from_slice(&ret.to_le_bytes());
    payload[LINUX_SYSCALL_REQ_BYTES..LINUX_SYSCALL_REQ_BYTES + bytes.len()]
        .copy_from_slice(bytes);
    let total = LINUX_SYSCALL_REQ_BYTES + bytes.len();
    if k.send(ch_w, tag, 0, &payload[..total], &[],
              aerosls_proto::kabi::TIMEOUT_NONE)
        .is_err()
    {
        enosys_log(0xF1F1, b"", b"[POSIX] linux reply (payload) send failed\n");
    }
}

/// Append `v` in decimal at `off`; returns the new offset. Total for any
/// u64 (20 digits max), so a future widening cannot print a truncated
/// number, and clamped to the buffer so a caller can compose a line
/// without measuring first.
fn dec_into(buf: &mut [u8], off: usize, v: u64) -> usize {
    let mut digits = [0u8; 20];
    let mut nd = 0usize;
    let mut x = v;
    loop {
        digits[nd] = b'0' + (x % 10) as u8;
        nd += 1;
        x /= 10;
        if x == 0 {
            break;
        }
    }
    let mut off = off;
    while nd > 0 && off < buf.len() {
        nd -= 1;
        buf[off] = digits[nd];
        off += 1;
    }
    off
}

/// Append `v` as 16 lowercase hex digits at `off`; returns the new
/// offset. Fixed width, no alloc, no fmt (the target build has neither)
/// — the mmap grant's answer line (increment 16) carries an ADDRESS,
/// which is unreadable in decimal on a serial log, and 16 digits covers
/// every canonical x86-64 user address without a length probe. Clamped
/// like dec_into, so a caller composes a line without measuring first.
fn hex_into(buf: &mut [u8], off: usize, v: u64) -> usize {
    const DIGITS: &[u8; 16] = b"0123456789abcdef";
    let mut o = off;
    let mut i = 0;
    while i < 16 && o < buf.len() {
        buf[o] = DIGITS[((v >> ((15 - i) * 4)) & 0xF) as usize];
        o += 1;
        i += 1;
    }
    o
}

/// Append `src` at `off`; returns the new offset (clamped to the buffer).
fn push_bytes(buf: &mut [u8], off: usize, src: &[u8]) -> usize {
    let n = src.len().min(buf.len().saturating_sub(off));
    buf[off..off + n].copy_from_slice(&src[..n]);
    off + n
}

/// One ENOSYS/protocol log line, host-build tolerant (entry.rs's klog is
/// the target-feature serial path; on host fakes the answers still run,
/// they just stay quiet).
///
/// Increment 14's composer: `prefix`, `sig_no` in decimal, `suffix` —
/// no alloc, no fmt (the target build has neither). Signal numbers are
/// 1..=SIG_MAX (two digits), but the digit loop is total for any u64 so
/// a widening cannot print a truncated number, and `sig=` sits at the
/// FRONT of every line this makes — enosys_log's 72-byte cap truncates
/// the tail, never the number that identifies the signal.
fn fmt_sig_line(buf: &mut [u8], prefix: &[u8], sig_no: u64, suffix: &[u8]) -> usize {
    let off = push_bytes(buf, 0, prefix);
    let off = dec_into(buf, off, sig_no);
    push_bytes(buf, off, suffix)
}

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

        /* E7 increment 5 (the fd class), extended by increment 10 (the
         * signal seam): the message length a request MUST arrive with is
         * derived from num and args — the TWIN of the kernel's attach
         * decision (write: args[2] behind the header; act/set: 32/8
         * bytes when args[1] != 0). A drifted attach can only arrive as
         * a wrong length, and a wrong length is the malformed arm:
         * num=0, which the kernel's echo check refuses -EPROTO — loud,
         * never a silent success and never bytes silently dropped. */
        let expected = expected_req_len(num, &args);
        if num == LINUX_WRITE
            && (args[2] as usize > FD_PAYLOAD_CAP || rr.len != expected)
        {
            enosys_log(num as u32, &name_buf[..nlen], b"[POSIX] write payload mismatch -> -EPROTO\n");
            reply(k, ch_w, rr.tag, 0, 0, E_NOSYS);
            continue;
        }
        if rr.len != expected {
            enosys_log(0xBAD0, &name_buf[..nlen], b"[POSIX] malformed request -> -EPROTO\n");
            reply(k, ch_w, rr.tag, 0, 0, E_NOSYS);
            continue;
        }
        let payload: &[u8] = &buf[LINUX_SYSCALL_REQ_BYTES..rr.len];

        let fds = tasks()[i].fds;
        let mut mem = MemBrk {
            base: tasks()[i].brk_base,
            limit: tasks()[i].brk_limit,
            cur: tasks()[i].brk_cur,
            mtop: tasks()[i].brk_mtop,
        };
        let mut sigstate = tasks()[i].sig; /* Copy — the seam's ledger, */
                                           /* written back like brk's   */

        let ans = answer(num, &args, &fds, &mut mem, &mut sigstate, payload);
        /* The ledger survives the reply: only a granted growth moves it
         * (the arm's own rule), and the KERNEL's brk_cur is its own
         * twin — the probe's denial-echo check is what proves the two
         * never drift. The same holds for mtop (increment 16): only the
         * mmap arm descends it, and linux_mmap_top is its twin. The
         * base and limit are never written here: they are the span,
         * decided once, at spawn. */
        tasks()[i].brk_cur = mem.cur;
        tasks()[i].brk_mtop = mem.mtop;
        /* E7 increment 10: the signal state survives the reply the same
         * way — tid, mask, pending and dispositions are each written only
         * by the arm that owns them, and a refusal arm mutated nothing
         * (refusal-before-mutation), so this unconditional write-back
         * carries exactly the change that was granted. A SignalKill then
         * spends the whole entry below — the state goes with the slot. */
        tasks()[i].sig = sigstate;

        match ans {
            Answer::Reply(ret, status) => {
                if status == E_NOSYS {
                    if num == LINUX_MMAP {
                        /* The refusal's ARGUMENTS ride the line — the
                         * SIGABRT reading's other half: what the caller
                         * asked the kernel for, by name, so "the allocator
                         * wanted len=X prot=P flags=F and this shim cannot
                         * service it" is a line, not a reconstruction. */
                        let mut ex = [0u8; 56];
                        let mut off =
                            push_bytes(&mut ex, 0, b" -> ENOSYS mmap(len=");
                        off = dec_into(&mut ex, off, args[1]);
                        off = push_bytes(&mut ex, off, b", prot=");
                        off = dec_into(&mut ex, off, args[2]);
                        off = push_bytes(&mut ex, off, b", flags=");
                        off = dec_into(&mut ex, off, args[3]);
                        off = push_bytes(&mut ex, off, b")");
                        enosys_log(num as u32, &name_buf[..nlen], &ex[..off]);
                    } else {
                        enosys_log(num as u32, &name_buf[..nlen], b" -> ENOSYS\n");
                    }
                } else if num == LINUX_MMAP && status != STATUS_OK {
                    /* The errno family of mmap refusals (increment 16):
                     * EPERM / EINVAL / ENOMEM rides with the same request
                     * words the ENOSYS line carries — WHICH shape the
                     * ladder or the ledger refused is read off the line.
                     * (A FIXED ask outside the span never reaches this
                     * line: the kernel's first trap refused it with the
                     * address on ITS line, before any post.) */
                    let mut ex = [0u8; 56];
                    let mut off = push_bytes(&mut ex, 0, b" -> ");
                    off = push_bytes(&mut ex, off,
                        if status == E_PERM   { b"EPERM mmap(len=" }
                        else if status == E_INVAL { b"EINVAL mmap(len=" }
                        else                  { b"ENOMEM mmap(len=" });
                    off = dec_into(&mut ex, off, args[1]);
                    off = push_bytes(&mut ex, off, b", prot=");
                    off = dec_into(&mut ex, off, args[2]);
                    off = push_bytes(&mut ex, off, b", flags=");
                    off = dec_into(&mut ex, off, args[3]);
                    off = push_bytes(&mut ex, off, b")");
                    enosys_log(num as u32, &name_buf[..nlen], &ex[..off]);
                } else if num == LINUX_MMAP {
                    /* A GRANTED map shows the address the ledger gave
                     * (increment 16) — in hex, because an address is
                     * unreadable in decimal on a serial log, and this
                     * line is the run's proof that the allocator's arena
                     * landed inside its span. */
                    let mut ex = [0u8; 40];
                    let mut off = push_bytes(&mut ex, 0, b" ok mmap -> 0x");
                    off = hex_into(&mut ex, off, ret);
                    off = push_bytes(&mut ex, off, b"\n");
                    enosys_log(num as u32, &name_buf[..nlen], &ex[..off]);
                } else if num == LINUX_TKILL {
                    /* tkill's SIG rides its line (increment 14): WHICH
                     * signal was asked for is the fact the pending →
                     * unblock flow later cashes in — read off the line,
                     * never reconstructed from which arms ran. The
                     * existence probe (sig 0) prints itself as itself. */
                    let mut ex = [0u8; 32];
                    let n = fmt_sig_line(&mut ex, b" ok sig=", args[1], b"\n");
                    enosys_log(num as u32, &name_buf[..nlen], &ex[..n]);
                } else {
                    /* An answered syscall must be visible too: this line
                     * is the serial-log half of E7's round-trip proof
                     * (a real task's real request arrived, and its reply
                     * is on its way back through §5.7). */
                    enosys_log(num as u32, &name_buf[..nlen], b" ok\n");
                }
                reply(k, ch_w, rr.tag, num, ret, status);
            }
            Answer::ReplyBytes(ret, len, bytes) => {
                /* E7 increment 12 (the reply payload): the BOUND over the
                 * answer — a reply-payload number's bytes must equal the
                 * clamp the forwarded out-pointer implies (non-NULL
                 * args[2] = this number's fixed size), and only a request
                 * that ASKED for one may carry one. A mismatch is never
                 * sent: num=0 makes the kernel's echo refuse -EPROTO, so
                 * the answer errors out loud instead of bytes landing in
                 * a destination nothing validated — the twin of the
                 * deliver's staged-len check, neither end load-bearing
                 * alone. The deliver performs the copy (the kernel owns
                 * the child's pages); this side only decides the bytes. */
                let want = if num == LINUX_RT_SIGACTION {
                    SIGACT_BYTES
                } else {
                    SIGSET_BYTES as usize
                };
                if args[2] == 0 || len != want || len > bytes.len() {
                    enosys_log(0xBAD1, &name_buf[..nlen],
                               b"[POSIX] reply payload mismatch -> -EPROTO\n");
                    reply(k, ch_w, rr.tag, 0, 0, E_NOSYS);
                    continue;
                }
                enosys_log(num as u32, &name_buf[..nlen], b" ok + payload\n");
                reply_bytes(k, ch_w, rr.tag, num, ret, &bytes[..len]);
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
                        /* Attribution — the SIGABRT reading's instrument:
                         * WHICH child wrote WHAT, on THIS line. Console
                         * output drains as one merged stream in flush
                         * order, so without the excerpt here a message in
                         * the drain belongs to any child of the
                         * environment. 24 bytes, printable bytes as-is and
                         * everything else as '.', so a short line's first
                         * words identify it and no control byte can break
                         * the log line. */
                        let mut ex = [0u8; 56];
                        let mut off = fmt_sig_line(&mut ex, b" write ok len=",
                                                   n as u64, b" |");
                        for &b in payload[..len].iter().take(24) {
                            if off + 1 >= ex.len() {
                                break;
                            }
                            ex[off] = if b.is_ascii_graphic() || b == b' ' {
                                b
                            } else {
                                b'.'
                            };
                            off += 1;
                        }
                        if off < ex.len() {
                            ex[off] = b'|';
                            off += 1;
                        }
                        enosys_log(num as u32, &name_buf[..nlen], &ex[..off]);
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
            Answer::SignalKill(kill_sig) => {
                /* E7 increment 10 (§10.13's step 2): a signal's default
                 * action ran. The line names the SIGNAL NUMBER (increment
                 * 14 — the Answer carries it) before the reap — the log is
                 * what the boot's attribution reads, and this is the
                 * difference between "the seam delivered" and "the task
                 * happened to die". The reap itself is
                 * exit_group's own path, twin for twin: same
                 * name->pid liveness test, same immediate teardown for a
                 * target parked awaiting THIS exchange, same refusal to
                 * strand a task when the name resolves nowhere. No reply:
                 * on real Linux the process dies inside the delivering
                 * syscall and never returns to its caller. */
                let mut ex = [0u8; 48];
                let n = fmt_sig_line(&mut ex, b" sig=", kill_sig,
                                     b" default-action -> kill\n");
                enosys_log(num as u32, &name_buf[..nlen], &ex[..n]);
                let name = core::str::from_utf8(&name_buf[..nlen]).unwrap_or("");
                let pid = k.sidecar_pid(name, partition);
                if pid == 0 {
                    enosys_log(num as u32, &name_buf[..nlen], b" sig kill: NOT-FOUND (reap refused)\n");
                    reply(k, ch_w, rr.tag, num, 0, E_CONFLICT);
                } else {
                    k.proc_kill(pid);
                    tasks()[i].used = false;
                }
            }
        }
    }
}
/* ─── E7 increment 10 (the signal seam): the pure half, driven ────────────
 * answer() has no Kernel, no channel and no task in its signature for
 * exactly this reason: the whole seam — the two refusals' twins, the mask
 * arithmetic, the tid match, pending/reveal, and the delivery decision —
 * is decisions over a request, a payload slice and a SigState. These tests
 * are the host teeth the boot cannot be (a wrong tid width or a mask that
 * moves on a refusal is invisible in any single trace); the boot then
 * measures the one flow these compose into: musl's raise() — block, tkill,
 * unblock — ending in kill with no [FAULT]. */
#[cfg(test)]
mod tests {
    use super::*;

    fn fds() -> [FdSlot; LINUX_MAX_FDS] {
        [FdSlot { used: false, kind: 0 }; LINUX_MAX_FDS]
    }
    fn ledger() -> MemBrk {
        MemBrk { base: 0, limit: 0, cur: 0, mtop: 0 }
    }
    /* The fixture span the mem-class tests share: 64 KiB exactly as the
     * probe's manifest stamps it (increment 16's ledger tests). */
    const SPAN_BASE: u64 = 0x807d000;
    const SPAN_LIMIT: u64 = SPAN_BASE + 0x10000;
    fn span() -> MemBrk {
        MemBrk { base: SPAN_BASE, limit: SPAN_LIMIT,
                 cur: SPAN_BASE, mtop: SPAN_LIMIT }
    }
    /* The fixture tid: what the candidate's set_tid_address answer echoes
     * (the address of __thread_list_lock, 0x807c398 in the committed
     * firstuser ELF). */
    const TID: u64 = 0x807c398;

    fn fresh() -> SigState {
        SigState { tid: TID, ..SigState::new() }
    }

    fn act_payload(handler: u64) -> [u8; SIGACT_BYTES] {
        let mut p = [0u8; SIGACT_BYTES];
        p[..8].copy_from_slice(&handler.to_le_bytes());
        p
    }
    fn set_payload(set: u64) -> [u8; SIGSET_BYTES as usize] {
        set.to_le_bytes()
    }

    #[test]
    fn expected_req_len_is_the_kernels_twin() {
        let mut a = [0u64; 6];
        assert_eq!(expected_req_len(LINUX_WRITE, &a), 72);
        a[2] = 512;
        assert_eq!(expected_req_len(LINUX_WRITE, &a), 72 + 512);
        a = [0; 6];
        a[1] = 0x1000; /* an act/set pointer is present */
        assert_eq!(expected_req_len(LINUX_RT_SIGACTION, &a), 72 + SIGACT_BYTES);
        assert_eq!(expected_req_len(LINUX_RT_SIGPROCMASK, &a), 72 + 8);
        a[1] = 0; /* NULL act/set: the plain 72-byte description */
        assert_eq!(expected_req_len(LINUX_RT_SIGACTION, &a), 72);
        assert_eq!(expected_req_len(LINUX_TKILL, &a), 72);
        assert_eq!(expected_req_len(999, &a), 72);
    }

    #[test]
    fn default_actions_match_posix() {
        /* terminate, incl. the CORE class (killed without a dump) and RT */
        for s in [1u64, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 14, 15, 16, 32, 64] {
            assert_eq!(default_action(s), ACT_TERMINATE, "sig {}", s);
        }
        /* default-ignore */
        for s in [17u64, 18, 23, 28] {
            assert_eq!(default_action(s), ACT_IGNORE, "sig {}", s);
        }
        /* default-stop: not landed */
        for s in [19u64, 20, 21, 22] {
            assert_eq!(default_action(s), ACT_STOP, "sig {}", s);
        }
    }

    #[test]
    fn sigaction_refuses_foreign_size_and_carries_oldact_back_as_payload() {
        let mut sig = fresh();
        /* sigsetsize != 8: Linux's own EINVAL — a plain reply, never
         * bytes: a refusal carries no payload (the deliver's shape-1
         * rule on the other end). */
        let a = answer(13, &[6, 0x4000, 0, 4, 0, 0], &fds(), &mut ledger(),
                       &mut sig, &act_payload(0));
        assert!(matches!(a, Answer::Reply(0, E_INVAL)), "size twin: {:?}", a);
        /* oldact != NULL with a NULL act — the QUERY shape (increment 12):
         * the old disposition comes back as the reply's payload, DFL (0)
         * while nothing has been recorded. The copy-IN gap's answer. */
        let a = answer(13, &[6, 0, 0x5000, 8, 0, 0], &fds(), &mut ledger(),
                       &mut sig, &[]);
        match a {
            Answer::ReplyBytes(ret, len, bytes) => {
                assert_eq!(ret, 0, "a query returns 0, not a count");
                assert_eq!(len, SIGACT_BYTES, "the full struct crosses");
                assert_eq!(read_u64(&bytes, 0), 0, "SIG_DFL has not changed");
                assert!(bytes[8..].iter().all(|&b| b == 0),
                        "flags/restorer/mask were never stored — zero IS\n        the truth, not a default invented for the reply");
            }
            other => panic!("oldact query: {:?}", other),
        }
        /* the old DISPOSITION crosses, captured BEFORE the new one is
         * recorded: record IGN (no oldact), then record DFL WITH oldact —
         * the payload must carry IGN, the disposition as it WAS. */
        let a = answer(13, &[6, 0x4000, 0, 8, 0, 0], &fds(), &mut ledger(),
                       &mut sig, &act_payload(1));
        assert!(matches!(a, Answer::Reply(0, STATUS_OK)), "IGN: {:?}", a);
        assert_eq!(sig.dispo[6], DISPO_IGN);
        let a = answer(13, &[6, 0x4000, 0x5000, 8, 0, 0], &fds(),
                       &mut ledger(), &mut sig, &act_payload(0));
        match a {
            Answer::ReplyBytes(ret, len, bytes) => {
                assert_eq!((ret, len), (0, SIGACT_BYTES));
                assert_eq!(read_u64(&bytes, 0), 1,
                           "oldact is the disposition BEFORE this call");
            }
            other => panic!("oldact on success: {:?}", other),
        }
        assert_eq!(sig.dispo[6], DISPO_DFL, "and the new one landed");
        /* a REFUSAL with oldact requested: the plain reply, no bytes —
         * an errno owes no payload, and a mutation-free refusal leaves
         * the out-pointer to the kernel's shape-1 path (which accepts an
         * errno reply without copying). */
        let a = answer(13, &[6, 0x4000, 0x5000, 8, 0, 0], &fds(), &mut ledger(),
                       &mut sig, &act_payload(0x800abcd));
        assert!(matches!(a, Answer::Reply(0, E_NOSYS)),
                "handler refusal carries no payload: {:?}", a);
        assert_eq!(sig.dispo[6], DISPO_DFL, "nothing moved");
        /* act NULL with no oldact: the no-op both sides answer 0 */
        let a = answer(13, &[6, 0, 0, 8, 0, 0], &fds(), &mut ledger(),
                       &mut sig, &[]);
        assert!(matches!(a, Answer::Reply(0, STATUS_OK)), "null act: {:?}", a);
    }

    #[test]
    fn sigaction_records_dfl_and_ign_but_refuses_handlers_before_mutation() {
        let mut sig = fresh();
        /* SIG_DFL recorded */
        let a = answer(13, &[6, 0x4000, 0, 8, 0, 0], &fds(), &mut ledger(),
                       &mut sig, &act_payload(0));
        assert!(matches!(a, Answer::Reply(0, STATUS_OK)), "DFL: {:?}", a);
        assert_eq!(sig.dispo[6], DISPO_DFL);
        /* SIG_IGN recorded */
        let a = answer(13, &[6, 0x4000, 0, 8, 0, 0], &fds(), &mut ledger(),
                       &mut sig, &act_payload(1));
        assert!(matches!(a, Answer::Reply(0, STATUS_OK)), "IGN: {:?}", a);
        assert_eq!(sig.dispo[6], DISPO_IGN);
        /* a handler address: ENOSYS, and the disposition UNCHANGED —
         * refusal-before-mutation, the property that keeps a later
         * delivery honest instead of half-recorded. */
        let a = answer(13, &[6, 0x4000, 0, 8, 0, 0], &fds(), &mut ledger(),
                       &mut sig, &act_payload(0x800abcd));
        assert!(matches!(a, Answer::Reply(0, E_NOSYS)), "handler: {:?}", a);
        assert_eq!(sig.dispo[6], DISPO_IGN, "the handler install must not land");
        /* signal-number rules Linux itself owns: 0 and >64 are EINVAL,
         * and SIGKILL/SIGSTOP refuse any change however benign. */
        for s in [0u64, 65, 9, 20] {
            let a = answer(13, &[s, 0x4000, 0, 8, 0, 0], &fds(), &mut ledger(),
                           &mut sig, &act_payload(0));
            assert!(matches!(a, Answer::Reply(0, E_INVAL)), "sig {}: {:?}", s, a);
        }
    }

    #[test]
    fn sigprocmask_is_exact_posix_arithmetic() {
        let mut sig = fresh();
        let run = |sig: &mut SigState, how: u64, set: u64| {
            answer(14, &[how, 0x4000, 0, 8, 0, 0], &fds(), &mut ledger(),
                   sig, &set_payload(set))
        };
        assert!(matches!(run(&mut sig, 2, 0b101), Answer::Reply(0, STATUS_OK)));
        assert_eq!(sig.mask, 0b101, "SETMASK");
        assert!(matches!(run(&mut sig, 0, 0b010), Answer::Reply(0, STATUS_OK)));
        assert_eq!(sig.mask, 0b111, "BLOCK");
        assert!(matches!(run(&mut sig, 1, 0b001), Answer::Reply(0, STATUS_OK)));
        assert_eq!(sig.mask, 0b110, "UNBLOCK");
        /* twins of the kernel's first-trap refusals, and the mask must
         * not move on either */
        let before = sig.mask;
        let a2 = answer(14, &[2, 0x4000, 0, 16, 0, 0], &fds(), &mut ledger(),
                        &mut sig, &set_payload(!0));
        assert!(matches!(a2, Answer::Reply(0, E_INVAL)), "size twin: {:?}", a2);
        assert_eq!(sig.mask, before, "a refused call never half-changes the mask");
        /* the oldset OUT-POINTER (increment 12, the copy-IN gap's land):
         * this call SUCCEEDS and the OLD mask crosses back as the reply
         * payload — captured BEFORE the mask moves — and only then does
         * the SETMASK land. A refusal twin remains below (`how`). */
        let a3 = answer(14, &[2, 0x4000, 0x5000, 8, 0, 0], &fds(), &mut ledger(),
                        &mut sig, &set_payload(!0));
        match a3 {
            Answer::ReplyBytes(ret, len, bytes) => {
                assert_eq!((ret, len), (0, SIGSET_BYTES as usize),
                           "a successful rt_sigprocmask returns 0, with the\n        8-byte oldset as payload");
                assert_eq!(read_u64(&bytes, 0), before,
                           "oldset is the mask BEFORE this call");
            }
            other => panic!("oldset twin: {:?}", other),
        }
        assert_eq!(sig.mask, !0, "and the SETMASK itself landed");
        let before = sig.mask;
        /* a `how` Linux does not have */
        let a = answer(14, &[9, 0x4000, 0, 8, 0, 0], &fds(), &mut ledger(),
                       &mut sig, &set_payload(0));
        assert!(matches!(a, Answer::Reply(0, E_INVAL)), "how: {:?}", a);
        assert_eq!(sig.mask, before, "EINVAL moved no mask either");
        /* the QUERY shape: set NULL with oldset — the current mask back
         * as payload, no matter what `how` says (Linux ignores `how`
         * when the set pointer is NULL, and so does the arm — the query
         * runs before the how match), and nothing about the mask moves. */
        let a = answer(14, &[9, 0, 0x6000, 8, 0, 0], &fds(), &mut ledger(),
                       &mut sig, &[]);
        match a {
            Answer::ReplyBytes(ret, len, bytes) => {
                assert_eq!((ret, len), (0, SIGSET_BYTES as usize));
                assert_eq!(read_u64(&bytes, 0), before,
                           "the query returns the CURRENT mask");
            }
            other => panic!("oldset query: {:?}", other),
        }
        assert_eq!(sig.mask, before, "a query never moves the mask");
    }

    #[test]
    fn tkill_matches_the_recorded_tid_at_u32_width() {
        /* The sign-extension case musl creates: the echo's low 32 bits
         * have bit 31 set, pt->tid stores them as a negative int32, and
         * tkill hands back the sign-extended form. Only a u32 compare
         * can see the two as one number. */
        let wide: u64 = 0x0000_0001_8000_0000;
        let mut sig = SigState { tid: wide, ..SigState::new() };
        /* sig 17 (default ignore) isolates the tid decision from delivery */
        let a = answer(200, &[0xFFFF_FFFF_8000_0000, 17, 0, 0, 0, 0],
                       &fds(), &mut ledger(), &mut sig, &[]);
        assert!(matches!(a, Answer::Reply(0, STATUS_OK)), "u32 match: {:?}", a);
        /* a tid that matches nothing: ESRCH (CAP_ERR_TARGET), never a
         * re-target at the sender */
        let a = answer(200, &[0x1234, 17, 0, 0, 0, 0],
                       &fds(), &mut ledger(), &mut sig, &[]);
        assert!(matches!(a, Answer::Reply(0, E_TARGET)), "mismatch: {:?}", a);
        /* a task that never declared a tid has no self to deliver to */
        let mut naked = SigState::new();
        let a = answer(200, &[0, 17, 0, 0, 0, 0],
                       &fds(), &mut ledger(), &mut naked, &[]);
        assert!(matches!(a, Answer::Reply(0, E_TARGET)), "no tid: {:?}", a);
    }

    #[test]
    fn tkill_delivery_decisions() {
        /* unblocked + default-terminate: the kill arm */
        let mut sig = fresh();
        let a = answer(200, &[TID, 6, 0, 0, 0, 0],
                       &fds(), &mut ledger(), &mut sig, &[]);
        assert!(matches!(a, Answer::SignalKill(6)), "SIGABRT: {:?}", a);
        /* existence probe: sig 0 validates the tid and delivers nothing */
        let a = answer(200, &[TID, 0, 0, 0, 0, 0],
                       &fds(), &mut ledger(), &mut sig, &[]);
        assert!(matches!(a, Answer::Reply(0, STATUS_OK)), "probe: {:?}", a);
        /* out of range */
        let a = answer(200, &[TID, 65, 0, 0, 0, 0],
                       &fds(), &mut ledger(), &mut sig, &[]);
        assert!(matches!(a, Answer::Reply(0, E_INVAL)), "range: {:?}", a);
        /* SIG_IGN disposition: 0 IS the delivery */
        answer(13, &[6, 0x4000, 0, 8, 0, 0], &fds(), &mut ledger(),
               &mut sig, &act_payload(1));
        let a = answer(200, &[TID, 6, 0, 0, 0, 0],
                       &fds(), &mut ledger(), &mut sig, &[]);
        assert!(matches!(a, Answer::Reply(0, STATUS_OK)), "ignored: {:?}", a);
        /* blocked: the signal PENDS and nothing dies here */
        let mut sig = fresh();
        answer(14, &[2, 0x4000, 0, 8, 0, 0], &fds(), &mut ledger(),
               &mut sig, &set_payload(1u64 << 5)); /* block SIGABRT */
        let a = answer(200, &[TID, 6, 0, 0, 0, 0],
                       &fds(), &mut ledger(), &mut sig, &[]);
        assert!(matches!(a, Answer::Reply(0, STATUS_OK)), "blocked: {:?}", a);
        assert_eq!(sig.pending & (1 << 5), 1 << 5, "the signal must pend");
        /* default-ignore signal, unblocked: answered, not killed */
        let a = answer(200, &[TID, 17, 0, 0, 0, 0],
                       &fds(), &mut ledger(), &mut sig, &[]);
        assert!(matches!(a, Answer::Reply(0, STATUS_OK)), "CHLD: {:?}", a);
        /* default-stop signal, unblocked: refused by name, never a kill */
        let a = answer(200, &[TID, 20, 0, 0, 0, 0],
                       &fds(), &mut ledger(), &mut sig, &[]);
        assert!(matches!(a, Answer::Reply(0, E_NOSYS)), "TSTP: {:?}", a);
    }

    #[test]
    fn raise_flow_block_tkill_unblock_kills() {
        /* musl raise() end to end: __block_app_sigs (14 BLOCK) ->
         * tkill(tid, SIGABRT) (200) -> __restore_sigs (14 UNBLOCK) -> the
         * default action runs INSIDE the unblock — the flow whose end the
         * E3 boot must show as a kill with no [FAULT] line. */
        let mut sig = fresh();
        let block_abrt = answer(14, &[0, 0x4000, 0, 8, 0, 0], &fds(),
                                &mut ledger(), &mut sig, &set_payload(1 << 5));
        assert!(matches!(block_abrt, Answer::Reply(0, STATUS_OK)));
        let tkill = answer(200, &[TID, 6, 0, 0, 0, 0], &fds(),
                           &mut ledger(), &mut sig, &[]);
        assert!(matches!(tkill, Answer::Reply(0, STATUS_OK)),
                "blocked tkill answers 0, not a kill: {:?}", tkill);
        assert_ne!(sig.pending, 0, "and the signal is pending");
        let unblock = answer(14, &[1, 0x4000, 0, 8, 0, 0], &fds(),
                             &mut ledger(), &mut sig, &set_payload(1 << 5));
        assert!(matches!(unblock, Answer::SignalKill(6)),
                "the revealed pending SIGABRT must kill: {:?}", unblock);
    }

    #[test]
    fn unblock_refuses_a_pending_stop_before_anything_moves() {
        let mut sig = fresh();
        answer(14, &[2, 0x4000, 0, 8, 0, 0], &fds(), &mut ledger(),
               &mut sig, &set_payload(1 << 19)); /* block SIGSTOP */
        let tkill = answer(200, &[TID, 20, 0, 0, 0, 0], &fds(),
                           &mut ledger(), &mut sig, &[]);
        assert!(matches!(tkill, Answer::Reply(0, STATUS_OK)));
        assert_ne!(sig.pending, 0);
        let before = sig.mask;
        let unblock = answer(14, &[1, 0x4000, 0, 8, 0, 0], &fds(),
                             &mut ledger(), &mut sig, &set_payload(1 << 19));
        assert!(matches!(unblock, Answer::Reply(0, E_NOSYS)),
                "a pending stop-default is refused by name: {:?}", unblock);
        assert_eq!(sig.mask, before, "the mask did not move on the refusal");
    }

    #[test]
    fn unblock_discards_a_pending_default_ignore_signal() {
        let mut sig = fresh();
        answer(14, &[2, 0x4000, 0, 8, 0, 0], &fds(), &mut ledger(),
               &mut sig, &set_payload(1 << 16)); /* block SIGCHLD */
        let tkill = answer(200, &[TID, 17, 0, 0, 0, 0], &fds(),
                           &mut ledger(), &mut sig, &[]);
        assert!(matches!(tkill, Answer::Reply(0, STATUS_OK)));
        assert_ne!(sig.pending, 0, "a blocked signal pends whatever its default");
        let unblock = answer(14, &[1, 0x4000, 0, 8, 0, 0], &fds(),
                             &mut ledger(), &mut sig, &set_payload(1 << 16));
        assert!(matches!(unblock, Answer::Reply(0, STATUS_OK)),
                "discard, no kill: {:?}", unblock);
        assert_eq!(sig.pending, 0, "the discarded pending bit is gone");
        assert_eq!(sig.mask & (1 << 16), 0, "and the mask unblocked it");
    }

    #[test]
    fn set_tid_address_records_the_tid_through_its_own_arm() {
        let mut sig = SigState::new();
        let a = answer(218, &[TID, 0, 0, 0, 0, 0], &fds(), &mut ledger(),
                       &mut sig, &[]);
        assert!(matches!(a, Answer::Reply(TID, STATUS_OK)), "echo: {:?}", a);
        assert_eq!(sig.tid, TID, "the echo IS the recorded tid");
    }

    #[test]
    fn poll_answers_the_zero_timeout_probe_and_refuses_to_wait() {
        /* The firstuser's ACTUAL call — poll(fds, 3, 0) from Rust std's
         * lang_start_internal: zero timeout, answered 0 events, the
         * array pointer never touched (no payload, no copy either way). */
        let a = answer(7, &[0x7FFF_FFFF_E000, 3, 0, 0, 0, 0],
                       &fds(), &mut ledger(), &mut fresh(), &[]);
        assert!(matches!(a, Answer::Reply(0, STATUS_OK)),
                "the zero-timeout probe answers 0: {:?}", a);
        /* nfds does not change the answer: nothing is read, and a
         * negative timeout is the -1 of forever (i32 truncation). */
        let a = answer(7, &[0x1234, 0, 0, 0, 0, 0],
                       &fds(), &mut ledger(), &mut fresh(), &[]);
        assert!(matches!(a, Answer::Reply(0, STATUS_OK)),
                "nfds 0, timeout 0: {:?}", a);
        /* A waiting poll — ms or forever — is ENOSYS by name, the twin
         * of the kernel's first-trap refusal; the u64 sign-extension
         * path (-1 as musl passes it) is the one that must not read as
         * a zero timeout. */
        for t in [1u64, 5000, 0xFFFF_FFFF_FFFF_FFFF] {
            let a = answer(7, &[0x7FFF_FFFF_E000, 3, t, 0, 0, 0],
                           &fds(), &mut ledger(), &mut fresh(), &[]);
            assert!(matches!(a, Answer::Reply(0, E_NOSYS)),
                    "a waiting poll (timeout {}) is refused: {:?}", t, a);
        }
        /* Linux's int width, the other direction: 0x1_00000000
         * truncates to a zero timeout and must NOT be refused. */
        let a = answer(7, &[0x7FFF_FFFF_E000, 3, 0x1_0000_0000, 0, 0, 0],
                       &fds(), &mut ledger(), &mut fresh(), &[]);
        assert!(matches!(a, Answer::Reply(0, STATUS_OK)),
                "int width: {:?}", a);
    }

    #[test]
    fn mmap_grants_anonymous_private_arenas_from_the_cursor() {
        /* The firstuser's arena shape, measured: mmap(NULL, 4096, RW,
         * ANON|PRIVATE). The cursor hands out the span's TOP page and
         * descends past it; brk's break is untouched. */
        let mut b = span();
        let a = answer(9, &[0, 0x1000, 3, 0x22, 0, 0],
                       &fds(), &mut b, &mut fresh(), &[]);
        match a {
            Answer::Reply(ret, STATUS_OK) =>
                assert_eq!(ret, SPAN_LIMIT - 0x1000,
                           "the arena lands at the span's top"),
            other => panic!("arena: {:?}", other),
        }
        assert_eq!(b.mtop, SPAN_LIMIT - 0x1000,
                   "the cursor descends past the grant");
        assert_eq!(b.cur, SPAN_BASE, "brk's break is untouched");
        /* A second arena: the next page down, never re-handed */
        let a = answer(9, &[0, 0x1000, 3, 0x22, 0, 0],
                       &fds(), &mut b, &mut fresh(), &[]);
        match a {
            Answer::Reply(ret, STATUS_OK) =>
                assert_eq!(ret, SPAN_LIMIT - 0x2000),
            other => panic!("arena 2: {:?}", other),
        }
        assert_eq!(b.mtop, SPAN_LIMIT - 0x2000);
        /* PROT_NONE (the guard shape) backs no frames but consumes the
         * same span: the cursor descends, the answer is the page. */
        let a = answer(9, &[0, 0x1000, 0, 0x22, 0, 0],
                       &fds(), &mut b, &mut fresh(), &[]);
        match a {
            Answer::Reply(ret, STATUS_OK) =>
                assert_eq!(ret, SPAN_LIMIT - 0x3000,
                           "a PROT_NONE reservation consumes the span"),
            other => panic!("guard: {:?}", other),
        }
        assert_eq!(b.mtop, SPAN_LIMIT - 0x3000);
        /* MAP_STACK and MAP_NORESERVE are accepted and ignored — the
         * documented no-op hints on x86-64 (Linux's own behavior). */
        let a = answer(9, &[0, 0x1000, 3, 0x20022, 0, 0],
                       &fds(), &mut b, &mut fresh(), &[]);
        assert!(matches!(a, Answer::Reply(_, STATUS_OK)),
                "STACK hint: {:?}", a);
        let a = answer(9, &[0, 0x1000, 3, 0x1022, 0, 0],
                       &fds(), &mut b, &mut fresh(), &[]);
        assert!(matches!(a, Answer::Reply(_, STATUS_OK)),
                "NORESERVE hint: {:?}", a);
    }

    #[test]
    fn mmap_fixed_echoes_the_address_inside_the_span_and_moves_no_cursor() {
        /* The firstuser's guard page, measured: FIXED|ANON|PRIVATE at
         * the heap base — the echo is the whole answer, and the cursor
         * does not move (last map wins, as Linux itself resolves it). */
        let mut b = span();
        let a = answer(9, &[SPAN_BASE, 0x1000, 0, 0x32, 0, 0],
                       &fds(), &mut b, &mut fresh(), &[]);
        match a {
            Answer::Reply(ret, STATUS_OK) =>
                assert_eq!(ret, SPAN_BASE, "FIXED echoes the ask exactly"),
            other => panic!("fixed guard: {:?}", other),
        }
        assert_eq!(b.mtop, SPAN_LIMIT, "a FIXED grant moves no cursor");
        assert_eq!(b.cur, SPAN_BASE, "nor the break");
        /* inside the span but NOT at its base: still an echo */
        let a = answer(9, &[SPAN_BASE + 0x4000, 0x1000, 3, 0x32, 0, 0],
                       &fds(), &mut b, &mut fresh(), &[]);
        assert!(matches!(a, Answer::Reply(r, STATUS_OK)
                         if r == SPAN_BASE + 0x4000),
                "fixed mid-span: {:?}", a);
    }

    #[test]
    fn mmap_refuses_the_word_shapes_by_name_at_this_end_too() {
        /* The kernel first-trap ladder's TWIN (guard V6): each shape
         * below was refused by the kernel before any post — this arm
         * answers the same way so neither end is load-bearing alone. */
        let mut b = span();
        /* W|X: EPERM — mprotect's §5.4 rule over a prot word */
        let a = answer(9, &[0, 0x1000, 6, 0x22, 0, 0],
                       &fds(), &mut b, &mut fresh(), &[]);
        assert!(matches!(a, Answer::Reply(0, E_PERM)), "wx: {:?}", a);
        /* PROT_EXEC alone: ENOSYS by name (no executable anonymous map) */
        let a = answer(9, &[0, 0x1000, 5, 0x22, 0, 0],
                       &fds(), &mut b, &mut fresh(), &[]);
        assert!(matches!(a, Answer::Reply(0, E_NOSYS)), "exec: {:?}", a);
        /* an unknown prot bit: EINVAL */
        let a = answer(9, &[0, 0x1000, 8, 0x22, 0, 0],
                       &fds(), &mut b, &mut fresh(), &[]);
        assert!(matches!(a, Answer::Reply(0, E_INVAL)), "prot: {:?}", a);
        /* zero length: EINVAL (Linux's own number) */
        let a = answer(9, &[0, 0, 3, 0x22, 0, 0],
                       &fds(), &mut b, &mut fresh(), &[]);
        assert!(matches!(a, Answer::Reply(0, E_INVAL)), "len0: {:?}", a);
        /* a length that overflows the page-round: EINVAL — the twin
         * answers the same rather than trusting the kernel's ladder */
        let a = answer(9, &[0, u64::MAX, 3, 0x22, 0, 0],
                       &fds(), &mut b, &mut fresh(), &[]);
        assert!(matches!(a, Answer::Reply(0, E_INVAL)), "overflow: {:?}", a);
        /* file-backed (no ANONYMOUS), shared, and unknown flag bits:
         * ENOSYS by name — the flags ride the log line */
        let a = answer(9, &[0, 0x1000, 3, 0x02, 0, 0],
                       &fds(), &mut b, &mut fresh(), &[]);
        assert!(matches!(a, Answer::Reply(0, E_NOSYS)), "file: {:?}", a);
        let a = answer(9, &[0, 0x1000, 3, 0x23, 0, 0],
                       &fds(), &mut b, &mut fresh(), &[]);
        assert!(matches!(a, Answer::Reply(0, E_NOSYS)), "shared: {:?}", a);
        let a = answer(9, &[0, 0x1000, 3, 0x62, 0, 0],
                       &fds(), &mut b, &mut fresh(), &[]);
        assert!(matches!(a, Answer::Reply(0, E_NOSYS)), "unknown: {:?}", a);
        /* a FIXED ask outside the span — below the base, past the
         * limit, straddling it — is ENOMEM: the budget shape */
        let a = answer(9, &[SPAN_BASE - 0x1000, 0x1000, 0, 0x32, 0, 0],
                       &fds(), &mut b, &mut fresh(), &[]);
        assert!(matches!(a, Answer::Reply(0, E_NOMEM)), "below: {:?}", a);
        let a = answer(9, &[SPAN_LIMIT, 0x1000, 0, 0x32, 0, 0],
                       &fds(), &mut b, &mut fresh(), &[]);
        assert!(matches!(a, Answer::Reply(0, E_NOMEM)), "past: {:?}", a);
        let a = answer(9, &[SPAN_LIMIT - 0x1000, 0x2000, 3, 0x32, 0, 0],
                       &fds(), &mut b, &mut fresh(), &[]);
        assert!(matches!(a, Answer::Reply(0, E_NOMEM)), "straddle: {:?}", a);
        /* a FIXED ask that is not page-aligned: EINVAL */
        let a = answer(9, &[SPAN_BASE + 1, 0x1000, 0, 0x32, 0, 0],
                       &fds(), &mut b, &mut fresh(), &[]);
        assert!(matches!(a, Answer::Reply(0, E_INVAL)), "align: {:?}", a);
    }

    #[test]
    fn mmap_cannot_land_under_the_break_and_brk_cannot_map_an_arena() {
        /* The two cursors, both ends (increment 16): a brk growth to
         * the span's limit is legal while the cursor is at the limit… */
        let mut b = span();
        let a = answer(12, &[SPAN_LIMIT, 0, 0, 0, 0, 0],
                       &fds(), &mut b, &mut fresh(), &[]);
        assert!(matches!(a, Answer::Reply(r, STATUS_OK)
                         if r == SPAN_LIMIT), "brk to limit: {:?}", a);
        assert_eq!(b.cur, SPAN_LIMIT);
        /* …and refused (the denial echo, the unchanged break) once an
         * arena owns the top page — a re-map would leak the frame and
         * zero the arena's bytes. The kernel's verdict caps by its OWN
         * linux_mmap_top for the same reason. */
        let mut b = span();
        let a = answer(9, &[0, 0x1000, 3, 0x22, 0, 0],
                       &fds(), &mut b, &mut fresh(), &[]);
        assert!(matches!(a, Answer::Reply(_, STATUS_OK)), "arena: {:?}", a);
        let a = answer(12, &[SPAN_LIMIT, 0, 0, 0, 0, 0],
                       &fds(), &mut b, &mut fresh(), &[]);
        assert!(matches!(a, Answer::Reply(r, STATUS_OK)
                         if r == SPAN_BASE),
                "brk past the cursor answers the denial echo: {:?}", a);
        assert_eq!(b.cur, SPAN_BASE, "the break did not move");
        /* brk to exactly the cursor is still legal (the span is
         * inclusive; the arena's first page is at the cursor) */
        let a = answer(12, &[SPAN_LIMIT - 0x1000, 0, 0, 0, 0, 0],
                       &fds(), &mut b, &mut fresh(), &[]);
        assert!(matches!(a, Answer::Reply(r, STATUS_OK)
                         if r == SPAN_LIMIT - 0x1000),
                "brk to the cursor: {:?}", a);
        /* and a non-FIXED mmap may not land under the break: the gap
         * between the break and the cursor is one page, the ask is
         * two — ENOMEM, and the refusal moves nothing. */
        let mut b = span();
        b.cur = SPAN_LIMIT - 0x1000;  /* one page of gap left */
        let a = answer(9, &[0, 0x2000, 3, 0x22, 0, 0],
                       &fds(), &mut b, &mut fresh(), &[]);
        assert!(matches!(a, Answer::Reply(0, E_NOMEM)),
                "an arena larger than the gap: {:?}", a);
        assert_eq!(b.mtop, SPAN_LIMIT, "a refusal moves nothing");
        /* the same one-page gap fits a one-page ask — the last page
         * is the arena's, exactly at the break's ceiling */
        let a = answer(9, &[0, 0x1000, 3, 0x22, 0, 0],
                       &fds(), &mut b, &mut fresh(), &[]);
        assert!(matches!(a, Answer::Reply(r, STATUS_OK)
                         if r == SPAN_LIMIT - 0x1000),
                "the last page is the arena's: {:?}", a);
    }

    #[test]
    fn hex_into_writes_fixed_width_lowercase() {
        let mut buf = [0u8; 20];
        let n = hex_into(&mut buf, 0, 0x807f000);
        assert_eq!(&buf[..n], b"000000000807f000",
                   "16 digits, zero-padded, lowercase");
        let mut buf = [0u8; 4];
        let n = hex_into(&mut buf, 0, 0xdeadbeefcafe);
        assert_eq!(n, 4, "clamped to the buffer like dec_into");
    }
}
