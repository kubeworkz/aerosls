//! M4.8 live hardening gate.
//!
//! One static musl process creates two kernel threads through clone3(435),
//! requeues a parent waiter from A to B, exits a thread with a robust futex
//! list (the kernel ORs FUTEX_OWNER_DIED and the sidecar wakes the waiter),
//! and tgkills a parked sibling by TGID/TID. It ends with a group-directed
//! kill so the existing group sweep is the final evidence.
//!
//! Markers:
//!   M4-CLONE3-OK, M4-REQUEUE-OK, M4-ROBUST-OK, M4-TGKILL-OK,
//!   M4-HARDENING-PASS, M4-GROUP-KILL-SENT; failures M4-HARDENING-FAIL.
//!
//! The static stack, non-PIE link and raw clone wrapper follow M4.7's
//! candidate: musl pthread is deliberately not used.

use core::cell::UnsafeCell;
use std::os::raw::{c_long, c_uint};
use std::sync::atomic::{AtomicU64, Ordering};

const SYS_FUTEX: c_long = 202;
const SYS_GETPID: c_long = 39;
const SYS_GETTID: c_long = 186;
const SYS_WRITE: c_long = 1;
const SYS_EXIT: c_long = 60;
const SYS_YIELD: c_long = 24;
const SYS_SET_ROBUST_LIST: c_long = 273;
const SYS_TGKILL: c_long = 234;
const SYS_KILL: c_long = 62;
const SYS_RT_SIGACTION: c_long = 13;

const FUTEX_WAIT_PRIVATE: c_long = 128;
const FUTEX_WAKE_PRIVATE: c_long = 129;
const FUTEX_REQUEUE_PRIVATE: c_long = 131;
const FUTEX_OWNER_DIED: u64 = 0x4000_0000;

const CLONE_VM: u64 = 0x0000_0100;
const CLONE_FS: u64 = 0x0000_0200;
const CLONE_FILES: u64 = 0x0000_0400;
const CLONE_SIGHAND: u64 = 0x0000_0800;
const CLONE_THREAD: u64 = 0x0001_0000;
const CLONE_SYSVSEM: u64 = 0x0004_0000;
const CLONE_CHILD_CLEARTID: u64 = 0x0020_0000;
const CLONE_CHILD_SETTID: u64 = 0x0100_0000;
const CLONE_FLAGS: u64 = CLONE_VM | CLONE_FS | CLONE_FILES | CLONE_SIGHAND
    | CLONE_THREAD | CLONE_SYSVSEM | CLONE_CHILD_SETTID | CLONE_CHILD_CLEARTID;
const SIGUSR1: u64 = 10;
const SIGUSR2: u64 = 12;
const SIGTERM: u64 = 15;
const SIGCONT: u64 = 18;
const SIGSTOP: u64 = 19;
const SA_RESTORER: u64 = 0x0400_0000;
const EAGAIN: c_long = -11;
const ETIMEDOUT: c_long = -110;

static A_WORD: AtomicU64 = AtomicU64::new(0);
static B_WORD: AtomicU64 = AtomicU64::new(0);
static DONE_WORD: AtomicU64 = AtomicU64::new(0);
static GO_EXIT: AtomicU64 = AtomicU64::new(0);
static ROBUST_READY: AtomicU64 = AtomicU64::new(0);
static ROBUST_WORD: AtomicU64 = AtomicU64::new(0);
static CTID1: AtomicU64 = AtomicU64::new(0);
static TID1: AtomicU64 = AtomicU64::new(0);
static WORKER_PARKED: AtomicU64 = AtomicU64::new(0);
static GROUP_HANDLER_HIT: AtomicU64 = AtomicU64::new(0);
static WORKER_RESUMED: AtomicU64 = AtomicU64::new(0);
static WORKER_STOP_PARKED: AtomicU64 = AtomicU64::new(0);
static WORKER_STOP_RESUMED: AtomicU64 = AtomicU64::new(0);
static TID3: AtomicU64 = AtomicU64::new(0);
static RUNNING_HANDLER_HIT: AtomicU64 = AtomicU64::new(0);
static RUNNING_TICKS: AtomicU64 = AtomicU64::new(0);
static RUNNING_GO_EXIT: AtomicU64 = AtomicU64::new(0);

