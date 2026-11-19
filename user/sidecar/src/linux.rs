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
//!   - `read`/`poll` (0/7 — E7 increment 24, the input class's waiting
//!     half): a console read with no buffered input PARKS its request
//!     until typed input (or the console's close) arrives; poll scans the
//!     caller's fd array against the task's table and the console, answers
//!     at once when a descriptor is ready or the timeout is 0, and
//!     otherwise holds the array until readiness or the deadline. A
//!     rewritten array crosses back on every poll that has one — a timeout
//!     answers zero revents, never an invented readiness;
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
//!   - `mprotect` (10, §5.4's protection seam): write+execute together
//!     is refused EPERM — the refusal H2's Node exclusion IS, and the
//!     twin of the kernel's first-trap refusal (both ends exist so
//!     neither alone is load-bearing). PROT_NONE and read-only/RW are
//!     served as word rules; the kernel applies them only to present
//!     leaves, and PROT_NONE parks the guard page's frame (non-present,
//!     frame retained) so the page faults and a later PROT_READ/PROT_WRITE
//!     can restore the same words — the Rust stack-overflow path's exact
//!     shape;
//!   - `rt_sigaction` (13) / `rt_sigprocmask` (14) / `rt_sigreturn` (15)
//!     / `sigaltstack` (131) / `tkill` (200) — the signal seam (§10.13's
//!     step 2, landed through M3.4): the act/set bytes arrive ATTACHED
//!     (the kernel copied them out of the child; this side never reads
//!     the child's memory), dispositions record DFL/IGN/handler with the
//!     handler address, flags, restorer and sa_mask, and the mask records
//!     BLOCK/UNBLOCK/SETMASK exactly. Delivery is REAL: a tkill to the
//!     task's own tid or an unblock reveal enters the recorded handler on
//!     the normal or alternate stack, `rt_sigreturn` restores the saved
//!     mask/context and redelivers the next pending handler, and a default
//!     terminate still converges on exit_group's own reap path. M3.5 adds
//!     the first asynchronous producer: a send that returns EPIPE consults
//!     the SIGPIPE disposition, and a handler's return carries -EPIPE back
//!     to the interrupted send. Refused by name: stop-by-default delivery
//!     and a tid that is not this task's own (ESRCH — cross-task signals
//!     are unscoped, never silently re-targeted at the sender). The
//!     oldact/oldset out-pointer is NO LONGER refused: E7 increment 12
//!     landed §10.10's copy-IN gap — the old disposition/mask travels back
//!     as this reply's PAYLOAD (Answer::ReplyBytes), the kernel copies it
//!     into the span its own first trap validated, and service() bounds the
//!     bytes against the forwarded out-pointer before anything is sent;
//!   - `mmap` (9, the mem class's anonymous-map half — E7 increment 16,
//!     §5.4): the ledger's cursor decides the address for an anonymous
//!     private map inside the budget span (or the caller's own FIXED
//!     address echoes back when it is inside the span), PROT_NONE maps
//!     no frames, W|X is EPERM and every un-served shape is refused by
//!     name with the request's words on the line — the kernel's
//!     first-trap ladder repeated as this side's twin, and the kernel's
//!     verdict re-checks every grant against its own span and its own
//!     cursors, so neither end is load-bearing alone;
//!   - `munmap` (11, the mem class's last seam — E7 increment 25,
//!     §5.4): the sidecar answers WHETHER (a page-aligned, nonzero,
//!     non-overflowing range inside the stamped span and at or above
//!     the break), and the kernel's page tables decide which pages are
//!     PRESENT — a double unmap is success, the frames return to the
//!     partition, and neither cursor moves (a freed range is never
//!     re-handed in v1);
//!   - everything else — `open`/`close`/`lseek` and the rest of the fd
//!     class (Phase 2 §3-§4), executable-only mprotect (ENOSYS by name),
//!     `clock_gettime` (needs the clock plus
//!     a child-buffer write) and any number outside the census —
//!     is answered `ENOSYS` with its number logged, §11 Q5's recommended
//!     policy: visible and cheap, never a silent success.
//!
//! Nothing here interprets a Linux syscall beyond choosing that answer: the
//! kernel's forward carries argument POSITIONS only, and the sidecar owns
//! every semantic decision (the §3.3 step-6 contract — there is exactly one
//! POSIX implementation and it is not in the kernel).

use alloc::boxed::Box;
use aerosls_blockcache::BufferAlloc;
use aerosls_proto::catalops::CatalogOps;
use aerosls_proto::kabi::{GrantedCap, Kernel, LinuxCloneThreadReq};
use aerosls_proto::sockops::SocketOps;
use aerosls_proto::catalops::{CAT_ECLOSED, CAT_EKERNEL, CAT_ENOCHAN, CAT_EPROTO};
use aerosls_proto::{
    CAT_ERR_BUSY,
    CAT_ERR_CONFLICT, CAT_ERR_CONSTRAINT, CAT_ERR_INTERNAL, CAT_ERR_NOENT,
    CAT_ERR_PERM, CAT_ERR_QUERY, CAT_ERR_SHAPE, CAT_ERR_TYPE,
    CAT_ERR_UNKNOWN, CAT_SQL_MAX,
};
use aerosls_proto::{NET_ERR_ADDRINUSE, NET_ERR_ADDRNOTAVAIL, NET_ERR_CAP,
    NET_ERR_CONNRESET, NET_ERR_INUSE, NET_ERR_INVAL, NET_ERR_IO,
    NET_ERR_MSGSIZE, NET_ERR_NOMEM, NET_ERR_NOTCONN, NET_ERR_PIPE,
    NET_ERR_PROTO, NET_ERR_WOULDBLOCK};
use aerosls_vfs::aerofs::FileType;
use aerosls_vfs::{CharNode, DirEnt, Errno, PollFd, Stat, Vfs,
    CLONE_FILES as VFS_CLONE_FILES, POLLIN, POLLNVAL, POLLOUT}; /* E7 increment 5: the fd
                              * table's console slot — service() holds the
                              * handle because only the entry loop has the
                              * CharNode. Step 4 (the file class): the VFS
                              * itself — every DECISION of the thirteen
                              * arms is this layer's; this module only
                              * translates (§5.6). */

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
const E_NOENT: i32 = 1;        /* CAP_ERR_NOTFOUND -> -ENOENT (§5.7): the
                                * catalog's NOENT is "not in your namespace" */
const E_IO: i32 = 2;           /* CAP_ERR_REVOKED -> -EIO (§5.7): a catalog
                                * call that failed under us / INTERNAL */
const E_PROTO: i32 = 10;       /* CAP_ERR_PROTO -> -EPROTO (§5.7): a catalog
                                * client reply that was not a reply */
const E_INTR: i32 = -34;       /* CAP_EINTR   -> -EINTR   (§5.7): a
                                * stopped parked exchange resumed by
                                * SIGCONT */
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

/* M4.5: how many times the exit_group sweep may yield and retry when the
 * kernel reports a runnable member (CAP_EAGAIN). Each retry is one scheduler
 * slice, so a CPU-bound sibling is preempted and the next sweep can prove it
 * dead; a target that never stops running is reported as a failure rather
 * than reaped from under its own execution. */
const GROUP_SWEEP_ATTEMPTS: u32 = 64;

/// The sig seam's numbers and sizes — twins of kernel/linux_abi.h's
/// LINUX_RT_SIGACTION / LINUX_RT_SIGPROCMASK / LINUX_SIGACT_BYTES /
/// LINUX_SIGSET_BYTES (the Rust side cannot include that header, so the
/// literals repeat, commented as twins; guard clause R pins both halves
/// and the kernel's plan derives the SAME attach length from these
/// same words: num and args[1] != 0).
const LINUX_RT_SIGACTION: u64 = 13;
const LINUX_RT_SIGPROCMASK: u64 = 14;
const LINUX_RT_SIGRETURN: u64 = 15;
/// M2b: the loader's stdio ioctl. musl's __stdout_write asks TIOCGWINSZ
/// on stdout before its first write (the answer only selects line
/// buffering; the winsize is never read) and its isatty asks TCGETS.
/// Those two are the whole arm; the console answers 0 for both without a
/// winsize/termios copy, which no caller here reads.
const LINUX_IOCTL: u64 = 16;
const LINUX_TCGETS: u64 = 0x5401;
const LINUX_TIOCGWINSZ: u64 = 0x5413;
const LINUX_SIGALTSTACK: u64 = 131;
const LINUX_TKILL: u64 = 200;
const LINUX_EXIT_GROUP: u64 = 231;
const LINUX_KILL: u64 = 62;
const LINUX_TGKILL: u64 = 234;
const LINUX_SET_ROBUST_LIST: u64 = 273;
const LINUX_GET_ROBUST_LIST: u64 = 274;
const LINUX_ROBUST_LIST_HEAD_BYTES: u64 = 24;
/// mmap(9) — armed since E7 increment 16 (§5.4's anonymous-map half):
/// the 9 arm below answers the measured allocator's shapes from the
/// ledger, and every refusal's LOG line carries the request's own
/// numbers (addr/len/prot/flags) so "the allocator asked for X and this
/// shim answered Y" is read off the line, never reconstructed.
const LINUX_MMAP: u64 = 9;
/// M2b: the kernel's synthetic file-map fill round — never a Linux
/// syscall number. It carries (Linux fd, file offset, bytes) and answers
/// count-truth bytes that the kernel's RAW deliver copies into the
/// already-mapped page.
const LINUX_MMAP_FILL: u64 = 0x4D4D_4650;
/// clone(56) — M4.2's thread create. Only the documented thread subset is
/// accepted; a fork-shaped word is ENOSYS by name (the kernel repeats the
/// same check, so neither end is load-bearing alone).
const LINUX_CLONE: u64 = 56;
const LINUX_CLONE3: u64 = 435;
const LINUX_CLONE_ARGS_BYTES: usize = 88;

/// M5.2: the project-private Linux CAT_EXEC number (kernel/linux_abi.h's
/// `SYS_SLS_CATALOG_EXEC`). It is NOT a census number; it rides the same
/// forwarded 72-byte description, with the bounded SQL behind the header.
/// The request carries no uid/partition — the kernel registration of THIS
/// sidecar's channel is the identity the catalog service uses. A wrong
/// personality never reaches this arm (the kernel refuses 334 in its legacy
/// chain), and the existing E7 cap-table guard still sees the child holding
/// exactly one channel.
const SYS_SLS_CATALOG_EXEC: u64 = 334;
const CLONE_VM: u64 = 0x0000_0100;
const CLONE_FS: u64 = 0x0000_0200;
const CLONE_FILES: u64 = 0x0000_0400;
const CLONE_SIGHAND: u64 = 0x0000_0800;
const CLONE_THREAD: u64 = 0x0001_0000;
const CLONE_SYSVSEM: u64 = 0x0004_0000;
const CLONE_SETTLS: u64 = 0x0008_0000;
const CLONE_PARENT_SETTID: u64 = 0x0010_0000;
const CLONE_CHILD_CLEARTID: u64 = 0x0020_0000;
const CLONE_CHILD_SETTID: u64 = 0x0100_0000;
/// Deprecated and ignored by Linux, but still set by musl's pthread_create.
const CLONE_DETACHED: u64 = 0x0040_0000;
const CLONE_M4_ALLOW: u64 = CLONE_VM | CLONE_FS | CLONE_FILES | CLONE_SIGHAND
    | CLONE_THREAD | CLONE_SYSVSEM | CLONE_SETTLS | CLONE_PARENT_SETTID
    | CLONE_CHILD_CLEARTID | CLONE_CHILD_SETTID | CLONE_DETACHED;
/// munmap(11) — E7 increment 25 (§5.4's last memory seam): the 11 arm
/// answers WHETHER over the ledger's span and break; the kernel's page
/// tables decide which pages are present, so a double unmap is success.
const LINUX_MUNMAP: u64 = 11;
/// clock_gettime (228) — the census's vDSO-backstopped row, and a REAL
/// trap here because this kernel ships no vDSO (§5.5). The reply is one
/// Linux `struct timespec` (two LE i64s) at args[1]; the kernel's plan
/// and deliver own the 16-byte copy-out.
const LINUX_CLOCK_GETTIME: u64 = 228;
const TIMESPEC_BYTES: usize = 16;
/// 2026-01-07T00:00:00Z — the day after the six staged old backups, so
/// the backup's timestamped name cannot collide with one of them. The
/// gate pins the value instead of reading a wall clock; a fixed epoch is
/// what makes the run's file name reproducible.
const GATE_EPOCH_SECS: u64 = 1_767_744_000;
const SIGACT_BYTES: usize = 32; /* kernel-ABI struct sigaction: four u64s */
const SIGSET_BYTES: u64 = 8;    /* the sigset this seam crosses            */
const STACK_T_BYTES: usize = 24; /* ss_sp u64, ss_flags u32 + pad, ss_size u64 */
const SS_ONSTACK: u32 = 1;
const SS_DISABLE: u32 = 2;
const MINSIGSTKSZ: u64 = 2048;

/// M3: the reply trailer that asks the kernel to build a signal frame.
/// Ten little-endian words; the kernel twin is
/// `struct LinuxSignalDelivery` in kernel/linux_abi.h.
const LINUX_SIGNAL_DELIVERY_MAGIC: u64 = 0x5349_4744; /* "SIGD" */
const LINUX_SIGNAL_DELIVERY_BYTES: usize = 80;
/// Linux's EPIPE, the value a SIGPIPE-producing send/write must return
/// after its handler returns. The CAP status this side answers with is
/// E_PIPE (-28); the kernel maps it to this Linux number.
const LINUX_EPIPE: i64 = 32;
const SIGPIPE: u64 = 13;
const SIGCONT: u64 = 18;
pub const LINUX_EINTR: i64 = 4;

/// How far a signal number may run: Linux x86-64 signals are 1..=64
/// (1..31 standard, 32..64 real-time; 0 is tkill's existence probe and
/// rt_sigaction's EINVAL case, never an index).
const SIG_MAX: usize = 64;
/// SIGKILL (9) and SIGSTOP (19) can never be blocked. Linux silently
/// drops those bits from every mask, including a handler's mask and the
/// mask rt_sigreturn restores.
const SIG_UNBLOCKABLE: u64 = (1u64 << 8) | (1u64 << 18);

/// Dispositions this v1 can honestly hold: SIG_DFL, SIG_IGN, and (M3) a
/// user handler. The handler's address, flags, restorer and sa_mask are
/// stored with it; the KERNEL owns the frame/register mechanism and this
/// side owns every semantic decision over them (D1's split).
const DISPO_DFL: u8 = 0;
const DISPO_IGN: u8 = 1;
const DISPO_HANDLER: u8 = 2;

/// Linux x86-64 sa_flags this increment can honour. Anything outside the
/// set changes the handler's calling convention or stack selection, so it
/// is refused by name at install rather than silently approximated.
const SA_SIGINFO: u64 = 0x4;
const SA_ONSTACK: u64 = 0x0800_0000;
const SA_RESTORER: u64 = 0x0400_0000;
const SA_NODEFER: u64 = 0x4000_0000;
const SA_RESETHAND: u64 = 0x8000_0000;
const SA_SUPPORTED: u64 = SA_SIGINFO | SA_ONSTACK | SA_RESTORER
    | SA_NODEFER | SA_RESETHAND;

#[derive(Clone, Copy)]
struct SigAct {
    handler: u64,
    flags: u64,
    restorer: u64,
    mask: u64,
}

impl SigAct {
    const fn new() -> Self {
        SigAct { handler: 0, flags: 0, restorer: 0, mask: 0 }
    }
}

/// M3: the handler-delivery request the kernel turns into a frame plus a
/// sysret into the handler. `oldset` is the pre-call mask the kernel writes
/// to rt_sigprocmask's staged out-pointer (ignored for tkill).
#[cfg_attr(test, derive(Debug))]
#[derive(Clone, Copy)]
struct SignalDelivery {
    signo: u64,
    handler: u64,
    flags: u64,
    restorer: u64,
    old_mask: u64,
    oldset: u64,
    alt_sp: u64,
    alt_size: u64,
    /// Saved rax restored by rt_sigreturn. Ordinary deliveries leave 0;
    /// SIGPIPE's send/write producer sets -EPIPE so the handler returns
    /// into the interrupted syscall with the errno Linux owes it.
    sysret: u64,
}

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
/// The stdio flush's gather (writev, 20). The kernel owns the iovec walk:
/// it validates the caller's array, copies every piece into one payload,
/// and rewrites args[2] to the gathered total (never the iovcnt). This
/// side therefore answers it with write's own arm and write's own count
/// truth — `expected_req_len` derives the attach from that same word.
const LINUX_WRITEV: u64 = 20;
const FD_PAYLOAD_CAP: usize = 1024;
/* M2b: the mmap fill round's cap — 4024 = CAP_MSG_MAX_PAYLOAD (4096) −
 * the 72-byte header, the largest payload one reply message can carry.
 * It is separate from FD_PAYLOAD_CAP so the write chunk and the read /
 * getdents mirrors keep their 1 KiB bound; only a fill round uses it,
 * which is why the loader's 20 KiB DSO map takes six rounds rather than
 * twenty — the boot's own sidecar self-kill tooth must not outrun a
 * mapping's fill. */
const FD_FILL_CAP: usize = 4024;

/// E7 step 4 (the file class, §5.6): the census's thirteen numbers as
/// this side sees them — literals repeated from kernel/linux_abi.h (the
/// Rust side cannot include that header; guard clause P pins both
/// halves), and read (0) joins them: the fd class's read arm routes
/// through this module now, where the fd table lives.
const LINUX_READ: u64 = 0;
const LINUX_OPEN: u64 = 2;
const LINUX_CLOSE: u64 = 3;
const LINUX_STAT: u64 = 4;
/// fstat(5) — M2b: the loader asks a descriptor for the DSO's size/mode
/// before it maps it. Twin of kernel/linux_abi.h's LINUX_FSTAT.
const LINUX_FSTAT: u64 = 5;
const LINUX_LSTAT: u64 = 6;
const LINUX_LSEEK: u64 = 8;
const LINUX_FCNTL: u64 = 72;
const LINUX_FSYNC: u64 = 74;
const LINUX_FTRUNCATE: u64 = 77;
const LINUX_RENAME: u64 = 82;
const LINUX_MKDIR: u64 = 83;
const LINUX_UNLINK: u64 = 87;
const LINUX_GETDENTS64: u64 = 217;

/// M1 — the Linux network bridge. The eight numbers are kernel/linux_abi.h's
/// LSC_NET rows; the literals repeat there because this side cannot include
/// that header (the same twin discipline as the fd class's numbers). Their
/// SEMANTICS are not repeated: every arm below is a translation onto the
/// SocketOps trait, exactly as the file class's arms translate onto the VFS.
const LINUX_SOCKET: u64 = 41;
const LINUX_CONNECT: u64 = 42;
const LINUX_ACCEPT: u64 = 43;
const LINUX_SENDTO: u64 = 44;
const LINUX_RECVFROM: u64 = 45;
const LINUX_SHUTDOWN: u64 = 48;
const LINUX_SENDTO_DEST_MARKER: u64 = 1;
const LINUX_BIND: u64 = 49;
const LINUX_LISTEN: u64 = 50;

/// The Linux ABI's IPv4 shapes, and the one transform this side owns: a
/// sockaddr_in carries sin_family in HOST order but sin_port/sin_addr in
/// NETWORK order, while SocketOps takes both as Host-order values. The
/// kernel attaches exactly 16 bytes for connect/bind; recvfrom/accept reply
/// with a 20-byte trailer (the 16-byte struct then the u32 addrlen) so the
/// kernel can write all of the caller's destinations from one payload.
const LINUX_SOCKADDR_IN_BYTES: usize = 16;
const LINUX_NET_ADDR_BYTES: usize = 20;
const NET_RECV_MAX: usize = FD_PAYLOAD_CAP - LINUX_NET_ADDR_BYTES;
const AF_INET: u16 = 2;
const SOCK_STREAM: u16 = 1;
const O_RDWR: u32 = 2;
const O_NONBLOCK: u32 = 0x800;

/// poll (7) — E7 increment 24: the array's shape is the kernel's too
/// (kernel/linux_abi.h's LINUX_POLL_MAX_FDS / LINUX_POLLFD_BYTES). The
/// Rust side cannot include that header, so the literals repeat as twins:
/// struct pollfd is fd: i32, events: i16, revents: i16; 16 entries cap
/// one call so neither end's frame can grow unbounded.
const LINUX_POLL: u64 = 7;
/// futex(202) — M4.4's group-keyed wait/wake. Only the private wait and
/// wake operations are served; requeue, PI, robust lists and bitsets are
/// named follow-ons and are refused ENOSYS rather than guessed. The kernel
/// attaches the observed word and optional relative timespec; this side
/// owns the queue and every policy decision (D6).
const LINUX_GETPID: u64 = 39;
const LINUX_EXIT: u64 = 60;
const LINUX_GETTID: u64 = 186;
const LINUX_FUTEX: u64 = 202;
const FUTEX_WAIT: u64 = 0;
const FUTEX_WAKE: u64 = 1;
const FUTEX_REQUEUE: u64 = 3;
const FUTEX_CMP_REQUEUE: u64 = 4;
const FUTEX_WAIT_BITSET: u64 = 9;
const FUTEX_WAKE_BITSET: u64 = 10;
const FUTEX_PRIVATE: u64 = 128;
const FUTEX_WAIT_PAYLOAD_BYTES: usize = 24;
const FUTEX_REQUEUE_PAYLOAD_BYTES: usize = 4;
const NS_PER_SEC: u64 = 1_000_000_000;
const POLLERR: u16 = 0x0008;
const LINUX_POLL_MAX_FDS: usize = 16;
const LINUX_POLLFD_BYTES: usize = 8;
const LINUX_POLL_PAYLOAD_BYTES: usize = LINUX_POLL_MAX_FDS * LINUX_POLLFD_BYTES;

/// What a parked task's pending exchange is (E7 increment 24): the
/// request has been read and its reply is deliberately NOT sent yet —
/// the child is parked awaiting it, so no second request can arrive.
const PENDING_NONE: u8 = 0;
const PENDING_READ: u8 = 1;
const PENDING_POLL: u8 = 2;
const PENDING_FUTEX: u8 = 3;

/// The path cap — the twin of kernel/linux_abi.h's LINUX_PATH_CAP and
/// the shim's NAMED divergence from PATH_MAX 4096 (the kernel refuses a
/// longer path at the first trap; this side validates the NULs it was
/// given rather than trusting a length it cannot check).
const LINUX_PATH_CAP: usize = 1024;
/// The recv buffer's attach ceiling: rename carries TWO paths back to
/// back behind the header (the twin of LINUX_FWD_ATTACH_CAP). The
/// write/sig payloads are smaller and fit unchanged.
const RECV_ATTACH_CAP: usize = 2 * (LINUX_PATH_CAP + 1);
/// The compact Stat payload (the twin of LINUX_STAT_PAYLOAD_BYTES): the
/// sidecar never sees musl's 144-byte layout — kernel/linux_abi.c's
/// linux_stat_bytes is the only place the two layouts meet.
const STAT_PAYLOAD_BYTES: usize = 24;
/// The sidecar's directory-listing payload cap (the twin of
/// LINUX_DENTS_PAYLOAD_CAP): names, not records — the kernel packs the
/// Linux records, and a short getdents is POSIX-legal (the caller
/// loops, and end-of-directory is 0 bytes).
const DENTS_PAYLOAD_CAP: usize = 1024;

/// The §5.7 map, this side: vfs::Errno → the CAP code whose kernel-side
/// arm delivers the right Linux errno. Total over vfs::Errno (the match
/// is exhaustive — a new errno is a compile error, not a silent EIO),
/// and every code here is one linux_errno_from_cap maps FORWARD to the
/// vfs errno's own number (guard clause P pins the round-trip in the
/// host test). Codes the existing family already names keep its
/// constants: CAP_ERR_NOTFOUND 1, CAP_ERR_REVOKED 2 (EIO),
/// CAP_EINVAL -1, CAP_EBADF -2, CAP_ENOMEM -7, CAP_ENOSPC -8,
/// CAP_ENOSYS -10, CAP_ECONFLICT -11 (EBUSY), CAP_EPERM -12, and the
/// file class's own (-13)..(-20). Divergences are named where they
/// exist: EDQUOT and EFBIG read as ENOSPC (both are "the store says
/// no"; the quota's own number is owed to v0.2's storage work), EMFILE
/// reads as ENFILE (a fixed table is system-scoped — cap.h's own note),
/// ENOTTY/EADDRINUSE/EXDEV/ENOPROTOOPT read as EINVAL/ENOSYS — none is
/// reachable from the census, and a wrong number would be worse than a
/// named one.
fn cap_of(e: Errno) -> i32 {
    const CAP_ERR_NOTFOUND: i32 = 1;
    match e {
        Errno::ENoent => CAP_ERR_NOTFOUND,
        Errno::EIo => E_REVOKED,           /* -EIO                    */
        Errno::EAcces => E_ACCES,          /* -EACCES (not EPERM)     */
        Errno::EBadf => E_BADF,
        Errno::EIsdir => E_ISDIR,
        Errno::ENotdir => E_NOTDIR,
        Errno::EExist => E_EXIST,
        Errno::ENotempty => E_NOTEMPTY,
        Errno::ENospc => E_NOSPC,
        Errno::EDquot => E_NOSPC,          /* named divergence        */
        Errno::ERofs => E_ROFS,
        Errno::EFbig => E_NOSPC,           /* named divergence        */
        Errno::EInval => E_INVAL,
        Errno::ENametoolong => E_NAMETOOLONG,
        Errno::ENomem => E_NOMEM,
        Errno::EAgain => E_AGAIN,
        Errno::EBusy => E_CONFLICT,        /* -EBUSY                  */
        Errno::EMfile => E_TABLEFULL,      /* ENFILE: a fixed table   */
        Errno::ENoexec => E_NOSYS,         /* execve is not landed    */
        Errno::ESPipe => E_SPIPE,
        Errno::EPipe => E_REVOKED,         /* no pipe fds yet; EIO    */
        Errno::ESRCH => E_TARGET,          /* -ESRCH                  */
        Errno::ENotty => E_INVAL,          /* named divergence        */
        Errno::EAddrInuse => E_INVAL,      /* named divergence        */
        Errno::EXDev => E_INVAL,           /* named divergence        */
        Errno::EAfNoSupport => E_NOSYS,    /* named divergence        */
        Errno::ENosys => E_NOSYS,
    }
}

/// The file class's CAP codes (cap.h's (-13)..(-20) and the existing
/// family members these arms use) — the values the reply's status
/// field carries, pinned here as twins.
const E_ACCES: i32 = -16;
const E_ISDIR: i32 = -15;
const E_NOTDIR: i32 = -14;
const E_EXIST: i32 = -13;
const E_NOTEMPTY: i32 = -19;
const E_ROFS: i32 = -17;
const E_SPIPE: i32 = -18;
const E_NAMETOOLONG: i32 = -20;
/* M1 network errno family (kernel/cap.h's CAP_E* twins). Distinct codes
 * matter here: a TCP caller branches on ECONNRESET vs ETIMEDOUT vs
 * ENOTCONN, and collapsing them into EIO loses the only recoverable
 * information the bridge can give it. */
const E_ADDRINUSE: i32 = -21;
const E_ADDRNOTAVAIL: i32 = -22;
const E_CONNRESET: i32 = -23;
const E_NOTCONN: i32 = -24;
const E_ISCONN: i32 = -25;
const E_DESTADDRREQ: i32 = -26;
const E_MSGSIZE: i32 = -27;
const E_PIPE: i32 = -28;
const E_CONNREFUSED: i32 = -29;
const E_TIMEOUT: i32 = 13;      /* CAP_ERR_TIMEOUT -> -ETIMEDOUT */
const E_NETUNREACH: i32 = -30;
const E_HOSTUNREACH: i32 = -31;
const E_AFNOSUPPORT: i32 = -32;
const E_AGAIN: i32 = -3;      /* CAP_EAGAIN  -> -EAGAIN (a read with
                               * nothing buffered; console reads park
                               * since increment 24)                   */
const E_NOSPC: i32 = -8;      /* CAP_ENOSPC  -> -ENOSPC                   */
const E_TABLEFULL: i32 = -4;  /* CAP_ETABLEFULL -> -ENFILE                */

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
    /// M2b: a FILE-BACKED mmap was granted at `ret` (the ledger's address
    /// or the caller's FIXED one). `service` sends the file's `len`
    /// page-rounded bytes at `off` from VFS fd `vfd` as chunk messages
    /// AHEAD of the grant reply, then the ordinary 72-byte answer — the
    /// kernel drains the chunks, stages them, and maps the granted
    /// frames from them (its pages cannot ride one 4 KiB reply).
    MmapFile(u64, u32, u64, u32),
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
    /// (step 4 widened it to FD_PAYLOAD_CAP: getdents64's name list;
    /// `len` is the exact count that crosses — the buffer's upper bytes
    /// never leave this side, and payload_bound is the per-number bound
    /// over it).
    ReplyBytes(u64, usize, [u8; FD_PAYLOAD_CAP]),
    /// M2b: a file-map fill round's bytes. Its own variant and buffer
    /// because a round is up to 4024 bytes — larger than any other
    /// reply payload — and the kernel copies it into the just-mapped
    /// page (LINUX_STAGE_FILL), not through a user-write pointer.
    ReplyFill(u64, usize, [u8; FD_FILL_CAP]),
    /// read(0) on the console with no buffered input (E7 increment 24):
    /// hold the request until input arrives or the console closes. The
    /// count is the caller's (already clamped by the kernel). `service`
    /// keeps it pending and answers from the bytes that actually arrive.
    ReadWait(usize),
    /// poll(7) with a timeout that allows waiting (increment 24): the fd
    /// array snapshot, its byte length, and the absolute deadline in
    /// nanoseconds (0 = POSIX's -1, wait forever). `service` re-scans the
    /// array on every pass; readiness or the deadline answers it.
    PollWait([u8; LINUX_POLL_PAYLOAD_BYTES], usize, u64),
    /// M4.4: FUTEX_WAIT matched the observed word. The task is enqueued in
    /// its group's FIFO wait list and parked until a FUTEX_WAKE or the
    /// deadline (0 = forever). No reply crosses on this arm.
    FutexWait(u64, u64),
    /// M4.4: FUTEX_WAKE removed these waiters, oldest first. `service` sends
    /// each one the deferred zero reply and answers the waker with the count.
    FutexWake(FutexWake),
    /// M4.8: FUTEX_REQUEUE woke this prefix and moved the rest to `uaddr`.
    /// `service` sends the deferred replies for `FutexWake` only; the moved
    /// waiters stay in PENDING_FUTEX under their original tags.
    FutexRequeue(FutexWake, u64, usize),
    /// M4.5: exit(60) ends only the calling thread; reply nothing.
    ExitThread,
    /// M4.5: exit_group(231) sweeps the whole thread group; reply nothing.
    ExitGroup,
    /// E7 increment 10 (the signal seam): a signal's DEFAULT ACTION ran —
    /// reap the task through the same exit path and reply nothing. On
    /// real Linux the process dies inside the delivering syscall and never
    /// returns to its caller, so "no reply" is the faithful shape; the
    /// variant is distinct from `Exit` only so the serial log names WHICH
    /// decision killed the task — the boot's attribution reads the line,
    /// not an inference. The payload is the SIGNAL NUMBER itself (increment
    /// 14): the kill line prints `sig=<N>`, so WHICH signal killed the task
    /// is read off the line rather than reconstructed from which arms ran.
    SignalKill(u64),
    /// M4.8: a default-terminate signal addressed to a SIBLING. `service`
    /// reaps that task, wakes its clear/robust futexes, and answers the
    /// sender 0 (the target, not the caller, dies).
    SignalKillTask(u32, u64),
    /// M4.8: a group-directed default-terminate signal. `service` folds this
    /// into the existing exit_group sweep, so no caller reply is fabricated.
    SignalGroupKill(u64),
    /// M3: a recorded handler is delivered. The kernel builds the x86-64
    /// signal frame and sysrets into `handler`; `rt_sigreturn` comes back
    /// as a normal num=15 request so this side restores the saved mask.
    SignalHandler(SignalDelivery),
    /// M4.10: a recorded handler addressed to a parked SIBLING. The target
    /// task index is carried so `service` can put the trailer on that
    /// task's own pending reply; the kernel builds the frame in the
    /// target's saved syscall context.
    SignalHandlerTask(u32, SignalDelivery),
    /// M4.11: a recorded handler addressed to a SIBLING that has no parked
    /// reply (it is running/suspended between syscalls). `service` asks the
    /// kernel to install the frame directly into its saved ring3 context.
    SignalHandlerRun(u32, SignalDelivery),
    /// M4.10: a stop-default signal addressed to a parked SIBLING. The
    /// target's pending exchange is held (no reply) until SIGCONT resumes
    /// it with -EINTR.
    SignalStopTask(u32, u64),
    /// M4.11: a process-directed stop broadcast. Every non-caller sibling is
    /// latched; parked targets keep their reply until SIGCONT, and
    /// non-parked targets are held by the kernel until SIGCONT releases
    /// them. The fixed set is the sidecar's own bounded table.
    SignalStopGroup(SignalStopGroup),
    /// M4.10: SIGCONT resumes the stopped sibling(s). The fixed set is the
    /// same bounded shape as FutexWake; `service` answers each parked
    /// target with -EINTR and clears the stop latch.
    SignalContinue(SignalContinue),
}

/// M4.4: the bounded set a FUTEX_WAKE selected. The count is the Linux
/// return value; the task indices are the waiters whose deferred replies
/// `service` sends after the queue transition.
#[cfg_attr(test, derive(Debug))]
#[derive(Clone, Copy)]
struct FutexWake {
    n: u8,
    tasks: [u8; MAX_FUTEX_WAITERS],
}

/// M4.10: the bounded set SIGCONT resumes. Same shape as FutexWake: the
/// sidecar's table is fixed, so the answer carries indices, never an
/// allocation.
#[cfg_attr(test, derive(Debug))]
#[derive(Clone, Copy)]
struct SignalContinue {
    n: u8,
    tasks: [u8; MAX_LINUX_TASKS],
}

/// M4.11: the bounded set a process-directed stop latched. `parked` targets
/// already have a reply to withhold; `running` targets have none and must be
/// held by the kernel until SIGCONT. `waiters` is the exact FUTEX_WAIT queue
/// entry removed for each task, so a failed broadcast can put the parked
/// target back in the queue instead of leaving it neither stopped nor
/// wakeable.
#[cfg_attr(test, derive(Debug))]
#[derive(Clone, Copy)]
struct SignalStopGroup {
    sig: u64,
    n: u8,
    parked: u8,
    tasks: [u8; MAX_LINUX_TASKS],
    waiters: [FutexWaiter; MAX_LINUX_TASKS],
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
    /// M2b: the task's loaded-image span, asked of the kernel that stamped
    /// it (the interpreter's half is the kernel's placement). Zero on a
    /// task with no image span or a pre-span boot: the ledger then admits
    /// the heap span alone, exactly as before.
    image_lo: u64,
    image_hi: u64,
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
    tid: u64,                 /* M4.5: the kernel-assigned TID (gettid)       */
    tgid: u64,                /* M4.5: the leader's TID (getpid)             */
    clear_tid: u64,           /* M4.5: set_tid_address/CLONE_CHILD_CLEARTID  */
    robust_head: u64,         /* M4.8: struct robust_list_head VA, or 0      */
    robust_len: u64,          /* M4.8: the size passed to set_robust_list    */
    mask: u64,                /* blocked signals, bit (sig - 1), signals 1..=64 */
    pending: u64,             /* blocked signals that have arrived              */
    dispo: [u8; SIG_MAX + 1], /* indexed by signal number: DFL / IGN / HANDLER  */
    act: [SigAct; SIG_MAX + 1], /* handler details, valid iff dispo == HANDLER  */
    alt_sp: u64,              /* sigaltstack: ss_sp, 0 when disabled            */
    alt_size: u64,            /* sigaltstack: ss_size                           */
    alt_flags: u32,           /* 0 / SS_DISABLE / SS_ONSTACK (query only)       */
    alt_depth: u32,           /* handlers currently running on the alt stack    */
    alt_trace: u64,           /* bit d: handler depth d was entered on the alt  */
    handler_depth: u8,        /* nested handler returns: which trace bit is next */
}

impl SigState {
    const fn new() -> Self {
        SigState {
            tid: 0,
            tgid: 0,
            clear_tid: 0,
            robust_head: 0,
            robust_len: 0,
            mask: 0,
            pending: 0,
            dispo: [DISPO_DFL; SIG_MAX + 1],
            act: [SigAct::new(); SIG_MAX + 1],
            alt_sp: 0,
            alt_size: 0,
            alt_flags: SS_DISABLE,
            alt_depth: 0,
            alt_trace: 0,
            handler_depth: 0,
        }
    }
}

/// The bytes an oldact reply payload carries (E7 increment 12, §10.13's
/// step 3) — the kernel-ABI struct sigaction as THIS state records it:
/// the exact four words this state stores. SIG_DFL/SIG_IGN report the
/// reserved handler word and zero flags/restorer/mask; a real handler
/// reports exactly what was installed. Twin of the kernel's staged clamp
/// (32 bytes — LINUX_SIGACT_BYTES), which the deliver bounds the copy
/// against.
fn old_act_bytes(sig: &SigState, no: u64) -> [u8; FD_PAYLOAD_CAP] {
    let mut b = [0u8; FD_PAYLOAD_CAP];
    let i = no as usize;
    let (handler, flags, restorer, mask) = match sig.dispo[i] {
        DISPO_IGN => (1, 0, 0, 0),
        DISPO_HANDLER => (sig.act[i].handler, sig.act[i].flags,
                          sig.act[i].restorer, sig.act[i].mask),
        _ => (0, 0, 0, 0),
    };
    b[0..8].copy_from_slice(&handler.to_le_bytes());
    b[8..16].copy_from_slice(&flags.to_le_bytes());
    b[16..24].copy_from_slice(&restorer.to_le_bytes());
    b[24..32].copy_from_slice(&mask.to_le_bytes());
    b
}

/// A SUCCESSFUL sig answer, with the out-pointer's payload attached iff
/// the forwarded args[2] (the oldact/oldset pointer) was non-NULL — the
/// arm-side half of service()'s bound: a NULL out-pointer owes no bytes
/// and must carry none (the deliver refuses payload on a request that
/// staged none), a non-NULL one owes exactly the struct's size. The two
/// refusal paths above never reach here — an errno reply carries no
/// payload, which is the deliver's shape-1 rule on the other end.
fn sig_ok(args: &[u64; 6], old: [u8; FD_PAYLOAD_CAP]) -> Answer {
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
fn sigset_payload(mask: u64) -> [u8; FD_PAYLOAD_CAP] {
    let mut b = [0u8; FD_PAYLOAD_CAP];
    b[..SIGSET_BYTES as usize].copy_from_slice(&mask.to_le_bytes());
    b
}

/// The Linux stack_t bytes for sigaltstack: ss_sp, ss_flags (u32 plus the
/// ABI's padding), ss_size. A disabled stack carries ss_sp=0, ss_size=0.
fn stack_t_payload(sp: u64, flags: u32, size: u64) -> [u8; FD_PAYLOAD_CAP] {
    let mut b = [0u8; FD_PAYLOAD_CAP];
    b[0..8].copy_from_slice(&sp.to_le_bytes());
    b[8..12].copy_from_slice(&flags.to_le_bytes());
    b[16..24].copy_from_slice(&size.to_le_bytes());
    b
}

fn read_stack_t(buf: &[u8]) -> (u64, u32, u64) {
    (read_u64(buf, 0),
     u32::from_le_bytes([buf[8], buf[9], buf[10], buf[11]]),
     read_u64(buf, 16))
}

/// M3: the mask in force while a handler runs — the old mask, the
/// action's sa_mask, and the signal itself unless SA_NODEFER. SIGKILL and
/// SIGSTOP can never be blocked; Linux silently drops those bits.
fn handler_mask(sig: &SigState, no: u64) -> u64 {
    let i = no as usize;
    let mut m = sig.mask | sig.act[i].mask;
    if sig.act[i].flags & SA_NODEFER == 0 {
        m |= 1u64 << (no - 1);
    }
    m & !SIG_UNBLOCKABLE
}

/// M3: commit the handler-disposition decision. The sidecar's mask moves to
/// the handler mask BEFORE the reply crosses; the kernel's frame carries the
/// old mask, and `rt_sigreturn` sends it back through num=15. `oldset` is
/// the pre-call mask for rt_sigprocmask's out-pointer.
fn deliver_handler(sig: &mut SigState, no: u64, oldset: u64) -> SignalDelivery {
    let i = no as usize;
    let old_mask = sig.mask;
    let flags = sig.act[i].flags;
    let handler = sig.act[i].handler;
    let restorer = sig.act[i].restorer;
    sig.mask = handler_mask(sig, no);
    let use_alt = flags & SA_ONSTACK != 0
        && sig.alt_flags != SS_DISABLE
        && sig.alt_sp != 0 && sig.alt_size >= 4096;
    let (alt_sp, alt_size) = if use_alt {
        (sig.alt_sp, sig.alt_size)
    } else {
        (0, 0)
    };
    let depth = sig.handler_depth as u32;
    if use_alt {
        sig.alt_depth = sig.alt_depth.saturating_add(1);
    }
    /* A mixed nesting (an ordinary handler delivered inside an alternate-
     * stack handler) must not make the ordinary return pop the alt depth.
     * The trace records which depth used the alternate stack; sigreturn pops
     * exactly the handler that is returning. */
    if depth < 64 {
        if use_alt {
            sig.alt_trace |= 1u64 << depth;
        } else {
            sig.alt_trace &= !(1u64 << depth);
        }
        sig.handler_depth = sig.handler_depth.saturating_add(1);
    }
    /* SA_RESETHAND resets before the handler runs, so a nested delivery
     * of the same signal sees SIG_DFL — Linux's order. */
    if flags & SA_RESETHAND != 0 {
        sig.dispo[i] = DISPO_DFL;
        sig.act[i] = SigAct::new();
    }
    SignalDelivery {
        signo: no,
        handler,
        flags,
        restorer,
        old_mask,
        oldset,
        alt_sp,
        alt_size,
        sysret: 0,
    }
}

/// M3.3: restore the mask saved by the handler frame and apply Linux's
/// return-time pending rule — the next unblocked pending signal is
/// delivered before the interrupted context resumes. One handler is
/// delivered per sigreturn; its own return runs this arm again, so a
/// chain of pending handlers unwinds in order. Ignore-default pending
/// bits are consumed, a terminate-default kills, and a stop-default is
/// still refused by name (the one default this shim cannot perform).
fn restore_and_redeliver(sig: &mut SigState, old_mask: u64) -> Answer {
    sig.mask = old_mask & !SIG_UNBLOCKABLE;
    if sig.handler_depth != 0 {
        sig.handler_depth -= 1;
        let depth = sig.handler_depth as u32;
        if sig.alt_trace & (1u64 << depth) != 0 {
            sig.alt_trace &= !(1u64 << depth);
            sig.alt_depth = sig.alt_depth.saturating_sub(1);
        }
    }
    let mut pending = sig.pending & !sig.mask;
    while pending != 0 {
        let bit = pending.trailing_zeros();
        pending &= !(1u64 << bit);
        let no = bit + 1;
        if sig.dispo[no as usize] == DISPO_IGN
            || default_action(no as u64) == ACT_IGNORE {
            sig.pending &= !(1u64 << bit);
            continue;
        }
        if sig.dispo[no as usize] == DISPO_HANDLER {
            sig.pending &= !(1u64 << bit);
            return Answer::SignalHandler(deliver_handler(sig, no as u64, 0));
        }
        if default_action(no as u64) == ACT_TERMINATE {
            return Answer::SignalKill(no as u64);
        }
        return Answer::Reply(0, E_NOSYS);
    }
    Answer::Reply(0, STATUS_OK)
}

/// The reply payload the kernel parses into a signal frame. Field order is
/// the kernel struct's; changing either side alone is a protocol break.
fn signal_delivery_payload(d: &SignalDelivery) -> [u8; LINUX_SIGNAL_DELIVERY_BYTES] {
    let mut b = [0u8; LINUX_SIGNAL_DELIVERY_BYTES];
    b[0..8].copy_from_slice(&LINUX_SIGNAL_DELIVERY_MAGIC.to_le_bytes());
    b[8..16].copy_from_slice(&d.signo.to_le_bytes());
    b[16..24].copy_from_slice(&d.handler.to_le_bytes());
    b[24..32].copy_from_slice(&d.flags.to_le_bytes());
    b[32..40].copy_from_slice(&d.restorer.to_le_bytes());
    b[40..48].copy_from_slice(&d.old_mask.to_le_bytes());
    b[48..56].copy_from_slice(&d.oldset.to_le_bytes());
    b[56..64].copy_from_slice(&d.alt_sp.to_le_bytes());
    b[64..72].copy_from_slice(&d.alt_size.to_le_bytes());
    b[72..80].copy_from_slice(&d.sysret.to_le_bytes());
    b
}

/// M3.5: the first asynchronous producer. Linux raises SIGPIPE when a
/// write/send reports EPIPE (unless the disposition is SIG_IGN). The
/// syscall still completes with EPIPE after a handler returns, so a
/// delivered handler's saved rax is -EPIPE, not the ordinary 0. A blocked
/// SIGPIPE pends and is delivered by the next unblock; a default action
/// kills through the one SignalKill path; SIG_IGN leaves the EPIPE reply
/// untouched. This is deliberately the send path only — the shim has no
/// pipe descriptors yet, and sendto is the one measured producer.
fn sigpipe_after_epipe(sig: &mut SigState, ans: Answer) -> Answer {
    if !matches!(ans, Answer::Reply(_, status) if status == E_PIPE) {
        return ans;
    }
    let bit = 1u64 << (SIGPIPE - 1);
    if sig.dispo[SIGPIPE as usize] == DISPO_IGN {
        return ans; /* ignored: EPIPE is the whole observable result */
    }
    if sig.mask & bit != 0 {
        sig.pending |= bit; /* blocked: arrive at the next unblock */
        return ans;
    }
    if sig.dispo[SIGPIPE as usize] == DISPO_HANDLER {
        let mut d = deliver_handler(sig, SIGPIPE, 0);
        d.sysret = (-LINUX_EPIPE) as u64;
        return Answer::SignalHandler(d);
    }
    match default_action(SIGPIPE) {
        ACT_TERMINATE => Answer::SignalKill(SIGPIPE),
        _ => ans, /* SIGPIPE has no ignore/stop default */
    }
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
    /* writev's gather is already write's payload and write's count word by
     * the time it reaches this arm (the kernel rewrote args[2]), so the
     * number is normalized here and the SAME console/E_BADF arm answers
     * both. The guard's M5 literal stays the one descriptor rule. */
    let num = if num == LINUX_WRITEV { LINUX_WRITE } else { num };
    match num {
        /* set_tid_address (218, LSC_LOCAL): single-threaded — the "address
         * of the cleared TID" can never be written, so the complete and
         * truthful answer is the address itself (§5.6: stored, ignored).
         * E7 increment 10: that echo IS this shim's tid — musl stores the
         * return into pt->tid as an int32 and tkill hands its low 32 bits
         * back — so the answer is also RECORDED here: the seam's whole
         * notion of "this task" for tkill's match (SigState's note). */
        /* M4.5: the real identities the kernel assigned. getpid is the
         * leader's TID (TGID); gettid is this thread's own TID. Neither is
         * the set_tid_address pointer. */
        LINUX_GETPID => Answer::Reply(sig.tgid, STATUS_OK),
        LINUX_GETTID => Answer::Reply(sig.tid, STATUS_OK),
        218 => {
            /* set_tid_address stores the user word; Linux returns the
             * caller's TID, and the kernel records the same word so the
             * exit path can zero it. */
            sig.clear_tid = args[0];
            Answer::Reply(sig.tid, STATUS_OK)
        }
        /* clock_gettime (228, LSC_LOCAL — E7 increment 19): the seam has
         * no vDSO to backstop it, so the sidecar answers the call itself.
         * CLOCK_REALTIME is the pinned gate epoch; CLOCK_MONOTONIC starts
         * at zero (the workload only needs a readable clock, not an
         * advancing one). Other clock ids are EINVAL, Linux's own answer
         * for a clock this shim does not carry. The kernel stages the
         * 16-byte destination and refuses a NULL/foreign span before any
         * post; args[1] == 0 here is unreachable, but answering EINVAL
         * (rather than fabricating a success) keeps the twin honest. */
        LINUX_CLOCK_GETTIME => {
            if args[1] == 0 {
                return Answer::Reply(0, E_INVAL);
            }
            let (sec, nsec): (u64, u64) = match args[0] {
                0 => (GATE_EPOCH_SECS, 0), /* CLOCK_REALTIME  */
                1 => (0, 0),               /* CLOCK_MONOTONIC */
                _ => return Answer::Reply(0, E_INVAL),
            };
            let mut b = [0u8; FD_PAYLOAD_CAP];
            b[0..8].copy_from_slice(&sec.to_le_bytes());
            b[8..16].copy_from_slice(&nsec.to_le_bytes());
            Answer::ReplyBytes(0, TIMESPEC_BYTES, b)
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
         * timeout 0 — the probe — answers 0 EVENTS: this shim can read
         * buffered console input but has no waiting/poll readiness state
         * (§10.13's named input class), so "nothing ready" is the whole
         * truth it can tell, and
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
         * waiting needs a wake against input readiness state that does not exist,
         * and answering 0 to a 5000 ms poll would make the caller spin
         * its five seconds inside its own loop on a lie. */
        /* poll (7) is no longer answered here: E7 increment 24 routed it
         * through answer_file, where the fd table and the console live. A
         * poll that somehow reached this pure arm without a task's table
         * falls to the default ENOSYS rather than inventing readiness. */
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
            let (addr, len, prot, flags, fd, foff) =
                (args[0], args[1], args[2], args[3], args[4], args[5]);
            let len_r = match len.checked_add(0xFFF) {
                Some(v) if len != 0 => v & !0xFFFu64,
                _ => return Answer::Reply(0, E_INVAL),
            };
            if prot & !PROT_MASK != 0 {
                Answer::Reply(0, E_INVAL)
            } else if prot & PROT_WRITE != 0 && prot & PROT_EXEC != 0 {
                Answer::Reply(0, E_PERM)
            } else if flags & MAP_PRIVATE == 0 || flags & !MAP_ALLOW != 0 {
                Answer::Reply(0, E_NOSYS)
            } else if flags & MAP_ANONYMOUS == 0 {
                /* M2b, the file-backed half: the loader's DT_NEEDED maps
                 * — MAP_PRIVATE, PROT_READ (the header/segment read),
                 * PROT_READ|PROT_WRITE (.data) or PROT_READ|PROT_EXEC
                 * (.text), a VFS fd and a page-aligned offset. The same ladder as the
                 * kernel's first trap (neither end load-bearing alone);
                 * the FILE's truth is the VFS's: a non-file or unreadable
                 * slot answers its own EBADF from `read_at`, which
                 * `service` turns into a refusal reply with no chunks
                 * sent. Write file maps stay ENOSYS by name; W|X is
                 * EPERM above, so an RX map can never become writable. */
                if prot != 0x1 && prot != 0x3 && prot != 0x5 {
                    Answer::Reply(0, E_NOSYS)
                } else if foff & 0xFFF != 0 {
                    Answer::Reply(0, E_INVAL)
                } else if len > LINUX_MMAP_FILE_MAX {
                    Answer::Reply(0, E_NOSYS)
                } else if fd > 0x7FFF_FFFF {
                    Answer::Reply(0, E_BADF)
                } else {
                    match fd_slot(fds, fd).copied().ok_or(E_BADF) {
                        Ok(s) if s.kind == FD_KIND_FILE => {
                            if flags & MAP_FIXED != 0 {
                                if addr & 0xFFF != 0 {
                                    Answer::Reply(0, E_INVAL)
                                } else if addr < brk.base || addr > brk.limit
                                       || len_r > brk.limit - addr {
                                    Answer::Reply(0, E_NOMEM)
                                } else {
                                    Answer::MmapFile(addr, s.vfs_fd, foff,
                                                     len_r as u32)
                                }
                            } else {
                                match brk.mtop.checked_sub(len_r) {
                                    Some(top) if top >= brk.cur => {
                                        brk.mtop = top;
                                        Answer::MmapFile(top, s.vfs_fd, foff,
                                                         len_r as u32)
                                    }
                                    _ => Answer::Reply(0, E_NOMEM),
                                }
                            }
                        }
                        Ok(_) => Answer::Reply(0, E_NOSYS),
                        Err(x) => Answer::Reply(0, x),
                    }
                }
            } else if prot & PROT_EXEC != 0 {
                /* an ANONYMOUS executable map stays the named v1 gap */
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
        /* munmap (11, LSC_MEM — E7 increment 25, §5.4's last memory
         * seam): the sidecar answers WHETHER, and its ladder is the
         * kernel's first-trap refusal repeated as this side's own twin
         * (guard clause X pins both). The kernel's page tables are the
         * truth about which pages are PRESENT, so a double unmap is
         * success; no mapping ledger is kept here. v1's policy: a
         * page-aligned, nonzero, non-overflowing range inside the
         * stamped span and at or above the current break — the break's
         * pages belong to brk. Either cursor moving would be a
         * re-hand, so neither does. */
        11 => {
            let (addr, len) = (args[0], args[1]);
            let len_r = match len.checked_add(0xFFF) {
                Some(v) if len != 0 => v & !0xFFFu64,
                _ => return Answer::Reply(0, E_INVAL),
            };
            if addr & 0xFFF != 0 {
                Answer::Reply(0, E_INVAL)
            } else if addr < brk.base || addr > brk.limit
                   || len_r > brk.limit - addr {
                Answer::Reply(0, E_INVAL)
            } else if addr < brk.cur {
                Answer::Reply(0, E_INVAL)
            } else {
                Answer::Reply(0, STATUS_OK)
            }
        }
        /* ioctl (16, the stdio path — M2b): musl's __stdout_write asks
         * TIOCGWINSZ on stdout before its first write, and its isatty
         * asks TCGETS. fds 0..2 are the task's console, so exactly the
         * console answers 0 for exactly those two requests; the kernel
         * writes nothing back, which is enough because both callers only
         * check the return. A valid non-console fd is a VFS file the VFS
         * does not serve ioctl for — EINVAL, not ENOSYS (the number is
         * served now). An unknown slot is EBADF. */
        16 => {
            let fd = args[0];
            match fd_slot(fds, fd).copied() {
                None => Answer::Reply(0, E_BADF),
                Some(s) if s.kind == FD_KIND_CONSOLE
                        && (args[1] == LINUX_TIOCGWINSZ
                            || args[1] == LINUX_TCGETS) =>
                    Answer::Reply(0, STATUS_OK),
                Some(_) => Answer::Reply(0, E_INVAL),
            }
        }
        /* mprotect (10, LSC_MEM — §5.4's W^X and the guard-page seam):
         * the word rules are this side's twin of the kernel's first trap.
         * Simultaneous write+execute is EPERM; executable-only remains
         * ENOSYS by name (the same v1 boundary as mmap); unknown prot
         * bits, a zero/overflowing length or an unaligned address are
         * EINVAL. A valid request
         * answers success and lets the kernel change the PRESENT leaf:
         * PROT_NONE parks the frame (present cleared), READ/WRITE restore
         * or replace the PTE flags on that same frame.
         * The kernel owns page presence, so this arm never fabricates a
         * grant for a page the page tables do not carry. */
        10 => {
            let (addr, len, prot) = (args[0], args[1], args[2]);
            let len_r = match len.checked_add(0xFFF) {
                Some(v) if len != 0 => v & !0xFFFu64,
                _ => return Answer::Reply(0, E_INVAL),
            };
            /* M2b: the kernel's own span is the heap stamp united with
             * the loaded image (the loader RELROs its image at
             * LINUX_INTERP_BASE and the program's at its linked
             * address, both outside the brk span). Mirror exactly that
             * arithmetic — min(base) and max(limit), image live when
             * image_lo is nonzero — so the twin refuses the same
             * addresses the kernel's first trap does. */
            let mut span_base = brk.base;
            let mut span_limit = brk.limit;
            if brk.image_lo != 0 && brk.image_lo < span_base {
                span_base = brk.image_lo;
            }
            if brk.image_hi > span_limit {
                span_limit = brk.image_hi;
            }
            if prot & !0x7 != 0 {
                Answer::Reply(0, E_INVAL)
            } else if args[2] & 0x2 != 0 && args[2] & 0x4 != 0 {
                Answer::Reply(0, E_PERM)
            } else if prot & 0x4 != 0 {
                Answer::Reply(0, E_NOSYS)
            } else if addr & 0xFFF != 0 {
                Answer::Reply(0, E_INVAL)
            } else if addr < span_base || addr > span_limit
                   || len_r > span_limit - addr {
                /* the ledger's span is this side's twin of the kernel's
                 * first-trap range check: a page outside it was never a
                 * mapping this shim granted, so no leaf may be retargeted
                 * by a success reply for it. */
                Answer::Reply(0, E_INVAL)
            } else {
                Answer::Reply(0, STATUS_OK)
            }
        }
        /* exit_group (231, LSC_EXIT): the sidecar reaps, the frames return
         * (§5.6) — handled by service(), which owns the pid lookup. */
        LINUX_EXIT => Answer::ExitThread,
        LINUX_EXIT_GROUP => Answer::ExitGroup,
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
         * M3 records a real handler too. The ordinary sa_handler form is
         * extended by SA_SIGINFO (siginfo/ucontext), SA_ONSTACK (the
         * recorded alternate stack) and SA_RESETHAND (reset before entry);
         * each is accepted only when the kernel's matching frame shape is
         * present. sa_mask and SA_NODEFER are honoured by the mask the
         * handler runs under. */
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
                        sig.act[args[0] as usize] = SigAct::new();
                        sig_ok(args, old)
                    }
                    1 => {
                        sig.dispo[args[0] as usize] = DISPO_IGN;
                        sig.act[args[0] as usize] = SigAct::new();
                        sig_ok(args, old)
                    }
                    handler => {
                        let flags = read_u64(payload, 8);
                        let restorer = read_u64(payload, 16);
                        let mask = read_u64(payload, 24);
                        if flags & !SA_SUPPORTED != 0 {
                            /* A flag outside the handler convention this
                             * seam can honour (SA_RESTART and the
                             * process-wide child flags included) is
                             * refused by name before mutation. */
                            Answer::Reply(0, E_NOSYS)
                        } else if restorer == 0 ||
                                  flags & SA_RESTORER == 0 {
                            /* x86-64 handlers return through a restorer
                             * that invokes rt_sigreturn; without one the
                             * handler has no return path. */
                            Answer::Reply(0, E_NOSYS)
                        } else {
                            let i = args[0] as usize;
                            sig.dispo[i] = DISPO_HANDLER;
                            sig.act[i] = SigAct { handler, flags, restorer, mask };
                            sig_ok(args, old)
                        }
                    }
                }
            }
        }
        /* sigaltstack (131, LSC_SIG — M3.2): the alternate-stack record.
         * `ss` NULL is a query; otherwise the 24-byte Linux stack_t rides
         * behind the request. A query returns the current record through
         * args[1]'s staged out-pointer; a successful set returns the OLD
         * record the same way. SS_ONSTACK is a query-only flag and changing
         * the stack while a handler is on it is EPERM, matching Linux. */
        LINUX_SIGALTSTACK => {
            let old_bytes = stack_t_payload(
                sig.alt_sp,
                if sig.alt_depth != 0 { SS_ONSTACK } else { sig.alt_flags },
                sig.alt_size);
            if args[0] == 0 {
                if args[1] != 0 {
                    Answer::ReplyBytes(0, STACK_T_BYTES as usize, old_bytes)
                } else {
                    Answer::Reply(0, STATUS_OK)
                }
            } else if payload.len() < STACK_T_BYTES {
                Answer::Reply(0, E_NOSYS)
            } else {
                let (sp, flags, size) = read_stack_t(payload);
                if flags & !(SS_ONSTACK | SS_DISABLE) != 0 ||
                    flags & SS_ONSTACK != 0 {
                    Answer::Reply(0, E_INVAL)
                } else if sig.alt_depth != 0 {
                    Answer::Reply(0, E_PERM)
                } else if flags & SS_DISABLE == 0 &&
                          (sp == 0 || size < MINSIGSTKSZ ||
                           sp.checked_add(size).is_none() ||
                           sp + size - 1 > 0x0000_7fff_ffff_ffff) {
                    Answer::Reply(0, E_INVAL)
                } else {
                    if flags & SS_DISABLE != 0 {
                        sig.alt_sp = 0;
                        sig.alt_size = 0;
                        sig.alt_flags = SS_DISABLE;
                    } else {
                        sig.alt_sp = sp;
                        sig.alt_size = size;
                        sig.alt_flags = 0;
                    }
                    if args[1] != 0 {
                        Answer::ReplyBytes(0, STACK_T_BYTES as usize, old_bytes)
                    } else {
                        Answer::Reply(0, STATUS_OK)
                    }
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
                        } & !SIG_UNBLOCKABLE;
                        /* Pass 1: what does the new mask REVEAL? Discards
                         * (an IGN disposition, or an ignore-by-default
                         * signal) are invisible either way; a
                         * default-terminate signal KILLS right here — no
                         * reply, this syscall never returns to its caller;
                         * a default-stop signal refuses the CALL before
                         * anything moves (not even the mask). */
                        let mut revealed = sig.pending & !new_mask;
                        let mut verdict = 0u8; /* 0 proceed, 1 kill, 2 refuse,
                                                * 3 handler delivery */
                        let mut kill_sig = 0u64; /* WHICH revealed signal earns
                                                  * the kill — carried on the
                                                  * Answer so the line names it
                                                  * (increment 14) */
                        let mut deliver_sig = 0u64;
                        while revealed != 0 && verdict == 0 {
                            let bit = revealed.trailing_zeros();
                            revealed &= !(1u64 << bit);
                            let sig_no = bit + 1;
                            if sig.dispo[sig_no as usize] == DISPO_IGN
                                || default_action(sig_no as u64) == ACT_IGNORE
                            {
                                continue; /* discard — its delivery is a no-op */
                            } else if sig.dispo[sig_no as usize] == DISPO_HANDLER {
                                /* The handler's mask will block this signal
                                 * again; the kernel frame carries the
                                 * post-unblock mask, so rt_sigreturn restores
                                 * exactly the completed call's state. */
                                verdict = 3;
                                deliver_sig = sig_no as u64;
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
                            3 => {
                                sig.pending &= !(1u64 << (deliver_sig - 1));
                                sig.mask = new_mask;
                                Answer::SignalHandler(deliver_handler(
                                    sig, deliver_sig, old))
                            }
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
        /* rt_sigreturn (15, LSC_SIG — M3): the restorer's syscall after a
         * handler returns. The KERNEL owns the frame and restores every
         * saved register; this arm restores the one semantic piece the
         * kernel does not own: the signal mask. args[0] is the frame's
         * saved mask, so the state after return is exactly the mask that
         * was in force when the handler was entered. */
        LINUX_RT_SIGRETURN => restore_and_redeliver(sig, args[0]),
        /* M4.8: set_robust_list(273). The kernel walker owns the actual
         * exit-time owner-died write; this side records the head and the
         * size so every teardown path can name it. get_robust_list(274)
         * would need a sibling's out-pointer context, so it stays a named
         * ENOSYS rather than a fabricated self-only answer. */
        LINUX_SET_ROBUST_LIST => {
            if args[1] != LINUX_ROBUST_LIST_HEAD_BYTES || args[0] == 0 {
                Answer::Reply(0, E_INVAL)
            } else {
                sig.robust_head = args[0];
                sig.robust_len = args[1];
                Answer::Reply(0, STATUS_OK)
            }
        }
        LINUX_GET_ROBUST_LIST => Answer::Reply(0, E_NOSYS),
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
            } else if sig.dispo[args[1] as usize] == DISPO_HANDLER {
                Answer::SignalHandler(deliver_handler(sig, args[1], 0))
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

/// M4.5: tkill addressed to a SIBLING by its real TID. Linux signal
/// targeting is per-thread, so a TID must resolve inside this group (a TID
/// in another group is ESRCH, not a cross-group delivery). This path is
/// deliberately narrow: existence, ignored dispositions and blocked signals
/// are exact; a handler or default action for another thread would need a
/// frame in that thread's context, which this v1 cannot build. Those answer
/// ENOSYS, named—never a fabricated delivery. The caller's own TID keeps the
/// existing answer() path and its handler/frame semantics.
fn answer_tkill_sibling(gid: usize, self_tid: u64,
                        args: &[u64; 6]) -> Option<Answer> {
    if args[0] == 0 || args[0] == self_tid {
        return None;
    }
    let target = args[0] as u32;
    let found = task_by_tid(gid, target);
    let Some(j) = found else {
        return Some(Answer::Reply(0, E_TARGET));
    };
    Some(signal_task(gid, j, args[1]))
}

fn task_by_tid(gid: usize, tid: u32) -> Option<usize> {
    if tid == 0 { return None; }
    for j in 0..MAX_LINUX_TASKS {
        if tasks()[j].used && tasks()[j].group == gid &&
            tasks()[j].sig.tid as u32 == tid {
            return Some(j);
        }
    }
    None
}

/// M4.8: deliver to a SIBLING task. Ignored dispositions and blocked signals
/// are exact; a recorded handler or a stop default still needs a frame in the
/// sibling's context, which this v1 cannot build, so those are named ENOSYS.
/// A terminate default is the one action whose context is not the sibling's:
/// the kernel tears it down and the sender gets Linux's 0.
fn signal_task(gid: usize, j: usize, sig: u64) -> Answer {
    if sig == 0 {
        return Answer::Reply(0, STATUS_OK);
    }
    if sig > SIG_MAX as u64 {
        return Answer::Reply(0, E_INVAL);
    }
    let s = sig as usize;

    /* SIGCONT is the resume edge, not a normal disposition decision. A
     * stopped parked sibling is released here; its interrupted syscall
     * gets -EINTR from `service`, exactly as Linux resumes a stopped
     * thread at the syscall boundary. */
    if sig == SIGCONT {
        if tasks()[j].stopped {
            tasks()[j].stopped = false;
            return Answer::SignalContinue(SignalContinue {
                n: 1,
                tasks: [j as u8; MAX_LINUX_TASKS],
            });
        }
        return Answer::Reply(0, STATUS_OK);
    }

    if groups()[gid].sig_dispo[s] == DISPO_IGN {
        return Answer::Reply(0, STATUS_OK);
    }
    let t = &mut tasks()[j];
    if t.sig.mask & (1u64 << (s - 1)) != 0 {
        t.sig.pending |= 1u64 << (s - 1);
        return Answer::Reply(0, STATUS_OK);
    }
    if groups()[gid].sig_dispo[s] == DISPO_HANDLER {
        /* A stopped target must stay stopped until SIGCONT; piling a
         * handler onto it now would run during the stop. A parked target
         * carries the trailer on its own reply (M4.10); a sibling between
         * syscalls has no reply, so M4.11 asks the kernel to install the
         * frame into its saved ring3 context. */
        if t.stopped {
            return Answer::Reply(0, E_NOSYS);
        }
        let parked = t.pending != PENDING_NONE;
        t.sig.dispo = groups()[gid].sig_dispo;
        t.sig.act = groups()[gid].sig_act;
        let mut d = deliver_handler(&mut t.sig, sig, 0);
        if parked {
            d.sysret = (-LINUX_EINTR) as u64;
        }
        /* SA_RESETHAND is group-wide: the sibling's delivery must reset
         * the shared action too, or a later thread would see the handler
         * that this delivery already consumed. */
        groups()[gid].sig_dispo[s] = t.sig.dispo[s];
        groups()[gid].sig_act[s] = t.sig.act[s];
        if parked {
            futex_remove(&mut groups()[gid], j);
            return Answer::SignalHandlerTask(t.sig.tid as u32, d);
        }
        return Answer::SignalHandlerRun(t.sig.tid as u32, d);
    }
    match default_action(sig) {
        ACT_TERMINATE => Answer::SignalKillTask(t.sig.tid as u32, sig),
        ACT_IGNORE => Answer::Reply(0, STATUS_OK),
        ACT_STOP => {
            if t.pending == PENDING_NONE || t.stopped {
                return Answer::Reply(0, E_NOSYS);
            }
            t.stopped = true;
            futex_remove(&mut groups()[gid], j);
            Answer::SignalStopTask(t.sig.tid as u32, sig)
        }
        _ => Answer::Reply(0, E_NOSYS),
    }
}

/// M4.10: choose a parked sibling for a process-directed signal. A handler
/// prefers a sibling that does not block it; a blocked-only set still pends
/// on the first parked sibling (Linux's process-directed rule). The caller
/// is skipped so the new path is a SIBLING delivery, not a second spelling
/// of tkill-to-self; the existing self path remains `answer()`.
fn parked_sibling(gid: usize, caller: usize, sig: u64) -> Option<usize> {
    let bit = if sig >= 1 && sig <= SIG_MAX as u64 {
        1u64 << (sig - 1)
    } else {
        0
    };
    let mut fallback = None;
    for j in 0..MAX_LINUX_TASKS {
        if j == caller || !tasks()[j].used || tasks()[j].group != gid ||
            tasks()[j].stopped || tasks()[j].pending == PENDING_NONE {
            continue;
        }
        if bit == 0 || tasks()[j].sig.mask & bit == 0 {
            return Some(j);
        }
        if fallback.is_none() {
            fallback = Some(j);
        }
    }
    fallback
}

/// M4.11: a process-directed handler's fallback when no sibling is parked.
/// A target between syscalls has no reply, but the kernel can install the
/// frame into its saved ring3 context. Stopped targets stay untouched until
/// SIGCONT, and a blocked-but-not-sidecar-parked target is still selected:
/// the service's kernel retry is what decides whether it can take the frame.
fn running_sibling(gid: usize, caller: usize, sig: u64) -> Option<usize> {
    let bit = if sig >= 1 && sig <= SIG_MAX as u64 {
        1u64 << (sig - 1)
    } else {
        0
    };
    let mut fallback = None;
    for j in 0..MAX_LINUX_TASKS {
        if j == caller || !tasks()[j].used || tasks()[j].group != gid ||
            tasks()[j].stopped || tasks()[j].pending != PENDING_NONE {
            continue;
        }
        if bit == 0 || tasks()[j].sig.mask & bit == 0 {
            return Some(j);
        }
        if fallback.is_none() {
            fallback = Some(j);
        }
    }
    fallback
}

/// M4.11: process-directed stop broadcast. The caller is deliberately left
/// runnable so it can send SIGCONT; every OTHER sibling is latched. A
/// target with a pending exchange keeps that reply for SIGCONT; a target
/// between exchanges has no reply, so `service` holds its kernel task and
/// releases it on the continue edge.
fn stop_group(gid: usize, caller: usize, sig: u64) -> Answer {
    let mut g = SignalStopGroup {
        sig, n: 0, parked: 0,
        tasks: [0; MAX_LINUX_TASKS],
        waiters: [FutexWaiter { used: false, task: 0, uaddr: 0, seq: 0,
                                bitset: 0 }; MAX_LINUX_TASKS],
    };
    for j in 0..MAX_LINUX_TASKS {
        if j == caller || !tasks()[j].used || tasks()[j].group != gid ||
            tasks()[j].stopped {
            continue;
        }
        g.tasks[g.n as usize] = j as u8;
        g.n += 1;
        if tasks()[j].pending != PENDING_NONE {
            g.parked += 1;
            g.waiters[j] = futex_take(&mut groups()[gid], j);
        }
        tasks()[j].stopped = true;
    }
    if g.n == 0 {
        Answer::Reply(0, STATUS_OK)
    } else {
        Answer::SignalStopGroup(g)
    }
}

/// M4.11: undo a broadcast that could not be committed. Every task in the
/// answer was latched by `stop_group`; release the ones the kernel already
/// held, put any removed FUTEX_WAIT back in its queue, and clear the stop
/// latch on all of them so no sibling is left half-stopped.
fn rollback_stop_group<K: Kernel>(k: &K, gid: usize,
                                  grp: &SignalStopGroup) -> u32 {
    let mut released = 0u32;
    for &idx in grp.tasks[..grp.n as usize].iter() {
        let j = idx as usize;
        if j >= MAX_LINUX_TASKS || !tasks()[j].used ||
            tasks()[j].group != gid {
            continue;
        }
        if tasks()[j].run_held {
            let _ = k.proc_release(tasks()[j].sig.tid as u32);
            tasks()[j].run_held = false;
            released += 1;
        }
        if grp.waiters[j].used {
            let _ = futex_restore(&mut groups()[gid], grp.waiters[j]);
        }
        tasks()[j].stopped = false;
    }
    released
}

/// M4.11: try to commit the hold half of a broadcast. Parked targets need
/// no hold; every other candidate is retried across yields. If any target
/// refuses, roll back the whole set before returning the error, so the
/// caller never observes a partially stopped group.
fn hold_stop_group<K: Kernel>(k: &K, gid: usize,
                              grp: &SignalStopGroup) -> Result<u32, i32> {
    let mut held = 0u32;
    for &idx in grp.tasks[..grp.n as usize].iter() {
        let j = idx as usize;
        if j >= MAX_LINUX_TASKS || !tasks()[j].used ||
            tasks()[j].group != gid || tasks()[j].pending != PENDING_NONE {
            continue;
        }
        let tid = tasks()[j].sig.tid as u32;
        let mut ok = false;
        for _ in 0..64 {
            match k.proc_hold(tid) {
                Ok(()) => { ok = true; break; }
                Err(E_AGAIN) => k.sched_yield(),
                Err(_) => break,
            }
        }
        if !ok {
            let _ = rollback_stop_group(k, gid, grp);
            return Err(E_CONFLICT);
        }
        tasks()[j].run_held = true;
        held += 1;
    }
    Ok(held)
}

/// M4.10: collect the group's stopped tasks for SIGCONT. A stopped task
/// with no pending exchange (a stop latched between requests) is still
/// cleared; the service arm sends -EINTR only where a reply is owed.
fn continue_group(gid: usize) -> Answer {
    let mut c = SignalContinue { n: 0, tasks: [0; MAX_LINUX_TASKS] };
    for j in 0..MAX_LINUX_TASKS {
        if tasks()[j].used && tasks()[j].group == gid && tasks()[j].stopped {
            tasks()[j].stopped = false;
            c.tasks[c.n as usize] = j as u8;
            c.n += 1;
        }
    }
    if c.n == 0 {
        Answer::Reply(0, STATUS_OK)
    } else {
        Answer::SignalContinue(c)
    }
}

/// M4.8: tgkill(234) names both the group and the target TID. Self keeps the
/// caller's own answer() path (handlers/frames are legal there); a sibling
/// goes through signal_task.
fn answer_tgkill(gid: usize, self_tid: u64,
                 args: &[u64; 6]) -> Option<Answer> {
    if args[0] == 0 || args[0] != groups()[gid].tgid {
        return Some(Answer::Reply(0, E_TARGET));
    }
    if args[1] == 0 {
        return Some(Answer::Reply(0, E_TARGET));
    }
    if args[1] == self_tid {
        return None;
    }
    let Some(j) = task_by_tid(gid, args[1] as u32) else {
        return Some(Answer::Reply(0, E_TARGET));
    };
    Some(signal_task(gid, j, args[2]))
}

/// M4.8: kill(62). A TID names one task (self or a sibling); the TGID names
/// the whole group. process-group id 0/-1 and cross-group targets are refused
/// by name: this v1 has no process-group namespace and never retargets a
/// signal outside the caller's own group.
fn answer_kill(gid: usize, caller: usize, self_tid: u64,
               args: &[u64; 6]) -> Option<Answer> {
    let pid = args[0];
    if pid == 0 || pid > u32::MAX as u64 {
        return Some(Answer::Reply(0, E_NOSYS));
    }
    let target = pid as u32;
    if target == groups()[gid].tgid as u32 {
        return Some(answer_group_signal(gid, caller, args[1]));
    }
    if target == self_tid as u32 {
        return None;
    }
    let Some(j) = task_by_tid(gid, target) else {
        return Some(Answer::Reply(0, E_TARGET));
    };
    Some(signal_task(gid, j, args[1]))
}

/// M4.10: a group-directed signal. IGN is a 0 reply; TERMINATE folds into
/// the existing exit_group sweep (no reply — the caller is one of the
/// dead); a HANDLER or STOP is delivered to a parked SIBLING through that
/// sibling's own deferred reply. The caller is left running so it can
/// observe the result; full POSIX group-stop broadcast is not claimed.
fn answer_group_signal(gid: usize, caller: usize, sig: u64) -> Answer {
    if sig == 0 { return Answer::Reply(0, STATUS_OK); }
    if sig > SIG_MAX as u64 { return Answer::Reply(0, E_INVAL); }
    if sig == SIGCONT {
        return continue_group(gid);
    }
    let s = sig as usize;
    if groups()[gid].sig_dispo[s] == DISPO_IGN {
        return Answer::Reply(0, STATUS_OK);
    }
    match groups()[gid].sig_dispo[s] {
        DISPO_HANDLER => match parked_sibling(gid, caller, sig)
            .or_else(|| running_sibling(gid, caller, sig)) {
            Some(j) => signal_task(gid, j, sig),
            None => Answer::Reply(0, E_NOSYS),
        },
        _ => match default_action(sig) {
            ACT_TERMINATE => Answer::SignalGroupKill(sig),
            ACT_IGNORE => Answer::Reply(0, STATUS_OK),
            ACT_STOP => stop_group(gid, caller, sig),
            _ => Answer::Reply(0, E_NOSYS),
        },
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
    /// E7 step 4: for the File kind, the VFS fd this descriptor names
    /// (the VFS table is the task model's own — one VFS task per Linux
    /// task, allocated at spawn). M1: for the Socket kind, the P2
    /// driver's socket id from SocketOps — the same backing-id role, one
    /// table over both object families. Console slots leave it 0.
    vfs_fd: u32,
    /// The LINUX open flags this descriptor answers F_GETFL with, and
    /// whose O_CLOEXEC bit is F_GETFD/F_SETFD's own state (the VFS's
    /// u16 flag word cannot hold either — that widening IS this
    /// increment, justified by the census's three measured opens, all
    /// carrying O_CLOEXEC and one carrying O_DIRECTORY).
    flags: u32,
}

const FD_KIND_CONSOLE: u8 = 0;
const FD_KIND_FILE: u8 = 1;
const FD_KIND_SOCKET: u8 = 2;
const LINUX_MAX_FDS: usize = 16;

/// Linux's own open-flag numbers as x86-64 musl hands them (measured by
/// the candidate's three opens under qemu-x86_64 -strace — the flag
/// contract is the trace, §5.6): O_LARGEFILE rides every musl open and
/// is a NO-OP on 64-bit (accepted, ignored — named); O_DIRECTORY and
/// O_CLOEXEC select the dir-open arm and the cloexec bit; everything
/// NOT in this mask is refused by NAME at open (ENOSYS), never silently
/// dropped — the trigger the design's flag table named ("widen the flag
/// word") is consumed right here, by the measurement.
const LO_LARGEFILE: u32 = 0x8000;
const LO_DIRECTORY: u32 = 0x10000;
const LO_CLOEXEC: u32 = 0x80000;
const LO_KNOWN: u32 = 0x3 | 0x40 | 0x80 | 0x200 | 0x400
    | LO_LARGEFILE | LO_DIRECTORY | LO_CLOEXEC;

/// fcntl's four answered commands (the census's fcntl rows are all
/// F_SETFD(1); F_GETFD/F_GETFL round out the four the design names).
/// Every other command — F_DUPFD included — is ENOSYS by name: a
/// fabricated answer to a command nobody measured is the placeholder
/// failure in a new shape.
const F_SETFD: u64 = 2;
const F_GETFD: u64 = 1;
const F_GETFL: u64 = 3;
const F_SETFL: u64 = 4;

/// Resolve a descriptor against a task's table — pure over the slice, so
/// every branch (slot present, slot empty, descriptor out of range) is
/// guard clause M4's to hold rather than an observation from one boot.
fn fd_kind(fds: &[FdSlot], fd: u64) -> Option<u8> {
    fd_slot(fds, fd).map(|s| s.kind)
}

fn fd_slot(fds: &[FdSlot], fd: u64) -> Option<&FdSlot> {
    if fd >= LINUX_MAX_FDS as u64 {
        return None;
    }
    let s = &fds[fd as usize];
    if s.used { Some(s) } else { None }
}

/// Mint the lowest free Linux fd for a VFS fd (open's own allocation
/// rule), refusing the table-full shape as EBADF-family: Linux would
/// say EMFILE, which this seam maps through cap.h's ETABLEFULL (-4 →
/// ENFILE — the fixed-table honesty cap.h itself notes).
fn alloc_fd(fds: &mut [FdSlot], kind: u8, vfs_fd: u32, flags: u32) -> Result<u64, i32> {
    for (i, s) in fds.iter_mut().enumerate() {
        if !s.used {
            *s = FdSlot { used: true, kind, vfs_fd, flags };
            return Ok(i as u64);
        }
    }
    Err(E_TABLEFULL)
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
        LINUX_WRITE | LINUX_WRITEV | LINUX_SENDTO =>
            LINUX_SYSCALL_REQ_BYTES + args[2] as usize,
        /* M1: connect/bind's one attached shape is the 16-byte sockaddr. */
        LINUX_CONNECT | LINUX_BIND =>
            LINUX_SYSCALL_REQ_BYTES + LINUX_SOCKADDR_IN_BYTES,
        /* poll: the caller's whole array rides behind the header. An nfds
         * above the cap is the kernel's first-trap EINVAL; this side keeps
         * the arithmetic total by expecting the plain header for it (the
         * arm then refuses the shape EINVAL rather than reading bytes it
         * did not expect). */
        LINUX_POLL if args[1] <= LINUX_POLL_MAX_FDS as u64 =>
            LINUX_SYSCALL_REQ_BYTES + args[1] as usize * LINUX_POLLFD_BYTES,
        LINUX_RT_SIGACTION if args[1] != 0 => LINUX_SYSCALL_REQ_BYTES + SIGACT_BYTES,
        LINUX_RT_SIGPROCMASK if args[1] != 0 => LINUX_SYSCALL_REQ_BYTES + SIGSET_BYTES as usize,
        LINUX_SIGALTSTACK if args[0] != 0 => LINUX_SYSCALL_REQ_BYTES + STACK_T_BYTES,
        /* M4.4: only the private WAIT shape carries the 24-byte observed/
         * timeout record. A WAKE and every unsupported operation are the
         * plain header, which the sidecar's named refusal can answer. */
        LINUX_FUTEX
            if args[1] == (FUTEX_WAIT | FUTEX_PRIVATE) ||
               args[1] == (FUTEX_WAIT_BITSET | FUTEX_PRIVATE) =>
            LINUX_SYSCALL_REQ_BYTES + FUTEX_WAIT_PAYLOAD_BYTES,
        LINUX_FUTEX if args[1] == (FUTEX_CMP_REQUEUE | FUTEX_PRIVATE) =>
            LINUX_SYSCALL_REQ_BYTES + FUTEX_REQUEUE_PAYLOAD_BYTES,
        LINUX_CLONE3 => LINUX_SYSCALL_REQ_BYTES + LINUX_CLONE_ARGS_BYTES,
        /* M5.2: CAT_EXEC's SQL rides behind the header; args[1] is its
         * length. The range below refuses zero and anything over the
         * service's own cap before the arm runs. */
        SYS_SLS_CATALOG_EXEC => LINUX_SYSCALL_REQ_BYTES + args[1] as usize,
        _ => LINUX_SYSCALL_REQ_BYTES,
    }
}

/// The message-length RANGE one request may arrive with (step 4): the
/// fixed numbers keep their one length (expected_req_len, still the
/// kernel plan's twin); the path numbers carry a string's own length
/// behind the header — 1 byte (the empty string's NUL) up to the cap
/// (both caps for rename) — mirroring the kernel's path plan bounds.
/// A length outside the range is the malformed arm (num=0 → -EPROTO),
/// so an attach that drifted from the plan is loud at BOTH ends.
fn expected_req_range(num: u64, args: &[u64; 6]) -> (usize, usize) {
    const H: usize = LINUX_SYSCALL_REQ_BYTES;
    match num {
        LINUX_OPEN | LINUX_STAT | LINUX_LSTAT | LINUX_UNLINK | LINUX_MKDIR =>
            (H + 1, H + LINUX_PATH_CAP + 1),
        LINUX_RENAME => (H + 2, H + RECV_ATTACH_CAP),
        SYS_SLS_CATALOG_EXEC => (H + 1, H + CAT_SQL_MAX),
        _ => (expected_req_len(num, args), expected_req_len(num, args)),
    }
}

/// The per-number bound over a REPLY payload (the twin of the kernel's
/// reply-plan stages): the sig class owes exactly its struct's size
/// when an out-pointer was passed, stat owes exactly the compact Stat,
/// read owes at most its own count (count-truth: the payload IS the
/// byte count, so `ret == len` is the deliver's shape rule), getdents64
/// owes at most the name-list cap. A payload the bound refuses never
/// crosses (service answers num=0 → -EPROTO) — the deliver's staged-len
/// check is the same rule on the other end; neither is load-bearing
/// alone.
fn payload_bound(num: u64, args: &[u64; 6], len: usize) -> bool {
    match num {
        LINUX_RT_SIGACTION => args[2] != 0 && len == SIGACT_BYTES,
        LINUX_RT_SIGPROCMASK => args[2] != 0 && len == SIGSET_BYTES as usize,
        LINUX_SIGALTSTACK => args[1] != 0 && len == STACK_T_BYTES,
        LINUX_STAT | LINUX_LSTAT | LINUX_FSTAT =>
            args[1] != 0 && len == STAT_PAYLOAD_BYTES,
        LINUX_READ => args[1] != 0 && len <= args[2] as usize
            && len <= FD_PAYLOAD_CAP,
        LINUX_GETDENTS64 => args[1] != 0 && len <= DENTS_PAYLOAD_CAP,
        /* M2b: a fill round is count-truth and exactly the round the
         * kernel asked for; the RAW deliver copies it into the mapped
         * page. */
        LINUX_MMAP_FILL => len == args[2] as usize
            && len <= LINUX_MMAP_FILL_CHUNK as usize
            && len <= FD_FILL_CAP,
        LINUX_CLOCK_GETTIME => args[1] != 0 && len == TIMESPEC_BYTES,
        /* M1: accept's reply is the fixed 20-byte address trailer when
         * the caller asked for an address; recvfrom's is its data plus
         * that trailer, and the data count is the return value (the
         * kernel's shape rule checks ret == len - 20). */
        LINUX_ACCEPT => args[1] != 0 && args[2] != 0 &&
            len == LINUX_NET_ADDR_BYTES,
        LINUX_RECVFROM => args[1] != 0 && args[2] != 0 &&
            len >= LINUX_NET_ADDR_BYTES &&
            len - LINUX_NET_ADDR_BYTES <=
                (args[2] as usize).min(NET_RECV_MAX),
        /* poll: the full array comes back so the kernel can rewrite the
         * caller's revents in place; a partial array would leave the tail
         * carrying pre-call garbage. */
        LINUX_POLL => args[1] != 0 && args[1] <= LINUX_POLL_MAX_FDS as u64
            && len == args[1] as usize * LINUX_POLLFD_BYTES,
        _ => false,
    }
}

/// E7 step 4 (the file class): the thirteen arms, each a TRANSLATION
/// over the VFS — flag words into vfs flags, vfs errnos into CAP codes
/// (cap_of), vfs results into the reply's own shape. `fds` is the
/// caller's local copy of the task's table, written back by service
/// like brk's ledger; `vt` is this task's VFS task. The console kind
/// keeps its rule: write performs on the handle (service's own arm);
/// read drains whatever the entry loop has already pushed into the
/// console, and answers EAGAIN when the buffer is empty. Waiting for
/// input is still the named input-class remainder (§10.13): this arm
/// never fabricates readiness or holds the sidecar's single service
/// thread on a future byte.
fn answer_file<K: Kernel, A: BufferAlloc>(
    num: u64,
    args: &[u64; 6],
    fds: &mut [FdSlot],
    vt: u32,
    vfs: &mut Vfs<K, A>,
    net: &mut Option<&mut Box<dyn SocketOps>>,
    payload: &[u8],
    console: &CharNode,
    now_ns: u64,  /* increment 24: the monotonic clock a poll deadline
                   * is measured from (the KERNEL's SYS_SLS_NOW_NS; a fake
                   * kernel's 0 simply never expires a finite wait) */
    gate: bool,   /* increment 21: this is the gate profile's first user */
) -> Answer {
    /* The descriptor each fd-numbered arm looks up once: EBADF by the
     * slot rule (guard M4's twin over the new table shape). The COPY
     * matters — FdSlot is Copy, so an arm that flips a flag bit after
     * the lookup (F_SETFD) mutates the table without a borrow of it
     * still live. */
    fn slot_of(fds: &[FdSlot], fd: u64) -> Result<FdSlot, i32> {
        fd_slot(fds, fd).copied().ok_or(E_BADF)
    }
    match num {
        /* poll (7, LSC_FD — E7 increment 24, the waiting half): the
         * KERNEL copied the caller's pollfd array in (linux_fd_write_plan)
         * and will copy the rewritten array back into the same span. This
         * side decides readiness: the task's fd table names each
         * descriptor, the console's own read_ready() answers its slots,
         * and the VFS answers a file-backed slot with its POLLIN/POLLOUT/
         * POLLNVAL word. A ready descriptor or a zero timeout answers at
         * once; otherwise Answer::PollWait parks the array until service()
         * sees readiness or the deadline passes. Only revents is written —
         * the caller's events mask is never changed, and a negative fd is
         * ignored with revents 0 (POSIX's own rule). */
        LINUX_POLL => poll_answer(fds, net, console, payload, args, now_ns,
            |vfs_fd, events| {
                let mut pf = [PollFd { fd: vfs_fd, events, revents: 0 }];
                match vfs.poll(vt, &mut pf) {
                    Ok(_) => Some(pf[0].revents),
                    Err(_) => None,
                }
            }),
        /* write (1): the file-class half of increment 5's console write.
         * The bytes arrived behind the request (the kernel's copy-out),
         * the fd table decides the destination, and the VFS's own write
         * returns the count — count-truth, like read in the other
         * direction. A console descriptor keeps the console arm's
         * Answer::Write (service drains it to the console); every other
         * kind falls to the fd table's own refusal. */
        LINUX_WRITE | LINUX_WRITEV => match slot_of(fds, args[0]) {
            Ok(s) if s.kind == FD_KIND_CONSOLE => Answer::Write(args[2] as usize),
            Ok(s) if s.kind == FD_KIND_FILE => {
                match vfs.write(vt, s.vfs_fd, payload) {
                    Ok(n) => Answer::Reply(n as u64, STATUS_OK),
                    Err(e) => Answer::Reply(0, cap_of(e)),
                }
            }
            /* M1: write on a socket is send — the Linux fd class's one
             * table, a second backing family. */
            Ok(s) if s.kind == FD_KIND_SOCKET => match net.as_deref_mut() {
                Some(n) => match n.send(s.vfs_fd, payload) {
                    Ok(got) => Answer::Reply(got as u64, STATUS_OK),
                    Err(e) => Answer::Reply(0, net_status(e)),
                },
                None => Answer::Reply(0, E_NOSYS),
            },
            Ok(_) => Answer::Reply(0, E_NOSYS),
            Err(x) => Answer::Reply(0, x),
        },
        /* read (0): data crosses back as the reply's PAYLOAD —
         * count-truth, the twin of write's attach (the kernel stages
         * the destination at the first trap and copies only within the
         * clamp; the deliver's rax is the payload's own length). The
         * clamp rides IN the description (args[2]), so header, payload
         * and the count the task sees are one truth. A console fd reads
         * the entry loop's already-buffered input; empty input is EAGAIN
         * (or EOF once the console channel has closed), never a invented
         * zero-byte success. */
        LINUX_READ => match slot_of(fds, args[0]) {
            Ok(s) if s.kind == FD_KIND_CONSOLE =>
                console_read_answer(console, args[2] as usize),
            /* M1: read on a socket is recv; data crosses back as the
             * reply's payload, count-truth like the file arm. */
            Ok(s) if s.kind == FD_KIND_SOCKET => {
                let want = (args[2] as usize).min(FD_PAYLOAD_CAP);
                if want == 0 {
                    return Answer::ReplyBytes(0, 0, [0u8; FD_PAYLOAD_CAP]);
                }
                match net.as_deref_mut() {
                    Some(n) => {
                        let mut buf = [0u8; FD_PAYLOAD_CAP];
                        match n.recv(s.vfs_fd, &mut buf[..want]) {
                            Ok(got) => Answer::ReplyBytes(got as u64, got, buf),
                            Err(e) => Answer::Reply(0, net_status(e)),
                        }
                    }
                    None => Answer::Reply(0, E_NOSYS),
                }
            }
            Ok(s) => {
                let want = (args[2] as usize).min(FD_PAYLOAD_CAP);
                /* E7 increment 21: the continuation's evidence. The offset
                 * the VFS is about to hand back IS src_off, and the gate
                 * workload's copy is a sequence of reads from fd 3 — so the
                 * gate tenant logs it, one line per read. The run's proof is
                 * then readable off the serial log without trusting anything
                 * reconstructed: boot 1's lines stop at 0x20000 (the fourth
                 * read is the one the kernel holds at, so it never reaches
                 * this arm), and boot 2's first line is 0x30000 — the
                 * continuation, not a re-run. */
                if gate {
                    if let Some(off) = vfs.offset_of(vt, s.vfs_fd) {
                        gate_read_log(off);
                    }
                }
                let mut buf = [0u8; FD_PAYLOAD_CAP];
                match vfs.read(vt, s.vfs_fd, &mut buf[..want]) {
                    Ok(n) => Answer::ReplyBytes(n as u64, n, buf),
                    Err(e) => Answer::Reply(0, cap_of(e)),
                }
            }
            Err(x) => Answer::Reply(0, x),
        },
        /* open (2): the measured flag contract (§5.6 — qemu -strace on
         * the real candidate): O_RDONLY|O_LARGEFILE|O_CLOEXEC and
         * O_WRONLY|O_CREAT|O_LARGEFILE|O_TRUNC|O_CLOEXEC, mode 0666;
         * O_DIRECTORY selects the dir arm (opendir's shape — the VFS's
         * plain open refuses a directory with EISDIR by design). Every
         * bit outside the measured set is refused by NAME (ENOSYS) —
         * never silently dropped — and the mode rides to the VFS
         * untouched (create_file's own umask-less semantics). */
        LINUX_OPEN => {
            let Some(path) = path_arg(payload, 0) else {
                return Answer::Reply(0, E_NOSYS); /* unterminated: service's
                                                   * range let it through;
                                                   * the NUL rule refuses */
            };
            let flags = args[1] as u32;
            let (vf, cloexec, want_dir) = match translate_open_flags(flags) {
                Ok(t) => t,
                Err(x) => return Answer::Reply(0, x),
            };
            /* O_LARGEFILE (0x8000) is accepted and IGNORED: off_t is
             * 64-bit here, so the bit carries no information — musl
             * ORs it into every open, and refusing it would refuse the
             * candidate's first open by name. */
            let opened = if want_dir {
                vfs.open_dir(vt, path)
            } else {
                vfs.open(vt, path, vf, args[2] as u16)
            };
            match opened {
                Ok(vfd) => {
                    if cloexec {
                        vfs.set_cloexec(vt, vfd, true).ok();
                    }
                    match alloc_fd(fds, FD_KIND_FILE, vfd, flags) {
                        Ok(li) => Answer::Reply(li, STATUS_OK),
                        Err(x) => {
                            vfs.close(vt, vfd).ok(); /* mint failed: the
                                                     * VFS fd goes back */
                            Answer::Reply(0, x)
                        }
                    }
                }
                Err(e) => Answer::Reply(0, cap_of(e)),
            }
        }
        /* close (3): the slot is spent on this side, the VFS fd on
         * that side — the descriptor's two halves close together, and
         * double-close is EBADF (the slot says so, as ever). */
        LINUX_CLOSE => {
            let i = args[0] as usize;
            let Some(s) = fds.get(i).filter(|s| s.used) else {
                return Answer::Reply(0, E_BADF);
            };
            if s.kind == FD_KIND_FILE {
                if let Err(e) = vfs.close(vt, s.vfs_fd) {
                    return Answer::Reply(0, cap_of(e));
                }
            } else if s.kind == FD_KIND_SOCKET {
                /* M1: the descriptor's two halves close together — the
                 * Linux slot here, the P2 driver's socket on the other
                 * side of SocketOps. */
                if let Some(n) = net.as_deref_mut() {
                    if let Err(e) = n.close_socket(s.vfs_fd) {
                        return Answer::Reply(0, net_status(e));
                    }
                }
            }
            fds[i].used = false;
            Answer::Reply(0, STATUS_OK)
        }
        /* stat (4) / lstat (6): one VFS arm, two numbers. This VFS has
         * no symlink follow at all (no symlink objects exist), so
         * stat == lstat IS the truth, not a shortcut — and the reply
         * carries the compact Stat (the kernel serializes musl's
         * layout; this side never sees it). */
        LINUX_STAT | LINUX_LSTAT => {
            let Some(path) = path_arg(payload, 0) else {
                return Answer::Reply(0, E_NOSYS);
            };
            match vfs.stat(vt, path) {
                Ok(st) => stat_answer(&st),
                Err(e) => Answer::Reply(0, cap_of(e)),
            }
        }
        /* fstat (5): the same compact Stat, selected by descriptor. The
         * loader's map_library asks for the DSO's size and mode before it
         * maps it; without this arm the DT_NEEDED chain stops at ENOSYS
         * even though the open succeeded. A console slot answers the
         * CharNode's own stat; the VFS owns every file descriptor. */
        /* M2b: one file-map fill round. The kernel has already mapped
         * the destination page and staged it as the RAW destination, so
         * the answer is the file's bytes at the kernel's own offset —
         * ZERO-PADDED to the round's length past EOF (the mapping's last
         * page owns those zeros; a short payload would leave the round
         * unfinished forever). A slot that is no longer a file refuses by
         * name and stops the fill. */
        LINUX_MMAP_FILL => {
            let want = (args[2] as usize).min(FD_FILL_CAP);
            if want == 0 {
                return Answer::Reply(0, E_INVAL);
            }
            match fd_slot(fds, args[0]).copied().ok_or(E_BADF) {
                Ok(s) if s.kind == FD_KIND_FILE => {
                    let mut buf = [0u8; FD_FILL_CAP];
                    let got = vfs.read_at(vt, s.vfs_fd, args[1],
                                          &mut buf[..want]).unwrap_or(0);
                    for b in &mut buf[got.min(want)..want] {
                        *b = 0;
                    }
                    Answer::ReplyFill(want as u64, want, buf)
                }
                Ok(_) => Answer::Reply(0, E_NOSYS),
                Err(x) => Answer::Reply(0, x),
            }
        }
        LINUX_FSTAT => match slot_of(fds, args[0]) {
            Ok(s) if s.kind == FD_KIND_FILE => match vfs.fstat(vt, s.vfs_fd) {
                Ok(st) => stat_answer(&st),
                Err(e) => Answer::Reply(0, cap_of(e)),
            },
            Ok(s) if s.kind == FD_KIND_CONSOLE => stat_answer(&console.stat()),
            Ok(_) => Answer::Reply(0, E_NOSYS),
            Err(x) => Answer::Reply(0, x),
        },
        /* lseek (8): whence is Linux's own 0/1/2 — the VFS's SEEK_*
         * constants ARE those numbers, so the word crosses untouched.
         * A console slot is ESPIPE, the number Linux gives. SEEK_HOLE/
         * SEEK_DATA (3/4) are not in the VFS's set and answer EINVAL —
         * the census's two lseeks are both SEEK_CUR. */
        LINUX_LSEEK => match slot_of(fds, args[0]) {
            Ok(s) if s.kind == FD_KIND_FILE => {
                match vfs.lseek(vt, s.vfs_fd, args[1] as i64, args[2] as u32) {
                    Ok(off) => Answer::Reply(off, STATUS_OK),
                    Err(e) => Answer::Reply(0, cap_of(e)),
                }
            }
            Ok(_) => Answer::Reply(0, E_SPIPE),
            Err(x) => Answer::Reply(0, x),
        },
        /* fcntl (72): the four answered commands — F_GETFD/F_SETFD
         * over the slot's own cloexec bit (the VFS's u16 flag word
         * cannot hold O_CLOEXEC; the bit lives HERE, which is the
         * widening the design's flag table demanded), F_GETFL echoing
         * the open flags, F_SETFL accepted only as the no-change ask
         * (a changed flag is ENOSYS by name: the VFS mints flags at
         * open and has no setter — a fabricated 0 would claim a
         * semantics nothing performs). Everything else — F_DUPFD
         * included — ENOSYS by name. */
        LINUX_FCNTL => match slot_of(fds, args[0]) {
            Ok(s) => match args[1] {
                F_GETFD => Answer::Reply(
                    if s.flags & LO_CLOEXEC != 0 { 1 } else { 0 },
                    STATUS_OK),
                F_SETFD => {
                    let i = args[0] as usize;
                    if args[2] & !1 != 0 {
                        Answer::Reply(0, E_INVAL) /* FD_CLOEXEC is 0 or 1 */
                    } else {
                        if args[2] & 1 != 0 {
                            fds[i].flags |= LO_CLOEXEC;
                        } else {
                            fds[i].flags &= !LO_CLOEXEC;
                        }
                        Answer::Reply(0, STATUS_OK)
                    }
                }
                F_GETFL => Answer::Reply(
                    (s.flags & !LO_CLOEXEC) as u64, STATUS_OK),
                F_SETFL => {
                    if (args[2] as u32) & !LO_CLOEXEC
                        == (s.flags & !LO_CLOEXEC)
                    {
                        Answer::Reply(0, STATUS_OK) /* the no-change ask */
                    } else {
                        Answer::Reply(0, E_NOSYS)
                    }
                }
                _ => Answer::Reply(0, E_NOSYS),
            },
            Err(x) => Answer::Reply(0, x),
        },
        /* fsync (74): the VFS's own rule — ramfs answers 0 (the
         * shmem-faithful no-op; there is no device to flush and
         * Linux's tmpfs fsync is a no-op), a device-backed slot answers
         * ENOSYS by name until the block cache has a flush entry point
         * (unreachable from the census's single fsync row, which lands
         * on /tmp). */
        LINUX_FSYNC => match slot_of(fds, args[0]) {
            Ok(s) if s.kind == FD_KIND_FILE => {
                match vfs.fsync(vt, s.vfs_fd) {
                    Ok(()) => Answer::Reply(0, STATUS_OK),
                    Err(e) => Answer::Reply(0, cap_of(e)),
                }
            }
            Ok(_) => Answer::Reply(0, E_SPIPE),
            Err(x) => Answer::Reply(0, x),
        },
        /* ftruncate (77): by fd, through the node's own (fs, ino) — a
         * renamed path truncates the object the descriptor names, as
         * POSIX requires (the VFS's truncate_at, not truncate(path)).
         * The census's shape: ftruncate(fd, 67108864) after the sparse
         * SEEK_CURs — the size lands exactly. */
        LINUX_FTRUNCATE => match slot_of(fds, args[0]) {
            Ok(s) if s.kind == FD_KIND_FILE => {
                match vfs.truncate_at(vt, s.vfs_fd, args[1]) {
                    Ok(()) => Answer::Reply(0, STATUS_OK),
                    Err(e) => Answer::Reply(0, cap_of(e)),
                }
            }
            Ok(_) => Answer::Reply(0, E_SPIPE),
            Err(x) => Answer::Reply(0, x),
        },
        /* rename (82): both paths ride back to back; the split is the
         * FIRST NUL, and each string is validated (NUL within cap,
         * UTF-8) before either is used — the twin of the kernel's
         * two-string attach. The VFS refuses a cross-mount rename and
         * a rename onto a non-empty directory by its own rules (the
         * design's named refusals, delivered through cap_of). */
        LINUX_RENAME => {
            let Some((old, new)) = path_pair(payload) else {
                return Answer::Reply(0, E_NOSYS);
            };
            match vfs.rename(vt, old, new) {
                Ok(()) => Answer::Reply(0, STATUS_OK),
                Err(e) => Answer::Reply(0, cap_of(e)),
            }
        }
        /* mkdir (83): the measured shape is mkdir(dir, 0777) with the
         * EEXIST tolerated by the caller (create_dir_all's own retry) —
         * so EEXIST must cross as 17, which is why the file class
         * brought its own CAP codes (cap.h's (-13) family). */
        LINUX_MKDIR => {
            let Some(path) = path_arg(payload, 0) else {
                return Answer::Reply(0, E_NOSYS);
            };
            match vfs.mkdir(vt, path, args[1] as u16) {
                Ok(()) => Answer::Reply(0, STATUS_OK),
                Err(e) => Answer::Reply(0, cap_of(e)),
            }
        }
        /* unlink (87): the VFS's own rules (a directory is EISDIR, a
         * missing name ENOENT — each crosses under its own number now).
         * The census unlinks three pruned backups, all files. */
        LINUX_UNLINK => {
            let Some(path) = path_arg(payload, 0) else {
                return Answer::Reply(0, E_NOSYS);
            };
            match vfs.unlink(vt, path) {
                Ok(()) => Answer::Reply(0, STATUS_OK),
                Err(e) => Answer::Reply(0, cap_of(e)),
            }
        }
        /* getdents64 (217): the sidecar sends the LISTING (names,
         * inodes, types) and the KERNEL packs the linux_dirent64
         * records — d_reclen math stays kernel-side, the sidecar never
         * sees a Linux layout (§5.6's trust boundary). args[5] is the
         * kernel-staged cursor (the syscall's own three arguments leave
         * that word unused): the tail from that entry index onward,
         * capped by the payload bound so one reply cannot run unbounded
         * — a short tail is POSIX-legal, the caller loops, and an empty
         * tail is end-of-directory (0 bytes packed, 0 entries). */
        LINUX_GETDENTS64 => match slot_of(fds, args[0]) {
            Ok(s) if s.kind == FD_KIND_FILE => {
                match vfs.read_dir_at(vt, s.vfs_fd, args[5]) {
                    Ok(ents) => {
                        let mut buf = [0u8; FD_PAYLOAD_CAP];
                        let n = dents_payload(&ents, &mut buf);
                        Answer::ReplyBytes(0, n, buf)
                    }
                    Err(e) => Answer::Reply(0, cap_of(e)),
                }
            }
            Ok(_) => Answer::Reply(0, E_NOTDIR),
            Err(x) => Answer::Reply(0, x),
        },
        /* Unreachable: service routes exactly the thirteen numbers
         * here. ENOSYS keeps the no-silent-success rule total. */
        _ => Answer::Reply(0, E_NOSYS),
    }
}

/// M1 — the socket lookup: a Linux fd resolves to the P2 driver's socket
/// id only when the slot names the Socket kind. A missing slot and a
/// non-socket descriptor both answer EBADF here (the fd table's own
/// refusal; ENOTSOCK is a refinement M1 does not need).
fn socket_id(fds: &[FdSlot], fd: u64) -> Result<u32, i32> {
    match fd_slot(fds, fd) {
        Some(s) if s.kind == FD_KIND_SOCKET => Ok(s.vfs_fd),
        _ => Err(E_BADF),
    }
}

/// Map a NET_ERR_* status onto the CAP code whose kernel-side arm delivers
/// the matching Linux errno. The network family is deliberately not
/// collapsed: EADDRINUSE, ECONNRESET, ENOTCONN, EMSGSIZE and EPIPE each
/// have a distinct CAP twin (kernel/cap.h), so a caller's error path sees
/// the same number Linux would have given it. NET_ERR_INUSE remains EBUSY;
/// it is the service's "socket already in use" shape, not EADDRINUSE.
fn net_status(e: u16) -> i32 {
    match e {
        NET_ERR_INVAL | NET_ERR_PROTO => E_INVAL,
        NET_ERR_ADDRINUSE => E_ADDRINUSE,
        NET_ERR_ADDRNOTAVAIL => E_ADDRNOTAVAIL,
        NET_ERR_CONNRESET => E_CONNRESET,
        NET_ERR_NOTCONN => E_NOTCONN,
        NET_ERR_MSGSIZE => E_MSGSIZE,
        NET_ERR_PIPE => E_PIPE,
        NET_ERR_INUSE => E_CONFLICT,
        NET_ERR_IO => E_REVOKED,
        NET_ERR_NOMEM => E_NOMEM,
        NET_ERR_CAP => E_PERM,
        NET_ERR_WOULDBLOCK => E_AGAIN,
        _ => E_REVOKED,
    }
}

/// Parse the 16 bytes the kernel attached for connect/bind: sin_family is
/// Host order, sin_port and sin_addr are network order. Only AF_INET is
/// served; a shorter span or another family is EINVAL (the kernel already
/// refused a non-16-byte addrlen at the first trap).
fn parse_sockaddr_in(payload: &[u8]) -> Result<(u32, u16), i32> {
    if payload.len() < LINUX_SOCKADDR_IN_BYTES {
        return Err(E_INVAL);
    }
    let family = u16::from_le_bytes([payload[0], payload[1]]);
    if family != AF_INET {
        return Err(E_INVAL);
    }
    let port = u16::from_be_bytes([payload[2], payload[3]]);
    let ip = u32::from_be_bytes([payload[4], payload[5], payload[6], payload[7]]);
    Ok((ip, port))
}

/// M1 — the eight network arms, translated onto SocketOps. The fd table is
/// the caller's local copy (written back by service, like the file class);
/// `net` is the manifest's network client, and its ABSENCE is ENOSYS by
/// name — an environment without a `network` cap cannot pretend to have a
/// network stack. The kernel has already copied out the sockaddr/data the
/// child's address space forced; every semantic decision is here.
fn answer_net(
    net: &mut Option<&mut Box<dyn SocketOps>>,
    num: u64,
    args: &[u64; 6],
    fds: &mut [FdSlot],
    payload: &[u8],
) -> Answer {
    let Some(net) = net.as_deref_mut() else {
        return Answer::Reply(0, E_NOSYS); /* no network cap in the manifest */
    };
    match num {
        /* socket(domain, type, protocol): AF_INET + SOCK_STREAM only, with
         * Linux's own type-flag word (CLOEXEC kept, NONBLOCK kept as the
         * client's own non-waiting behavior). A protocol of 0 or TCP is
         * the same stream. Every other shape is refused EINVAL by name. */
        LINUX_SOCKET => {
            let ty = args[1] as u32;
            if args[0] != AF_INET as u64 || (ty & 0xf) != SOCK_STREAM as u32 {
                return Answer::Reply(0, E_INVAL);
            }
            if args[2] != 0 && args[2] != 6 {
                return Answer::Reply(0, E_INVAL); /* non-TCP protocol */
            }
            if ty & !(0xf | LO_CLOEXEC | O_NONBLOCK) != 0 {
                return Answer::Reply(0, E_NOSYS); /* flag not named */
            }
            match net.socket(SOCK_STREAM) {
                Ok(id) => match alloc_fd(fds, FD_KIND_SOCKET, id,
                                         O_RDWR | (ty & (LO_CLOEXEC | O_NONBLOCK))) {
                    Ok(fd) => Answer::Reply(fd, STATUS_OK),
                    Err(x) => {
                        let _ = net.close_socket(id);
                        Answer::Reply(0, x)
                    }
                },
                Err(e) => Answer::Reply(0, net_status(e)),
            }
        }
        LINUX_CONNECT | LINUX_BIND => {
            let id = match socket_id(fds, args[0]) {
                Ok(id) => id,
                Err(x) => return Answer::Reply(0, x),
            };
            let (ip, port) = match parse_sockaddr_in(payload) {
                Ok(v) => v,
                Err(x) => return Answer::Reply(0, x),
            };
            let r = if num == LINUX_CONNECT {
                net.connect(id, ip, port)
            } else {
                net.bind(id, ip, port)
            };
            match r {
                Ok(()) => Answer::Reply(0, STATUS_OK),
                Err(e) => Answer::Reply(0, net_status(e)),
            }
        }
        LINUX_LISTEN => {
            let id = match socket_id(fds, args[0]) {
                Ok(id) => id,
                Err(x) => return Answer::Reply(0, x),
            };
            /* backlog is a hint this driver does not consume (the same
             * no-op Linux permits); the call still means "start listening". */
            match net.listen(id) {
                Ok(()) => Answer::Reply(0, STATUS_OK),
                Err(e) => Answer::Reply(0, net_status(e)),
            }
        }
        LINUX_ACCEPT => {
            let id = match socket_id(fds, args[0]) {
                Ok(id) => id,
                Err(x) => return Answer::Reply(0, x),
            };
            match net.accept_peer(id) {
                Ok((new_id, rip, rport)) => match alloc_fd(fds, FD_KIND_SOCKET, new_id, O_RDWR) {
                    Ok(fd) => {
                        if args[1] == 0 {
                            /* addr (and therefore addrlen) was NULL: the
                             * kernel staged no payload, so the new fd is
                             * the whole answer. */
                            Answer::Reply(fd, STATUS_OK)
                        } else {
                            let mut b = [0u8; FD_PAYLOAD_CAP];
                            b[0..2].copy_from_slice(&AF_INET.to_le_bytes());
                            b[2..4].copy_from_slice(&rport.to_be_bytes());
                            b[4..8].copy_from_slice(&rip.to_be_bytes());
                            b[16..20].copy_from_slice(
                                &(LINUX_SOCKADDR_IN_BYTES as u32).to_le_bytes());
                            Answer::ReplyBytes(fd, LINUX_NET_ADDR_BYTES, b)
                        }
                    }
                    Err(x) => {
                        let _ = net.close_socket(new_id);
                        Answer::Reply(0, x)
                    }
                },
                Err(e) => Answer::Reply(0, net_status(e)),
            }
        }
        /* sendto on a connected TCP socket is send; Linux ignores the
         * destination address in that shape. The kernel packed a non-NULL
         * destination into args[4]/args[5] at the first trap; decode it
         * here only to reject a malformed bridge request, then let
         * NetClient decide whether the socket state makes the send legal. */
        LINUX_SENDTO => {
            let id = match socket_id(fds, args[0]) {
                Ok(id) => id,
                Err(x) => return Answer::Reply(0, x),
            };
            let dest = if args[4] != 0 {
                if args[5] != LINUX_SENDTO_DEST_MARKER {
                    return Answer::Reply(0, E_INVAL);
                }
                Some(((args[4] >> 16) as u32, (args[4] & 0xffff) as u16))
            } else {
                None
            };
            match net.send_to(id, payload, dest) {
                Ok(n) => Answer::Reply(n as u64, STATUS_OK),
                Err(e) => Answer::Reply(0, net_status(e)),
            }
        }
        /* recvfrom: data first, then the 20-byte trailer. NetClient keeps
         * the peer for connected and accepted sockets; fill the source
         * address from it so recvfrom reports the sender, not zeroes. */
        LINUX_RECVFROM => {
            let id = match socket_id(fds, args[0]) {
                Ok(id) => id,
                Err(x) => return Answer::Reply(0, x),
            };
            let want = (args[2] as usize).min(NET_RECV_MAX);
            if want == 0 {
                return Answer::Reply(0, STATUS_OK); /* zero-length receive */
            }
            let peer = net.peer_addr(id);
            let mut b = [0u8; FD_PAYLOAD_CAP];
            match net.recv(id, &mut b[..want]) {
                Ok(got) => {
                    let t = got;
                    b[t..t + 2].copy_from_slice(&AF_INET.to_le_bytes());
                    if let Some((ip, port)) = peer {
                        b[t + 2..t + 4].copy_from_slice(&port.to_be_bytes());
                        b[t + 4..t + 8].copy_from_slice(&ip.to_be_bytes());
                    }
                    b[t + 16..t + 20].copy_from_slice(
                        &(LINUX_SOCKADDR_IN_BYTES as u32).to_le_bytes());
                    Answer::ReplyBytes(got as u64,
                                       got + LINUX_NET_ADDR_BYTES, b)
                }
                Err(e) => Answer::Reply(0, net_status(e)),
            }
        }
        LINUX_SHUTDOWN => {
            let id = match socket_id(fds, args[0]) {
                Ok(id) => id,
                Err(x) => return Answer::Reply(0, x),
            };
            if args[1] > 2 {
                return Answer::Reply(0, E_INVAL);
            }
            match net.shutdown(id, args[1] as u8) {
                Ok(()) => Answer::Reply(0, STATUS_OK),
                Err(e) => Answer::Reply(0, net_status(e)),
            }
        }
        _ => Answer::Reply(0, E_NOSYS),
    }
}

/// The console half of read(0): the entry loop has already pushed whatever
/// arrived from the kernel's console channel into this node, so the arm can
/// answer from bytes that exist now. A zero-byte request is a success even
/// when the buffer is empty; an empty buffer PARKS the request, and a closed console
/// channel is EOF. The kernel's reply shape carries the count as both rax and
/// payload length, so `n` is the whole truth here as well.
fn console_read_answer(console: &CharNode, want: usize) -> Answer {
    let want = want.min(FD_PAYLOAD_CAP);
    if want == 0 {
        return Answer::ReplyBytes(0, 0, [0u8; FD_PAYLOAD_CAP]);
    }
    /* E7 increment 24: no buffered input means the request WAITS — the
     * child is parked awaiting this exchange, so parking the request here
     * is the blocking read POSIX describes, not a busy loop and not a
     * fabricated zero-byte EOF. A closed console is ready (EOF), so the
     * read below answers 0 for it. */
    if !console.read_ready() {
        return Answer::ReadWait(want);
    }
    let mut buf = [0u8; FD_PAYLOAD_CAP];
    match console.read(&mut buf[..want]) {
        Ok(n) => Answer::ReplyBytes(n as u64, n, buf),
        Err(e) => Answer::Reply(0, cap_of(e)),
    }
}

/// E7 increment 24: the absolute deadline of a poll wait. POSIX's -1 is
/// "no deadline" (0 here — the sentinel poll_expired never treats as
/// elapsed), and any non-negative millisecond count becomes now plus that
/// many nanoseconds, saturating rather than wrapping a huge ask.
fn poll_deadline(now_ns: u64, timeout_ms: i32) -> u64 {
    if timeout_ms < 0 {
        0
    } else {
        now_ns.saturating_add((timeout_ms as u64).saturating_mul(1_000_000))
    }
}

/// Has a poll wait's deadline passed? A zero deadline is POSIX's forever:
/// no elapsed clock can expire it.
fn poll_expired(deadline: u64, now_ns: u64) -> bool {
    deadline != 0 && now_ns >= deadline
}

/// Scan one pollfd array against a task's fd table and the console,
/// rewriting each entry's revents. Pure over the table, the console and
/// the closure that answers a VFS-backed descriptor (`Some(revents)` from
/// Vfs::poll, `None` for a descriptor the VFS cannot name — POLLNVAL).
///
/// POSIX's rules this implements literally: a negative fd is ignored
/// (revents 0, not an error); a descriptor with no slot is POLLNVAL
/// whatever was asked; the console reads ready when input is buffered or
/// the channel has closed, and never blocks a write; events the caller did
/// not ask for are not reported, except the error bits the VFS's own poll
/// already treats as always-reported. Returns the count of fds with
/// non-zero revents — the value Linux returns.
fn poll_scan<F>(
    fds: &[FdSlot],
    net: &mut Option<&mut Box<dyn SocketOps>>,
    arr: &mut [u8; LINUX_POLL_PAYLOAD_BYTES],
    nfds: usize,
    console: &CharNode,
    file_revents: &mut F,
) -> usize
where
    F: FnMut(u32, u16) -> Option<u16>,
{
    let mut ready = 0usize;
    for i in 0..nfds {
        let off = i * LINUX_POLLFD_BYTES;
        let fd = i32::from_le_bytes([
            arr[off], arr[off + 1], arr[off + 2], arr[off + 3],
        ]);
        let events = u16::from_le_bytes([arr[off + 4], arr[off + 5]]);
        let revents: u16 = if fd < 0 {
            0 /* POSIX: a negative fd is ignored */
        } else {
            match fd_slot(fds, fd as u64) {
                None => POLLNVAL,
                Some(s) if s.kind == FD_KIND_CONSOLE => {
                    let mut r = 0u16;
                    if events & POLLIN != 0 && console.read_ready() {
                        r |= POLLIN;
                    }
                    /* A console's write side never blocks. */
                    if events & POLLOUT != 0 {
                        r |= POLLOUT;
                    }
                    r
                }
                Some(s) if s.kind == FD_KIND_SOCKET => {
                    match net.as_deref_mut() {
                        Some(n) => match n.poll(s.vfs_fd) {
                            Ok(ev) => {
                                let mut r = 0u16;
                                if events & POLLIN != 0 && ev & 0x1 != 0 { r |= POLLIN; }
                                if events & POLLOUT != 0 && ev & 0x4 != 0 { r |= POLLOUT; }
                                r
                            }
                            Err(_) => POLLERR,
                        },
                        None => POLLNVAL,
                    }
                }
                Some(s) => match file_revents(s.vfs_fd, events) {
                    Some(r) => r,
                    None => POLLNVAL,
                },
            }
        };
        arr[off + 6..off + 8].copy_from_slice(&revents.to_le_bytes());
        if revents != 0 {
            ready += 1;
        }
    }
    ready
}

/// The poll(7) answer for one request (E7 increment 24): readiness now, a
/// zero-timeout snapshot, or a parked wait with its deadline. `payload` is
/// the array the kernel copied in; nfds and the timeout come from the
/// forwarded words, never from the payload's length alone. The full array
/// crosses back on every reply that has fds, because the kernel writes it
/// into the caller's own span and a partial array would leave the tail
/// carrying pre-call revents.
fn poll_answer<F>(
    fds: &[FdSlot],
    net: &mut Option<&mut Box<dyn SocketOps>>,
    console: &CharNode,
    payload: &[u8],
    args: &[u64; 6],
    now_ns: u64,
    mut file_revents: F,
) -> Answer
where
    F: FnMut(u32, u16) -> Option<u16>,
{
    let nfds = args[1] as usize;
    if nfds > LINUX_POLL_MAX_FDS {
        return Answer::Reply(0, E_INVAL);
    }
    let len = nfds * LINUX_POLLFD_BYTES;
    if payload.len() < len {
        /* The service range already made the attach length the kernel
         * plan's twin; a mismatch here is malformed, never read past. */
        return Answer::Reply(0, E_NOSYS);
    }
    let mut arr = [0u8; LINUX_POLL_PAYLOAD_BYTES];
    arr[..len].copy_from_slice(&payload[..len]);
    let ready = poll_scan(fds, net, &mut arr, nfds, console, &mut file_revents);
    let timeout_ms = args[2] as i32;
    if ready > 0 || timeout_ms == 0 {
        if nfds == 0 {
            Answer::Reply(0, STATUS_OK)
        } else {
            /* ReplyBytes' buffer is the fd class's general 1024-byte
             * payload; only the poll array's own len crosses. */
            let mut out = [0u8; FD_PAYLOAD_CAP];
            out[..len].copy_from_slice(&arr[..len]);
            Answer::ReplyBytes(ready as u64, len, out)
        }
    } else {
        Answer::PollWait(arr, len, poll_deadline(now_ns, timeout_ms))
    }
}

/// Translate a Linux open-flag word into (vfs flags, cloexec, dir-open
/// selector) — the MEASURED contract (qemu -strace on the real
/// candidate), pure so the guard suite can pin it without a boot:
/// the candidate's three opens all fit, an accmode of 3 is EINVAL
/// (Linux's own number), and any bit outside the measured set is
/// ENOSYS BY NAME — refused, never silently dropped (the trigger the
/// design's flag table named, consumed here). O_LARGEFILE rides every
/// musl open and is accepted as a NO-OP: off_t is 64-bit, so the bit
/// carries no information, and refusing it would refuse the
/// candidate's first open by name.
fn translate_open_flags(flags: u32) -> Result<(u16, bool, bool), i32> {
    if flags & 0x3 == 0x3 {
        return Err(E_INVAL); /* O_ACCMODE 3: no such access mode */
    }
    if flags & !LO_KNOWN != 0 {
        return Err(E_NOSYS); /* an unmeasured bit, refused by name */
    }
    let want_dir = flags & LO_DIRECTORY != 0;
    let cloexec = flags & LO_CLOEXEC != 0;
    let mut vf: u16 = (flags & 0x3) as u16;
    if flags & 0x40 != 0 { vf |= aerosls_vfs::O_CREAT; }
    if flags & 0x80 != 0 { vf |= aerosls_vfs::O_EXCL; }
    if flags & 0x200 != 0 { vf |= aerosls_vfs::O_TRUNC; }
    if flags & 0x400 != 0 { vf |= aerosls_vfs::O_APPEND; }
    Ok((vf, cloexec, want_dir))
}

/// Split the path payload at its NULs: one string for the single-path
/// numbers, both strings for rename. Validated BY NAME — a missing NUL
/// (the kernel's plan always attaches one) or a non-UTF-8 byte is
/// refused rather than read past or lossily coerced; the cap is the
/// kernel's own (LINUX_PATH_CAP per string, so a payload can never
/// name more than the plan staged).
fn path_arg(payload: &[u8], which: usize) -> Option<&str> {
    let mut start = 0usize;
    for _ in 0..which {
        start += payload.get(start..)?.iter().position(|&b| b == 0)? + 1;
    }
    let rest = payload.get(start..)?;
    let end = rest.iter().position(|&b| b == 0)?;
    if end > LINUX_PATH_CAP {
        return None;
    }
    core::str::from_utf8(&rest[..end]).ok()
}

fn path_pair(payload: &[u8]) -> Option<(&str, &str)> {
    let old = path_arg(payload, 0)?;
    let new = path_arg(payload, 1)?;
    Some((old, new))
}

/// The compact Stat payload (the twin of LINUX_STAT_PAYLOAD_BYTES, the
/// kernel's linux_stat_bytes reads these exact offsets): mode/uid/gid
/// as the vfs::Stat carries them, size, and the one timestamp it has.
/// The reply both stat shapes carry: the compact Stat behind the fixed
/// payload length, which payload_bound and the kernel's stat stage agree on.
fn stat_answer(st: &Stat) -> Answer {
    let mut buf = [0u8; FD_PAYLOAD_CAP];
    buf[..STAT_PAYLOAD_BYTES].copy_from_slice(&stat_payload(st));
    Answer::ReplyBytes(0, STAT_PAYLOAD_BYTES, buf)
}

fn stat_payload(st: &Stat) -> [u8; STAT_PAYLOAD_BYTES] {
    let mut b = [0u8; STAT_PAYLOAD_BYTES];
    b[0..2].copy_from_slice(&st.mode.to_le_bytes());
    b[2..4].copy_from_slice(&st.uid.to_le_bytes());
    b[4..6].copy_from_slice(&st.gid.to_le_bytes());
    b[8..16].copy_from_slice(&st.size.to_le_bytes());
    b[16..20].copy_from_slice(&st.mtime.to_le_bytes());
    b
}

/// Linux d_type values (the kernel passes them through verbatim into
/// linux_dirent64.d_type) — the vfs FileType's mapping, decided here
/// once.
fn dt_of(ty: FileType) -> u8 {
    match ty {
        FileType::Fifo => 1,  /* DT_FIFO */
        FileType::Char => 2,  /* DT_CHR  */
        FileType::Dir => 4,   /* DT_DIR  */
        FileType::File => 8,  /* DT_REG  */
    }
}

/// The directory-listing payload: u32 count, then per entry u64 ino,
/// u8 ty (DT_*), u16 name_len, name bytes — the kernel's
/// linux_dirent_pack reads this exact shape. Entries are appended until
/// the payload cap would break mid-record: the tail continues on the
/// next call (the kernel advances its cursor only by what it packed).
fn dents_payload(ents: &[DirEnt], buf: &mut [u8; FD_PAYLOAD_CAP]) -> usize {
    let mut off = 4usize;
    let mut count = 0u32;
    for e in ents {
        let nl = e.name.len();
        if nl == 0 || nl > 255 {
            continue; /* a name the record format cannot carry */
        }
        let need = 11 + nl;
        if off + need > DENTS_PAYLOAD_CAP {
            break;
        }
        buf[off..off + 8].copy_from_slice(&e.ino.to_le_bytes());
        buf[off + 8] = dt_of(e.ty);
        buf[off + 9..off + 11].copy_from_slice(&(nl as u16).to_le_bytes());
        buf[off + 11..off + 11 + nl].copy_from_slice(e.name.as_bytes());
        off += need;
        count += 1;
    }
    buf[0..4].copy_from_slice(&count.to_le_bytes());
    off
}

/// M4.4: one parked FUTEX_WAIT. The queue is bounded by the task table,
/// because a task can hold at most one parked request and every waiter is
/// therefore a live `LinuxTask`. `seq` gives FIFO wake order independent of
/// slot reuse; `task` is the sidecar table index whose reply the wake sends.
#[cfg_attr(test, derive(Debug))]
#[derive(Clone, Copy)]
struct FutexWaiter {
    used: bool,
    task: u8,
    uaddr: u64,
    seq: u64,
    bitset: u32,
}

const MAX_FUTEX_WAITERS: usize = 8;

/// M4.3: the state `CLONE_VM`/`CLONE_FILES`/`CLONE_SIGHAND` make shared.
/// A Linux thread group owns exactly one of these; every task in the group
/// points at it. The fd table, the §5.4 heap ledger and the signal
/// dispositions live here because Linux shares them at thread creation,
/// not because this side happens to have one table per tracked child. The
/// thread count is the group's reference count: the last release retires
/// the group and its tables together.
#[derive(Clone, Copy)]
struct LinuxGroup {
    used: bool,
    threads: u16,
    tgid: u64,                /* M4.5: the leader's kernel TID             */
    partition: u32,
    gate: bool,
    /// M5.2a: mirror console writes to the kernel serial log even when this
    /// is not the gate profile. Set only by test-profile spawn paths; the
    /// gate keeps its own `gate` bit for the stop/read instrument.
    mirror_console: bool,
    fds: [FdSlot; LINUX_MAX_FDS],
    brk_base: u64,
    brk_limit: u64,
    brk_cur: u64,
    brk_mtop: u64,
    image_lo: u64,
    image_hi: u64,
    sig_dispo: [u8; SIG_MAX + 1],
    sig_act: [SigAct; SIG_MAX + 1],
    futex: [FutexWaiter; MAX_FUTEX_WAITERS],
    futex_seq: u64,
}

impl LinuxGroup {
    const fn new() -> Self {
        LinuxGroup {
            used: false,
            threads: 0,
            tgid: 0,
            partition: 0,
            gate: false,
            mirror_console: false,
            fds: [FdSlot { used: false, kind: 0, vfs_fd: 0, flags: 0 };
                  LINUX_MAX_FDS],
            brk_base: 0,
            brk_limit: 0,
            brk_cur: 0,
            brk_mtop: 0,
            image_lo: 0,
            image_hi: 0,
            sig_dispo: [DISPO_DFL; SIG_MAX + 1],
            sig_act: [SigAct::new(); SIG_MAX + 1],
            futex: [FutexWaiter { used: false, task: 0, uaddr: 0, seq: 0,
                                 bitset: 0 };
                    MAX_FUTEX_WAITERS],
            futex_seq: 0,
        }
    }
}

/// One tracked Linux thread. M4.3 splits the old single record: the group
/// owns fds/heap/dispositions, while this record owns the channel, the
/// name-to-PID registry entry, the VFS task, and the thread's own signal
/// mask, pending bits, tid and alternate stack. `owns_fds` is true only
/// for a clone without `CLONE_FILES`; a normal pthread-style clone leaves
/// it false and reads the group's table. The per-thread `sig` keeps the
/// group disposition fields as a scratch copy so the existing pure answer
/// seam keeps its shape; `service` loads and stores the authoritative
/// group copy around every request.
#[derive(Clone, Copy)]   /* Copy so the table can be a const-initialized
                         * static array ([..; N] repeat needs it) */
struct LinuxTask {
    used: bool,
    ch_r: u32,   /* the child's forwards arrive here (our recv handle)   */
    ch_w: u32,   /* replies leave here (our send handle)                 */
    name: [u8; 48],
    name_len: usize,
    partition: u32,  /* the registry scope every name->pid resolve below
                      * passes: 0 = THIS sidecar's own partition, the
                      * kernel's reading of a 0 request partition. It is
                      * not "PARTITION_SYSTEM": the child is spawned with
                      * target 0 (inherit), so it lives where we live. */
    group: usize,    /* index into groups(); the group owns shared state  */
    vfs_task: u32,   /* this thread's VFS task; its fd table is shared
                      * with the group's siblings when CLONE_FILES      */
    owns_fds: bool,  /* clone without CLONE_FILES: this thread's own     */
    fds: [FdSlot; LINUX_MAX_FDS], /* private table when owns_fds      */
    sig: SigState,   /* per-thread mask/pending/tid/alt stack; the      */
                     /* disposition fields are a service-loaded cache   */
    /* E7 increment 24 (the waiting half): a console read or a timed poll
     * whose reply is deliberately NOT sent yet. The child is parked in
     * the kernel awaiting this exchange, so `pending` also means "no
     * further request can arrive"; `service` checks readiness and the
     * deadline before it polls the channel. Nothing here is inherited by
     * a reused slot (track() resets all of it). */
    pending: u8,      /* PENDING_NONE / PENDING_READ / PENDING_POLL /     */
                      /* PENDING_FUTEX                                    */
    pending_num: u64, /* the syscall number of the pending exchange: the
                       * signal trailer must echo THIS number, and it may
                       * be a futex/read/poll, not only a signal number */
    pending_tag: u32, /* the tag the reply must echo                            */
    stopped: bool,    /* M4.10: a group-directed stop latched this sibling,
                       * parked or running; service holds the parked reply
                       * or the kernel task until SIGCONT */
    run_held: bool,    /* M4.11: `stopped` is backed by SYS_SLS_PROC_HOLD
                       * rather than a withheld reply; SIGCONT releases it */
    pending_arg: u64, /* read: the clamped count; poll: nfds                    */
    pending_len: usize, /* poll: array bytes (nfds * 8; 0 for a pure sleep)     */
    pending_deadline: u64, /* poll: absolute ns, 0 = wait forever              */
    pending_bytes: [u8; LINUX_POLL_PAYLOAD_BYTES], /* poll: the snapshot     */
}

const MAX_LINUX_TASKS: usize = 8;
const MAX_LINUX_GROUPS: usize = MAX_LINUX_TASKS;

#[allow(static_mut_refs)]   /* single-threaded sidecar, entry-loop confined */
fn groups() -> &'static mut [LinuxGroup; MAX_LINUX_GROUPS] {
    static mut TABLE: [LinuxGroup; MAX_LINUX_GROUPS] = [LinuxGroup::new();
                                                         MAX_LINUX_GROUPS];
    unsafe { &mut TABLE }
}

#[allow(static_mut_refs)]   /* single-threaded sidecar, entry-loop confined */
fn tasks() -> &'static mut [LinuxTask; MAX_LINUX_TASKS] {
    static mut TABLE: [LinuxTask; MAX_LINUX_TASKS] = [LinuxTask {
        used: false,
        ch_r: 0,
        ch_w: 0,
        name: [0; 48],
        name_len: 0,
        partition: 0,
        group: 0,
        vfs_task: 0,
        owns_fds: false,
        fds: [FdSlot { used: false, kind: 0, vfs_fd: 0, flags: 0 }; LINUX_MAX_FDS],
        sig: SigState::new(),
        pending: PENDING_NONE,
        pending_num: 0,
        pending_tag: 0,
        stopped: false,
        run_held: false,
        pending_arg: 0,
        pending_len: 0,
        pending_deadline: 0,
        pending_bytes: [0; LINUX_POLL_PAYLOAD_BYTES],
    }; MAX_LINUX_TASKS];
    unsafe { &mut TABLE }
}

/// Register a spawned Linux child as the leader of a new thread group.
/// The spawn caller (`Kernel::linux_spawn`) records the returned handles
/// here — before or after the child first runs, both safe: the child's
/// forward parks until a reply, and its message simply waits in the
/// channel queue until the next `service()` pass drains it. Returns false
/// when either table is full (the child then parks unanswered; a spawn
/// path SHOULD check).
pub fn track(
    ch_r: u32,
    ch_w: u32,
    name: &str,
    partition: u32,
    heap_base: u64,
    heap_limit: u64,
    image: (u64, u64), /* M2b: the loaded-image span the kernel stamped,
                        * from `Kernel::linux_image_span`; (0, 0) when
                        * the task has none, and then the ledger's
                        * mprotect span is the heap's alone            */
    vfs_task: u32,   /* step 4: the VFS task id the file-class arms
                      * operate over — allocated by the spawn path
                      * (it owns the ProcManager) before the track   */
    leader_tid: u64, /* M4.5: the kernel PID/TID of this leader — the
                      * value getpid/gettid and tkill use              */
    gate: bool,      /* increment 19: this is the gate profile's first
                      * user — mirror its console output to the serial
                      * log (the tenant console is otherwise unobserved) */
) -> bool {
    track_mode(ch_r, ch_w, name, partition, heap_base, heap_limit, image,
               vfs_task, leader_tid, gate, gate)
}

/// M5.2a: track an ordinary-tenant test child whose console must be visible
/// on the serial log, without claiming the gate profile. `gate` stays false,
/// so the gate read/stop instrument is not armed; only console mirroring is
/// enabled.
pub fn track_mirrored(
    ch_r: u32,
    ch_w: u32,
    name: &str,
    partition: u32,
    heap_base: u64,
    heap_limit: u64,
    image: (u64, u64),
    vfs_task: u32,
    leader_tid: u64,
) -> bool {
    track_mode(ch_r, ch_w, name, partition, heap_base, heap_limit, image,
               vfs_task, leader_tid, false, true)
}

fn track_mode(
    ch_r: u32,
    ch_w: u32,
    name: &str,
    partition: u32,
    heap_base: u64,
    heap_limit: u64,
    image: (u64, u64),
    vfs_task: u32,
    leader_tid: u64,
    gate: bool,
    mirror_console: bool,
) -> bool {
    let nlen = name.len().min(48);
    let Some(tslot) = tasks().iter().position(|t| !t.used) else {
        return false;
    };
    let Some(gslot) = groups().iter().position(|g| !g.used) else {
        return false;
    };

    /* The group is born with the leader. fds 0, 1, 2 are its console,
     * registered at spawn like every process's stdio; all three accept
     * writes (a tty is bidirectional; write(0) is not EBADF on Linux).
     * The §5.4 span and the signal dispositions belong to the group too,
     * because CLONE_VM/CLONE_SIGHAND make them siblings' shared state. */
    let g = &mut groups()[gslot];
    g.used = true;
    g.threads = 1;
    g.tgid = leader_tid;
    g.partition = partition;
    g.gate = gate;
    g.mirror_console = mirror_console;
    g.brk_base = heap_base;
    g.brk_limit = heap_limit;
    g.brk_cur = heap_base;
    g.brk_mtop = heap_limit;
    g.image_lo = image.0;
    g.image_hi = image.1;
    g.sig_dispo = [DISPO_DFL; SIG_MAX + 1];
    g.sig_act = [SigAct::new(); SIG_MAX + 1];
    for slot in g.fds.iter_mut().take(3) {
        slot.used = true;
        slot.kind = FD_KIND_CONSOLE;
    }

    let t = &mut tasks()[tslot];
    t.used = true;
    t.ch_r = ch_r;
    t.ch_w = ch_w;
    t.name[..nlen].copy_from_slice(&name.as_bytes()[..nlen]);
    t.name_len = nlen;
    t.partition = partition;
    t.group = gslot;
    t.vfs_task = vfs_task;
    t.owns_fds = false;
    /* The signal state is born with the thread: a reused registry slot
     * must not inherit its predecessor's mask, pending bits, TID or
     * alternate stack (proc_clear_resume_state's discipline, sidecar
     * side). The disposition cache starts at the group's DFL table. */
    t.sig = SigState::new();
    t.sig.tid = leader_tid;
    t.sig.tgid = leader_tid;
    t.sig.dispo = g.sig_dispo;
    t.sig.act = g.sig_act;
    /* A reused slot must not inherit a parked exchange either: the tag
     * belongs to a child that is gone, and answering it would put a
     * reply on the next child's channel. */
    t.pending = PENDING_NONE;
    t.pending_num = 0;
    t.pending_tag = 0;
    t.stopped = false;
    t.run_held = false;
    t.pending_arg = 0;
    t.pending_len = 0;
    t.pending_deadline = 0;
    t.pending_bytes = [0; LINUX_POLL_PAYLOAD_BYTES];
    true
}

/// A cloned thread needs one free task slot; checked before the kernel
/// create so a full sidecar table refuses without leaving a released child
/// to kill.
pub fn can_track_thread() -> bool {
    tasks().iter().any(|t| !t.used)
}

/// M4.3: register a clone child in its parent's group. The group id is
/// returned for the serial line; the child's shared fds/heap/dispositions
/// come from the group, while its channel, name, VFS task and signal
/// mask/pending/tid are its own. `share_fds` mirrors `CLONE_FILES`: a
/// thread without it gets a private copy of the group table, exactly as
/// Linux gives it its own files_struct. Returns None only when a slot is
/// full or the parent/group is no longer live, and increments the group's
/// reference count only after every field has landed.
pub fn track_thread(
    ch_r: u32,
    ch_w: u32,
    name: &str,
    parent: usize,
    vfs_task: u32,
    share_fds: bool,
    tid: u64,
    clear_tid: u64,
) -> Option<u32> {
    if parent >= MAX_LINUX_TASKS || !tasks()[parent].used {
        return None;
    }
    let gid = tasks()[parent].group;
    if gid >= MAX_LINUX_GROUPS || !groups()[gid].used {
        return None;
    }
    let tslot = tasks().iter().position(|t| !t.used)?;
    let nlen = name.len().min(48);
    let parent_mask = tasks()[parent].sig.mask;
    let g = &mut groups()[gid];

    let t = &mut tasks()[tslot];
    t.used = true;
    t.ch_r = ch_r;
    t.ch_w = ch_w;
    t.name[..nlen].copy_from_slice(&name.as_bytes()[..nlen]);
    t.name_len = nlen;
    t.partition = g.partition;
    t.group = gid;
    t.vfs_task = vfs_task;
    t.owns_fds = !share_fds;
    if share_fds {
        t.fds = [FdSlot { used: false, kind: 0, vfs_fd: 0, flags: 0 };
                 LINUX_MAX_FDS];
    } else {
        t.fds = g.fds;
    }
    /* pthread-style inheritance: a new thread starts with the creating
     * thread's blocked mask, no pending signals, no ALT stack and no tid
     * declaration. Dispositions remain the group's authoritative copy. */
    t.sig = SigState::new();
    t.sig.tid = tid;
    t.sig.tgid = g.tgid;
    t.sig.clear_tid = clear_tid;
    t.sig.mask = parent_mask;
    t.sig.dispo = g.sig_dispo;
    t.sig.act = g.sig_act;
    t.pending = PENDING_NONE;
    t.pending_num = 0;
    t.pending_tag = 0;
    t.stopped = false;
    t.run_held = false;
    t.pending_arg = 0;
    t.pending_len = 0;
    t.pending_deadline = 0;
    t.pending_bytes = [0; LINUX_POLL_PAYLOAD_BYTES];
    g.threads = g.threads.saturating_add(1);
    Some(gid as u32)
}

/// Drop one task's sidecar record and release its group reference. The
/// returned VFS task is the caller's to `exit_task`; doing the filesystem
/// teardown outside this pure state transition keeps the host test able to
/// drive group lifetime without constructing a Vfs. The group (and its
/// fds/heap/dispositions) retires only when the last thread releases it.
fn drop_task_state(i: usize) -> Option<u32> {
    if i >= MAX_LINUX_TASKS || !tasks()[i].used {
        return None;
    }
    let gid = tasks()[i].group;
    let vt = tasks()[i].vfs_task;
    tasks()[i].used = false;
    if gid < MAX_LINUX_GROUPS && groups()[gid].used {
        let g = &mut groups()[gid];
        futex_remove(g, i);
        g.threads = g.threads.saturating_sub(1);
        if g.threads == 0 {
            *g = LinuxGroup::new();
        }
    }
    Some(vt)
}

/// The VFS half of release: its `exit_task` already keeps a CLONE_FILES
/// table alive while a sibling still holds it, so every thread calls it and
/// the last one clears the table.
fn release_task<K: Kernel, A: BufferAlloc>(i: usize, vfs: &mut Vfs<K, A>) {
    if let Some(vt) = drop_task_state(i) {
        let _ = vfs.exit_task(vt);
    }
}

/// M4.5: is this sidecar record's kernel task gone? `sidecar_pid` resolves
/// only an ACTIVE process, so 0 is the liveness test the sweep uses before
/// it may reap a record. A slot that is already unused is dead by definition.
fn task_is_dead<K: Kernel>(k: &K, i: usize, partition: u32) -> bool {
    if i >= MAX_LINUX_TASKS || !tasks()[i].used {
        return true;
    }
    let n = tasks()[i].name_len;
    let mut name_buf = [0u8; 48];
    name_buf[..n].copy_from_slice(&tasks()[i].name[..n]);
    let name = core::str::from_utf8(&name_buf[..n]).unwrap_or("");
    k.sidecar_pid(name, partition) == 0
}

fn group_live_records<K: Kernel>(k: &K, members: &[usize], partition: u32) -> usize {
    members.iter().filter(|&&j| !task_is_dead(k, j, partition)).count()
}

/// M4.4: remove a task's queued FUTEX_WAIT and return the exact entry. A
/// group-stop rollback uses the returned snapshot to restore the queue slot
/// it removed; callers that only need removal can ignore it.
fn futex_take(g: &mut LinuxGroup, task: usize) -> FutexWaiter {
    let mut taken = FutexWaiter { used: false, task: 0, uaddr: 0, seq: 0,
                                  bitset: 0 };
    for w in g.futex.iter_mut() {
        if w.used && w.task as usize == task {
            taken = *w;
            *w = FutexWaiter { used: false, task: 0, uaddr: 0, seq: 0,
                               bitset: 0 };
        }
    }
    taken
}

/// M4.4: remove a task's queued FUTEX_WAIT. Called on every release before
/// the group count drops, so a killed waiter can never be answered later by
/// a wake that selected a stale task index.
fn futex_remove(g: &mut LinuxGroup, task: usize) {
    let _ = futex_take(g, task);
}

/// M4.11: restore one exact waiter snapshot. The queue slot removed by
/// `futex_take` is free again, so this normally succeeds; an already-present
/// entry is left alone to keep rollback idempotent.
fn futex_restore(g: &mut LinuxGroup, waiter: FutexWaiter) -> bool {
    if !waiter.used {
        return true;
    }
    if g.futex.iter().any(|w| w.used && w.task == waiter.task) {
        return true;
    }
    if let Some(slot) = g.futex.iter_mut().find(|w| !w.used) {
        *slot = waiter;
        true
    } else {
        false
    }
}

/// Parse the fixed wait record's relative timespec. 0 means no timeout;
/// a zero relative timespec is Linux's immediate ETIMEDOUT; a negative or
/// out-of-range value is EINVAL. The addition is checked, so a tenant's
/// huge timeout cannot wrap into an accidental early wake.
fn futex_deadline(payload: &[u8], now: u64, absolute: bool) -> Result<u64, i32> {
    if payload.len() != FUTEX_WAIT_PAYLOAD_BYTES {
        return Err(E_NOSYS);
    }
    let has = u32::from_le_bytes([payload[4], payload[5], payload[6], payload[7]]);
    if has == 0 {
        return Ok(0);
    }
    let sec = read_u64(payload, 8) as i64;
    let nsec = read_u64(payload, 16);
    if sec < 0 || nsec >= NS_PER_SEC {
        return Err(E_INVAL);
    }
    let rel = (sec as u64).checked_mul(NS_PER_SEC)
        .and_then(|s| s.checked_add(nsec))
        .ok_or(E_INVAL)?;
    if rel == 0 {
        return Err(E_TIMEOUT);
    }
    if absolute {
        // FUTEX_WAIT_BITSET's timespec is absolute against the same clock
        // that produced `now` (the service pass's monotonic sample).
        if rel <= now {
            return Err(E_TIMEOUT);
        }
        Ok(rel)
    } else {
        now.checked_add(rel).ok_or(E_INVAL)
    }
}

fn futex_enqueue(g: &mut LinuxGroup, task: usize, uaddr: u64,
                 bitset: u32) -> bool {
    if let Some(w) = g.futex.iter_mut().find(|w| !w.used) {
        w.used = true;
        w.task = task as u8;
        w.uaddr = uaddr;
        w.seq = g.futex_seq;
        w.bitset = bitset;
        g.futex_seq = g.futex_seq.wrapping_add(1);
        true
    } else {
        false
    }
}

fn futex_wake(g: &mut LinuxGroup, uaddr: u64, want: usize,
              bitset: u32) -> FutexWake {
    let mut out = FutexWake { n: 0, tasks: [0; MAX_FUTEX_WAITERS] };
    while (out.n as usize) < want {
        let mut pick: Option<usize> = None;
        for (i, w) in g.futex.iter().enumerate() {
            if !w.used || w.uaddr != uaddr || w.bitset & bitset == 0 {
                continue;
            }
            if pick.map_or(true, |p| w.seq < g.futex[p].seq) {
                pick = Some(i);
            }
        }
        let Some(i) = pick else { break };
        out.tasks[out.n as usize] = g.futex[i].task;
        out.n += 1;
        g.futex[i] = FutexWaiter { used: false, task: 0, uaddr: 0, seq: 0,
                                   bitset: 0 };
    }
    out
}

/// M4.8: FUTEX_REQUEUE's queue transition. Wake the oldest `nr_wake` waiters
/// on `from`, then move the oldest remaining `nr_requeue` waiters to `to`
/// without replying to them. Returns the wake set and the number moved; the
/// Linux return value is only the wake count. FIFO order is the `seq` field,
/// exactly as a plain wake reads it.
fn futex_requeue(g: &mut LinuxGroup, from: u64, to: u64,
                 nr_wake: usize, nr_requeue: usize,
                 bitset: u32) -> (FutexWake, usize) {
    let w = futex_wake(g, from, nr_wake.min(MAX_FUTEX_WAITERS), bitset);
    let mut moved = 0usize;
    while moved < nr_requeue {
        let mut pick: Option<usize> = None;
        for (i, q) in g.futex.iter().enumerate() {
            if !q.used || q.uaddr != from || q.bitset & bitset == 0 {
                continue;
            }
            if pick.map_or(true, |p| q.seq < g.futex[p].seq) {
                pick = Some(i);
            }
        }
        let Some(i) = pick else { break };
        g.futex[i].uaddr = to;
        /* Linux appends the moved waiters to the destination's tail. A new
         * sequence (rather than the source's old one) keeps that order when
         * a destination waiter already exists and was enqueued earlier. */
        g.futex[i].seq = g.futex_seq;
        g.futex_seq = g.futex_seq.wrapping_add(1);
        moved += 1;
    }
    (w, moved)
}

/// M4.5: send the deferred zero replies for a wake produced by a thread
/// exit's clear-child-tid rule. This is the same reply shape as a FUTEX_WAKE
/// answer; the wake itself is already committed to the group queue.
fn futex_wake_reply<K: Kernel>(k: &K, gid: usize, uaddr: u64, want: usize) {
    let w = futex_wake(&mut groups()[gid], uaddr, want, u32::MAX);
    for &idx in w.tasks[..w.n as usize].iter() {
        let j = idx as usize;
        if j >= MAX_LINUX_TASKS || !tasks()[j].used ||
            tasks()[j].pending != PENDING_FUTEX {
            continue;
        }
        let wake_tag = tasks()[j].pending_tag;
        let wake_ch = tasks()[j].ch_w;
        tasks()[j].pending = PENDING_NONE;
        reply(k, wake_ch, wake_tag, LINUX_FUTEX, 0, STATUS_OK);
    }
}

/// M4.5: a thread left (exit, signal kill, or channel close). The kernel has
/// already zeroed its CLONE_CHILD_CLEARTID/set_tid_address word through the
/// checked copy-in; this side owns the futex queue, so it wakes one waiter
/// on that address before the task index can be reused. A task with no
/// clear-tid word is a no-op, and `release_task` clears the stored address.
fn wake_clear_tid<K: Kernel>(k: &K, gid: usize, i: usize) {
    if i >= MAX_LINUX_TASKS || !tasks()[i].used {
        return;
    }
    let ctid = tasks()[i].sig.clear_tid;
    if ctid != 0 {
        futex_wake_reply(k, gid, ctid, 1);
        tasks()[i].sig.clear_tid = 0;
    }
}

/// M4.8: collect one exiting thread's robust-list futex words. The kernel
/// does the checked child-memory walk and ORs FUTEX_OWNER_DIED; this side
/// only remembers the addresses until after the kill/teardown so a failed
/// sweep cannot wake waiters on a still-live group. The head is consumed
/// once, whether the walk succeeds or not.
fn robust_exit_collect<K: Kernel>(k: &K, i: usize) -> ([u64; 8], usize) {
    let mut out = [0u64; 8];
    if i >= MAX_LINUX_TASKS || !tasks()[i].used {
        return (out, 0);
    }
    let head = tasks()[i].sig.robust_head;
    let len = tasks()[i].sig.robust_len;
    tasks()[i].sig.robust_head = 0;
    if head == 0 || len != LINUX_ROBUST_LIST_HEAD_BYTES {
        return (out, 0);
    }
    let n = tasks()[i].name_len;
    let mut name_buf = [0u8; 48];
    name_buf[..n].copy_from_slice(&tasks()[i].name[..n]);
    let name = core::str::from_utf8(&name_buf[..n]).unwrap_or("");
    let pid = k.sidecar_pid(name, tasks()[i].partition);
    if pid == 0 {
        return (out, 0);
    }
    match k.linux_robust_exit(pid, head, len, &mut out) {
        Ok(n) => (out, n.min(out.len())),
        Err(_) => (out, 0),
    }
}

/// M4.8: wake every queue named by a robust walk. The owner-died bit is
/// already in the child word; the wake is what releases FUTEX_WAIT's
/// deferred reply, exactly like clear-child-tid's join wake.
fn robust_wake<K: Kernel>(k: &K, gid: usize, addrs: &[u64; 8], n: usize) {
    for &uaddr in addrs[..n].iter() {
        futex_wake_reply(k, gid, uaddr, 1);
    }
}

/// M4.4's pure futex arm. The group owns the queue; `task` is the caller's
/// table index (recorded in the wait entry) and `now` is the service pass's
/// one clock sample. WAIT compares the kernel's observed word with the
/// caller's expected value before enqueueing; WAKE removes the oldest
/// matching entries up to the requested count and reports them back for the
/// deferred replies. Both operations are private-only in this increment.
fn answer_futex(
    g: &mut LinuxGroup,
    task: usize,
    args: &[u64; 6],
    payload: &[u8],
    now: u64,
) -> Answer {
    let op = args[1];
    if op == (FUTEX_WAIT | FUTEX_PRIVATE) ||
       op == (FUTEX_WAIT_BITSET | FUTEX_PRIVATE) {
        if payload.len() != FUTEX_WAIT_PAYLOAD_BYTES {
            return Answer::Reply(0, E_NOSYS);
        }
        let observed = u32::from_le_bytes([payload[0], payload[1],
                                            payload[2], payload[3]]);
        if observed != args[2] as u32 {
            return Answer::Reply(0, E_AGAIN);
        }
        // WAKE_BITSET's match word is args[5]; a plain WAIT is MATCH_ANY.
        // Linux refuses an empty bitset rather than treating it as MATCH_ANY.
        let bitset = if op == (FUTEX_WAIT_BITSET | FUTEX_PRIVATE) {
            if args[5] == 0 { return Answer::Reply(0, E_INVAL); }
            args[5] as u32
        } else {
            u32::MAX
        };
        let absolute = op == (FUTEX_WAIT_BITSET | FUTEX_PRIVATE);
        let deadline = match futex_deadline(payload, now, absolute) {
            Ok(d) => d,
            Err(E_TIMEOUT) => return Answer::Reply(0, E_TIMEOUT),
            Err(e) => return Answer::Reply(0, e),
        };
        if !futex_enqueue(g, task, args[0], bitset) {
            return Answer::Reply(0, E_TABLEFULL);
        }
        Answer::FutexWait(args[0], deadline)
    } else if op == (FUTEX_WAKE | FUTEX_PRIVATE) ||
              op == (FUTEX_WAKE_BITSET | FUTEX_PRIVATE) {
        let want = args[2].min(MAX_FUTEX_WAITERS as u64) as usize;
        let bitset = if op == (FUTEX_WAKE_BITSET | FUTEX_PRIVATE) {
            if args[5] == 0 { return Answer::Reply(0, E_INVAL); }
            args[5] as u32
        } else {
            u32::MAX
        };
        Answer::FutexWake(futex_wake(g, args[0], want, bitset))
    } else if op == (FUTEX_REQUEUE | FUTEX_PRIVATE) ||
              op == (FUTEX_CMP_REQUEUE | FUTEX_PRIVATE) {
        /* M4.8: move waiters from args[0] to args[4]. Plain REQUEUE has
         * no payload; CMP_REQUEUE carries the kernel's four-byte snapshot
         * of the destination word and refuses with EAGAIN when the
         * caller's expected word no longer matches. A zero destination or
         * the same queue on both ends is EINVAL, as Linux's futex code
         * names it. */
        let cmp = op == (FUTEX_CMP_REQUEUE | FUTEX_PRIVATE);
        if cmp && payload.len() != FUTEX_REQUEUE_PAYLOAD_BYTES {
            return Answer::Reply(0, E_NOSYS);
        }
        if args[4] == 0 || args[4] == args[0] {
            return Answer::Reply(0, E_INVAL);
        }
        if cmp {
            let observed = u32::from_le_bytes([payload[0], payload[1],
                                                payload[2], payload[3]]);
            if observed != args[5] as u32 {
                return Answer::Reply(0, E_AGAIN);
            }
        }
        let nr_wake = args[2].min(MAX_FUTEX_WAITERS as u64) as usize;
        let nr_requeue = args[3].min(MAX_FUTEX_WAITERS as u64) as usize;
        let (w, moved) = futex_requeue(g, args[0], args[4], nr_wake,
                                        nr_requeue, u32::MAX);
        Answer::FutexRequeue(w, args[4], moved)
    } else {
        Answer::Reply(0, E_NOSYS)
    }
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

/// E7 increment 24 / M4.4: the nearest finite poll or futex deadline, in
/// nanoseconds from `now`, or TIMEOUT_NONE when no parked request has one
/// (a blocking console read and POSIX's -1 poll/futex wait on readiness or
/// wake alone). The entry loop hands this to `Kernel::wait`, so a timed
/// request wakes the sidecar when it expires; without it a 5000 ms poll or
/// a futex timeout would sleep until some unrelated channel woke the loop,
/// which is not the timeout the caller asked for. The kernel clock is ~10 ms
/// granular, so a short deadline lands within one tick — a resolver, not a
/// real-time guarantee.
pub fn next_wait_ns<K: Kernel>(k: &K) -> u64 {
    let now = k.now_ns();
    let mut best = aerosls_proto::kabi::TIMEOUT_NONE;
    for t in tasks().iter() {
        if t.used && (t.pending == PENDING_POLL || t.pending == PENDING_FUTEX)
            && t.pending_deadline != 0 {
            let rem = t.pending_deadline.saturating_sub(now);
            if rem < best {
                best = rem;
            }
        }
    }
    best
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
/// M2b: the fill round's length — the kernel asks for at most this many
/// bytes per round (its staged clamp), and this side's payload buffer is
/// the same 1 KiB, so one round is one message either way.
const LINUX_MMAP_FILL_CHUNK: u64 = 4024;
const LINUX_MMAP_FILE_MAX: u64 = 32768;

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
    /* The buffer is the LARGEST any reply can be — a fill round's 4024
     * bytes plus the header — so one builder serves every number; the
     * per-number bound is payload_bound's, not this array's. */
    let mut payload = [0u8; LINUX_SYSCALL_REQ_BYTES + FD_FILL_CAP];
    debug_assert!(bytes.len() <= FD_FILL_CAP);
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

/// Increment 21: the gate tenant's file-class read offset, in hex (klog's
/// own encoding). Target builds only — the host fakes have no serial path,
/// exactly like enosys_log.
fn gate_read_log(off: u64) {
    #[cfg(feature = "target")]
    {
        crate::entry::klog(b"[E7-GATE] read off=", off as u32, b"");
    }
    #[cfg(not(feature = "target"))]
    {
        let _ = off;
    }
}

/// M4.2: clone(56)'s answer. The kernel does the mechanism; this function
/// owns the policy seam — validate the same flag subset, ask the kernel for
/// a held thread, allocate the sidecar-side tracking slot, and only then
/// tell the child it exists. The child is already released by the kernel
/// when this returns; a tracking failure kills it rather than leaving an
/// untracked Linux task parked forever.
fn answer_clone<K: Kernel, A: BufferAlloc>(
    k: &K,
    i: usize,
    args: &[u64; 6],
    vfs: &mut Vfs<K, A>,
    num: u64,
) -> Answer {
    let flags = args[0];
    if flags & !CLONE_M4_ALLOW != 0 {
        return Answer::Reply(0, E_NOSYS);
    }
    if flags & CLONE_VM == 0 || flags & CLONE_THREAD == 0 ||
        flags & CLONE_SIGHAND == 0 {
        return Answer::Reply(0, E_INVAL);
    }
    /* A thread created without CLONE_FILES has a private table. M4.3's
     * group record models the shared-table shape; refusing a further clone
     * from a private task is honest, rather than copying the GROUP's table
     * and silently giving the grandchild the wrong descriptors. */
    if tasks()[i].owns_fds {
        return Answer::Reply(0, E_NOSYS);
    }
    /* Refuse a full sidecar table before the kernel allocates a child: the
     * failure then costs no release/kill dance and cannot leave a running
     * thread whose forwards nobody answers. */
    if !can_track_thread() {
        return Answer::Reply(0, E_NOMEM);
    }

    let nlen = tasks()[i].name_len;
    let parent_vt = tasks()[i].vfs_task;
    let share_fds = flags & CLONE_FILES != 0;
    let mut pname = [0u8; 48];
    pname[..nlen].copy_from_slice(&tasks()[i].name[..nlen]);
    let partition = tasks()[i].partition;
    let pname_str = match core::str::from_utf8(&pname[..nlen]) {
        Ok(s) => s,
        Err(_) => return Answer::Reply(0, E_INVAL),
    };
    let parent_pid = k.sidecar_pid(pname_str, partition);
    if parent_pid == 0 {
        return Answer::Reply(0, E_TARGET);
    }

    let mut req = LinuxCloneThreadReq {
        status: 0,
        parent_pid,
        flags,
        child_stack: args[1],
        tls: args[4],
        ptid: args[2],
        ctid: args[3],
        child_tid: 0,
        child_r: 0,
        child_w: 0,
        sidecar_r: 0,
        sidecar_w: 0,
        sidecar_ch_w: tasks()[i].ch_w as u16,
        reserved16: 0,
        tgid: 0,
    };
    let child_tid = match k.linux_clone_thread(&mut req) {
        Ok(t) => t,
        Err(cap) => {
            /* The kernel's error is already a CAP code; `reply` carries
             * status through the same §5.7 map as every other arm, so it
             * must stay in that family. Mapping it to a Linux errno here
             * would make the kernel map an already-mapped value (EINVAL
             * -22 has no CAP preimage and would arrive as EIO). */
            enosys_log(num as u32, &pname[..nlen], b" clone refused\n");
            return Answer::Reply(0, cap);
        }
    };

    let mut cname = [0u8; 48];
    let mut off = push_bytes(&mut cname, 0, &pname[..nlen]);
    off = push_bytes(&mut cname, off, b".t");
    off = dec_into(&mut cname, off, child_tid as u64);
    /* The VFS task is the thread's own task record, but `CLONE_FILES`
     * makes it share the parent's fd-table object: close/open/dup in one
     * thread is then visible in every sibling, which is exactly what the
     * sidecar group's one Linux fd table models. Without CLONE_FILES the
     * VFS helper copies the table, and the private Linux slots below copy
     * too — the flag is honoured rather than treated as always-on. */
    let clone_flags = if share_fds { VFS_CLONE_FILES } else { 0 };
    let vt = match vfs.clone_task(parent_vt, clone_flags) {
        Ok(vt) => vt,
        Err(_) => {
            k.proc_kill(child_tid);
            return Answer::Reply(0, E_NOMEM);
        }
    };
    let cname_str = core::str::from_utf8(&cname[..off]).unwrap_or("");
    /* track_thread stores OUR ends (the sidecar's receive/send handles),
     * exactly like linux_spawn's return values — the child's own slots are
     * already in the child's table and are not the sidecar's to read. */
    let gid = match track_thread(
        req.sidecar_r as u32,
        req.sidecar_w as u32,
        cname_str,
        i,
        vt,
        share_fds,
        child_tid as u64,
        if flags & CLONE_CHILD_CLEARTID != 0 { args[3] } else { 0 },
    ) {
        Some(gid) => gid,
        None => {
            let _ = vfs.exit_task(vt);
            k.proc_kill(child_tid);
            return Answer::Reply(0, E_NOMEM);
        }
    };
    let mut ex = [0u8; 48];
    let mut off = push_bytes(&mut ex, 0, b" clone group=");
    off = dec_into(&mut ex, off, gid as u64);
    off = push_bytes(&mut ex, off, if share_fds { b" files shared\n" }
                                          else { b" files copied\n" });
    enosys_log(num as u32, &pname[..nlen], &ex[..off]);
    Answer::Reply(child_tid as u64, STATUS_OK)
}

/// M4.8: clone3(435)'s answer. The kernel copied the 88-byte v1
/// `struct clone_args`; this function applies field policy and reuses the
/// clone(56) mechanism/policy seam. Every unsupported extension is refused
/// by NAME (ENOSYS) rather than silently ignored; a non-thread exit signal
/// or a size the kernel already rejected is EINVAL.
fn answer_clone3<K: Kernel, A: BufferAlloc>(
    k: &K,
    i: usize,
    payload: &[u8],
    vfs: &mut Vfs<K, A>,
) -> Answer {
    if payload.len() != LINUX_CLONE_ARGS_BYTES {
        return Answer::Reply(0, E_NOSYS);
    }
    let flags = read_u64(payload, 0);
    let pidfd = read_u64(payload, 8);
    let child_tid = read_u64(payload, 16);
    let parent_tid = read_u64(payload, 24);
    let exit_signal = read_u64(payload, 32);
    let stack = read_u64(payload, 40);
    let stack_size = read_u64(payload, 48);
    let tls = read_u64(payload, 56);
    let set_tid = read_u64(payload, 64);
    let set_tid_size = read_u64(payload, 72);
    let cgroup = read_u64(payload, 80);

    if pidfd != 0 || stack_size != 0 || set_tid != 0 ||
        set_tid_size != 0 || cgroup != 0 {
        return Answer::Reply(0, E_NOSYS);
    }
    /* A thread's exit_signal is ignored by Linux; glibc still passes
     * SIGCHLD in its clone3 shape, so accept 0 and SIGCHLD only. */
    if exit_signal != 0 && exit_signal != 17 {
        return Answer::Reply(0, E_INVAL);
    }
    let args = [flags, stack, parent_tid, child_tid, tls, 0];
    answer_clone(k, i, &args, vfs, LINUX_CLONE3)
}

/// The CAP code's errno NAME — the value §5.7 delivers, not the status
/// word. The generic answer line used to print "ok" for every non-ENOSYS
/// status, so the M2b probe's four failed opens looked like successes while
/// the loader reported ENOENT. Name every code this sidecar can answer
/// with; an unknown value prints `E?` rather than a lie.
fn errno_name(status: i32) -> &'static [u8] {
    match status {
        E_NOSYS => b"ENOSYS",
        E_NOENT => b"ENOENT",
        E_IO => b"EIO",             /* E_REVOKED shares 2 */
        E_PROTO => b"EPROTO",
        E_INTR => b"EINTR",
        E_CONFLICT => b"EBUSY",
        E_INVAL => b"EINVAL",
        E_BADF => b"EBADF",
        E_PERM => b"EPERM",
        E_NOMEM => b"ENOMEM",
        E_TARGET => b"ESRCH",
        E_ACCES => b"EACCES",
        E_ISDIR => b"EISDIR",
        E_NOTDIR => b"ENOTDIR",
        E_EXIST => b"EEXIST",
        E_NOTEMPTY => b"ENOTEMPTY",
        E_ROFS => b"EROFS",
        E_SPIPE => b"ESPIPE",
        E_NAMETOOLONG => b"ENAMETOOLONG",
        E_ADDRINUSE => b"EADDRINUSE",
        E_ADDRNOTAVAIL => b"EADDRNOTAVAIL",
        E_CONNRESET => b"ECONNRESET",
        E_NOTCONN => b"ENOTCONN",
        E_ISCONN => b"EISCONN",
        E_DESTADDRREQ => b"EDESTADDRREQ",
        E_MSGSIZE => b"EMSGSIZE",
        E_PIPE => b"EPIPE",
        E_CONNREFUSED => b"ECONNREFUSED",
        E_TIMEOUT => b"ETIMEDOUT",
        E_NETUNREACH => b"ENETUNREACH",
        E_HOSTUNREACH => b"EHOSTUNREACH",
        E_AFNOSUPPORT => b"EAFNOSUPPORT",
        E_AGAIN => b"EAGAIN",
        E_NOSPC => b"ENOSPC",
        E_TABLEFULL => b"ENFILE",
        _ => b"E?",
    }
}

fn enosys_log(tag_num: u32, child: &[u8], extra: &[u8]) {
    #[cfg(feature = "target")]
    {
        /* 160: a failed open's name + path must fit; the old 72 admitted
         * only the first few bytes of a search path, which is exactly the
         * fact the line now exists to carry. */
        let mut line = [0u8; 160];
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

/// A parked child's endpoint went away while its request was pending
/// (E7 increment 24): the exchange can never be answered, so the slot goes
/// with the task — the recv CLOSE arm's own teardown, reached through a
/// PEEK because the pending request means `service` deliberately skips the
/// recv that would consume the close. Without this, a child killed while
/// blocked on a console read or a poll would leave a used slot and its VFS
/// task behind forever.
fn reap_pending_if_closed<K: Kernel, A: BufferAlloc>(
    k: &K,
    i: usize,
    vfs: &mut Vfs<K, A>,
) {
    if let Ok(kind) = k.poll(tasks()[i].ch_r) {
        if kind == aerosls_proto::CH_KIND_CLOSE {
            let gid = tasks()[i].group as usize;
            wake_clear_tid(k, gid, i);
            release_task(i, vfs);
        }
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
/// M5.2: one CAT_EXEC request, answered through the ProcManager's installed
/// `CatalogOps` — the same object `ctx.catalog()` reaches. The syscall's
/// words carry no identity: the catalog client's channel registration is the
/// bound owner the service checks. A refusal is translated to the CAP family
/// the kernel's §5.7 map already understands, so the task sees a Linux errno
/// and never a `CAT_ERR_*` number.
fn catalog_status_to_cap(status: u16) -> i32 {
    match status {
        CAT_ERR_NOENT => E_NOENT,
        CAT_ERR_PERM => E_PERM,
        CAT_ERR_SHAPE | CAT_ERR_TYPE | CAT_ERR_QUERY | CAT_ERR_CONSTRAINT => E_INVAL,
        CAT_ERR_CONFLICT | CAT_ERR_BUSY => E_CONFLICT,
        CAT_ERR_INTERNAL | CAT_EKERNEL | CAT_ECLOSED => E_IO,
        CAT_EPROTO => E_PROTO,
        CAT_ERR_UNKNOWN | CAT_ENOCHAN => E_NOSYS,
        _ => E_NOSYS,
    }
}

/// The first cut is one statement. This is the sidecar's twin of the
/// kernel's post-copy verdict: a semicolon outside a single-quoted string
/// would name a second statement, while one inside a literal is data. The
/// kernel refuses it before any post; this arm repeats the rule so neither
/// end is load-bearing alone.
fn catalog_sql_one_statement(sql: &[u8]) -> bool {
    let mut in_quote = false;
    let mut i = 0;
    while i < sql.len() {
        let c = sql[i];
        if in_quote {
            if c == b'\'' {
                if i + 1 < sql.len() && sql[i + 1] == b'\'' {
                    i += 1;             /* doubled quote: still inside */
                } else {
                    in_quote = false;
                }
            }
        } else if c == b'\'' {
            in_quote = true;
        } else if c == b';' {
            return false;
        }
        i += 1;
    }
    true
}

fn answer_catalog_exec(
    catalog: &mut Option<&mut Box<dyn CatalogOps>>,
    args: &[u64; 6],
    payload: &[u8],
) -> Answer {
    let len = args[1];
    if args[2] != 0 || len == 0 || len > CAT_SQL_MAX as u64 ||
        payload.len() != len as usize
    {
        return Answer::Reply(0, E_INVAL);
    }
    let sql = &payload[..len as usize];
    if sql.contains(&0) || !catalog_sql_one_statement(sql) {
        return Answer::Reply(0, E_INVAL);
    }
    let Some(client) = catalog.as_mut() else {
        return Answer::Reply(0, E_NOSYS);
    };
    match client.cat_exec(sql) {
        Ok(exec) => Answer::Reply(exec.affected_rows as u64, STATUS_OK),
        Err(refusal) => Answer::Reply(0, catalog_status_to_cap(refusal.status)),
    }
}

pub fn service<K: Kernel, A: BufferAlloc>(
    k: &K,
    console: &CharNode,
    vfs: &mut Vfs<K, A>,
    net: Option<&mut Box<dyn SocketOps>>,
    catalog: Option<&mut Box<dyn CatalogOps>>,
) {
    /* One clock sample per pass (E7 increment 24): every parked poll's
     * deadline below is measured against the same now, so two waits that
     * expire together resolve in the same pass. */
    let now_ns = k.now_ns();
    /* M1: reborrowed per task, never held across an iteration, so every
     * Linux task's network arm reaches the one manifest-owned client. */
    let mut net = net;
    /* M5.2: reborrowed per task exactly like `net`, so every CAT_EXEC
     * reaches the one client the ProcManager installed at boot. */
    let mut catalog = catalog;
    for i in 0..MAX_LINUX_TASKS {
        if !tasks()[i].used {
            continue;
        }
        let gid = tasks()[i].group;
        if gid >= MAX_LINUX_GROUPS || !groups()[gid].used {
            /* A live task without its group cannot share anything; drop
             * the orphan rather than answer from a retired table. */
            tasks()[i].used = false;
            continue;
        }
        /* M4.10: a stop latched on a parked sibling holds its pending
         * exchange. The target's channel is not polled until SIGCONT
         * clears the latch and answers the interrupted syscall. */
        if tasks()[i].stopped {
            continue;
        }
        let ch_r = tasks()[i].ch_r;
        let ch_w = tasks()[i].ch_w;
        let partition = tasks()[i].partition;
        let mut name_buf = [0u8; 48];
        let nlen = tasks()[i].name_len;
        name_buf[..nlen].copy_from_slice(&tasks()[i].name[..nlen]);

        /* E7 increment 24 (the waiting half): a parked exchange has no
         * queued request — the child is blocked in the kernel awaiting
         * this reply — so readiness and the deadline are checked BEFORE
         * the recv. Still not resolvable: stay parked (a finite poll's
         * wake comes from the entry loop's next_wait_ns deadline; a
         * blocking read's from console input on the channel set). */
        match tasks()[i].pending {
            PENDING_READ => {
                let tag = tasks()[i].pending_tag;
                let want = tasks()[i].pending_arg as usize;
                match console_read_answer(console, want) {
                    Answer::ReadWait(_) => {} /* still empty: stay parked */
                    Answer::ReplyBytes(ret, len, bytes) => {
                        tasks()[i].pending = PENDING_NONE;
                        tasks()[i].pending_num = 0;
                        enosys_log(LINUX_READ as u32, &name_buf[..nlen],
                                   b" ok + payload (waited)\n");
                        reply_bytes(k, ch_w, tag, LINUX_READ, ret,
                                    &bytes[..len]);
                    }
                    Answer::Reply(ret, status) => {
                        tasks()[i].pending = PENDING_NONE;
                        tasks()[i].pending_num = 0;
                        enosys_log(LINUX_READ as u32, &name_buf[..nlen],
                                   b" read wait failed\n");
                        reply(k, ch_w, tag, LINUX_READ, ret, status);
                    }
                    _ => {
                        tasks()[i].pending = PENDING_NONE;
                        tasks()[i].pending_num = 0;
                        reply(k, ch_w, tag, LINUX_READ, 0, E_NOSYS);
                    }
                }
                if tasks()[i].pending != PENDING_NONE {
                    reap_pending_if_closed(k, i, vfs);
                    continue;
                }
            }
            PENDING_POLL => {
                let tag = tasks()[i].pending_tag;
                let nfds = tasks()[i].pending_arg as usize;
                let len = tasks()[i].pending_len;
                let deadline = tasks()[i].pending_deadline;
                let vt = tasks()[i].vfs_task;
                let fds_copy = if tasks()[i].owns_fds {
                    tasks()[i].fds
                } else {
                    groups()[gid].fds
                };
                let mut arr = tasks()[i].pending_bytes;
                let ready = poll_scan(&fds_copy, &mut net, &mut arr, nfds, console,
                    &mut |vfs_fd, events| {
                        let mut pf = [PollFd { fd: vfs_fd, events,
                                              revents: 0 }];
                        match vfs.poll(vt, &mut pf) {
                            Ok(_) => Some(pf[0].revents),
                            Err(_) => None,
                        }
                    });
                if ready > 0 || poll_expired(deadline, now_ns) {
                    tasks()[i].pending = PENDING_NONE;
                    tasks()[i].pending_num = 0;
                    if len == 0 {
                        enosys_log(LINUX_POLL as u32, &name_buf[..nlen],
                                   b" poll timeout (no fds)\n");
                        reply(k, ch_w, tag, LINUX_POLL, 0, STATUS_OK);
                    } else {
                        enosys_log(LINUX_POLL as u32, &name_buf[..nlen],
                                   b" poll wake\n");
                        reply_bytes(k, ch_w, tag, LINUX_POLL,
                                    ready as u64, &arr[..len]);
                    }
                } else {
                    reap_pending_if_closed(k, i, vfs);
                    continue;
                }
            }
            PENDING_FUTEX => {
                let tag = tasks()[i].pending_tag;
                let deadline = tasks()[i].pending_deadline;
                if deadline != 0 && poll_expired(deadline, now_ns) {
                    tasks()[i].pending = PENDING_NONE;
                    tasks()[i].pending_num = 0;
                    futex_remove(&mut groups()[gid], i);
                    enosys_log(LINUX_FUTEX as u32, &name_buf[..nlen],
                               b" wait timed out\n");
                    reply(k, ch_w, tag, LINUX_FUTEX, 0, E_TIMEOUT);
                } else {
                    reap_pending_if_closed(k, i, vfs);
                    continue;
                }
            }
            _ => {}
        }

        /* The recv buffer now carries the fd class's attached bytes
         * behind the header: 72 + the mirror of LINUX_FD_WRITE_CHUNK.
         * Undersize would be ERR_BUFSZ — which does NOT consume the
         * message (transport spec §3.4) — so this capacity is what stops
         * a write from wedging its own channel. */
        let mut buf = [0u8; LINUX_SYSCALL_REQ_BYTES + RECV_ATTACH_CAP];
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
                /* The thread's end is gone: release its VFS task and its
                 * group reference. The kernel has already zeroed its
                 * clear-child-tid word; wake the joiner before the slot can
                 * be reused. A sibling still holds the group and the shared
                 * CLONE_FILES table alive; the last release retires both. */
                wake_clear_tid(k, gid, i);
                release_task(i, vfs);
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
        let (lo, hi) = expected_req_range(num, &args);
        if num == LINUX_WRITE
            && (args[2] as usize > FD_PAYLOAD_CAP || rr.len != lo)
        {
            enosys_log(num as u32, &name_buf[..nlen], b"[POSIX] write payload mismatch -> -EPROTO\n");
            reply(k, ch_w, rr.tag, 0, 0, E_NOSYS);
            continue;
        }
        /* The path numbers carry a VARIABLE attach (a string's own
         * length, both strings for rename) — a RANGE where every other
         * number has one length. Its bounds are the twin of the
         * kernel's path plan: at least one NUL's byte, at most the cap
         * (two for rename); the arm below validates the NULs
         * themselves, so an over-long or unterminated string is refused
         * by name rather than read past. */
        if rr.len < lo || rr.len > hi {
            enosys_log(0xBAD0, &name_buf[..nlen], b"[POSIX] malformed request -> -EPROTO\n");
            reply(k, ch_w, rr.tag, 0, 0, E_NOSYS);
            continue;
        }
        let payload: &[u8] = &buf[LINUX_SYSCALL_REQ_BYTES..rr.len];

        /* M4.3: the group is the authority for fds, heap and signal
         * dispositions. A thread without CLONE_FILES reads its private
         * table instead; every other task reads the one group table. */
        let mut fds = if tasks()[i].owns_fds {
            tasks()[i].fds
        } else {
            groups()[gid].fds
        };
        let mut mem = MemBrk {
            base: groups()[gid].brk_base,
            limit: groups()[gid].brk_limit,
            cur: groups()[gid].brk_cur,
            mtop: groups()[gid].brk_mtop,
            image_lo: groups()[gid].image_lo,
            image_hi: groups()[gid].image_hi,
        };
        let mut sigstate = tasks()[i].sig; /* per-thread half: mask, */
                                           /* pending, tid, alt stack */
        sigstate.dispo = groups()[gid].sig_dispo;
        sigstate.act = groups()[gid].sig_act;

        /* E7 step 4 (the file class): the thirteen numbers route to the
         * VFS bridge — every DECISION (rights, flags, errno) is the
         * VFS's, and this module translates. Everything else stays the
         * pure answer() it has always been; the fd table is written
         * back like brk's ledger (each arm mutates only what it owns:
         * open mints a slot, close spends it, fcntl flips one bit).
         * read (0) joins the bridge here — the fd table lives in this
         * module, so the fd class's read arm cannot stay kernel-side
         * (its data crosses back as the reply's payload, count-truth). */
        let mut ans = match num {
            /* M4.2: the thread-create seam. It is deliberately its own arm:
             * it needs the kernel primitive, and it is the only arm that
             * grows the sidecar's task table from inside service(). */
            LINUX_CLONE => answer_clone(k, i, &args, vfs, LINUX_CLONE),
            LINUX_CLONE3 => answer_clone3(k, i, payload, vfs),
            /* M4.4: the group-keyed wait/wake queue. The observed word and
             * relative timeout are the kernel's attached record; the group
             * owns the queue, and the deferred replies are sent below. */
            LINUX_FUTEX => answer_futex(&mut groups()[gid], i, &args,
                                        payload, now_ns),
            /* M1: the network class's eight numbers route to their own
             * translation over SocketOps; the file class below is
             * unchanged. */
            LINUX_SOCKET | LINUX_CONNECT | LINUX_ACCEPT | LINUX_SENDTO
            | LINUX_RECVFROM | LINUX_SHUTDOWN | LINUX_BIND | LINUX_LISTEN =>
                answer_net(&mut net, num, &args, &mut fds, payload),
            /* M5.2: the Linux CAT_EXEC path. It uses the existing
             * CatalogOps client through the environment's one catalog
             * channel — no second client, no kernel.env.catalog reach. */
            SYS_SLS_CATALOG_EXEC =>
                answer_catalog_exec(&mut catalog, &args, payload),
            LINUX_READ | LINUX_WRITE | LINUX_WRITEV | LINUX_OPEN | LINUX_CLOSE
            | LINUX_STAT | LINUX_FSTAT
            | LINUX_LSTAT | LINUX_LSEEK | LINUX_FCNTL | LINUX_FSYNC
            | LINUX_FTRUNCATE | LINUX_RENAME | LINUX_MKDIR | LINUX_UNLINK
            | LINUX_GETDENTS64 | LINUX_POLL | LINUX_MMAP_FILL =>
                answer_file(num, &args, &mut fds, tasks()[i].vfs_task, vfs,
                            &mut net, payload, console, now_ns, groups()[gid].gate),
            LINUX_TKILL => match answer_tkill_sibling(gid, sigstate.tid, &args) {
                Some(a) => a,
                None => answer(num, &args, &fds, &mut mem, &mut sigstate, payload),
            },
            LINUX_TGKILL => match answer_tgkill(gid, sigstate.tid, &args) {
                Some(a) => a,
                None => answer(LINUX_TKILL,
                               &[sigstate.tid, args[2], 0, 0, 0, 0],
                               &fds, &mut mem, &mut sigstate, payload),
            },
            LINUX_KILL => match answer_kill(gid, i, sigstate.tid, &args) {
                Some(a) => a,
                None => answer(LINUX_TKILL,
                               &[sigstate.tid, args[1], 0, 0, 0, 0],
                               &fds, &mut mem, &mut sigstate, payload),
            },
            _ => answer(num, &args, &fds, &mut mem, &mut sigstate, payload),
        };
        /* M4.5: set_tid_address(218) also records the word on the kernel
         * descriptor. The sidecar stores it for the join wake; the kernel
         * owns the actual zero at exit, through the checked copy-in. */
        if num == 218 {
            let name = core::str::from_utf8(&name_buf[..nlen]).unwrap_or("");
            let pid = k.sidecar_pid(name, partition);
            if pid != 0 {
                let _ = k.linux_set_clear_tid(pid, args[0]);
            }
        }
        /* M3.5: a send that the socket service refused with EPIPE is the
         * first asynchronous signal producer. The SIGPIPE decision runs
         * after the network arm and before the state is written back, so
         * a delivered handler's mask and a pended signal are both part
         * of the same exchange. */
        if num == LINUX_SENDTO {
            ans = sigpipe_after_epipe(&mut sigstate, ans);
        }
        if tasks()[i].owns_fds {
            tasks()[i].fds = fds;
        } else {
            groups()[gid].fds = fds;
        }
        /* The ledger survives the reply: only a granted growth moves it
         * (the arm's own rule), and the KERNEL's brk_cur is its own
         * twin — the probe's denial-echo check is what proves the two
         * never drift. The same holds for mtop (increment 16): only the
         * mmap arm descends it, and linux_mmap_top is its twin. The
         * base and limit are never written here: they are the span,
         * decided once, at spawn. The group owns all four numbers now. */
        groups()[gid].brk_cur = mem.cur;
        groups()[gid].brk_mtop = mem.mtop;
        /* E7 increment 10: the per-thread signal state survives the reply
         * the same way — mask, pending, tid and alt stack are each written
         * only by the arm that owns them. Dispositions are the group's:
         * they leave this thread's scratch copy and return to the group so
         * a sibling sees an rt_sigaction installed by any thread. */
        groups()[gid].sig_dispo = sigstate.dispo;
        groups()[gid].sig_act = sigstate.act;
        tasks()[i].sig = sigstate;

        /* M4.8: a group-directed default-terminate signal is the existing
         * exit_group decision, not a second sweep implementation. The log
         * names the signal before the fold so the serial line attributes
         * the death to kill(2), not to an unrelated exit_group. */
        if let Answer::SignalGroupKill(gsig) = &ans {
            let mut ex = [0u8; 48];
            let n = fmt_sig_line(&mut ex, b" group sig=", *gsig,
                                 b" -> exit_group sweep\n");
            enosys_log(num as u32, &name_buf[..nlen], &ex[..n]);
            ans = Answer::ExitGroup;
        }

        match ans {
            Answer::MmapFile(ret, vfd, foff, flen) => {
                /* M2b: the grant line names the SOURCE (fd and offset) —
                 * the run's proof that the pages came from the VFS file
                 * the loader asked for, not from a lookalike. The chunks
                 * go first so the kernel's wake (the reply) is never
                 * ahead of its data. */
                let mut ex = [0u8; 96];
                let mut off = push_bytes(&mut ex, 0, b" ok mmap -> 0x");
                off = hex_into(&mut ex, off, ret);
                off = push_bytes(&mut ex, off, b" fd=");
                off = dec_into(&mut ex, off, args[4]);
                off = push_bytes(&mut ex, off, b" off=0x");
                off = hex_into(&mut ex, off, foff);
                off = push_bytes(&mut ex, off, b"\n");
                enosys_log(num as u32, &name_buf[..nlen], &ex[..off]);
                let _ = (vfd, flen);   /* the rounds carry these words */
                reply(k, ch_w, rr.tag, num, ret, STATUS_OK);
            }
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
                } else if num == LINUX_MUNMAP && status != STATUS_OK {
                    /* Munmap's refusal family (increment 25): EINVAL is
                     * the only arm a well-formed request reaches — the
                     * span, the alignment, the break or the length,
                     * with the request's own words on the line. */
                    let mut ex = [0u8; 96];
                    let mut off = push_bytes(&mut ex, 0, b" -> ");
                    off = push_bytes(&mut ex, off,
                        if status == E_INVAL { b"EINVAL munmap(addr=0x" }
                        else               { b"refused munmap(addr=0x" });
                    off = hex_into(&mut ex, off, args[0]);
                    off = push_bytes(&mut ex, off, b", len=");
                    off = dec_into(&mut ex, off, args[1]);
                    off = push_bytes(&mut ex, off, b")\n");
                    enosys_log(num as u32, &name_buf[..nlen], &ex[..off]);
                } else if num == LINUX_MUNMAP {
                    /* Both success and a double unmap land here: the
                     * kernel's page tables are the truth, and 0 is the
                     * answer either way. The request words ride the
                     * line so the run's proof names the range, not the
                     * arm (increment 14/15's rule). */
                    let mut ex = [0u8; 96];
                    let mut off = push_bytes(&mut ex, 0, b" ok munmap addr=0x");
                    off = hex_into(&mut ex, off, args[0]);
                    off = push_bytes(&mut ex, off, b" len=");
                    off = dec_into(&mut ex, off, args[1]);
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
                } else if num == LINUX_IOCTL {
                    /* The stdio ioctl arm is only meaningful if the request
                     * is named: the fd and the request word ride the line,
                     * and a refusal carries its own errno (the console/no
                     * console split is what the arm decided). */
                    let mut ex = [0u8; 64];
                    let mut off;
                    if status == STATUS_OK {
                        off = push_bytes(&mut ex, 0, b" ok ioctl fd=");
                    } else {
                        off = push_bytes(&mut ex, 0, b" -> ");
                        off = push_bytes(&mut ex, off, errno_name(status));
                        off = push_bytes(&mut ex, off, b" ioctl fd=");
                    }
                    off = dec_into(&mut ex, off, args[0]);
                    off = push_bytes(&mut ex, off, b" req=0x");
                    off = hex_into(&mut ex, off, args[1]);
                    enosys_log(num as u32, &name_buf[..nlen], &ex[..off]);
                } else {
                    /* An answered syscall must be visible too: this line
                     * is the serial-log half of E7's round-trip proof
                     * (a real task's real request arrived, and its reply
                     * is on its way back through §5.7). An ERROR carries
                     * its own name rather than the old blanket "ok", and
                     * a failed open also carries the PATH it tried — the
                     * request's own bytes name the missing object, not a
                     * reconstruction from search order. */
                    let mut ex = [0u8; 160];
                    let mut off;
                    if status == STATUS_OK {
                        off = push_bytes(&mut ex, 0, b" ok\n");
                    } else {
                        off = push_bytes(&mut ex, 0, b" -> ");
                        off = push_bytes(&mut ex, off, errno_name(status));
                        if num == LINUX_OPEN {
                            if let Some(path) = path_arg(payload, 0) {
                                off = push_bytes(&mut ex, off, b" open ");
                                off = push_bytes(&mut ex, off, path.as_bytes());
                            }
                        }
                        off = push_bytes(&mut ex, off, b"\n");
                    }
                    enosys_log(num as u32, &name_buf[..nlen], &ex[..off]);
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
                /* The bound is now per-number (the sig class's exact
                 * size, stat's compact 24, read's own count, getdents'
                 * cap) — payload_bound derives it from the same words
                 * the kernel's reply plan stages, so a drifted clamp
                 * arrives here as a mismatch and goes out as num=0. */
                if !payload_bound(num, &args, len) || len > bytes.len() {
                    enosys_log(0xBAD1, &name_buf[..nlen],
                               b"[POSIX] reply payload mismatch -> -EPROTO\n");
                    reply(k, ch_w, rr.tag, 0, 0, E_NOSYS);
                    continue;
                }
                enosys_log(num as u32, &name_buf[..nlen], b" ok + payload\n");
                reply_bytes(k, ch_w, rr.tag, num, ret, &bytes[..len]);
            }
            Answer::ReplyFill(ret, len, bytes) => {
                /* M2b: the same bound story as ReplyBytes, against the
                 * fill cap — the round must be exactly the kernel's ask
                 * and no larger than one message; a mismatch goes out as
                 * num=0 and the kernel refuses -EPROTO rather than
                 * leaving a partially backed mapping behind. */
                if !payload_bound(num, &args, len) || len > bytes.len() {
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
                        /* Increment 19: the gate tenant's console is its OWN
                         * environment console, which no attach surface is
                         * reading in this run. M5.2a's ordinary-tenant test
                         * profile has the same observability problem. Mirror
                         * the child's bytes to the kernel serial log so the
                         * work-half measurement (Copying / Done / pruned) and
                         * the tenant probe's marker are observed, not inferred
                         * from a buffered stream nobody drained. */
                        #[cfg(feature = "target")]
                        if groups()[gid].mirror_console {
                            crate::entry::serial_write_raw(&payload[..len]);
                        }
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
            Answer::ReadWait(want) => {
                /* The console has no input yet (increment 24): park the
                 * request. Nothing is replied — the child is blocked in
                 * the kernel awaiting exactly this exchange — and the
                 * console channel is already in the entry loop's wait set,
                 * so the next typed byte wakes the sidecar and the pending
                 * block above answers from it. */
                tasks()[i].pending = PENDING_READ;
                tasks()[i].pending_num = LINUX_READ;
                tasks()[i].pending_tag = rr.tag;
                tasks()[i].pending_arg = want as u64;
                tasks()[i].pending_deadline = 0;
                enosys_log(num as u32, &name_buf[..nlen],
                           b" wait (console read)\n");
            }
            Answer::PollWait(arr, len, deadline) => {
                /* Nothing ready and the timeout allows waiting (increment
                 * 23): park the array and its deadline. The entry loop
                 * passes next_wait_ns to Kernel::wait, so a finite deadline
                 * wakes this loop; an infinite one wakes on console input
                 * like the read above. On wake the reply is the array with
                 * the revents this side actually observed (a timeout's
                 * array carries zeros — never invented readiness). */
                tasks()[i].pending = PENDING_POLL;
                tasks()[i].pending_num = LINUX_POLL;
                tasks()[i].pending_tag = rr.tag;
                tasks()[i].pending_arg = args[1];
                tasks()[i].pending_len = len;
                tasks()[i].pending_bytes = arr;
                tasks()[i].pending_deadline = deadline;
                enosys_log(num as u32, &name_buf[..nlen],
                           b" wait (poll)\n");
            }
            Answer::FutexWait(uaddr, deadline) => {
                tasks()[i].pending = PENDING_FUTEX;
                tasks()[i].pending_num = LINUX_FUTEX;
                tasks()[i].pending_tag = rr.tag;
                tasks()[i].pending_arg = uaddr;
                tasks()[i].pending_deadline = deadline;
                let mut ex = [0u8; 56];
                let mut off = push_bytes(&mut ex, 0, b" wait (futex) uaddr=0x");
                off = hex_into(&mut ex, off, uaddr);
                off = push_bytes(&mut ex, off, b" q=");
                off = dec_into(&mut ex, off,
                               groups()[gid].futex.iter()
                                   .filter(|w| w.used).count() as u64);
                off = push_bytes(&mut ex, off, b"\n");
                enosys_log(num as u32, &name_buf[..nlen], &ex[..off]);
            }
            Answer::FutexWake(w) => {
                for &idx in w.tasks[..w.n as usize].iter() {
                    let j = idx as usize;
                    if j >= MAX_LINUX_TASKS || !tasks()[j].used ||
                        tasks()[j].pending != PENDING_FUTEX {
                        continue;
                    }
                    let wake_tag = tasks()[j].pending_tag;
                    let wake_ch = tasks()[j].ch_w;
                    tasks()[j].pending = PENDING_NONE;
                    reply(k, wake_ch, wake_tag, LINUX_FUTEX, 0, STATUS_OK);
                }
                enosys_log(num as u32, &name_buf[..nlen], b" wake (futex)\n");
                reply(k, ch_w, rr.tag, num, w.n as u64, STATUS_OK);
            }
            Answer::FutexRequeue(w, to, moved) => {
                for &idx in w.tasks[..w.n as usize].iter() {
                    let j = idx as usize;
                    if j >= MAX_LINUX_TASKS || !tasks()[j].used ||
                        tasks()[j].pending != PENDING_FUTEX {
                        continue;
                    }
                    let wake_tag = tasks()[j].pending_tag;
                    let wake_ch = tasks()[j].ch_w;
                    tasks()[j].pending = PENDING_NONE;
                    reply(k, wake_ch, wake_tag, LINUX_FUTEX, 0, STATUS_OK);
                }
                let mut ex = [0u8; 48];
                let mut off = push_bytes(&mut ex, 0, b" requeue from=0x");
                off = hex_into(&mut ex, off, args[0]);
                off = push_bytes(&mut ex, off, b" -> 0x");
                off = hex_into(&mut ex, off, to);
                off = push_bytes(&mut ex, off, b" woke=");
                off = dec_into(&mut ex, off, w.n as u64);
                off = push_bytes(&mut ex, off, b" moved=");
                off = dec_into(&mut ex, off, moved as u64);
                off = push_bytes(&mut ex, off, b"\n");
                enosys_log(num as u32, &name_buf[..nlen], &ex[..off]);
                reply(k, ch_w, rr.tag, num, w.n as u64, STATUS_OK);
            }
            Answer::ExitThread => {
                /* M4.5: exit(60) ends only this thread. The kernel's
                 * teardown zeroes clear_child_tid through its checked
                 * copy-in; the sidecar then wakes the join futex and drops
                 * only this task, leaving siblings and the group alive. */
                let mut ex = [0u8; 48];
                let n = fmt_sig_line(&mut ex, b" exit status=",
                                     args[0], b"\n");
                enosys_log(num as u32, &name_buf[..nlen], &ex[..n]);
                let name = core::str::from_utf8(&name_buf[..nlen]).unwrap_or("");
                let pid = k.sidecar_pid(name, partition);
                let (raddr, rn) = robust_exit_collect(k, i);
                if pid == 0 {
                    enosys_log(num as u32, &name_buf[..nlen], b" exit: NOT-FOUND\n");
                    reply(k, ch_w, rr.tag, num, 0, E_CONFLICT);
                } else {
                    k.proc_kill(pid);
                    wake_clear_tid(k, gid, i);
                    robust_wake(k, gid, &raddr, rn);
                    release_task(i, vfs);
                }
            }
            Answer::ExitGroup => {
                /* M4.5: exit_group(231) is a GROUP sweep, not a pid loop.
                 * The kernel resolves the TGID, terminates every member,
                 * and zeroes each clear-tid word; this side then wakes the
                 * joiners and retires every sidecar/VFS record. No reply:
                 * the caller is one of the tasks the sweep just ended. */
                let mut ex = [0u8; 48];
                let n = fmt_sig_line(&mut ex, b" exit_group status=",
                                     args[0], b"\n");
                enosys_log(num as u32, &name_buf[..nlen], &ex[..n]);

                let tgid = groups()[gid].tgid;
                let mut members = [0usize; MAX_LINUX_TASKS];
                let mut robust = [[0u64; 8]; MAX_LINUX_TASKS];
                let mut robust_n = [0usize; MAX_LINUX_TASKS];
                let mut nm = 0usize;
                for j in 0..MAX_LINUX_TASKS {
                    if tasks()[j].used && tasks()[j].group == gid {
                        members[nm] = j;
                        let (a, n) = robust_exit_collect(k, j);
                        robust[nm] = a;
                        robust_n[nm] = n;
                        nm += 1;
                    }
                }
                /* Death proof before reap. The kernel's sweep returns
                 * CAP_EAGAIN while any member is runnable or a kill
                 * deferred; yield and retry so a CPU-bound sibling gets
                 * preempted. Success needs the kernel's proof AND every
                 * sidecar record resolve failing; a stale-TGID error is
                 * acceptable only when every record is already gone (an
                 * earlier partial sweep retired the group). A record that
                 * still resolves is never released: the group stays live and
                 * the caller is answered with EBUSY instead of being told the
                 * group was swept. */
                let mut dead = 0usize;
                let mut proved = false;
                for attempt in 0..GROUP_SWEEP_ATTEMPTS {
                    let result = k.linux_group_sweep(tgid as u32);
                    dead = nm - group_live_records(k, &members[..nm], partition);
                    match result {
                        Ok(_) => {
                            proved = true;
                            if dead != nm {
                                enosys_log(num as u32, &name_buf[..nlen],
                                           b" group sweep: kernel success but live record\n");
                            }
                        }
                        Err(E_AGAIN) => {
                            /* A runnable member or a deferred kill: the
                             * kernel has not proved the group dead, whatever
                             * the records say. Yield and try again. */
                            proved = false;
                        }
                        Err(_) => {
                            /* A stale TGID can mean an earlier partial sweep
                             * already retired the group; the records are the
                             * only remaining proof in that case. */
                            proved = dead == nm;
                            break;
                        }
                    }
                    if proved && dead == nm {
                        break;
                    }
                    if attempt + 1 < GROUP_SWEEP_ATTEMPTS {
                        k.sched_yield();
                    }
                }
                if dead != nm || !proved {
                    enosys_log(num as u32, &name_buf[..nlen],
                               b" group sweep FAILED: live member not reaped\n");
                    /* Answer the caller, which is still parked on 231 and
                     * still owns a live record. Do not release live records:
                     * doing so would be the false kill this arm exists to
                     * prevent. Dead records may still be retired. */
                    reply(k, ch_w, rr.tag, num, 0, E_CONFLICT);
                    for &j in members[..nm].iter() {
                        if task_is_dead(k, j, partition) {
                            wake_clear_tid(k, gid, j);
                            let pos = members[..nm].iter().position(|&m| m == j).unwrap_or(0);
                            robust_wake(k, gid, &robust[pos], robust_n[pos]);
                            release_task(j, vfs);
                        }
                    }
                    let mut ex = [0u8; 64];
                    let mut off = push_bytes(&mut ex, 0, b" group sweep FAILED live=");
                    off = dec_into(&mut ex, off, (nm - dead) as u64);
                    off = push_bytes(&mut ex, off, b" not reaped\n");
                    enosys_log(num as u32, &name_buf[..nlen], &ex[..off]);
                    continue;
                }
                for (pos, &j) in members[..nm].iter().enumerate() {
                    wake_clear_tid(k, gid, j);
                    robust_wake(k, gid, &robust[pos], robust_n[pos]);
                    release_task(j, vfs);
                }
                let mut ex = [0u8; 64];
                let mut off = push_bytes(&mut ex, 0, b" group swept=");
                off = dec_into(&mut ex, off, nm as u64);
                off = push_bytes(&mut ex, off, b" dead=");
                off = dec_into(&mut ex, off, dead as u64);
                off = push_bytes(&mut ex, off, b" live=0\n");
                enosys_log(num as u32, &name_buf[..nlen], &ex[..off]);
                /* Every record (including the caller's) was just released;
                 * do not fall through to the per-task state write-back. */
                continue;
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
                let (raddr, rn) = robust_exit_collect(k, i);
                if pid == 0 {
                    enosys_log(num as u32, &name_buf[..nlen], b" sig kill: NOT-FOUND (reap refused)\n");
                    reply(k, ch_w, rr.tag, num, 0, E_CONFLICT);
                } else {
                    k.proc_kill(pid);
                    wake_clear_tid(k, gid, i);
                    robust_wake(k, gid, &raddr, rn);
                    release_task(i, vfs);
                }
            }
            Answer::SignalKillTask(tid, kill_sig) => {
                /* M4.8: tgkill/kill addressed a SIBLING. The target's own
                 * robust list and clear-tid are consumed before its record
                 * is released; the sender receives 0 because the kernel's
                 * signal delivery did not kill the sender. */
                let Some(j) = task_by_tid(gid, tid) else {
                    reply(k, ch_w, rr.tag, num, 0, E_TARGET);
                    continue;
                };
                let name = core::str::from_utf8(&name_buf[..nlen]).unwrap_or("");
                let pid = k.sidecar_pid(name, partition);
                if pid == 0 {
                    reply(k, ch_w, rr.tag, num, 0, E_TARGET);
                    continue;
                }
                let (raddr, rn) = robust_exit_collect(k, j);
                k.proc_kill(tid);
                wake_clear_tid(k, gid, j);
                robust_wake(k, gid, &raddr, rn);
                release_task(j, vfs);
                let mut ex = [0u8; 48];
                let n = fmt_sig_line(&mut ex, b" sibling sig=", kill_sig,
                                     b" -> kill\n");
                enosys_log(num as u32, &name_buf[..nlen], &ex[..n]);
                reply(k, ch_w, rr.tag, num, 0, STATUS_OK);
            }
            Answer::SignalGroupKill(_) => {
                /* The pre-match transform below folds this into ExitGroup;
                 * this arm is unreachable but keeps the match total. */
                reply(k, ch_w, rr.tag, num, 0, E_CONFLICT);
            }
            Answer::SignalHandlerTask(tid, d) => {
                /* M4.10: the trailer rides the TARGET's pending reply, not
                 * the caller's. `pending_num` is the parked syscall the
                 * kernel will interrupt (futex/read/poll); the target's
                 * saved park_ctx is what builds the handler frame. */
                let Some(j) = task_by_tid(gid, tid) else {
                    reply(k, ch_w, rr.tag, num, 0, E_TARGET);
                    continue;
                };
                if tasks()[j].pending == PENDING_NONE || tasks()[j].stopped {
                    reply(k, ch_w, rr.tag, num, 0, E_CONFLICT);
                    continue;
                }
                let target_tag = tasks()[j].pending_tag;
                let target_ch = tasks()[j].ch_w;
                let target_num = tasks()[j].pending_num;
                tasks()[j].pending = PENDING_NONE;
                tasks()[j].pending_num = 0;
                tasks()[j].stopped = false;
                let bytes = signal_delivery_payload(&d);
                let mut ex = [0u8; 56];
                let n = fmt_sig_line(&mut ex, b" sibling sig=", d.signo,
                                     b" handler -> frame\n");
                enosys_log(num as u32, &name_buf[..nlen], &ex[..n]);
                reply_bytes(k, target_ch, target_tag, target_num, 0, &bytes);
                reply(k, ch_w, rr.tag, num, 0, STATUS_OK);
            }
            Answer::SignalHandlerRun(tid, d) => {
                /* M4.11: the target is between syscalls, so there is no
                 * pending reply to carry the trailer. Ask the kernel to
                 * install the frame into that task's saved ring3 context;
                 * CAP_EAGAIN means the timer has not saved it yet. */
                let Some(j) = task_by_tid(gid, tid) else {
                    reply(k, ch_w, rr.tag, num, 0, E_TARGET);
                    continue;
                };
                if tasks()[j].pending != PENDING_NONE || tasks()[j].stopped {
                    reply(k, ch_w, rr.tag, num, 0, E_CONFLICT);
                    continue;
                }
                let bytes = signal_delivery_payload(&d);
                let tgid = groups()[gid].tgid as u32;
                let mut delivered = false;
                for _ in 0..64 {
                    match k.linux_signal_task(tgid, tid, &bytes) {
                        Ok(()) => { delivered = true; break; }
                        Err(E_AGAIN) => k.sched_yield(),
                        Err(_) => break,
                    }
                }
                if !delivered {
                    reply(k, ch_w, rr.tag, num, 0, E_CONFLICT);
                    continue;
                }
                let mut ex = [0u8; 64];
                let n = fmt_sig_line(&mut ex, b" sibling sig=", d.signo,
                                     b" handler -> running frame\n");
                enosys_log(num as u32, &name_buf[..nlen], &ex[..n]);
                reply(k, ch_w, rr.tag, num, 0, STATUS_OK);
            }
            Answer::SignalStopTask(tid, stop_sig) => {
                /* The target's deferred reply is deliberately withheld:
                 * it is parked, and nothing may answer it until SIGCONT
                 * releases the stop latch. */
                let Some(j) = task_by_tid(gid, tid) else {
                    reply(k, ch_w, rr.tag, num, 0, E_TARGET);
                    continue;
                };
                if tasks()[j].pending == PENDING_NONE {
                    reply(k, ch_w, rr.tag, num, 0, E_CONFLICT);
                    continue;
                }
                tasks()[j].stopped = true;
                futex_remove(&mut groups()[gid], j);
                let mut ex = [0u8; 48];
                let n = fmt_sig_line(&mut ex, b" sibling sig=", stop_sig,
                                     b" -> stop (pending reply held)\n");
                enosys_log(num as u32, &name_buf[..nlen], &ex[..n]);
                reply(k, ch_w, rr.tag, num, 0, STATUS_OK);
            }
            Answer::SignalStopGroup(grp) => {
                /* M4.11: every non-caller sibling was latched by
                 * stop_group. A parked target's reply stays withheld; a
                 * target between syscalls has no reply, so hold its kernel
                 * task until SIGCONT. The hold is transactional: one refusal
                 * rolls back every held and latched sibling before the
                 * caller sees E_CONFLICT. */
                let held = match hold_stop_group(k, gid, &grp) {
                    Ok(h) => h,
                    Err(status) => {
                        let mut ex = [0u8; 64];
                        let n = push_bytes(&mut ex, 0,
                            b" M4.11 group stop refused -> rollback\n");
                        enosys_log(num as u32, &name_buf[..nlen], &ex[..n]);
                        reply(k, ch_w, rr.tag, num, 0, status);
                        continue;
                    }
                };
                for &idx in grp.tasks[..grp.n as usize].iter() {
                    let j = idx as usize;
                    if j >= MAX_LINUX_TASKS || !tasks()[j].used ||
                        tasks()[j].group != gid ||
                        tasks()[j].pending == PENDING_NONE {
                        continue;
                    }
                    let mut ex = [0u8; 48];
                    let n = fmt_sig_line(&mut ex, b" sibling sig=", grp.sig,
                                         b" -> stop (pending reply held)\n");
                    enosys_log(num as u32, &name_buf[..nlen], &ex[..n]);
                }
                for &idx in grp.tasks[..grp.n as usize].iter() {
                    let j = idx as usize;
                    if j >= MAX_LINUX_TASKS || !tasks()[j].used ||
                        tasks()[j].group != gid || !tasks()[j].run_held {
                        continue;
                    }
                    let mut ex = [0u8; 48];
                    let n = fmt_sig_line(&mut ex, b" sibling sig=", grp.sig,
                                         b" -> stop (kernel hold)\n");
                    enosys_log(num as u32, &name_buf[..nlen], &ex[..n]);
                }
                let mut ex = [0u8; 96];
                let mut off = push_bytes(&mut ex, 0, b" M4.11 group stop broadcast targets=");
                off = dec_into(&mut ex, off, grp.n as u64);
                off = push_bytes(&mut ex, off, b" parked=");
                off = dec_into(&mut ex, off, grp.parked as u64);
                off = push_bytes(&mut ex, off, b" held=");
                off = dec_into(&mut ex, off, held as u64);
                off = push_bytes(&mut ex, off, b"\n");
                enosys_log(num as u32, &name_buf[..nlen], &ex[..off]);
                reply(k, ch_w, rr.tag, num, 0, STATUS_OK);
            }
            Answer::SignalContinue(c) => {
                /* SIGCONT resumes each stopped target at its syscall
                 * boundary: a parked exchange is answered -EINTR, a task
                 * held between requests is released by the kernel. */
                for &idx in c.tasks[..c.n as usize].iter() {
                    let j = idx as usize;
                    if j >= MAX_LINUX_TASKS || !tasks()[j].used ||
                        tasks()[j].group != gid {
                        continue;
                    }
                    tasks()[j].stopped = false;
                    if tasks()[j].run_held {
                        let tid = tasks()[j].sig.tid as u32;
                        let _ = k.proc_release(tid);
                        tasks()[j].run_held = false;
                        let mut ex = [0u8; 48];
                        let n = push_bytes(&mut ex, 0,
                            b" SIGCONT -> sibling released (kernel hold)\n");
                        enosys_log(num as u32, &name_buf[..nlen], &ex[..n]);
                    }
                    if tasks()[j].pending != PENDING_NONE {
                        let target_tag = tasks()[j].pending_tag;
                        let target_ch = tasks()[j].ch_w;
                        let target_num = tasks()[j].pending_num;
                        tasks()[j].pending = PENDING_NONE;
                        tasks()[j].pending_num = 0;
                        reply(k, target_ch, target_tag, target_num, 0, E_INTR);
                    }
                }
                enosys_log(num as u32, &name_buf[..nlen],
                           b" SIGCONT -> sibling resumed (-EINTR)\n");
                reply(k, ch_w, rr.tag, num, 0, STATUS_OK);
            }
            Answer::SignalHandler(d) => {
                /* M3: the sidecar has already moved its mask to the
                 * handler mask; the kernel builds the frame from this
                 * trailer and sysrets into the handler. The oldset word
                 * is the pre-call mask for rt_sigprocmask's staged
                 * out-pointer; tkill has no stage and the kernel ignores
                 * it there. */
                let bytes = signal_delivery_payload(&d);
                let mut ex = [0u8; 56];
                let n = fmt_sig_line(&mut ex, b" sig=", d.signo,
                                     b" handler -> frame\n");
                enosys_log(num as u32, &name_buf[..nlen], &ex[..n]);
                reply_bytes(k, ch_w, rr.tag, num, 0, &bytes);
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
    use alloc::rc::Rc;
    use alloc::vec::Vec;
    use core::cell::{Cell, RefCell};
    use aerosls_proto::catalops::{CatPage, CatRefusal, CatResult};
    use aerosls_proto::{CatExec, CatInfo, CatRows, CatStat, CatValue};

    /// M5.2: one scripted CatalogOps. `calls` is the whole point — the
    /// syscall arm must reach the ONE client the ProcManager installed, not
    /// construct a second wire client. The recorded SQL lets the test pin
    /// that the bytes the kernel copied are the bytes the service receives.
    struct FakeCatalog {
        calls: Rc<Cell<u32>>,
        sql: Rc<RefCell<Vec<u8>>>,
        reply: CatResult<CatExec>,
    }

    impl FakeCatalog {
        fn ok(rows: u32, calls: Rc<Cell<u32>>, sql: Rc<RefCell<Vec<u8>>>) -> Self {
            FakeCatalog {
                calls,
                sql,
                reply: Ok(CatExec { affected_rows: rows, flags: 0 }),
            }
        }
        fn err(status: u16, calls: Rc<Cell<u32>>, sql: Rc<RefCell<Vec<u8>>>) -> Self {
            FakeCatalog {
                calls,
                sql,
                reply: Err(CatRefusal::new(status, "fake refusal")),
            }
        }
    }

    impl CatalogOps for FakeCatalog {
        fn cat_info(&mut self) -> CatResult<CatInfo> {
            Err(CatRefusal::new(CAT_ERR_UNKNOWN, "not used"))
        }
        fn cat_list(&mut self, _: u16, _: u16) -> CatResult<CatPage> {
            Err(CatRefusal::new(CAT_ERR_UNKNOWN, "not used"))
        }
        fn cat_stat(&mut self, _: &[u8]) -> CatResult<CatStat> {
            Err(CatRefusal::new(CAT_ERR_UNKNOWN, "not used"))
        }
        fn cat_read(&mut self, _: &[u8], _: &[u8]) -> CatResult<CatValue> {
            Err(CatRefusal::new(CAT_ERR_UNKNOWN, "not used"))
        }
        fn cat_query(&mut self, _: &[u8], _: u16, _: u8) -> CatResult<CatRows> {
            Err(CatRefusal::new(CAT_ERR_UNKNOWN, "not used"))
        }
        fn cat_exec(&mut self, sql: &[u8]) -> CatResult<CatExec> {
            self.calls.set(self.calls.get() + 1);
            *self.sql.borrow_mut() = sql.to_vec();
            self.reply
        }
    }

    fn catalog_args(len: u64, flags: u64) -> [u64; 6] {
        [0x1000, len, flags, 0, 0, 0]
    }

    /// M5.2: the syscall's sidecar arm calls the one CatalogOps client, and
    /// every CAT_ERR_* becomes the CAP family code whose §5.7 map delivers
    /// the Linux errno. A refused shape reaches NO client call.
    #[test]
    fn catalog_exec_reuses_one_client_and_maps_every_refusal() {
        let sql = b"INSERT INTO m52_bob (id) VALUES (1)";
        let calls = Rc::new(Cell::new(0));
        let seen = Rc::new(RefCell::new(Vec::new()));
        let mut boxed: Box<dyn CatalogOps> =
            Box::new(FakeCatalog::ok(3, calls.clone(), seen.clone()));
        let mut catalog = Some(&mut boxed);
        match answer_catalog_exec(&mut catalog, &catalog_args(sql.len() as u64, 0), sql) {
            Answer::Reply(3, 0) => {}
            other => panic!("expected the fake's affected rows, got {:?}", other),
        }
        assert_eq!(calls.get(), 1, "the arm called the installed client once");
        assert_eq!(&seen.borrow()[..], sql,
                   "the copied bytes reached that client unchanged");

        let cases = [
            (CAT_ERR_NOENT, E_NOENT),
            (CAT_ERR_PERM, E_PERM),
            (CAT_ERR_SHAPE, E_INVAL),
            (CAT_ERR_TYPE, E_INVAL),
            (CAT_ERR_QUERY, E_INVAL),
            (CAT_ERR_CONSTRAINT, E_INVAL),
            (CAT_ERR_CONFLICT, E_CONFLICT),
            (CAT_ERR_BUSY, E_CONFLICT),
            (CAT_ERR_INTERNAL, E_IO),
            (CAT_ERR_UNKNOWN, E_NOSYS),
            (CAT_EPROTO, E_PROTO),
            (CAT_EKERNEL, E_IO),
            (CAT_ECLOSED, E_IO),
            (CAT_ENOCHAN, E_NOSYS),
        ];
        for (status, want) in cases {
            let calls = Rc::new(Cell::new(0));
            let seen = Rc::new(RefCell::new(Vec::new()));
            let mut boxed: Box<dyn CatalogOps> =
                Box::new(FakeCatalog::err(status, calls.clone(), seen.clone()));
            let mut catalog = Some(&mut boxed);
            match answer_catalog_exec(&mut catalog, &catalog_args(sql.len() as u64, 0), sql) {
                Answer::Reply(0, got) => assert_eq!(got, want,
                    "CAT_ERR status {} maps to CAP {} (then a Linux errno)", status, want),
                other => panic!("status {} did not answer a refusal: {:?}", status, other),
            }
            assert_eq!(calls.get(), 1,
                       "a received refusal is still one call to the same client");
        }
    }

    /// M5.2: every malformed request is refused before the client is called.
    /// This is the sidecar twin of the kernel's pre-post copy/verdict gate.
    #[test]
    fn catalog_exec_bad_shape_never_reaches_the_client() {
        let sql = b"DELETE FROM m52_bob";
        let calls = Rc::new(Cell::new(0));
        let seen = Rc::new(RefCell::new(Vec::new()));
        let mut boxed: Box<dyn CatalogOps> =
            Box::new(FakeCatalog::ok(1, calls.clone(), seen.clone()));
        let mut catalog = Some(&mut boxed);

        for (args, payload, why) in [
            (catalog_args(0, 0), &sql[..0], "zero length"),
            (catalog_args(sql.len() as u64 + 1, 0), sql, "length mismatch"),
            (catalog_args(sql.len() as u64, 1), sql, "reserved flags"),
            (catalog_args(sql.len() as u64, 0), &sql[..sql.len() - 1], "short payload"),
        ] {
            match answer_catalog_exec(&mut catalog, &args, payload) {
                Answer::Reply(0, E_INVAL) => {}
                other => panic!("{} was not EINVAL: {:?}", why, other),
            }
        }
        match answer_catalog_exec(&mut catalog, &catalog_args(2, 0), b"a\0b") {
            Answer::Reply(0, E_INVAL) => {}
            other => panic!("a NUL was not EINVAL: {:?}", other),
        }
        let two = b"INSERT INTO m52_bob (id) VALUES (1); DELETE FROM m52_bob";
        match answer_catalog_exec(&mut catalog,
                                  &catalog_args(two.len() as u64, 0), two) {
            Answer::Reply(0, E_INVAL) => {}
            other => panic!("a second statement was not EINVAL: {:?}", other),
        }
        assert_eq!(calls.get(), 0,
                   "no malformed request may open a catalog call/transaction");

        let mut none: Option<&mut Box<dyn CatalogOps>> = None;
        match answer_catalog_exec(&mut none, &catalog_args(sql.len() as u64, 0), sql) {
            Answer::Reply(0, E_NOSYS) => {}
            other => panic!("a missing client was not ENOSYS: {:?}", other),
        }
    }

    /// M1: a SocketOps implementation whose answers are fixed, so the
    /// eight arms' TRANSLATION is what the test observes — the fd table,
    /// the return shapes and the 20-byte trailer, never a driver.
    struct FakeNet;

    impl SocketOps for FakeNet {
        fn socket(&mut self, _: u16) -> Result<u32, u16> { Ok(1) }
        fn connect(&mut self, _: u32, _: u32, _: u16) -> Result<(), u16> { Ok(()) }
        fn send(&mut self, _: u32, data: &[u8]) -> Result<usize, u16> {
            Ok(data.len())
        }
        fn recv(&mut self, _: u32, buf: &mut [u8]) -> Result<usize, u16> {
            let n = buf.len().min(5);
            buf[..n].copy_from_slice(&b"hello"[..n]);
            Ok(n)
        }
        fn bind(&mut self, _: u32, _: u32, _: u16) -> Result<(), u16> { Ok(()) }
        fn listen(&mut self, _: u32) -> Result<(), u16> { Ok(()) }
        fn accept(&mut self, _: u32) -> Result<u32, u16> { Ok(2) }
        fn accept_peer(&mut self, _: u32) -> Result<(u32, u32, u16), u16> {
            Ok((2, 0x7F00_0001, 8080))
        }
        fn peer_addr(&self, _: u32) -> Option<(u32, u16)> {
            Some((0x7F00_0001, 8080))
        }
        fn close_socket(&mut self, _: u32) -> Result<(), u16> { Ok(()) }
        fn poll(&mut self, _: u32) -> Result<u16, u16> { Ok(1) }
        fn shutdown(&mut self, _: u32, _: u8) -> Result<(), u16> { Ok(()) }
    }

    /// M1: every one of the eight numbers reaches SocketOps through the
    /// fd table, and the replies carry the shapes the kernel stages (a
    /// socket fd, a zero for the control calls, and the 20-byte address
    /// trailer for accept/recvfrom). This is the sidecar's half of the
    /// bridge, driven without a channel or a boot.
    #[test]
    fn m1_network_arms_translate_onto_socketops() {
        let mut fds = fds();
        let mut boxed: Box<dyn SocketOps> = Box::new(FakeNet);
        let mut net = Some(&mut boxed);

        let fd = match answer_net(&mut net, LINUX_SOCKET,
                                  &[AF_INET as u64, SOCK_STREAM as u64,
                                    0, 0, 0, 0], &mut fds, &[]) {
            Answer::Reply(fd, 0) => fd,
            _ => panic!("socket did not answer a fd"),
        };
        assert_eq!(fds[fd as usize].kind, FD_KIND_SOCKET);
        assert_eq!(fds[fd as usize].vfs_fd, 1);

        let sa = [2u8, 0, 0x1f, 0x90, 127, 0, 0, 1,
                  0, 0, 0, 0, 0, 0, 0, 0];
        match answer_net(&mut net, LINUX_CONNECT,
                         &[fd, 0, 16, 0, 0, 0], &mut fds, &sa) {
            Answer::Reply(0, 0) => {}
            _ => panic!("connect did not answer 0"),
        }
        match answer_net(&mut net, LINUX_BIND,
                         &[fd, 0, 16, 0, 0, 0], &mut fds, &sa) {
            Answer::Reply(0, 0) => {}
            _ => panic!("bind did not answer 0"),
        }
        match answer_net(&mut net, LINUX_LISTEN,
                         &[fd, 16, 0, 0, 0, 0], &mut fds, &[]) {
            Answer::Reply(0, 0) => {}
            _ => panic!("listen did not answer 0"),
        }
        match answer_net(&mut net, LINUX_SENDTO,
                         &[fd, 0, 2, 0, 0, 0], &mut fds, b"hi") {
            Answer::Reply(2, 0) => {}
            _ => panic!("sendto did not answer its byte count"),
        }
        /* A destination is packed by the kernel into args[4] with the
         * marker in args[5]; the sidecar decodes it and invokes send_to. */
        let packed = ((0x0A00_020Fu64) << 16) | 9999u64;
        match answer_net(&mut net, LINUX_SENDTO,
                         &[fd, 0, 2, 0, packed, LINUX_SENDTO_DEST_MARKER],
                         &mut fds, b"hi") {
            Answer::Reply(2, 0) => {}
            _ => panic!("sendto with a destination did not answer"),
        }
        assert!(matches!(answer_net(&mut net, LINUX_SENDTO,
                                     &[fd, 0, 2, 0, packed, 0], &mut fds, b"hi"),
                         Answer::Reply(0, E_INVAL)));

        let (ret, n, b) = match answer_net(&mut net, LINUX_ACCEPT,
                                           &[fd, 0x1000, 0x2000, 0, 0, 0],
                                           &mut fds, &[]) {
            Answer::ReplyBytes(r, l, b) => (r, l, b),
            _ => panic!("accept did not carry the address trailer"),
        };
        assert_eq!(ret, 1);          /* the next fd the table mints */
        assert_eq!(n, LINUX_NET_ADDR_BYTES);
        assert_eq!(&b[0..2], &AF_INET.to_le_bytes());
        assert_eq!(&b[2..4], &8080u16.to_be_bytes());
        assert_eq!(&b[4..8], &0x7F00_0001u32.to_be_bytes());
        assert_eq!(&b[16..20], &16u32.to_le_bytes());

        let (ret, n, b) = match answer_net(&mut net, LINUX_RECVFROM,
                                           &[fd, 0x3000, 8, 0, 0x4000, 0x5000],
                                           &mut fds, &[]) {
            Answer::ReplyBytes(r, l, b) => (r, l, b),
            _ => panic!("recvfrom did not carry data plus the trailer"),
        };
        assert_eq!(ret, 5);
        assert_eq!(n, 5 + LINUX_NET_ADDR_BYTES);
        assert_eq!(&b[..5], b"hello");
        assert_eq!(&b[5..7], &AF_INET.to_le_bytes());
        assert_eq!(&b[7..9], &8080u16.to_be_bytes());
        assert_eq!(&b[9..13], &0x7F00_0001u32.to_be_bytes());
        assert_eq!(&b[21..25], &16u32.to_le_bytes());

        match answer_net(&mut net, LINUX_SHUTDOWN,
                         &[fd, 1, 0, 0, 0, 0], &mut fds, &[]) {
            Answer::Reply(0, 0) => {}
            other => panic!("shutdown did not answer 0: {:?}", other),
        }
        assert!(matches!(answer_net(&mut net, LINUX_SHUTDOWN,
                                    &[fd, 3, 0, 0, 0, 0], &mut fds, &[]),
                         Answer::Reply(0, E_INVAL)));
    }

    #[test]
    fn socket_poll_maps_service_events_and_unknown_fds() {
        let console = CharNode::console();
        let mut fds = fds();
        fds[3] = FdSlot { used: true, kind: FD_KIND_SOCKET, vfs_fd: 9, flags: 0 };
        let mut boxed: Box<dyn SocketOps> = Box::new(FakeNet);
        let mut net = Some(&mut boxed);
        let mut payload = [0u8; LINUX_POLL_PAYLOAD_BYTES];
        payload[..8].copy_from_slice(&pollfd(3, POLLIN | POLLOUT));
        let args = [0x1000u64, 1, 0, 0, 0, 0];
        match poll_answer(&fds, &mut net, &console, &payload[..8], &args, 0,
                          |_, _| panic!("a socket must not reach the VFS")) {
            Answer::ReplyBytes(ret, len, arr) => {
                assert_eq!((ret, len), (1, 8));
                assert_eq!(u16::from_le_bytes([arr[6], arr[7]]), POLLIN);
            }
            other => panic!("socket poll: {:?}", other),
        }
        assert_eq!(net_status(NET_ERR_CONNRESET), E_CONNRESET);
        assert_eq!(net_status(NET_ERR_ADDRINUSE), E_ADDRINUSE);
        assert_eq!(net_status(NET_ERR_NOTCONN), E_NOTCONN);
        assert_eq!(net_status(NET_ERR_PIPE), E_PIPE);
    }

    /// M1: the sockaddr transform and the network request/reply bounds —
    /// the pure halves both ends of the seam derive independently. A
    /// sockaddr_in is host-order family, network-order port/address; the
    /// attach lengths and reply shapes are the kernel plan's twins.
    #[test]
    fn m1_network_sockaddr_and_bounds() {
        let sa = [2u8, 0, 0x1f, 0x90, 127, 0, 0, 1,
                  0, 0, 0, 0, 0, 0, 0, 0];
        assert_eq!(parse_sockaddr_in(&sa), Ok((0x7F00_0001, 8080)));
        assert_eq!(parse_sockaddr_in(&sa[..8]), Err(E_INVAL));
        assert_eq!(expected_req_len(LINUX_CONNECT,
                                    &[0, 0, 16, 0, 0, 0]),
                   LINUX_SYSCALL_REQ_BYTES + LINUX_SOCKADDR_IN_BYTES);
        assert_eq!(expected_req_len(LINUX_SENDTO, &[0, 0, 7, 0, 0, 0]),
                   LINUX_SYSCALL_REQ_BYTES + 7);
        assert!(payload_bound(LINUX_ACCEPT, &[0, 1, 1, 0, 0, 0], 20));
        assert!(!payload_bound(LINUX_ACCEPT, &[0, 0, 1, 0, 0, 0], 20));
        assert!(payload_bound(LINUX_RECVFROM, &[0, 1, 32, 0, 0, 0], 20));
        assert!(!payload_bound(LINUX_RECVFROM, &[0, 1, 32, 0, 0, 0], 53));
    }

    fn fds() -> [FdSlot; LINUX_MAX_FDS] {
        [FdSlot { used: false, kind: 0, vfs_fd: 0, flags: 0 }; LINUX_MAX_FDS]
    }
    /* E7 increment 23: the three console descriptors every task is born
     * with (track() registers them), for the poll/read waiting tests. */
    fn console_fds() -> [FdSlot; LINUX_MAX_FDS] {
        let mut f = fds();
        for s in f.iter_mut().take(3) {
            s.used = true;
            s.kind = FD_KIND_CONSOLE;
        }
        f
    }
    fn pollfd(fd: i32, events: u16) -> [u8; LINUX_POLLFD_BYTES] {
        let mut b = [0u8; LINUX_POLLFD_BYTES];
        b[0..4].copy_from_slice(&fd.to_le_bytes());
        b[4..6].copy_from_slice(&events.to_le_bytes());
        b
    }
    fn ledger() -> MemBrk {
        MemBrk { base: 0, limit: 0, cur: 0, mtop: 0, image_lo: 0, image_hi: 0 }
    }
    /* The fixture span the mem-class tests share: 64 KiB exactly as the
     * probe's manifest stamps it (increment 16's ledger tests). */
    const SPAN_BASE: u64 = 0x807d000;
    const SPAN_LIMIT: u64 = SPAN_BASE + 0x10000;
    fn span() -> MemBrk {
        MemBrk { base: SPAN_BASE, limit: SPAN_LIMIT,
                 cur: SPAN_BASE, mtop: SPAN_LIMIT,
                 image_lo: 0, image_hi: 0 }
    }
    /* The fixture tid: what the candidate's set_tid_address answer echoes
     * (the address of __thread_list_lock, 0x807c398 in the committed
     * firstuser ELF). */
    const TID: u64 = 0x807c398;

    fn fresh() -> SigState {
        SigState { tid: TID, tgid: TID, ..SigState::new() }
    }

    fn act_payload(handler: u64) -> [u8; SIGACT_BYTES] {
        let mut p = [0u8; SIGACT_BYTES];
        p[..8].copy_from_slice(&handler.to_le_bytes());
        p
    }
    fn handler_payload(handler: u64, flags: u64, restorer: u64,
                       mask: u64) -> [u8; SIGACT_BYTES] {
        let mut p = [0u8; SIGACT_BYTES];
        p[0..8].copy_from_slice(&handler.to_le_bytes());
        p[8..16].copy_from_slice(&flags.to_le_bytes());
        p[16..24].copy_from_slice(&restorer.to_le_bytes());
        p[24..32].copy_from_slice(&mask.to_le_bytes());
        p
    }
    fn set_payload(set: u64) -> [u8; SIGSET_BYTES as usize] {
        set.to_le_bytes()
    }
    fn stack_payload(sp: u64, flags: u32, size: u64) -> [u8; STACK_T_BYTES] {
        let mut b = [0u8; STACK_T_BYTES];
        b[0..8].copy_from_slice(&sp.to_le_bytes());
        b[8..12].copy_from_slice(&flags.to_le_bytes());
        b[16..24].copy_from_slice(&size.to_le_bytes());
        b
    }

    #[test]
    fn clock_gettime_answers_the_pinned_epoch_and_zero_monotonic() {
        let mut brk = ledger();
        let mut sig = SigState::new();
        let a = answer(228, &[0, 0x5000, 0, 0, 0, 0], &fds(), &mut brk,
                       &mut sig, &[]);
        match a {
            Answer::ReplyBytes(ret, len, bytes) => {
                assert_eq!(ret, 0);
                assert_eq!(len, TIMESPEC_BYTES);
                assert_eq!(read_u64(&bytes, 0), GATE_EPOCH_SECS);
                assert_eq!(read_u64(&bytes, 8), 0);
            }
            _ => panic!("CLOCK_REALTIME did not answer a timespec: {:?}", a),
        }
        let a = answer(228, &[1, 0x5000, 0, 0, 0, 0], &fds(), &mut brk,
                       &mut sig, &[]);
        match a {
            Answer::ReplyBytes(ret, len, bytes) => {
                assert_eq!(ret, 0);
                assert_eq!(len, TIMESPEC_BYTES);
                assert_eq!(read_u64(&bytes, 0), 0);
                assert_eq!(read_u64(&bytes, 8), 0);
            }
            _ => panic!("CLOCK_MONOTONIC did not answer a timespec: {:?}", a),
        }
        let a = answer(228, &[7, 0x5000, 0, 0, 0, 0], &fds(), &mut brk,
                       &mut sig, &[]);
        assert!(matches!(a, Answer::Reply(0, E_INVAL)),
                "an unknown clock id is EINVAL: {:?}", a);
        let a = answer(228, &[0, 0, 0, 0, 0, 0], &fds(), &mut brk,
                       &mut sig, &[]);
        assert!(matches!(a, Answer::Reply(0, E_INVAL)),
                "a NULL timespec is refused defensively: {:?}", a);
    }

    #[test]
    fn expected_req_len_is_the_kernels_twin() {
        let mut a = [0u64; 6];
        assert_eq!(expected_req_len(LINUX_WRITE, &a), 72);
        a[2] = 512;
        assert_eq!(expected_req_len(LINUX_WRITE, &a), 72 + 512);
        /* poll: the whole array rides behind the header; an nfds above
         * the cap keeps the arithmetic total (the arm refuses it EINVAL). */
        a = [0; 6];
        a[1] = 3;
        assert_eq!(expected_req_len(LINUX_POLL, &a), 72 + 24);
        a[1] = (LINUX_POLL_MAX_FDS + 1) as u64;
        assert_eq!(expected_req_len(LINUX_POLL, &a), 72);
        a = [0; 6];
        a[1] = 0x1000; /* an act/set pointer is present */
        assert_eq!(expected_req_len(LINUX_RT_SIGACTION, &a), 72 + SIGACT_BYTES);
        assert_eq!(expected_req_len(LINUX_RT_SIGPROCMASK, &a), 72 + 8);
        a[1] = 0; /* NULL act/set: the plain 72-byte description */
        assert_eq!(expected_req_len(LINUX_RT_SIGACTION, &a), 72);
        assert_eq!(expected_req_len(LINUX_TKILL, &a), 72);
        a = [0x7000, FUTEX_WAIT | FUTEX_PRIVATE, 1, 0, 0, 0];
        assert_eq!(expected_req_len(LINUX_FUTEX, &a), 72 + 24);
        a[1] = FUTEX_WAKE | FUTEX_PRIVATE;
        assert_eq!(expected_req_len(LINUX_FUTEX, &a), 72);
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
        /* SIGKILL/SIGSTOP are silently dropped from a requested mask. */
        assert!(matches!(run(&mut sig, 2, SIG_UNBLOCKABLE | 0b1000),
                         Answer::Reply(0, STATUS_OK)));
        assert_eq!(sig.mask, 0b1000, "SIGKILL/SIGSTOP cannot be blocked");
        assert!(matches!(run(&mut sig, 2, 0b110), Answer::Reply(0, STATUS_OK)));
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
        assert_eq!(sig.mask, !0 & !SIG_UNBLOCKABLE,
                   "and the SETMASK itself landed, minus the unblockable bits");
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
    fn handler_install_delivers_a_frame_and_sigreturn_restores_the_mask() {
        let mut sig = fresh();
        let p = handler_payload(0x401000, SA_RESTORER, 0x402000, 1 << 2);
        let a = answer(13, &[6, 0x4000, 0, 8, 0, 0], &fds(), &mut ledger(),
                       &mut sig, &p);
        assert!(matches!(a, Answer::Reply(0, STATUS_OK)), "install: {:?}", a);
        assert_eq!(sig.dispo[6], DISPO_HANDLER);
        assert_eq!(sig.act[6].handler, 0x401000);
        assert_eq!(sig.act[6].flags, SA_RESTORER);
        assert_eq!(sig.act[6].restorer, 0x402000);
        assert_eq!(sig.act[6].mask, 1 << 2);

        /* tkill with the signal unblocked delivers NOW. The handler mask
         * is the old mask plus the action's sa_mask plus the signal itself
         * (no SA_NODEFER), and the frame saves the old mask for 15. */
        let d = match answer(200, &[TID, 6, 0, 0, 0, 0], &fds(),
                             &mut ledger(), &mut sig, &[]) {
            Answer::SignalHandler(d) => d,
            other => panic!("handler delivery: {:?}", other),
        };
        assert_eq!(d.signo, 6);
        assert_eq!(d.handler, 0x401000);
        assert_eq!(d.restorer, 0x402000);
        assert_eq!(d.old_mask, 0);
        assert_eq!(sig.mask, (1 << 5) | (1 << 2));
        let restored = answer(15, &[d.old_mask, 0, 0, 0, 0, 0], &fds(),
                              &mut ledger(), &mut sig, &[]);
        assert!(matches!(restored, Answer::Reply(0, STATUS_OK)));
        assert_eq!(sig.mask, 0, "rt_sigreturn restores the saved mask");
    }

    #[test]
    fn sigpipe_is_produced_by_epipe_with_the_right_action_and_return() {
        /* A handler is delivered for an unblocked EPIPE, and the saved rax
         * is -EPIPE: the send completes with the errno Linux owes after
         * the handler returns, not the ordinary syscall-interrupt 0. */
        let mut sig = fresh();
        let p = handler_payload(0x401000, SA_RESTORER, 0x402000, 0);
        let a = answer(13, &[13, 0x4000, 0, 8, 0, 0], &fds(),
                       &mut ledger(), &mut sig, &p);
        assert!(matches!(a, Answer::Reply(0, STATUS_OK)), "install: {:?}", a);
        let d = match sigpipe_after_epipe(&mut sig, Answer::Reply(0, E_PIPE)) {
            Answer::SignalHandler(d) => d,
            other => panic!("SIGPIPE handler: {:?}", other),
        };
        assert_eq!(d.signo, SIGPIPE);
        assert_eq!(d.handler, 0x401000);
        assert_eq!(d.sysret, (-LINUX_EPIPE) as u64);
        assert_eq!(sig.mask & (1 << (SIGPIPE - 1)), 1 << (SIGPIPE - 1),
                   "the handler mask blocks SIGPIPE while it runs");
        let restored = answer(15, &[d.old_mask, 0, 0, 0, 0, 0], &fds(),
                              &mut ledger(), &mut sig, &[]);
        assert!(matches!(restored, Answer::Reply(0, STATUS_OK)));

        /* A blocked SIGPIPE pends and the EPIPE answer stands. */
        let mut blocked = fresh();
        answer(14, &[2, 0x4000, 0, 8, 0, 0], &fds(), &mut ledger(),
               &mut blocked, &set_payload(1u64 << (SIGPIPE - 1)));
        let a = sigpipe_after_epipe(&mut blocked, Answer::Reply(0, E_PIPE));
        assert!(matches!(a, Answer::Reply(0, E_PIPE)), "blocked: {:?}", a);
        assert_eq!(blocked.pending & (1 << (SIGPIPE - 1)),
                   1 << (SIGPIPE - 1));

        /* SIG_IGN leaves the EPIPE reply untouched; the default kills. */
        let mut ignored = fresh();
        answer(13, &[13, 0x4000, 0, 8, 0, 0], &fds(), &mut ledger(),
               &mut ignored, &act_payload(1));
        let a = sigpipe_after_epipe(&mut ignored, Answer::Reply(0, E_PIPE));
        assert!(matches!(a, Answer::Reply(0, E_PIPE)), "ignored: {:?}", a);
        let mut dfl = fresh();
        let a = sigpipe_after_epipe(&mut dfl, Answer::Reply(0, E_PIPE));
        assert!(matches!(a, Answer::SignalKill(SIGPIPE)), "default: {:?}", a);

        /* A non-EPIPE answer never produces a signal. */
        let a = sigpipe_after_epipe(&mut fresh(), Answer::Reply(4, STATUS_OK));
        assert!(matches!(a, Answer::Reply(4, STATUS_OK)));
    }

    #[test]
    fn sigaltstack_round_trips_and_refuses_a_stack_change_while_in_use() {
        let mut sig = fresh();
        let q = answer(131, &[0, 0x5000, 0, 0, 0, 0], &fds(),
                       &mut ledger(), &mut sig, &[]);
        match q {
            Answer::ReplyBytes(ret, len, b) => {
                assert_eq!((ret, len), (0, STACK_T_BYTES));
                assert_eq!(read_stack_t(&b), (0, SS_DISABLE, 0));
            }
            other => panic!("disabled query: {:?}", other),
        }
        let set = answer(131, &[0x4000, 0x5000, 0, 0, 0, 0], &fds(),
                         &mut ledger(), &mut sig,
                         &stack_payload(0x800000, 0, 8192));
        assert!(matches!(set, Answer::ReplyBytes(0, 24, _)),
                "set: {:?}", set);
        assert_eq!((sig.alt_sp, sig.alt_size, sig.alt_flags),
                   (0x800000, 8192, 0));
        let q = answer(131, &[0, 0x5000, 0, 0, 0, 0], &fds(),
                       &mut ledger(), &mut sig, &[]);
        match q {
            Answer::ReplyBytes(_, _, b) =>
                assert_eq!(read_stack_t(&b), (0x800000, 0, 8192)),
            other => panic!("enabled query: {:?}", other),
        }
        /* A handler on the alternate stack cannot change it underneath
         * itself; Linux answers EPERM. */
        let p = handler_payload(0x401000,
                                SA_RESTORER | SA_ONSTACK, 0x402000, 0);
        answer(13, &[6, 0x4000, 0, 8, 0, 0], &fds(), &mut ledger(),
               &mut sig, &p);
        let d = match answer(200, &[TID, 6, 0, 0, 0, 0], &fds(),
                             &mut ledger(), &mut sig, &[]) {
            Answer::SignalHandler(d) => d,
            other => panic!("alt delivery: {:?}", other),
        };
        assert_eq!((d.alt_sp, d.alt_size), (0x800000, 8192));
        assert_eq!(sig.alt_depth, 1);
        let change = answer(131, &[0x4000, 0, 0, 0, 0, 0], &fds(),
                            &mut ledger(), &mut sig,
                            &stack_payload(0x900000, 0, 8192));
        assert!(matches!(change, Answer::Reply(0, E_PERM)),
                "change while on alt stack: {:?}", change);
        let _ = answer(15, &[d.old_mask, 0, 0, 0, 0, 0], &fds(),
                       &mut ledger(), &mut sig, &[]);
        assert_eq!(sig.alt_depth, 0);
        let disable = answer(131, &[0x4000, 0, 0, 0, 0, 0], &fds(),
                             &mut ledger(), &mut sig,
                             &stack_payload(0, SS_DISABLE, 0));
        assert!(matches!(disable, Answer::Reply(0, STATUS_OK)));
        assert_eq!((sig.alt_sp, sig.alt_size, sig.alt_flags),
                   (0, 0, SS_DISABLE));
    }

    #[test]
    fn nested_alt_and_normal_returns_pop_their_own_depth() {
        let mut sig = fresh();
        let set = answer(131, &[0x4000, 0, 0, 0, 0, 0], &fds(),
                         &mut ledger(), &mut sig,
                         &stack_payload(0x800000, 0, 8192));
        assert!(matches!(set, Answer::Reply(0, STATUS_OK)), "set: {:?}", set);
        /* 6 is ordinary; 10 carries SA_ONSTACK. Deliver 6 first, then 10
         * inside it: the ordinary return must not consume the alt depth. */
        let pn = handler_payload(0x401000, SA_RESTORER, 0x402000, 0);
        answer(13, &[6, 0x4000, 0, 8, 0, 0], &fds(), &mut ledger(),
               &mut sig, &pn);
        let pa = handler_payload(0x411000,
                                 SA_RESTORER | SA_ONSTACK, 0x412000, 0);
        answer(13, &[10, 0x4000, 0, 8, 0, 0], &fds(), &mut ledger(),
               &mut sig, &pa);
        let dn = match answer(200, &[TID, 6, 0, 0, 0, 0], &fds(),
                              &mut ledger(), &mut sig, &[]) {
            Answer::SignalHandler(d) => d,
            other => panic!("normal delivery: {:?}", other),
        };
        assert_eq!(sig.handler_depth, 1);
        assert_eq!(sig.alt_depth, 0);
        let da = match answer(200, &[TID, 10, 0, 0, 0, 0], &fds(),
                              &mut ledger(), &mut sig, &[]) {
            Answer::SignalHandler(d) => d,
            other => panic!("nested alt delivery: {:?}", other),
        };
        assert_eq!((sig.handler_depth, sig.alt_depth), (2, 1));
        assert!(matches!(answer(15, &[da.old_mask, 0, 0, 0, 0, 0],
                                &fds(), &mut ledger(), &mut sig, &[]),
                         Answer::Reply(0, STATUS_OK)));
        assert_eq!((sig.handler_depth, sig.alt_depth), (1, 0));
        assert!(matches!(answer(15, &[dn.old_mask, 0, 0, 0, 0, 0],
                                &fds(), &mut ledger(), &mut sig, &[]),
                         Answer::Reply(0, STATUS_OK)));
        assert_eq!((sig.handler_depth, sig.alt_depth), (0, 0));
    }

    #[test]
    fn sa_resethand_resets_the_action_before_entry() {
        let mut sig = fresh();
        let p = handler_payload(0x401000,
                                SA_RESTORER | SA_RESETHAND, 0x402000, 0);
        answer(13, &[6, 0x4000, 0, 8, 0, 0], &fds(), &mut ledger(),
               &mut sig, &p);
        let d = match answer(200, &[TID, 6, 0, 0, 0, 0], &fds(),
                             &mut ledger(), &mut sig, &[]) {
            Answer::SignalHandler(d) => d,
            other => panic!("handler: {:?}", other),
        };
        assert_eq!(sig.dispo[6], DISPO_DFL,
                   "SA_RESETHAND lands before the handler runs");
        let _ = answer(15, &[d.old_mask, 0, 0, 0, 0, 0], &fds(),
                       &mut ledger(), &mut sig, &[]);
        assert!(matches!(answer(200, &[TID, 6, 0, 0, 0, 0], &fds(),
                                &mut ledger(), &mut sig, &[]),
                         Answer::SignalKill(6)));
    }

    #[test]
    fn sigreturn_redelivers_the_next_pending_handler_on_the_restored_mask() {
        let mut sig = fresh();
        let p = handler_payload(0x401000, SA_RESTORER, 0x402000, 0);
        answer(13, &[6, 0x4000, 0, 8, 0, 0], &fds(), &mut ledger(),
               &mut sig, &p);
        let first = match answer(200, &[TID, 6, 0, 0, 0, 0], &fds(),
                                 &mut ledger(), &mut sig, &[]) {
            Answer::SignalHandler(d) => d,
            other => panic!("first: {:?}", other),
        };
        /* The handler mask blocks SIGABRT; a second self-signal pends. */
        assert!(matches!(answer(200, &[TID, 6, 0, 0, 0, 0], &fds(),
                                &mut ledger(), &mut sig, &[]),
                         Answer::Reply(0, STATUS_OK)));
        assert_ne!(sig.pending & (1 << 5), 0);
        match answer(15, &[first.old_mask, 0, 0, 0, 0, 0], &fds(),
                     &mut ledger(), &mut sig, &[]) {
            Answer::SignalHandler(d) => assert_eq!(d.signo, 6),
            other => panic!("pending redelivery: {:?}", other),
        }
        assert_eq!(sig.pending & (1 << 5), 0,
                   "the redelivered signal left pending");
    }

    #[test]
    fn unblocking_a_pending_handler_signal_delivers_with_the_oldset() {
        let mut sig = fresh();
        let p = handler_payload(0x401000, SA_RESTORER, 0x402000, 0);
        answer(13, &[6, 0x4000, 0, 8, 0, 0], &fds(), &mut ledger(),
               &mut sig, &p);
        /* Block SIGABRT, post it, then unblock: raise()'s exact shape. */
        answer(14, &[2, 0x4000, 0, 8, 0, 0], &fds(), &mut ledger(),
               &mut sig, &set_payload(1 << 5));
        let pending = answer(200, &[TID, 6, 0, 0, 0, 0], &fds(),
                             &mut ledger(), &mut sig, &[]);
        assert!(matches!(pending, Answer::Reply(0, STATUS_OK)));
        let d = match answer(14, &[1, 0x4000, 0x5000, 8, 0, 0], &fds(),
                             &mut ledger(), &mut sig,
                             &set_payload(1 << 5)) {
            Answer::SignalHandler(d) => d,
            other => panic!("revealed handler: {:?}", other),
        };
        assert_eq!(d.signo, 6);
        assert_eq!(d.old_mask, 0, "the completed unblock's mask is saved");
        assert_eq!(d.oldset, 1 << 5, "oldset is the pre-call mask");
        assert_eq!(sig.pending, 0, "the delivered signal left pending");
        assert_eq!(sig.mask, 1 << 5, "handler mask blocks SIGABRT");
    }

    #[test]
    fn handler_install_refuses_unsupported_flags_before_mutation() {
        let mut sig = fresh();
        let unsupported = SA_SUPPORTED | (1u64 << 28); /* SA_RESTART */
        let p = handler_payload(0x401000, unsupported, 0x402000, 0);
        let a = answer(13, &[6, 0x4000, 0, 8, 0, 0], &fds(),
                       &mut ledger(), &mut sig, &p);
        assert!(matches!(a, Answer::Reply(0, E_NOSYS)),
                "unsupported flag: {:?}", a);
        assert_eq!(sig.dispo[6], DISPO_DFL);
        for flags in [SA_RESTORER | SA_SIGINFO, SA_RESTORER | SA_ONSTACK,
                      SA_RESTORER | SA_RESETHAND] {
            let mut supported = fresh();
            let p = handler_payload(0x401000, flags, 0x402000, 0);
            let a = answer(13, &[6, 0x4000, 0, 8, 0, 0], &fds(),
                           &mut ledger(), &mut supported, &p);
            assert!(matches!(a, Answer::Reply(0, STATUS_OK)),
                    "flags 0x{:x}: {:?}", flags, a);
            assert_eq!(supported.dispo[6], DISPO_HANDLER);
        }
        /* A handler with no x86-64 restorer has no return path. */
        let p = handler_payload(0x401000, SA_RESTORER, 0, 0);
        let a = answer(13, &[6, 0x4000, 0, 8, 0, 0], &fds(), &mut ledger(),
                       &mut sig, &p);
        assert!(matches!(a, Answer::Reply(0, E_NOSYS)));
        assert_eq!(sig.dispo[6], DISPO_DFL);
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
    fn set_tid_address_records_the_pointer_but_returns_the_real_tid() {
        let mut sig = fresh();
        let a = answer(218, &[0x5000, 0, 0, 0, 0, 0], &fds(), &mut ledger(),
                       &mut sig, &[]);
        assert!(matches!(a, Answer::Reply(TID, STATUS_OK)), "tid: {:?}", a);
        assert_eq!(sig.tid, TID, "the kernel TID did not become the pointer");
        assert_eq!(sig.clear_tid, 0x5000, "the word is recorded for exit");
    }

    #[test]
    fn getpid_and_gettid_are_per_thread_identities() {
        let mut sig = fresh();
        sig.tgid = 0x1111;
        sig.tid = 0x2222;
        assert!(matches!(answer(39, &[0; 6], &fds(), &mut ledger(),
                                 &mut sig, &[]),
                         Answer::Reply(0x1111, STATUS_OK)));
        assert!(matches!(answer(186, &[0; 6], &fds(), &mut ledger(),
                                  &mut sig, &[]),
                         Answer::Reply(0x2222, STATUS_OK)));
    }

    #[test]
    fn poll_parks_without_readiness_and_reports_it_when_input_arrives() {
        let console = CharNode::console();
        let f = console_fds();
        let mut payload = [0u8; LINUX_POLL_PAYLOAD_BYTES];
        payload[..8].copy_from_slice(&pollfd(0, POLLIN));
        let args = [0x1000u64, 1, 5000, 0, 0, 0];
        /* Nothing buffered, 5 s timeout: the request WAITS, and the
         * parked array is the caller's own bytes (revents untouched until
         * a readiness decision is real). */
        match poll_answer(&f, &mut None, &console, &payload[..8], &args, 100,
                          |_, _| None) {
            Answer::PollWait(arr, len, deadline) => {
                assert_eq!(len, 8);
                assert_eq!(deadline, 100 + 5_000 * 1_000_000,
                           "the deadline is now + the caller's ms");
                assert_eq!(&arr[..6], &payload[..6]);
            }
            other => panic!("waiting poll: {:?}", other),
        }
        /* The timeout's whole decision: at the deadline the wait expires
         * (and the reply carries zero revents — never invented input). */
        assert!(poll_expired(100 + 5_000 * 1_000_000,
                             100 + 5_000 * 1_000_000));
        assert!(!poll_expired(100 + 5_000 * 1_000_000, 100));
        /* A typed byte makes the same entry ready; the caller's events
         * mask is never rewritten, only revents. */
        console.console_io().push_input(b"x");
        match poll_answer(&f, &mut None, &console, &payload[..8], &args, 200,
                          |_, _| None) {
            Answer::ReplyBytes(ret, len, arr) => {
                assert_eq!((ret, len), (1, 8), "one ready fd, full array");
                assert_eq!(u16::from_le_bytes([arr[4], arr[5]]), POLLIN,
                           "events is the caller's mask");
                assert_eq!(u16::from_le_bytes([arr[6], arr[7]]), POLLIN,
                           "revents is the observed readiness");
            }
            other => panic!("ready poll: {:?}", other),
        }
    }

    #[test]
    fn poll_timeout_shapes_bad_descriptors_and_file_ready_are_posix() {
        let console = CharNode::console();
        let f = console_fds();
        let mut payload = [0u8; LINUX_POLL_PAYLOAD_BYTES];
        let zero = [0x1000u64, 1, 0, 0, 0, 0];
        payload[..8].copy_from_slice(&pollfd(0, POLLIN));
        /* Timeout 0 is the snapshot: 0 ready, but the full array still
         * crosses back (the kernel writes it into the caller's span). */
        match poll_answer(&f, &mut None, &console, &payload[..8], &zero, 0,
                          |_, _| None) {
            Answer::ReplyBytes(ret, len, arr) => {
                assert_eq!((ret, len), (0, 8));
                assert_eq!(arr[6..8], [0, 0]);
            }
            other => panic!("zero timeout: {:?}", other),
        }
        /* An fd with no slot is POLLNVAL and READY — a bad descriptor is
         * an error to report, not a wait. */
        payload[..8].copy_from_slice(&pollfd(9, POLLIN));
        let timed = [0x1000u64, 1, 50, 0, 0, 0];
        match poll_answer(&f, &mut None, &console, &payload[..8], &timed, 0,
                          |_, _| None) {
            Answer::ReplyBytes(ret, _, arr) => {
                assert_eq!(ret, 1);
                assert_eq!(u16::from_le_bytes([arr[6], arr[7]]), POLLNVAL);
            }
            other => panic!("POLLNVAL: {:?}", other),
        }
        /* A VFS-backed slot is answered by the VFS closure; the console
         * slots never consult it. */
        let mut file_fds = console_fds();
        file_fds[3] = FdSlot { used: true, kind: FD_KIND_FILE, vfs_fd: 7,
                               flags: 0 };
        payload[..8].copy_from_slice(&pollfd(3, POLLIN));
        match poll_answer(&file_fds, &mut None, &console, &payload[..8], &timed, 0,
                          |vfd, ev| {
                              assert_eq!((vfd, ev), (7, POLLIN));
                              Some(POLLIN)
                          }) {
            Answer::ReplyBytes(ret, _, arr) => {
                assert_eq!(ret, 1);
                assert_eq!(u16::from_le_bytes([arr[6], arr[7]]), POLLIN);
            }
            other => panic!("file readiness: {:?}", other),
        }
        /* A negative fd is ignored: revents 0 and it cannot make a wait
         * ready (POSIX's own rule). */
        payload[..8].copy_from_slice(&pollfd(-1, POLLIN));
        match poll_answer(&f, &mut None, &console, &payload[..8], &timed, 0,
                          |_, _| None) {
            Answer::PollWait(arr, ..) => assert_eq!(arr[6..8], [0, 0]),
            other => panic!("negative fd: {:?}", other),
        }
        /* nfds 0 with timeout -1 is a pure sleep: parked with no array
         * and no deadline (0 is "forever", never "already expired"). */
        match poll_answer(&f, &mut None, &console, &[], &[0, 0, u64::MAX, 0, 0, 0],
                          0, |_, _| None) {
            Answer::PollWait(_, 0, 0) => {}
            other => panic!("infinite sleep: {:?}", other),
        }
        assert!(!poll_expired(0, u64::MAX));
        assert_eq!(poll_deadline(10, -1), 0);
        assert_eq!(poll_deadline(10, 2), 10 + 2_000_000);
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
    fn console_read_serves_buffered_input_and_never_invents_readiness() {
        let console = CharNode::console();
        console.console_io().push_input(b"ls\n");

        let a = console_read_answer(&console, 8);
        match a {
            Answer::ReplyBytes(ret, len, bytes) => {
                assert_eq!((ret, len), (3, 3), "the count is the payload length");
                assert_eq!(&bytes[..3], b"ls\n", "the typed bytes came back whole");
            }
            other => panic!("buffered console read: {:?}", other),
        }
        assert!(matches!(console_read_answer(&console, 8),
                         Answer::ReadWait(8)),
                "a second read finds the buffer empty and PARKS — never a fabricated zero-byte EOF, and never EAGAIN on a blocking descriptor");
        console.console_io().push_input(b"x");
        assert!(matches!(console_read_answer(&console, 8),
                         Answer::ReplyBytes(1, 1, _)),
                "the parked read is served by the next typed byte");
        assert!(matches!(console_read_answer(&console, 0),
                         Answer::ReplyBytes(0, 0, _)),
                "read(fd, buf, 0) succeeds without consuming or claiming data");

        console.console_io().close();
        assert!(matches!(console_read_answer(&console, 8),
                         Answer::ReplyBytes(0, 0, _)),
                "a closed console is EOF after its buffered input is drained");
    }

    #[test]
    fn mprotect_serves_the_rust_guard_page_and_keeps_the_word_rules() {
        /* The E7 gate tenant's Rust std maps its alternate stack, then
         * calls mprotect(first_page, 4096, PROT_NONE) for the guard. That
         * exact shape must be a success at this end; ENOSYS here is what
         * aborted the tenant before its copy continuation. */
        let mut b = span();
        let guard = SPAN_LIMIT - 0x1000;
        let a = answer(10, &[guard, 0x1000, 0, 0, 0, 0],
                       &fds(), &mut b, &mut fresh(), &[]);
        assert!(matches!(a, Answer::Reply(0, STATUS_OK)),
                "PROT_NONE guard: {:?}", a);
        /* Read-only and RW are the same word rules; they are answered
         * for the kernel to install on the present leaf. */
        for prot in [1u64, 2, 3] {
            let a = answer(10, &[guard, 0x1000, prot, 0, 0, 0],
                           &fds(), &mut b, &mut fresh(), &[]);
            assert!(matches!(a, Answer::Reply(0, STATUS_OK)),
                    "prot {}: {:?}", prot, a);
        }
        /* W|X keeps the shim's §5.4 refusal; executable-only remains
         * ENOSYS by name; unknown bits are EINVAL. */
        let a = answer(10, &[guard, 0x1000, 6, 0, 0, 0],
                       &fds(), &mut b, &mut fresh(), &[]);
        assert!(matches!(a, Answer::Reply(0, E_PERM)), "W|X: {:?}", a);
        let a = answer(10, &[guard, 0x1000, 4, 0, 0, 0],
                       &fds(), &mut b, &mut fresh(), &[]);
        assert!(matches!(a, Answer::Reply(0, E_NOSYS)), "exec: {:?}", a);
        let a = answer(10, &[guard, 0x1000, 8, 0, 0, 0],
                       &fds(), &mut b, &mut fresh(), &[]);
        assert!(matches!(a, Answer::Reply(0, E_INVAL)), "prot bits: {:?}", a);
        /* malformed ranges are first-trap refusals, never a fake grant */
        for addr_len in [(guard + 1, 0x1000), (guard, 0),
                         (SPAN_BASE - 0x1000, 0x1000),
                         (SPAN_LIMIT, 0x1000)] {
            let a = answer(10, &[addr_len.0, addr_len.1, 0, 0, 0, 0],
                           &fds(), &mut b, &mut fresh(), &[]);
            assert!(matches!(a, Answer::Reply(0, E_INVAL)),
                    "range {:#x}/{:#x}: {:?}", addr_len.0, addr_len.1, a);
        }
        assert_eq!((b.cur, b.mtop), (SPAN_BASE, SPAN_LIMIT),
                   "mprotect never moves a heap cursor");
    }

    #[test]
    fn stdio_ioctl_answers_the_two_musl_asks_on_the_console_only() {
        /* M2b: musl's __stdout_write asks TIOCGWINSZ before its first
         * stdout write, and isatty asks TCGETS; both only check the
         * return, so the console answers 0 for exactly those words. */
        let mut b = ledger();
        for req in [0x5401u64, 0x5413] {
            let a = answer(16, &[1, req, 0, 0, 0, 0],
                           &console_fds(), &mut b, &mut fresh(), &[]);
            assert!(matches!(a, Answer::Reply(0, STATUS_OK)),
                    "req {:#x}: {:?}", req, a);
        }
        /* a console fd with any other request, and a served file fd, are
         * EINVAL; the slot beyond the table is EBADF. */
        let a = answer(16, &[1, 0x1234, 0, 0, 0, 0],
                       &console_fds(), &mut b, &mut fresh(), &[]);
        assert!(matches!(a, Answer::Reply(0, E_INVAL)), "other: {:?}", a);
        let mut f = fds();
        f[3] = FdSlot { used: true, kind: FD_KIND_FILE, vfs_fd: 7, flags: 0 };
        let a = answer(16, &[3, 0x5413, 0, 0, 0, 0],
                       &f, &mut b, &mut fresh(), &[]);
        assert!(matches!(a, Answer::Reply(0, E_INVAL)), "file: {:?}", a);
        let a = answer(16, &[9, 0x5413, 0, 0, 0, 0],
                       &console_fds(), &mut b, &mut fresh(), &[]);
        assert!(matches!(a, Answer::Reply(0, E_BADF)), "slot: {:?}", a);
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
        /* a file-shaped ask is no longer refused by shape — it reaches
         * the fd table, and an empty slot there is EBADF (M2b serves
         * READ, RW and RX file maps). Shared and unknown flags stay
         * ENOSYS by name. */
        let a = answer(9, &[0, 0x1000, 3, 0x02, 0, 0],
                       &fds(), &mut b, &mut fresh(), &[]);
        assert!(matches!(a, Answer::Reply(0, E_BADF)), "file: {:?}", a);
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
    fn munmap_frees_only_the_span_above_the_break_without_rewinding_the_cursor() {
        let mut b = span();
        b.cur = SPAN_BASE + 0x2000;
        let mtop = b.mtop;
        let a = answer(11, &[SPAN_BASE + 0x2000, 0x1000, 0, 0, 0, 0],
                       &fds(), &mut b, &mut fresh(), &[]);
        assert!(matches!(a, Answer::Reply(0, STATUS_OK)),
                "the arena above the break unmaps: {:?}", a);
        assert_eq!((b.cur, b.mtop), (SPAN_BASE + 0x2000, mtop),
                   "munmap moves neither the break nor the map cursor");
        /* POSIX: a second unmap of the same range is success — the
         * kernel's page tables, not a sidecar ledger, are the truth */
        let a = answer(11, &[SPAN_BASE + 0x2000, 0x1000, 0, 0, 0, 0],
                       &fds(), &mut b, &mut fresh(), &[]);
        assert!(matches!(a, Answer::Reply(0, STATUS_OK)),
                "a double unmap is success: {:?}", a);
        /* the page-round is the caller's byte length: the last page of
         * the span, one byte long, still frees one page */
        let a = answer(11, &[SPAN_LIMIT - 0x1000, 1, 0, 0, 0, 0],
                       &fds(), &mut b, &mut fresh(), &[]);
        assert!(matches!(a, Answer::Reply(0, STATUS_OK)),
                "a one-byte tail still names its page: {:?}", a);
        /* below the break: EINVAL — v1 keeps the break's pages with brk */
        let a = answer(11, &[SPAN_BASE, 0x1000, 0, 0, 0, 0],
                       &fds(), &mut b, &mut fresh(), &[]);
        assert!(matches!(a, Answer::Reply(0, E_INVAL)),
                "the break's pages are not munmap's: {:?}", a);
        /* every malformed span is EINVAL, and a refusal moves nothing */
        for args in [
            [SPAN_BASE + 0x2001, 0x1000, 0, 0, 0, 0], /* unaligned      */
            [SPAN_BASE + 0x2000, 0, 0, 0, 0, 0],      /* zero length    */
            [SPAN_BASE + 0x2000, u64::MAX, 0, 0, 0, 0], /* overflow     */
            [SPAN_BASE - 0x1000, 0x1000, 0, 0, 0, 0], /* below the span */
            [SPAN_LIMIT, 0x1000, 0, 0, 0, 0],         /* at the limit   */
            [SPAN_LIMIT - 0x1000, 0x2000, 0, 0, 0, 0], /* over the end  */
        ] {
            let before = (b.cur, b.mtop);
            let a = answer(11, &args, &fds(), &mut b, &mut fresh(), &[]);
            assert!(matches!(a, Answer::Reply(0, E_INVAL)),
                    "refused munmap shape {:?}: {:?}", args, a);
            assert_eq!((b.cur, b.mtop), before,
                       "a refused munmap moves no cursor");
        }
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

    /* ─── E7 step 4 (the file class): the pure half, measured ──────────
     * The flag contract, the errno map, the reply-payload bound, the
     * request range and the two payload builders — decisions over words
     * and bytes, driven here without a VFS, a table or a boot. The VFS
     * arms themselves are the vfs crate's tests plus the on-target
     * probe; these are the twins the kernel's halves pin against. */

    #[test]
    fn flag_contract_matches_the_measured_opens() {
        /* The three opens qemu -strace measured, one for one: the
         * storage read, the .partial write, the opendir. */
        let (vf, ce, dir) = translate_open_flags(0x88000)  /* O_RDONLY|
                                 * O_LARGEFILE|O_CLOEXEC          */
            .expect("open 1 is refused by its own flag word");
        assert_eq!((vf, ce, dir), (0, true, false));
        let (vf, ce, dir) = translate_open_flags(0x88241)  /* O_WRONLY|
                                 * O_CREAT|O_TRUNC|O_LARGEFILE|O_CLOEXEC */
            .expect("open 2 is refused by its own flag word");
        assert_eq!(vf, aerosls_vfs::O_WRONLY | aerosls_vfs::O_CREAT
                       | aerosls_vfs::O_TRUNC);
        assert!((ce, dir) == (true, false));
        let (vf, ce, dir) = translate_open_flags(0x98000)  /* O_RDONLY|
                                 * O_DIRECTORY|O_LARGEFILE|O_CLOEXEC */
            .expect("the opendir is refused by its own flag word");
        assert_eq!((vf, ce, dir), (0, true, true));
        /* musl ORs O_LARGEFILE into every open — accepted as a no-op */
        let (..) = translate_open_flags(0x88000).expect("LARGEFILE no-op");
        /* An accmode of 3: EINVAL — Linux's own number, not ENOSYS. */
        assert_eq!(translate_open_flags(0x80803), Err(E_INVAL));
        /* An unmeasured bit (O_NOFOLLOW): ENOSYS BY NAME — refused,
         * never silently dropped. */
        assert_eq!(translate_open_flags(0x82000), Err(E_NOSYS));
        /* O_DIRECTORY with a write mode is still translatable: the VFS
         * decides the node's type (ENOTDIR), this fn decides words. */
        assert_eq!(translate_open_flags(0x10001).map(|t| t.2), Ok(true));
    }

    #[test]
    fn cap_of_pins_every_vfs_errno_to_its_cap_code() {
        /* The values kernel/cap.h defines and linux_errno_from_cap maps
         * FORWARD to these errnos' own Linux numbers — pinned here so a
         * drift on either side is a red test before it is a wrong errno
         * in a tenant binary's error path. Divergences are pinned as
         * their honest selves: quota/EFBIG read as ENOSPC, EMFILE as
         * ENFILE (cap.h's fixed-table note), ENOTTY-class as EINVAL. */
        assert_eq!(cap_of(Errno::ENoent), 1);      /* CAP_ERR_NOTFOUND */
        assert_eq!(cap_of(Errno::EIo), 2);         /* CAP_ERR_REVOKED -> EIO */
        assert_eq!(cap_of(Errno::EBadf), -2);
        assert_eq!(cap_of(Errno::EInval), -1);
        assert_eq!(cap_of(Errno::EAgain), -3);
        assert_eq!(cap_of(Errno::EMfile), -4);     /* -> ENFILE        */
        assert_eq!(cap_of(Errno::ENomem), -7);
        assert_eq!(cap_of(Errno::ENospc), -8);
        assert_eq!(cap_of(Errno::ENosys), -10);
        assert_eq!(cap_of(Errno::EBusy), -11);     /* -> EBUSY         */
        assert_eq!(cap_of(Errno::EExist), -13);
        assert_eq!(cap_of(Errno::ENotdir), -14);
        assert_eq!(cap_of(Errno::EIsdir), -15);
        assert_eq!(cap_of(Errno::EAcces), -16);    /* EACCES, NOT EPERM */
        assert_eq!(cap_of(Errno::ERofs), -17);
        assert_eq!(cap_of(Errno::ESPipe), -18);
        assert_eq!(cap_of(Errno::ENotempty), -19);
        assert_eq!(cap_of(Errno::ENametoolong), -20);
        assert_eq!(cap_of(Errno::EDquot), -8);     /* named divergence  */
        assert_eq!(cap_of(Errno::EFbig), -8);      /* named divergence  */
        assert_eq!(cap_of(Errno::ESRCH), 7);       /* CAP_ERR_TARGET    */
        assert_eq!(cap_of(Errno::ENoexec), -10);   /* execve not landed */
    }

    #[test]
    fn payload_bound_derives_each_numbers_own_clamp() {
        /* The sig class: exactly its struct, and only when an
         * out-pointer was passed (the kernel's plan staged only then). */
        assert!(payload_bound(13, &[0, 0x4000, 0x5000, 8, 0, 0], 32));
        assert!(!payload_bound(13, &[0, 0x4000, 0x5000, 8, 0, 0], 31));
        assert!(!payload_bound(13, &[0, 0x4000, 0, 8, 0, 0], 32));
        assert!(payload_bound(14, &[2, 0x4000, 0x5000, 8, 0, 0], 8));
        /* stat: exactly the compact Stat, at the statbuf pointer. */
        assert!(payload_bound(LINUX_STAT, &[0, 0x6000, 0, 0, 0, 0],
                              STAT_PAYLOAD_BYTES));
        assert!(!payload_bound(LINUX_STAT, &[0, 0x6000, 0, 0, 0, 0],
                               STAT_PAYLOAD_BYTES - 1));
        assert!(!payload_bound(LINUX_STAT, &[0, 0, 0, 0, 0, 0],
                               STAT_PAYLOAD_BYTES));
        /* read: count-truth — at most the count the first trap clamped
         * (which rides IN the description), at most the chunk. */
        assert!(payload_bound(LINUX_READ, &[3, 0x7000, 64, 0, 0, 0], 64));
        assert!(payload_bound(LINUX_READ, &[3, 0x7000, 4096, 0, 0, 0],
                              FD_PAYLOAD_CAP));
        assert!(!payload_bound(LINUX_READ, &[3, 0x7000, 64, 0, 0, 0], 65));
        /* getdents64: at most the name-list cap. */
        assert!(payload_bound(LINUX_GETDENTS64,
                              &[3, 0x7000, 2048, 0, 0, 0],
                              DENTS_PAYLOAD_CAP));
        assert!(!payload_bound(LINUX_GETDENTS64,
                               &[3, 0x7000, 2048, 0, 0, 0],
                               DENTS_PAYLOAD_CAP + 1));
        /* poll: the full array (nfds * 8), and only when there is one. */
        let pa = [0u64, 3, 5000, 0, 0, 0];
        assert!(payload_bound(LINUX_POLL, &pa, 24));
        assert!(!payload_bound(LINUX_POLL, &pa, 23));
        assert!(!payload_bound(LINUX_POLL, &[0, 0, 5000, 0, 0, 0], 0));
        /* a number that owes no payload is total-false. */
        assert!(!payload_bound(231, &[0, 0, 0, 0, 0, 0], 0));
    }

    #[test]
    fn expected_req_range_bounds_the_path_attaches() {
        /* One string behind the header: an empty path's NUL up to the
         * cap + 1 — the twin of the kernel's path plan. */
        let (lo, hi) = expected_req_range(LINUX_OPEN, &[0, 0x4000, 0, 0, 0, 0]);
        assert_eq!((lo, hi), (LINUX_SYSCALL_REQ_BYTES + 1,
                              LINUX_SYSCALL_REQ_BYTES + LINUX_PATH_CAP + 1));
        /* rename: two strings, both bounded. */
        let (lo, hi) = expected_req_range(LINUX_RENAME,
                                          &[0x1000, 0x2000, 0, 0, 0, 0]);
        assert_eq!((lo, hi), (LINUX_SYSCALL_REQ_BYTES + 2,
                              LINUX_SYSCALL_REQ_BYTES + RECV_ATTACH_CAP));
        /* every other number keeps its exact length. */
        let mut a = [0u64; 6];
        assert_eq!(expected_req_range(LINUX_WRITE, &a), (72, 72));
        a[2] = 512;
        assert_eq!(expected_req_range(LINUX_WRITE, &a), (584, 584));
        a = [0; 6];
        a[1] = 0x1000;
        assert_eq!(expected_req_range(13, &a), (72 + 32, 72 + 32));
        assert_eq!(expected_req_range(LINUX_GETDENTS64, &a), (72, 72));
    }

    #[test]
    fn stat_payload_is_the_compact_24_the_kernel_reads_back() {
        use aerosls_vfs::aerofs::FileType as FT;
        let st = Stat { mode: 0o100644, uid: 1000, gid: 1000,
                        size: 67_108_864, mtime: 1_792_715_282,
                        ty: FT::File };
        let b = stat_payload(&st);
        assert_eq!(&b[0..2], &0o100644u16.to_le_bytes(), "mode @0");
        assert_eq!(&b[2..4], &1000u16.to_le_bytes(), "uid  @2");
        assert_eq!(&b[4..6], &1000u16.to_le_bytes(), "gid  @4");
        assert_eq!(&b[8..16], &67_108_864u64.to_le_bytes(), "size @8");
        assert_eq!(&b[16..20], &1_792_715_282u32.to_le_bytes(),
                   "mtime @16 — the offsets kernel/linux_abi.c's
        linux_stat_bytes reads");
        assert_eq!(b.len(), STAT_PAYLOAD_BYTES);
    }

    #[test]
    fn dents_payload_packs_names_inodes_and_types_for_the_kernel() {
        /* The measured listing's shape: dot entries first (the VFS's
         * read_dir includes them), then files — u32 count, then per
         * entry ino/ty/name_len/name, which linux_dirent_pack parses. */
        let mk = |name: &str, ino: u64, ty: FileType| DirEnt {
            name: alloc::string::String::from(name), ino, ty,
        };
        let ents = [mk(".", 1, FileType::Dir), mk("..", 1, FileType::Dir),
                    mk("sls_storage-20260101-000000.img", 5, FileType::File),
                    mk("ptmx", 9, FileType::Char)];
        let mut buf = [0u8; FD_PAYLOAD_CAP];
        let n = dents_payload(&ents, &mut buf);
        assert_eq!(u32::from_le_bytes([buf[0], buf[1], buf[2], buf[3]]), 4,
                   "count");
        assert_eq!(&buf[4..12], &1u64.to_le_bytes(), "first ino");
        assert_eq!(buf[12], 4, "DT_DIR for \".\"");
        assert_eq!(&buf[13..15], &1u16.to_le_bytes(), "name_len=1");
        assert_eq!(buf[15], b'.');
        /* the file's DT_REG and its exact name land where the kernel's
         * parser expects them. The sidecar's format is UNPADDED (the
         * kernel's packer owns d_reclen): record N+1 starts where
         * record N's name ends — "." ends at 4+11+1 = 16, ".." (a
         * 2-byte name) at 16+11+2 = 29, the 32-byte file at 29+11+32
         * = 72. */
        let off2 = 4 + 11 + 1;                  /* "." record ends   */
        assert_eq!(buf[off2 + 8], 4, ".. is DT_DIR");
        assert_eq!(&buf[off2 + 9..off2 + 11], &2u16.to_le_bytes(),
                   ".. has a 2-byte name");
        let off3 = off2 + 11 + 2;               /* ".." record ends  */
        let file_off = off3;                    /* the file record   */
        const FNAME: &[u8] = b"sls_storage-20260101-000000.img"; /* 31 bytes */
        assert_eq!(buf[file_off + 8], 8, "a file is DT_REG");
        assert_eq!(&buf[file_off + 9..file_off + 11],
                   &(FNAME.len() as u16).to_le_bytes());
        assert_eq!(&buf[file_off + 11..file_off + 11 + FNAME.len()],
                   FNAME);
        /* and the fourth record — ptmx as DT_CHR — then the payload
         * ends: exactly four records plus the count word, no padding
         * (the kernel's packer owns d_reclen). */
        let chr_off = file_off + 11 + FNAME.len();
        assert_eq!(buf[chr_off + 8], 2, "ptmx is DT_CHR");
        assert_eq!(&buf[chr_off + 11..chr_off + 11 + 4], b"ptmx");
        assert_eq!(chr_off + 11 + 4, n, "the payload is exactly four
        records plus the count word — no padding crosses this side");
        assert!(n <= DENTS_PAYLOAD_CAP);
        /* The cap: a listing that cannot fit stops before the record
         * that would break it — the tail continues next call (a short
         * getdents is POSIX-legal; the kernel advances its cursor only
         * by what it packed). */
        let long = alloc::vec::Vec::from_iter(
            (0..64).map(|i| mk(&alloc::format!("some-directory-name-{:04}", i),
                               i as u64 + 2, FileType::File)));
        let n = dents_payload(&long, &mut buf);
        assert!(n <= DENTS_PAYLOAD_CAP, "bounded: {}", n);
        let count = u32::from_le_bytes([buf[0], buf[1], buf[2], buf[3]])
            as usize;
        assert!(count < 64, "not every entry fit: {}", count);
        /* end-of-directory: an empty listing still carries count=0. */
        let n = dents_payload(&[], &mut buf);
        assert_eq!((n, buf[0]), (4, 0));
    }

    #[test]
    fn path_args_validate_the_nul_rules_by_name() {
        assert_eq!(path_arg(b"/tmp/e7/x.img\0rest", 0), Some("/tmp/e7/x.img"));
        assert_eq!(path_arg(b"/tmp/a\0/two\0", 1), Some("/two"));
        /* no NUL — the kernel's plan always attaches one; a payload
         * without it is malformed, never read past. */
        assert_eq!(path_arg(b"/tmp/no-nul", 0), None);
        /* over the cap: refused by name (the twin of the kernel's
         * first-trap -ENAMETOOLONG). */
        let mut over = alloc::vec::Vec::new();
        over.extend_from_slice(b"/");
        over.extend_from_slice(&[b'a'; LINUX_PATH_CAP]);
        over.push(0);
        assert_eq!(path_arg(&over, 0), None);
        /* a second string whose index does not exist. */
        assert_eq!(path_arg(b"only-one\0", 1), None);
        /* non-UTF-8 is refused, not lossily coerced. */
        assert_eq!(path_arg(&[0x2f, 0xff, 0xfe, 0], 0), None);
        let p = path_pair(b"/old.partial\0/new.img\0").expect("pair");
        assert_eq!(p, ("/old.partial", "/new.img"));
        assert!(path_pair(b"/old.partial\0/no-second-nul").is_none());
    }

    /* M4.3: the sidecar table split, driven without a Vfs. The group owns
     * the fds/heap/dispositions, the thread owns its channel/name/private
     * table, and the LAST release retires the group exactly once. A private
     * clone (no CLONE_FILES) starts from a copy and cannot mutate the
     * group's table; a shared clone sees every slot the leader opened. */
    fn reset_registry() {
        for t in tasks().iter_mut() {
            t.used = false;
        }
        for g in groups().iter_mut() {
            *g = LinuxGroup::new();
        }
    }

    #[test]
    fn clone_tracking_shares_group_state_and_last_release_retires_it() {
        /* the registry is process-global and every task test rewrites it:
         * without the lock the m4.10/11 tests race this slot's tid. */
        let _guard = M4_10_TEST_LOCK.lock().unwrap();
        reset_registry();
        assert!(track(0x10, 0x11, "m4.3.leader", 0,
                      0x80_0000, 0x81_0000, (0, 0), 7, 0x7000, false));
        let gid = tasks()[0].group;
        assert_eq!(tasks()[0].sig.tid, 0x7000, "the leader's real TID");
        assert_eq!(tasks()[0].sig.tgid, 0x7000, "the leader is the TGID");
        assert!(groups()[gid].used);
        assert_eq!(groups()[gid].threads, 1);
        assert!(!tasks()[0].owns_fds, "the leader reads the group table");
        groups()[gid].fds[3] = FdSlot { used: true, kind: FD_KIND_FILE,
                                      vfs_fd: 9, flags: 0 };
        groups()[gid].brk_cur = 0x80_3000;
        groups()[gid].sig_dispo[10] = DISPO_HANDLER;
        groups()[gid].sig_act[10] = SigAct { handler: 0x401000, flags: 0,
                                             restorer: 0, mask: 0 };
        tasks()[0].sig.mask = 1 << 5; /* SIGABRT blocked */

        assert_eq!(track_thread(0x20, 0x21, "m4.3.leader.t20", 0, 8, true,
                                0x7001, 0x9000),
                   Some(gid as u32));
        assert_eq!(tasks()[1].sig.tid, 0x7001);
        assert_eq!(tasks()[1].sig.tgid, 0x7000);
        assert_eq!(tasks()[1].sig.clear_tid, 0x9000);
        assert_eq!(groups()[gid].threads, 2);
        assert_eq!(tasks()[1].group, gid);
        assert!(!tasks()[1].owns_fds, "CLONE_FILES shares the group table");
        assert_eq!(groups()[gid].fds[3].vfs_fd, 9,
                   "the sibling sees the leader's open fd");
        assert_eq!(groups()[gid].brk_cur, 0x80_3000,
                   "the sibling sees the group's break");
        assert_eq!(tasks()[1].sig.dispo[10], DISPO_HANDLER,
                   "the sibling sees the group's disposition");
        assert_eq!(tasks()[1].sig.act[10].handler, 0x401000,
                   "the handler details are the group's");
        assert_eq!(tasks()[1].sig.mask, 1 << 5,
                   "the new thread inherits the creator's blocked mask");
        assert_eq!(tasks()[1].sig.pending, 0,
                   "pending signals are per-thread and start clear");

        assert_eq!(track_thread(0x30, 0x31, "m4.3.leader.t30", 0, 9, false,
                                0x7002, 0),
                   Some(gid as u32));
        assert!(tasks()[2].owns_fds, "no CLONE_FILES: private table copy");
        assert_eq!(tasks()[2].fds[3].vfs_fd, 9,
                   "the private clone starts from the group's slots");
        tasks()[2].fds[3].vfs_fd = 10;
        tasks()[2].fds[3].used = false;
        assert_eq!(groups()[gid].fds[3].vfs_fd, 9,
                   "a private close cannot spend the group's slot");
        assert_eq!(groups()[gid].threads, 3);

        assert_eq!(drop_task_state(0), Some(7));
        assert!(groups()[gid].used, "the group survives the leader");
        assert_eq!(groups()[gid].threads, 2);
        assert_eq!(drop_task_state(1), Some(8));
        assert!(groups()[gid].used, "the group survives the first thread");
        assert_eq!(groups()[gid].threads, 1);
        assert_eq!(drop_task_state(2), Some(9));
        assert!(!groups()[gid].used, "the last release retires the group");
        assert_eq!(groups()[gid].threads, 0);
        assert_eq!(drop_task_state(2), None, "release is idempotent");
        assert!(!tasks()[0].used && !tasks()[1].used && !tasks()[2].used);
        assert!(can_track_thread());
    }

    fn futex_payload(observed: u32, timeout: Option<(u64, u64)>) -> [u8; 24] {
        let mut b = [0u8; 24];
        b[0..4].copy_from_slice(&observed.to_le_bytes());
        if let Some((sec, nsec)) = timeout {
            b[4] = 1;
            b[8..16].copy_from_slice(&sec.to_le_bytes());
            b[16..24].copy_from_slice(&nsec.to_le_bytes());
        }
        b
    }

    #[test]
    fn futex_wait_wake_is_group_keyed_fifo_with_timeout_and_release() {
        let _guard = M4_10_TEST_LOCK.lock().unwrap();
        reset_registry();
        assert!(track(0x10, 0x11, "m4.4.leader", 0, 0x80_0000, 0x81_0000, (0, 0),
                      7, 0x7000, false));
        let gid = tasks()[0].group;
        for (i, (ch_r, ch_w, name)) in [
            (0x20, 0x21, "m4.4.leader.t20"),
            (0x30, 0x31, "m4.4.leader.t30"),
            (0x40, 0x41, "m4.4.leader.t40"),
        ].iter().enumerate() {
            assert_eq!(track_thread(*ch_r, *ch_w, name, 0, 8 + i as u32, true,
                                    0x7001 + i as u64, 0),
                       Some(gid as u32));
        }
        let uaddr = 0x7000_1000u64;
        let other = uaddr + 8;
        let args = [uaddr, FUTEX_WAIT | FUTEX_PRIVATE, 7, 0, 0, 0];
        match answer_futex(&mut groups()[gid], 1, &args,
                           &futex_payload(7, None), 1_000) {
            Answer::FutexWait(got, deadline) => {
                assert_eq!((got, deadline), (uaddr, 0));
            }
            other => panic!("wait did not park: {:?}", other),
        }
        assert!(groups()[gid].futex[0].used);
        assert_eq!(groups()[gid].futex[0].task, 1);

        let mismatch = [uaddr, FUTEX_WAIT | FUTEX_PRIVATE, 7, 0, 0, 0];
        assert!(matches!(answer_futex(&mut groups()[gid], 2, &mismatch,
                                      &futex_payload(8, None), 1_000),
                         Answer::Reply(0, E_AGAIN)));
        assert_eq!(groups()[gid].futex.iter().filter(|w| w.used).count(), 1,
                   "a mismatched expected value must not enqueue");

        assert!(matches!(answer_futex(&mut groups()[gid], 2, &args,
                                      &futex_payload(7, None), 1_000),
                         Answer::FutexWait(_, 0)));
        let args_other = [other, FUTEX_WAIT | FUTEX_PRIVATE, 0, 0, 0, 0];
        assert!(matches!(answer_futex(&mut groups()[gid], 3, &args_other,
                                      &futex_payload(0, None), 1_000),
                         Answer::FutexWait(_, 0)));

        let two = match answer_futex(&mut groups()[gid], 0,
                                     &[uaddr, FUTEX_WAKE | FUTEX_PRIVATE, 2, 0, 0, 0],
                                     &[], 1_000) {
            Answer::FutexWake(w) => w,
            other => panic!("wake did not report waiters: {:?}", other),
        };
        assert_eq!(two.n, 2);
        assert_eq!(&two.tasks[..2], &[1, 2], "FIFO by the group's sequence");
        assert_eq!(groups()[gid].futex.iter().filter(|w| w.used).count(), 1);

        let one = match answer_futex(&mut groups()[gid], 0,
                                     &[other, FUTEX_WAKE | FUTEX_PRIVATE, 1, 0, 0, 0],
                                     &[], 1_000) {
            Answer::FutexWake(w) => w,
            other => panic!("wake did not report the other key: {:?}", other),
        };
        assert_eq!(one.n, 1);
        assert_eq!(one.tasks[0], 3);

        let timed = [uaddr, FUTEX_WAIT | FUTEX_PRIVATE, 7, 0x1234, 0, 0];
        match answer_futex(&mut groups()[gid], 1, &timed,
                           &futex_payload(7, Some((0, 10_000_000))), 1_000) {
            Answer::FutexWait(_, deadline) => {
                assert_eq!(deadline, 1_000 + 10_000_000);
                assert!(poll_expired(deadline, deadline));
            }
            other => panic!("timed wait did not park: {:?}", other),
        }
        assert!(matches!(answer_futex(&mut groups()[gid], 2, &timed,
                                      &futex_payload(7, Some((0, 0))), 1_000),
                         Answer::Reply(0, E_TIMEOUT)));
        assert!(matches!(answer_futex(&mut groups()[gid], 2, &timed,
                                      &futex_payload(7, Some((0, 1_000_000_000))), 1_000),
                         Answer::Reply(0, E_INVAL)));

        /* A released waiter is removed from the queue before its slot is
         * reused; a later wake must not answer a dead task index. */
        assert_eq!(drop_task_state(1), Some(8));
        assert_eq!(groups()[gid].futex.iter().filter(|w| w.used).count(), 0);
        let none = match answer_futex(&mut groups()[gid], 0,
                                      &[uaddr, FUTEX_WAKE | FUTEX_PRIVATE, 8, 0, 0, 0],
                                      &[], 1_000) {
            Answer::FutexWake(w) => w,
            other => panic!("wake after release: {:?}", other),
        };
        assert_eq!(none.n, 0);

        assert_eq!(drop_task_state(2), Some(9));
        assert_eq!(drop_task_state(3), Some(10));
        assert_eq!(drop_task_state(0), Some(7));
        assert!(!groups()[gid].used);
    }

    #[test]
    fn futex_bitset_matches_only_intersecting_wakes_and_is_absolute() {
        /* A local group keeps this queue test independent of the global task
         * registry, so the parallel unit suite cannot make its records race. */
        let mut g = LinuxGroup::new();
        let uaddr = 0x7000_2000u64;
        let wait_a = [uaddr, FUTEX_WAIT_BITSET | FUTEX_PRIVATE, 7, 0, 0, 0b01];
        let wait_b = [uaddr, FUTEX_WAIT_BITSET | FUTEX_PRIVATE, 7, 0, 0, 0b10];
        assert!(matches!(answer_futex(&mut g, 1, &wait_a,
                                      &futex_payload(7, None), 1_000),
                         Answer::FutexWait(_, 0)));
        assert!(matches!(answer_futex(&mut g, 2, &wait_b,
                                      &futex_payload(7, None), 1_000),
                         Answer::FutexWait(_, 0)));
        assert_eq!(g.futex.iter().filter(|w| w.used).count(), 2);

        let one = match answer_futex(&mut g, 0,
                                     &[uaddr, FUTEX_WAKE_BITSET | FUTEX_PRIVATE,
                                       8, 0, 0, 0b01],
                                     &[], 1_000) {
            Answer::FutexWake(w) => w,
            other => panic!("bitset wake did not report waiters: {:?}", other),
        };
        assert_eq!(one.n, 1, "only the intersecting bitset is woken");
        assert_eq!(one.tasks[0], 1);
        assert_eq!(g.futex.iter().filter(|w| w.used).count(), 1);

        let one = match answer_futex(&mut g, 0,
                                     &[uaddr, FUTEX_WAKE_BITSET | FUTEX_PRIVATE,
                                       8, 0, 0, 0b10],
                                     &[], 1_000) {
            Answer::FutexWake(w) => w,
            other => panic!("second bitset wake did not report a waiter: {:?}", other),
        };
        assert_eq!(one.n, 1);
        assert_eq!(one.tasks[0], 2);
        assert_eq!(g.futex.iter().filter(|w| w.used).count(), 0);

        /* WAIT_BITSET's timespec is absolute; a zero deadline is already
         * expired and must not enqueue. */
        assert!(matches!(answer_futex(&mut g, 1, &wait_a,
                                      &futex_payload(7, Some((0, 0))), 1_000),
                         Answer::Reply(0, E_TIMEOUT)));
        assert_eq!(g.futex.iter().filter(|w| w.used).count(), 0);

        /* Linux refuses the empty bitset on both sides; it is not MATCH_ANY. */
        let empty = [uaddr, FUTEX_WAIT_BITSET | FUTEX_PRIVATE, 7, 0, 0, 0];
        assert!(matches!(answer_futex(&mut g, 1, &empty,
                                      &futex_payload(7, None), 1_000),
                         Answer::Reply(0, E_INVAL)));
        let empty_wake = [uaddr, FUTEX_WAKE_BITSET | FUTEX_PRIVATE, 8, 0, 0, 0];
        assert!(matches!(answer_futex(&mut g, 0, &empty_wake, &[], 1_000),
                         Answer::Reply(0, E_INVAL)));
        assert_eq!(g.futex.iter().filter(|w| w.used).count(), 0);
    }

    #[test]
    fn m4_8_requeue_moves_the_oldest_remainder_and_cmp_checks_the_destination() {
        let mut g = LinuxGroup::new();
        let from = 0x7000_3000u64;
        let to = 0x7000_4000u64;
        for t in 0..3u8 {
            assert!(futex_enqueue(&mut g, t as usize, from, u32::MAX));
        }
        let (w, moved) = futex_requeue(&mut g, from, to, 1, 1, u32::MAX);
        assert_eq!((w.n, w.tasks[0], moved), (1, 0, 1));
        assert_eq!(g.futex.iter().filter(|q| q.used && q.uaddr == to).count(), 1);
        assert_eq!(g.futex.iter().filter(|q| q.used && q.uaddr == from).count(), 1);
        assert_eq!(g.futex.iter().filter(|q| q.used).count(), 2);

        /* CMP_REQUEUE's attached word is compared before any queue move; a
         * mismatch is EAGAIN and leaves the queue untouched. */
        let cmp = [from, FUTEX_CMP_REQUEUE | FUTEX_PRIVATE, 0, 1, to, 7];
        let payload = 8u32.to_le_bytes();
        assert!(matches!(answer_futex(&mut g, 0, &cmp, &payload, 1_000),
                         Answer::Reply(0, E_AGAIN)));
        assert_eq!(g.futex.iter().filter(|q| q.used).count(), 2);
        let payload = 7u32.to_le_bytes();
        match answer_futex(&mut g, 0, &cmp, &payload, 1_000) {
            Answer::FutexRequeue(_, dst, moved) => {
                assert_eq!((dst, moved), (to, 1));
            }
            other => panic!("CMP_REQUEUE did not move: {:?}", other),
        }
        assert_eq!(g.futex.iter().filter(|q| q.used && q.uaddr == to).count(), 2);
        assert_eq!(g.futex.iter().filter(|q| q.used).count(), 2);
    }

    #[test]
    fn m4_8_requeue_appends_moved_waiters_to_destination_tail() {
        let mut g = LinuxGroup::new();
        let from = 0x7000_5000u64;
        let to = 0x7000_6000u64;
        assert!(futex_enqueue(&mut g, 0, from, u32::MAX));
        assert!(futex_enqueue(&mut g, 1, to, u32::MAX));
        let (_, moved) = futex_requeue(&mut g, from, to, 0, 1, u32::MAX);
        assert_eq!(moved, 1);
        /* The destination's existing waiter (1) was enqueued first and must
         * be woken before the moved source waiter (0). */
        let w = futex_wake(&mut g, to, 2, u32::MAX);
        assert_eq!((w.n, w.tasks[0], w.tasks[1]), (2, 1, 0));
    }

    /* M4.10: the two focused tests both drive the process-global task
     * table; keep them from resetting each other's registry. */
    static M4_10_TEST_LOCK: std::sync::Mutex<()> = std::sync::Mutex::new(());

    /* M4.10: a process-directed handler is not refused any more. The
     * sidecar chooses a parked sibling and hands the caller the TARGET's
     * tid plus a delivery whose sysret is -EINTR; the target's own reply
     * carries the trailer with its real parked syscall number. */
    #[test]
    fn m4_10_group_handler_targets_a_parked_sibling() {
        let _guard = M4_10_TEST_LOCK.lock().unwrap();
        reset_registry();
        assert!(track(0x10, 0x11, "m4.10.leader", 0,
                      0x80_0000, 0x81_0000, (0, 0), 7, 0x7000, false));
        let gid = tasks()[0].group;
        assert_eq!(track_thread(0x20, 0x21, "m4.10.leader.t20", 0, 8, true,
                                0x7001, 0x9000),
                   Some(gid as u32));
        groups()[gid].sig_dispo[10] = DISPO_HANDLER;
        groups()[gid].sig_act[10] = SigAct { handler: 0x401000,
                                             flags: SA_RESTORER,
                                             restorer: 0x402000, mask: 0 };
        tasks()[1].pending = PENDING_FUTEX;
        tasks()[1].pending_num = LINUX_FUTEX;
        tasks()[1].pending_tag = 0x55;

        let a = answer_kill(gid, 0, 0x7000,
                            &[0x7000, 10, 0, 0, 0, 0]).unwrap();
        match a {
            Answer::SignalHandlerTask(tid, d) => {
                assert_eq!(tid, 0x7001, "the parked sibling, not the caller");
                assert_eq!(d.signo, 10);
                assert_eq!(d.handler, 0x401000);
                assert_eq!(d.sysret, (-LINUX_EINTR) as u64,
                           "the interrupted futex returns -EINTR");
            }
            other => panic!("handler was not delivered to the sibling: {:?}",
                            other),
        }
        assert_ne!(tasks()[1].sig.mask & (1u64 << 9), 0,
                   "the sibling's handler mask took the signal");
        assert_eq!(tasks()[0].sig.mask & (1u64 << 9), 0,
                   "the caller's mask was not touched");
        assert_eq!(tasks()[1].pending, PENDING_FUTEX,
                   "the service layer still owns clearing the pending slot");
    }

    /* M4.10: a stop-default group signal holds a parked sibling's reply,
     * and SIGCONT releases it with the same pending exchange intact so the
     * service can answer -EINTR. */
    #[test]
    fn m4_10_group_stop_holds_then_sigcont_resumes_the_sibling() {
        let _guard = M4_10_TEST_LOCK.lock().unwrap();
        reset_registry();
        assert!(track(0x10, 0x11, "m4.10.stop.leader", 0,
                      0x80_0000, 0x81_0000, (0, 0), 7, 0x7100, false));
        let gid = tasks()[0].group;
        assert_eq!(track_thread(0x20, 0x21, "m4.10.stop.leader.t20", 0, 8,
                                true, 0x7101, 0),
                   Some(gid as u32));
        tasks()[1].pending = PENDING_FUTEX;
        tasks()[1].pending_num = LINUX_FUTEX;
        tasks()[1].pending_tag = 0x66;

        match answer_kill(gid, 0, 0x7100,
                          &[0x7100, 19, 0, 0, 0, 0]).unwrap() {
            Answer::SignalStopGroup(g) => {
                assert_eq!((g.n, g.tasks[0], g.sig, g.parked),
                           (1, 1, 19, 1));
            }
            other => panic!("SIGSTOP did not stop the sibling: {:?}", other),
        }
        assert!(tasks()[1].stopped, "the stop latch is set");
        assert_eq!(tasks()[1].pending, PENDING_FUTEX,
                   "the parked reply is held, not answered");

        match answer_kill(gid, 0, 0x7100,
                          &[0x7100, 18, 0, 0, 0, 0]).unwrap() {
            Answer::SignalContinue(c) => {
                assert_eq!(c.n, 1);
                assert_eq!(c.tasks[0], 1);
            }
            other => panic!("SIGCONT did not resume the sibling: {:?}", other),
        }
        assert!(!tasks()[1].stopped, "the stop latch is clear");
        assert_eq!(tasks()[1].pending, PENDING_FUTEX,
                   "the held exchange is still the one to interrupt");
        assert_eq!(tasks()[1].pending_num, LINUX_FUTEX);
    }

    /* M4.11: a process-directed stop latches EVERY non-caller sibling, not
     * just the first parked one. A parked target keeps its reply; a target
     * between syscalls is represented in the same set so `service` can hold
     * its kernel task until SIGCONT. */
    #[test]
    fn m4_11_group_stop_broadcasts_to_parked_and_running_siblings() {
        let _guard = M4_10_TEST_LOCK.lock().unwrap();
        reset_registry();
        assert!(track(0x10, 0x11, "m4.11.stop.leader", 0,
                      0x80_0000, 0x81_0000, (0, 0), 7, 0x7200, false));
        let gid = tasks()[0].group;
        assert_eq!(track_thread(0x20, 0x21, "m4.11.stop.leader.t20", 0, 8,
                                true, 0x7201, 0),
                   Some(gid as u32));
        assert_eq!(track_thread(0x30, 0x31, "m4.11.stop.leader.t30", 0, 8,
                                true, 0x7202, 0),
                   Some(gid as u32));
        tasks()[1].pending = PENDING_FUTEX;
        tasks()[1].pending_num = LINUX_FUTEX;
        tasks()[1].pending_tag = 0x77;
        tasks()[2].pending = PENDING_NONE;

        match answer_kill(gid, 0, 0x7200,
                          &[0x7200, 19, 0, 0, 0, 0]).unwrap() {
            Answer::SignalStopGroup(g) => {
                assert_eq!(g.n, 2, "both siblings are in the broadcast");
                assert_eq!(&g.tasks[..2], &[1, 2]);
                assert_eq!((g.sig, g.parked), (19, 1));
            }
            other => panic!("group stop did not broadcast: {:?}", other),
        }
        assert!(tasks()[1].stopped && tasks()[2].stopped);
        assert_eq!(tasks()[1].pending, PENDING_FUTEX,
                   "the parked reply remains for SIGCONT");
        assert_eq!(tasks()[2].pending, PENDING_NONE,
                   "the running target has no reply to hold");

        match answer_kill(gid, 0, 0x7200,
                          &[0x7200, 18, 0, 0, 0, 0]).unwrap() {
            Answer::SignalContinue(c) => {
                assert_eq!(c.n, 2);
                assert_eq!(&c.tasks[..2], &[1, 2]);
            }
            other => panic!("group SIGCONT did not resume: {:?}", other),
        }
        assert!(!tasks()[1].stopped && !tasks()[2].stopped);
        assert_eq!(tasks()[1].pending, PENDING_FUTEX);
    }

    /* M4.11: a broadcast that cannot hold every running sibling is not
     * committed. The one successful hold is released, the parked target's
     * FUTEX_WAIT is restored, and every stop latch is cleared before the
     * caller receives E_CONFLICT. */
    struct StopHoldKernel {
        fail_pid: u32,
        releases: RefCell<Vec<u32>>,
    }

    impl Kernel for StopHoldKernel {
        fn wait(&self, _: &[u32], _: u64) -> Result<(usize, u16), i32> {
            Err(E_CONFLICT)
        }
        fn recv(&self, _: u32, _: &mut [u8], _: &mut [GrantedCap])
            -> Result<aerosls_proto::kabi::RecvResult, i32> {
            Err(E_CONFLICT)
        }
        fn send(&self, _: u32, _: u32, _: u16, _: &[u8],
                _: &[aerosls_proto::kabi::SendCap], _: u64)
            -> Result<(), i32> {
            Err(E_CONFLICT)
        }
        fn close(&self, _: u32, _: u16, _: u32) -> Result<(), i32> {
            Ok(())
        }
        fn cap_info(&self, _: u32)
            -> Result<aerosls_proto::kabi::CapInfo, i32> {
            Err(E_CONFLICT)
        }
        fn proc_hold(&self, pid: u32) -> Result<(), i32> {
            if pid == self.fail_pid { Err(E_AGAIN) } else { Ok(()) }
        }
        fn proc_release(&self, pid: u32) -> Result<(), i32> {
            self.releases.borrow_mut().push(pid);
            Ok(())
        }
    }

    #[test]
    fn m4_11_group_stop_failure_rolls_back_held_and_latched_siblings() {
        let _guard = M4_10_TEST_LOCK.lock().unwrap();
        reset_registry();
        assert!(track(0x10, 0x11, "m4.11.rollback.leader", 0,
                      0x80_0000, 0x81_0000, (0, 0), 7, 0x7500, false));
        let gid = tasks()[0].group;
        for (ch, tid, name) in [
            (0x20, 0x7501, "m4.11.rollback.leader.t20"),
            (0x30, 0x7502, "m4.11.rollback.leader.t30"),
            (0x40, 0x7503, "m4.11.rollback.leader.t40"),
        ] {
            assert_eq!(track_thread(ch, ch + 1, name, 0, 8, true, tid, 0),
                       Some(gid as u32));
        }

        let uaddr = 0x7500_1000u64;
        tasks()[1].pending = PENDING_FUTEX;
        tasks()[1].pending_num = LINUX_FUTEX;
        tasks()[1].pending_tag = 0x7501;
        tasks()[1].pending_arg = uaddr;
        assert!(futex_enqueue(&mut groups()[gid], 1, uaddr, u32::MAX));

        let grp = match answer_kill(gid, 0, 0x7500,
                                    &[0x7500, 19, 0, 0, 0, 0]).unwrap() {
            Answer::SignalStopGroup(g) => g,
            other => panic!("SIGSTOP did not broadcast: {:?}", other),
        };
        assert_eq!((grp.n, grp.parked), (3, 1));
        assert!(tasks()[1].stopped && tasks()[2].stopped && tasks()[3].stopped);
        assert_eq!(groups()[gid].futex.iter().filter(|w| w.used).count(), 0,
                   "stop removes the parked waiter until commit");

        let k = StopHoldKernel { fail_pid: 0x7503,
                                 releases: RefCell::new(Vec::new()) };
        match hold_stop_group(&k, gid, &grp) {
            Err(E_CONFLICT) => {}
            other => panic!("failed hold did not return a clean conflict: {:?}",
                            other),
        }
        assert!(!tasks()[1].stopped && !tasks()[2].stopped &&
                !tasks()[3].stopped,
                "no sibling may remain latched after rollback");
        assert!(!tasks()[1].run_held && !tasks()[2].run_held &&
                !tasks()[3].run_held,
                "no sibling may remain kernel-held after rollback");
        assert_eq!(&*k.releases.borrow(), &[0x7502],
                   "the successful hold was released exactly once");
        assert_eq!(groups()[gid].futex.iter().filter(|w| w.used).count(), 1);
        let restored = groups()[gid].futex.iter().find(|w| w.used).unwrap();
        assert_eq!((restored.task, restored.uaddr, restored.bitset),
                   (1, uaddr, u32::MAX));
        assert_eq!(tasks()[1].pending, PENDING_FUTEX,
                   "the parked exchange is still the one to wake");
    }

    /* M4.11: a handler for a sibling with no parked exchange is a direct
     * ring3 install, not the parked-reply trailer. The sidecar must leave
     * the sysret at 0 (the interrupted task resumes its own rax) and mark
     * only the target's mask. */
    #[test]
    fn m4_11_running_sibling_handler_uses_direct_delivery() {
        let _guard = M4_10_TEST_LOCK.lock().unwrap();
        reset_registry();
        assert!(track(0x10, 0x11, "m4.11.run.leader", 0,
                      0x80_0000, 0x81_0000, (0, 0), 7, 0x7300, false));
        let gid = tasks()[0].group;
        assert_eq!(track_thread(0x20, 0x21, "m4.11.run.leader.t20", 0, 8,
                                true, 0x7301, 0),
                   Some(gid as u32));
        groups()[gid].sig_dispo[12] = DISPO_HANDLER;
        groups()[gid].sig_act[12] = SigAct { handler: 0x403000,
                                             flags: SA_RESTORER,
                                             restorer: 0x404000, mask: 0 };
        assert_eq!(tasks()[1].pending, PENDING_NONE);

        let a = answer_tgkill(gid, 0x7300,
                              &[0x7300, 0x7301, 12, 0, 0, 0]).unwrap();
        match a {
            Answer::SignalHandlerRun(tid, d) => {
                assert_eq!(tid, 0x7301);
                assert_eq!(d.signo, 12);
                assert_eq!(d.handler, 0x403000);
                assert_eq!(d.sysret, 0,
                           "a running sibling resumes its own rax, not -EINTR");
            }
            other => panic!("running sibling did not get a direct frame: {:?}",
                            other),
        }
        assert_ne!(tasks()[1].sig.mask & (1u64 << 11), 0);
        assert_eq!(tasks()[1].pending, PENDING_NONE,
                   "no parked exchange was created");
        assert_eq!(tasks()[0].sig.mask & (1u64 << 11), 0);
    }

    /* M4.11: the process-directed handler path also reaches a running
     * sibling when no sibling is parked. This is the group-signal fallback,
     * distinct from the explicit tkill case above. */
    #[test]
    fn m4_11_group_handler_falls_back_to_a_running_sibling() {
        let _guard = M4_10_TEST_LOCK.lock().unwrap();
        reset_registry();
        assert!(track(0x10, 0x11, "m4.11.grouprun.leader", 0,
                      0x80_0000, 0x81_0000, (0, 0), 7, 0x7400, false));
        let gid = tasks()[0].group;
        assert_eq!(track_thread(0x20, 0x21, "m4.11.grouprun.leader.t20", 0, 8,
                                true, 0x7401, 0),
                   Some(gid as u32));
        groups()[gid].sig_dispo[10] = DISPO_HANDLER;
        groups()[gid].sig_act[10] = SigAct { handler: 0x405000,
                                             flags: SA_RESTORER,
                                             restorer: 0x406000, mask: 0 };
        assert_eq!(tasks()[1].pending, PENDING_NONE);

        match answer_kill(gid, 0, 0x7400,
                          &[0x7400, 10, 0, 0, 0, 0]).unwrap() {
            Answer::SignalHandlerRun(tid, d) => {
                assert_eq!(tid, 0x7401);
                assert_eq!((d.signo, d.sysret), (10, 0));
            }
            other => panic!("group handler did not target the running sibling: {:?}",
                            other),
        }
    }

    #[test]
    fn m4_8_expected_lengths_include_clone3_and_cmp_requeue() {
        let mut a = [0u64; 6];
        assert_eq!(expected_req_len(LINUX_CLONE3, &a),
                   LINUX_SYSCALL_REQ_BYTES + LINUX_CLONE_ARGS_BYTES);
        a[1] = FUTEX_CMP_REQUEUE | FUTEX_PRIVATE;
        assert_eq!(expected_req_len(LINUX_FUTEX, &a),
                   LINUX_SYSCALL_REQ_BYTES + FUTEX_REQUEUE_PAYLOAD_BYTES);
        a[1] = FUTEX_REQUEUE | FUTEX_PRIVATE;
        assert_eq!(expected_req_len(LINUX_FUTEX, &a), LINUX_SYSCALL_REQ_BYTES);
    }

    /* M2b: the reply builder's buffer is the LARGEST message any number
     * can send — a fill round's 4024 bytes plus the 72-byte header. It was
     * once FD_PAYLOAD_CAP (1024) while the fill arm produced 4024, and the
     * tenant panicked on the first round with "range end index 4096 out of
     * range for slice of length 1096". This test drives the real builder
     * with the maximum round: a shrunken buffer panics here, and a builder
     * that drops any round byte fails the length pin. */
    #[derive(Default)]
    struct SendRec(RefCell<Vec<Vec<u8>>>);

    impl Kernel for SendRec {
        fn wait(&self, _: &[u32], _: u64) -> Result<(usize, u16), i32> {
            Err(E_CONFLICT)
        }
        fn recv(&self, _: u32, _: &mut [u8], _: &mut [GrantedCap])
            -> Result<aerosls_proto::kabi::RecvResult, i32> {
            Err(E_CONFLICT)
        }
        fn send(&self, _: u32, _: u32, _: u16, payload: &[u8],
                _: &[aerosls_proto::kabi::SendCap], _: u64)
            -> Result<(), i32> {
            self.0.borrow_mut().push(payload.to_vec());
            Ok(())
        }
        fn close(&self, _: u32, _: u16, _: u32) -> Result<(), i32> {
            Ok(())
        }
        fn cap_info(&self, _: u32)
            -> Result<aerosls_proto::kabi::CapInfo, i32> {
            Err(E_CONFLICT)
        }
        fn proc_hold(&self, _: u32) -> Result<(), i32> { Ok(()) }
        fn proc_release(&self, _: u32) -> Result<(), i32> { Ok(()) }
    }

    #[test]
    fn file_maps_carry_the_requested_fd_and_offset() {
        let mut b = span();
        let mut f = fds();
        f[3] = FdSlot { used: true, kind: FD_KIND_FILE, vfs_fd: 7, flags: 0 };
        /* the loader's .text segment, as the kernel stages it: FIXED,
         * READ|EXEC, fd 3, file offset 0x1000 */
        match answer(9, &[SPAN_BASE + 0x1000, 0x1000, 0x5, 0x12, 3, 0x1000],
                     &f, &mut b, &mut fresh(), &[]) {
            Answer::MmapFile(ret, vfd, foff, flen) => {
                assert_eq!(ret, SPAN_BASE + 0x1000,
                           "a FIXED grant echoes the address asked");
                assert_eq!((vfd, foff, flen), (7, 0x1000, 0x1000),
                           concat!("the answer names the VFS fd AND the ",
                                   "file offset the fill rounds read from"));
            }
            other => panic!("RX file map: {:?}", other),
        }
        /* the writable .data segment: served, and the answer names the
         * same source words so its fill rounds read from 0x2000 */
        match answer(9, &[SPAN_BASE + 0x2000, 0x2000, 0x3, 0x12, 3, 0x2000],
                     &f, &mut b, &mut fresh(), &[]) {
            Answer::MmapFile(ret, vfd, foff, flen) => {
                assert_eq!((ret, vfd, foff, flen),
                           (SPAN_BASE + 0x2000, 7, 0x2000, 0x2000),
                           "an RW file map is granted from the same words");
            }
            other => panic!("RW file map: {:?}", other),
        }
        /* W|X stays refused at the sidecar's own door */
        assert!(matches!(
            answer(9, &[SPAN_BASE + 0x2000, 0x2000, 0x7, 0x12, 3, 0x2000],
                   &f, &mut b, &mut fresh(), &[]),
            Answer::Reply(0, E_PERM)),
            "a W|X file map is EPERM, never a grant");
    }

    #[test]
    fn reply_bytes_sends_a_full_fill_round_whole() {
        let k = SendRec::default();
        let round = [0xA5u8; FD_FILL_CAP];
        reply_bytes(&k, 0x21, 0x99, LINUX_MMAP_FILL, 0, &round);
        let sent = k.0.borrow();
        assert_eq!(sent.len(), 1, "one reply message per round");
        let m = &sent[0];
        assert_eq!(m.len(), LINUX_SYSCALL_REQ_BYTES + FD_FILL_CAP,
                   "the whole round plus its header crosses");
        assert_eq!(m.len(), 4096,
                   "72 + 4024 is the transport's 4 KiB payload bound");
        assert_eq!(&m[0..8], &LINUX_MMAP_FILL.to_le_bytes(),
                   "the round's number is in the header");
        assert!(m[LINUX_SYSCALL_REQ_BYTES..].iter().all(|&b| b == 0xA5),
                "every round byte crosses — not the buffer's zero padding");
    }
}