#[repr(C)]
#[derive(Clone, Copy)]
struct CloneArgs {
    flags: u64,
    pidfd: u64,
    child_tid: u64,
    parent_tid: u64,
    exit_signal: u64,
    stack: u64,
    stack_size: u64,
    tls: u64,
    set_tid: u64,
    set_tid_size: u64,
    cgroup: u64,
}

impl CloneArgs {
    const fn zero() -> Self {
        CloneArgs {
            flags: 0, pidfd: 0, child_tid: 0, parent_tid: 0,
            exit_signal: 0, stack: 0, stack_size: 0, tls: 0,
            set_tid: 0, set_tid_size: 0, cgroup: 0,
        }
    }
}

#[repr(align(16))]
struct Stack(UnsafeCell<[u8; 64 * 1024]>);
unsafe impl Sync for Stack {}
static STACK1: Stack = Stack(UnsafeCell::new([0u8; 64 * 1024]));
static STACK2: Stack = Stack(UnsafeCell::new([0u8; 64 * 1024]));
static STACK3: Stack = Stack(UnsafeCell::new([0u8; 64 * 1024]));

#[repr(C)]
struct RobustEntry {
    next: *mut RobustEntry,
}
#[repr(C)]
struct RobustHead {
    list: RobustEntry,
    futex_offset: i64,
    list_op_pending: u64,
}
struct HeadCell(UnsafeCell<RobustHead>);
struct EntryCell(UnsafeCell<RobustEntry>);
unsafe impl Sync for HeadCell {}
unsafe impl Sync for EntryCell {}
static ROBUST_HEAD: HeadCell = HeadCell(UnsafeCell::new(RobustHead {
    list: RobustEntry { next: core::ptr::null_mut() },
    futex_offset: 0,
    list_op_pending: 0,
}));
static ROBUST_ENTRY: EntryCell = EntryCell(UnsafeCell::new(RobustEntry {
    next: core::ptr::null_mut(),
}));

struct ArgsCell(UnsafeCell<CloneArgs>);
unsafe impl Sync for ArgsCell {}
static CLONE_ARGS: ArgsCell = ArgsCell(UnsafeCell::new(CloneArgs::zero()));

unsafe extern "C" {
    fn syscall(num: c_long, ...) -> c_long;
    fn aerosls_clone3_spawn(
        args: *const CloneArgs,
        size: u64,
        worker: extern "C" fn(),
    ) -> i64;
}

core::arch::global_asm!(
    r#"
    .text
    .global aerosls_clone3_spawn
    .type aerosls_clone3_spawn, @function
aerosls_clone3_spawn:
    /* rdi=args, rsi=size, rdx=worker */
    pushq %rbx
    movq %rdx, %rbx
    movl $435, %eax
    syscall
    testq %rax, %rax
    jne 2f
    xorl %ebp, %ebp
    callq *%rbx
    movl $60, %eax
    xorl %edi, %edi
    syscall
1:  jmp 1b
2:  popq %rbx
    ret
    .size aerosls_clone3_spawn, .-aerosls_clone3_spawn
    "#,
    options(att_syntax)
);

#[repr(C)]
struct Timespec {
    tv_sec: i64,
    tv_nsec: i64,
}

/* x86-64 rt_sigaction's 32-byte kernel shape: handler, flags, restorer,
 * mask. The sidecar reads the same four words. */
#[repr(C)]
struct KernelSigaction {
    handler: u64,
    flags: u64,
    restorer: u64,
    mask: u64,
}

unsafe extern "C" {
    fn aerosls_sigreturn_restorer();
}

core::arch::global_asm!(
    r#"
    .text
    .global aerosls_sigreturn_restorer
    .type aerosls_sigreturn_restorer, @function
aerosls_sigreturn_restorer:
    movl $15, %eax
    syscall
1:  jmp 1b
    .size aerosls_sigreturn_restorer, .-aerosls_sigreturn_restorer
    "#,
    options(att_syntax)
);

extern "C" fn group_handler(_signo: i32) {
    GROUP_HANDLER_HIT.fetch_add(1, Ordering::SeqCst);
    write_bytes(b"M4-GROUP-HANDLER-OK\n");
}

extern "C" fn running_handler(_signo: i32) {
    RUNNING_HANDLER_HIT.fetch_add(1, Ordering::SeqCst);
    write_bytes(b"M4-RUNNING-HANDLER-OK\n");
}

fn futex(addr: *const AtomicU64, op: c_long, val: u64,
         timeout: *const Timespec, addr2: u64, val3: u64) -> c_long {
    unsafe {
        syscall(
            SYS_FUTEX,
            addr as c_long,
            op as c_uint as c_long,
            val as c_long,
            timeout as c_long,
            addr2 as c_long,
            val3 as c_long,
        )
    }
}

fn gettid() -> u64 { unsafe { syscall(SYS_GETTID) as u64 } }
fn getpid() -> u64 { unsafe { syscall(SYS_GETPID) as u64 } }

fn exit_now() -> ! {
    unsafe { syscall(SYS_EXIT, 0 as c_long); }
    loop { core::hint::spin_loop(); }
}

fn write_bytes(mut bytes: &[u8]) {
    while !bytes.is_empty() {
        let n = unsafe {
            syscall(SYS_WRITE, 1 as c_long,
                    bytes.as_ptr() as c_long, bytes.len() as c_long)
        };
        if n <= 0 { return; }
        bytes = &bytes[n as usize..];
    }
}

fn write_u64(mut value: u64) {
    let mut buf = [0u8; 20];
    let mut i = buf.len();
    loop {
        i -= 1;
        buf[i] = b'0' + (value % 10) as u8;
        value /= 10;
        if value == 0 { break; }
    }
    write_bytes(&buf[i..]);
}

fn say(prefix: &[u8], value: u64) {
    write_bytes(prefix);
    write_u64(value);
    write_bytes(b"\n");
}

fn say_msg(msg: &[u8]) {
    write_bytes(msg);
    write_bytes(b"\n");
}

fn fail(stage: &[u8]) -> ! {
    write_bytes(b"M4-HARDENING-FAIL ");
    say_msg(stage);
    // Do not leave a wedged probe behind: the group kill is also the
    // cleanup path when an earlier arm refused.
    unsafe { syscall(SYS_KILL, getpid() as c_long, SIGTERM as c_long); }
    loop { unsafe { syscall(SYS_YIELD); } }
}

fn stack_top(stack: &Stack) -> u64 {
    unsafe {
        let s = &mut *stack.0.get();
        (s.as_mut_ptr().add(s.len()) as usize & !15) as u64
    }
}

fn spawn3(flags: u64, stack: u64, ctid: u64, worker: extern "C" fn()) -> i64 {
    unsafe {
        let args = &mut *CLONE_ARGS.0.get();
        *args = CloneArgs::zero();
        args.flags = flags;
        args.child_tid = ctid;
        args.stack = stack;
        aerosls_clone3_spawn(args as *const CloneArgs, 88, worker)
    }
}

extern "C" fn worker_a() {
    TID1.store(gettid(), Ordering::SeqCst);
    say(b"M4-WORKER-A-TID=", TID1.load(Ordering::SeqCst));

    // Retry until the parent is actually enqueued on A. The parent may not
    // have reached its wait yet (or a requeue may land before it parks);
    // a requeue before the parent parks moves nobody and wakes nobody, so
    // the next pass sees it.
    // the parent parks moves nobody and wakes nobody; the next pass sees it.
    let mut attempts = 0u64;
    loop {
        // futex(2)'s fourth register is nr_requeue for REQUEUE (the same
        // slot WAIT uses for its timeout pointer). Passing NULL here was
        // measured as nr_requeue=0: the sidecar reported moved=0 forever.
        let n = futex(&A_WORD, FUTEX_REQUEUE_PRIVATE, 0,
                      1usize as *const Timespec,
                      &B_WORD as *const _ as u64, 0);
        let w = futex(&B_WORD, FUTEX_WAKE_PRIVATE, 1,
                      core::ptr::null(), 0, 0);
        if w > 0 {
            say(b"M4-REQUEUE-WORKER-WOKE=", w as u64);
            break;
        }
        attempts += 1;
        if attempts > 1_000_000 {
            say(b"M4-REQUEUE-WORKER-STUCK=", n as u64);
            fail(b"requeue");
        }
        unsafe { syscall(SYS_YIELD); }
    }
    say_msg(b"M4-REQUEUE-OK");

    // Tell the parent this sibling is about to park, then wait for the
    // group-directed SIGUSR1 handler. The handler interrupts THIS futex;
    // the sidecar must answer the parked exchange with -EINTR and the
    // kernel must build the frame from this task's saved context.
    WORKER_PARKED.store(1, Ordering::SeqCst);
    let handler_rc = futex(&DONE_WORD, FUTEX_WAIT_PRIVATE, 0,
                           core::ptr::null(), 0, 0);
    say(b"M4-GROUP-HANDLER-RC=", handler_rc as u64);
    // The load-bearing evidence is that the handler ran on THIS task and
    // the parked syscall resumed. The saved rax is logged separately; the
    // M4.10 contract pins -EINTR in linux.rs's focused host test.
    WORKER_RESUMED.store(1, Ordering::SeqCst);
    say_msg(b"M4-GROUP-HANDLER-RESUMED");

    // M4.10 stop arm: park, let the parent send SIGSTOP to the TGID, then
    // SIGCONT. service() must withhold this reply while stopped and answer
    // it when the group is continued.
    WORKER_STOP_PARKED.store(1, Ordering::SeqCst);
    let stop_rc = futex(&DONE_WORD, FUTEX_WAIT_PRIVATE, 0,
                        core::ptr::null(), 0, 0);
    say(b"M4-GROUP-STOP-RC=", stop_rc as u64);
    WORKER_STOP_RESUMED.store(1, Ordering::SeqCst);
    say_msg(b"M4-GROUP-STOP-RESUMED");

    // Park again for the parent's tgkill, keeping the M4.8 arm intact.
    let _ = futex(&DONE_WORD, FUTEX_WAIT_PRIVATE, 0,
                  core::ptr::null(), 0, 0);
    say_msg(b"M4-WORKER-A-UNEXPECTED");
    exit_now();
}

extern "C" fn worker_c() {
    TID3.store(gettid(), Ordering::SeqCst);
    say(b"M4-RUNNING-TID=", TID3.load(Ordering::SeqCst));
    // No syscall in this loop: the sidecar's target must be timer-suspended,
    // not blocked on a forwarded request, when the parent's tkill arrives.
    let mut n = 0u64;
    while RUNNING_GO_EXIT.load(Ordering::SeqCst) == 0 {
        n = n.wrapping_add(1);
        if n & 0x3ff == 0 {
            RUNNING_TICKS.store(n, Ordering::SeqCst);
        }
    }
    say_msg(b"M4-RUNNING-EXIT");
    exit_now();
}

extern "C" fn worker_b() {
    unsafe {
        let entry = &mut *ROBUST_ENTRY.0.get();
        let head = &mut *ROBUST_HEAD.0.get();
        entry.next = head as *mut RobustHead as *mut RobustEntry;
        head.list.next = entry as *mut RobustEntry;
        head.futex_offset = (&ROBUST_WORD as *const _ as i64)
            - (entry as *const RobustEntry as i64);
        head.list_op_pending = 0;
        let rc = syscall(SYS_SET_ROBUST_LIST,
                         head as *const RobustHead as c_long,
                         core::mem::size_of::<RobustHead>() as c_long);
        say(b"M4-ROBUST-SET=", rc as u64);
    }
    ROBUST_READY.store(1, Ordering::SeqCst);
    while GO_EXIT.load(Ordering::SeqCst) == 0 {
        let _ = futex(&GO_EXIT, FUTEX_WAIT_PRIVATE, 0,
                      core::ptr::null(), 0, 0);
    }
    say_msg(b"M4-ROBUST-WORKER-EXIT");
    exit_now();
}

fn main() {
    say_msg(b"M4-HARDENING-START");
    let tgid = getpid();
    say(b"M4-HARDENING-TGID=", tgid);

    let tid1 = spawn3(CLONE_FLAGS, stack_top(&STACK1),
                      &CTID1 as *const _ as u64, worker_a);
    if tid1 <= 0 {
        fail(b"clone3-a");
    }
    say(b"M4-CLONE3-OK=", tid1 as u64);
    say(b"M4-CLONE3-CTID=", CTID1.load(Ordering::SeqCst));

    // Leave the worker ample scheduling room: QEMU's 4-way TCG run also
    // services the gate's other tenants, and a 10 s wall wait was measured
    // expiring before a spinning worker got its second slice.
    let timeout = Timespec { tv_sec: 60, tv_nsec: 0 };
    let rc = futex(&A_WORD, FUTEX_WAIT_PRIVATE, 0, &timeout, 0, 0);
    if rc != 0 && rc != EAGAIN {
        say(b"M4-REQUEUE-PARENT-WAIT=", rc as u64);
        fail(b"requeue-wait");
    }
    say_msg(b"M4-REQUEUE-PARENT-OK");

    // M4.10: install a group-wide SIGUSR1 handler, wait until worker A is
    // parked, then send a process-directed signal to the TGID. The target
    // must receive the frame between its futex's saved registers and see
    // -EINTR when the handler returns; the caller stays alive.
    let act = KernelSigaction {
        handler: group_handler as *const () as u64,
        flags: SA_RESTORER,
        restorer: aerosls_sigreturn_restorer as *const () as u64,
        mask: 0,
    };
    let sa_rc = unsafe {
        syscall(SYS_RT_SIGACTION, SIGUSR1 as c_long,
                &act as *const KernelSigaction as c_long,
                0 as c_long, 8 as c_long)
    };
    say(b"M4-GROUP-SIGACTION=", sa_rc as u64);
    if sa_rc != 0 { fail(b"sigaction"); }
    while WORKER_PARKED.load(Ordering::SeqCst) == 0 {
        unsafe { syscall(SYS_YIELD); }
    }
    // The flag is set immediately before the futex; give the worker a few
    // slices to enter the parked syscall before the single group attempt.
    for _ in 0..32 { unsafe { syscall(SYS_YIELD); } }
    let group_rc = unsafe {
        syscall(SYS_KILL, tgid as c_long, SIGUSR1 as c_long)
    };
    say(b"M4-GROUP-HANDLER-SENT=", group_rc as u64);
    if group_rc != 0 { fail(b"group-handler-send"); }
    let mut handler_seen = false;
    for _ in 0..2_000_000 {
        if GROUP_HANDLER_HIT.load(Ordering::SeqCst) != 0 &&
            WORKER_RESUMED.load(Ordering::SeqCst) != 0 {
            handler_seen = true;
            break;
        }
        unsafe { syscall(SYS_YIELD); }
    }
    if !handler_seen {
        say(b"M4-GROUP-HANDLER-HIT=", GROUP_HANDLER_HIT.load(Ordering::SeqCst));
        say(b"M4-GROUP-HANDLER-RESUMED=",
            WORKER_RESUMED.load(Ordering::SeqCst));
        fail(b"group-handler");
    }
    say_msg(b"M4-GROUP-HANDLER-PASS");

    // M4.11 running-sibling arm: a sibling between syscalls has no parked
    // reply to carry the trailer. tkill it with a second handler and let the
    // kernel install the frame into its saved ring3 context.
    let act2 = KernelSigaction {
        handler: running_handler as *const () as u64,
        flags: SA_RESTORER,
        restorer: aerosls_sigreturn_restorer as *const () as u64,
        mask: 0,
    };
    let sa2_rc = unsafe {
        syscall(SYS_RT_SIGACTION, SIGUSR2 as c_long,
                &act2 as *const KernelSigaction as c_long,
                0 as c_long, 8 as c_long)
    };
    say(b"M4-RUNNING-SIGACTION=", sa2_rc as u64);
    if sa2_rc != 0 { fail(b"running-sigaction"); }

    let tid3 = spawn3(CLONE_FLAGS & !CLONE_CHILD_SETTID & !CLONE_CHILD_CLEARTID,
                       stack_top(&STACK3), 0, worker_c);
    if tid3 <= 0 { fail(b"clone3-c"); }
    say(b"M4-CLONE3-C-OK=", tid3 as u64);
    for _ in 0..64 { unsafe { syscall(SYS_YIELD); } }
    let running_rc = unsafe {
        syscall(SYS_TGKILL, tgid as c_long, tid3 as c_long, SIGUSR2 as c_long)
    };
    say(b"M4-RUNNING-HANDLER-SENT=", running_rc as u64);
    if running_rc != 0 { fail(b"running-handler-send"); }
    let mut running_seen = false;
    for _ in 0..2_000_000 {
        if RUNNING_HANDLER_HIT.load(Ordering::SeqCst) != 0 {
            running_seen = true;
            break;
        }
        unsafe { syscall(SYS_YIELD); }
    }
    if !running_seen {
        say(b"M4-RUNNING-HANDLER-HIT=",
            RUNNING_HANDLER_HIT.load(Ordering::SeqCst));
        fail(b"running-handler");
    }
    say_msg(b"M4-RUNNING-HANDLER-PASS");

    // M4.10 stop arm. Retry until the worker is actually parked (the first
    // attempt can beat the futex), then continue the group; the sidecar's
    // stop latch holds the reply in between.
    while WORKER_STOP_PARKED.load(Ordering::SeqCst) == 0 {
        unsafe { syscall(SYS_YIELD); }
    }
    for _ in 0..32 { unsafe { syscall(SYS_YIELD); } }
    let mut stop_rc = 0;
    for _ in 0..1_000 {
        stop_rc = unsafe {
            syscall(SYS_KILL, tgid as c_long, SIGSTOP as c_long)
        };
        if stop_rc == 0 { break; }
        unsafe { syscall(SYS_YIELD); }
    }
    say(b"M4-GROUP-STOP-SENT=", stop_rc as u64);
    if stop_rc != 0 { fail(b"group-stop-send"); }
    let cont_rc = unsafe {
        syscall(SYS_KILL, tgid as c_long, SIGCONT as c_long)
    };
    say(b"M4-GROUP-CONT-SENT=", cont_rc as u64);
    if cont_rc != 0 { fail(b"group-cont-send"); }
    let mut stop_seen = false;
    for _ in 0..2_000_000 {
        if WORKER_STOP_RESUMED.load(Ordering::SeqCst) != 0 {
            stop_seen = true;
            break;
        }
        unsafe { syscall(SYS_YIELD); }
    }
    if !stop_seen { fail(b"group-stop-resume"); }

    // The group stop also held the running sibling. Its progress counter must
    // move after SIGCONT and before we ask it to exit; the sidecar's broadcast
    // log is the count proof, this is the live resume proof.
    let run_before = RUNNING_TICKS.load(Ordering::SeqCst);
    let mut running_resumed = false;
    for _ in 0..2_000_000 {
        if RUNNING_TICKS.load(Ordering::SeqCst) > run_before {
            running_resumed = true;
            break;
        }
        unsafe { syscall(SYS_YIELD); }
    }
    if !running_resumed { fail(b"group-stop-broadcast"); }
    say_msg(b"M4-GROUP-STOP-BROADCAST-PASS");
    say_msg(b"M4-GROUP-STOP-PASS");
    RUNNING_GO_EXIT.store(1, Ordering::SeqCst);

    // The second clone has no clear-tid; its robust list is the exit proof.
    let tid2 = spawn3(CLONE_FLAGS & !CLONE_CHILD_SETTID & !CLONE_CHILD_CLEARTID,
                       stack_top(&STACK2), 0, worker_b);
    if tid2 <= 0 {
        fail(b"clone3-b");
    }
    say(b"M4-CLONE3-B-OK=", tid2 as u64);
    while ROBUST_READY.load(Ordering::SeqCst) == 0 {
        unsafe { syscall(SYS_YIELD); }
    }
    GO_EXIT.store(1, Ordering::SeqCst);
    let _ = futex(&GO_EXIT, FUTEX_WAKE_PRIVATE, 1, core::ptr::null(), 0, 0);

    let short = Timespec { tv_sec: 0, tv_nsec: 100_000_000 };
    let mut robust_ok = false;
    for _ in 0..200 {
        if ROBUST_WORD.load(Ordering::SeqCst) & FUTEX_OWNER_DIED != 0 {
            robust_ok = true;
            break;
        }
        let r = futex(&ROBUST_WORD, FUTEX_WAIT_PRIVATE, 0,
                      &short, 0, 0);
        if r == ETIMEDOUT { continue; }
    }
    if !robust_ok {
        say(b"M4-ROBUST-WORD=", ROBUST_WORD.load(Ordering::SeqCst));
        fail(b"robust");
    }
    write_bytes(b"M4-ROBUST-BIT=1073741824\n");
    say(b"M4-ROBUST-OK=", ROBUST_WORD.load(Ordering::SeqCst));

    let kill_rc = unsafe {
        syscall(SYS_TGKILL, tgid as c_long, tid1 as c_long, SIGTERM as c_long)
    };
    say(b"M4-TGKILL-SENT=", kill_rc as u64);
    let mut ctid_ok = false;
    for _ in 0..200 {
        if CTID1.load(Ordering::SeqCst) == 0 {
            ctid_ok = true;
            break;
        }
        let r = futex(&CTID1, FUTEX_WAIT_PRIVATE, tid1 as u64,
                      &short, 0, 0);
        if r == ETIMEDOUT { continue; }
    }
    // The target's record must also be gone: its TID no longer resolves.
    let probe = unsafe {
        syscall(SYS_TGKILL, tgid as c_long, tid1 as c_long, 0 as c_long)
    };
    if !ctid_ok || probe >= 0 {
        say(b"M4-TGKILL-CTID=", CTID1.load(Ordering::SeqCst));
        say(b"M4-TGKILL-PROBE=", probe as u64);
        fail(b"tgkill");
    }
    say_msg(b"M4-TGKILL-OK");

    say_msg(b"M4-HARDENING-PASS");
    say_msg(b"M4-GROUP-KILL-SENT");
    unsafe { syscall(SYS_KILL, tgid as c_long, SIGTERM as c_long); }
    say_msg(b"M4-HARDENING-AFTER-GROUP-KILL");
    loop { unsafe { syscall(SYS_YIELD); } }
}
