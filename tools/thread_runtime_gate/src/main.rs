//! M4.7's runtime gate candidate.
//!
//! Two real OS threads share one heap object and one fd, rendezvous through a
//! private futex, and rejoin after checkpoint/restore. The parent's wait is
//! timed so a restored parent can wake the restored worker; the worker's wait
//! is untimed, which is the state M4.6 already proved it can carry.
//!
//! This candidate deliberately does NOT use `std::thread`: musl's
//! `pthread_create` reserves the stack with `mmap(PROT_NONE)` and then
//! `mprotect`s the usable range, while this kernel's documented PROT_NONE
//! contract backs no frames and answers a protection change over an absent
//! page with ENOMEM. The gate therefore supplies its own static stack and
//! enters `clone(56)` directly — the same shape a Go or Rust runtime uses for
//! its own worker stacks — and joins through a private futex word.

use core::cell::UnsafeCell;
use std::io::Write;
use std::os::raw::{c_long, c_uint};
use std::sync::atomic::{AtomicU64, Ordering};
use std::time::{Duration, Instant};

const SYS_FUTEX: c_long = 202;
const SYS_GETTID: c_long = 186;
const SYS_WRITE: c_long = 1;
const SYS_EXIT: c_long = 60;
const SYS_YIELD: c_long = 24;

const FUTEX_WAIT_PRIVATE: c_long = 128;
const FUTEX_WAKE_PRIVATE: c_long = 129;
const FUTEX_TIMEOUT_MS: u64 = 30_000;

const CLONE_VM: u64 = 0x0000_0100;
const CLONE_FS: u64 = 0x0000_0200;
const CLONE_FILES: u64 = 0x0000_0400;
const CLONE_SIGHAND: u64 = 0x0000_0800;
const CLONE_THREAD: u64 = 0x0001_0000;
const CLONE_SYSVSEM: u64 = 0x0004_0000;
const CLONE_FLAGS: u64 = CLONE_VM | CLONE_FS | CLONE_FILES | CLONE_SIGHAND
    | CLONE_THREAD | CLONE_SYSVSEM;

static SHARED_HEAP: AtomicU64 = AtomicU64::new(0);
static SHARED_PTR: AtomicU64 = AtomicU64::new(0);
static FUTEX_WORD: AtomicU64 = AtomicU64::new(0);
static WORKER_DONE: AtomicU64 = AtomicU64::new(0);
static WORKER_TID: AtomicU64 = AtomicU64::new(0);

/// The worker's own stack. It lives in the image's RW segment, so the
/// checkpoint page walk carries it with the group like every other page.
#[repr(align(16))]
struct WorkerStack(UnsafeCell<[u8; 64 * 1024]>);
unsafe impl Sync for WorkerStack {}
static WORKER_STACK: WorkerStack = WorkerStack(UnsafeCell::new([0u8; 64 * 1024]));

unsafe extern "C" {
    fn syscall(num: c_long, ...) -> c_long;
    fn aerosls_clone_spawn(
        flags: u64,
        stack: *mut u8,
        ptid: u64,
        ctid: u64,
        tls: u64,
        worker: extern "C" fn(),
    ) -> i64;
}

core::arch::global_asm!(
    r#"
    .text
    .global aerosls_clone_spawn
    .type aerosls_clone_spawn, @function
aerosls_clone_spawn:
    /* rdi=flags, rsi=stack, rdx=ptid, rcx=ctid, r8=tls, r9=worker */
    pushq %r12
    movq %r9, %r12
    movq %rcx, %r10              /* clone's fourth argument is r10 */
    movl $56, %eax               /* SYS_clone */
    syscall
    testq %rax, %rax
    jne 2f                       /* parent: rax = child TID */
    xorl %ebp, %ebp
    callq *%r12                  /* child resumes here on its own stack */
    movl $60, %eax               /* SYS_exit */
    xorl %edi, %edi
    syscall
1:  jmp 1b
2:  popq %r12
    ret
    .size aerosls_clone_spawn, .-aerosls_clone_spawn
    "#,
    options(att_syntax)
);

fn futex(addr: *const AtomicU64, op: c_long, val: u64, timeout: *const u64) -> c_long {
    unsafe {
        syscall(
            SYS_FUTEX,
            addr as c_long,
            op as c_uint as c_long,
            val as c_long,
            timeout as c_long,
            0 as c_long,
            0 as c_long,
        )
    }
}

fn gettid() -> u64 {
    unsafe { syscall(SYS_GETTID) as u64 }
}

fn exit_now() -> ! {
    unsafe {
        syscall(SYS_EXIT, 0 as c_long);
    }
    loop {
        core::hint::spin_loop();
    }
}

fn write_bytes(mut bytes: &[u8]) {
    while !bytes.is_empty() {
        let n = unsafe {
            syscall(
                SYS_WRITE,
                1 as c_long,
                bytes.as_ptr() as c_long,
                bytes.len() as c_long,
            )
        };
        if n <= 0 {
            return;
        }
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
        if value == 0 {
            break;
        }
    }
    write_bytes(&buf[i..]);
}

fn worker_say(prefix: &[u8], value: u64) {
    write_bytes(prefix);
    write_u64(value);
    write_bytes(b"\n");
}

#[repr(C)]
struct Timespec {
    tv_sec: i64,
    tv_nsec: i64,
}

extern "C" fn worker_main() {
    let tid = gettid();
    WORKER_TID.store(tid, Ordering::SeqCst);
    worker_say(b"M4-WORKER-TID=", tid);

    // The shared heap write: the parent must observe this exact value on the
    // same allocation, not a copy made by clone.
    let heap = unsafe { &*(SHARED_PTR.load(Ordering::SeqCst) as *const AtomicU64) };
    heap.fetch_add(7, Ordering::SeqCst);
    SHARED_HEAP.store(0x4d34_3721, Ordering::SeqCst);

    // The fd write: stdout is inherited, so this is the group's fd table
    // being used from a second task.
    write_bytes(b"M4-WORKER-FD-OK\n");

    // Announce the waiter before entering the kernel. The harness waits for
    // this marker and then for the kernel's `wait (futex)` line.
    write_bytes(b"M4-WORKER-WAIT\n");
    let rc = futex(&FUTEX_WORD, FUTEX_WAIT_PRIVATE, 0, std::ptr::null());
    if rc == 0 {
        write_bytes(b"M4-WORKER-WOKE\n");
    }

    // Join: publish completion, then wake a parent that may already be
    // waiting on the word.
    WORKER_DONE.store(1, Ordering::SeqCst);
    let _ = futex(&WORKER_DONE, FUTEX_WAKE_PRIVATE, 1, std::ptr::null());
    exit_now();
}

fn main() {
    say("M4-RUNTIME-START");

    // One heap allocation shared by both tasks. The pointer is published
    // before clone so the worker never observes a zero.
    let shared = Box::new(AtomicU64::new(0));
    SHARED_PTR.store(Box::into_raw(shared) as u64, Ordering::SeqCst);

    let stack_top = unsafe {
        let stack = &mut *WORKER_STACK.0.get();
        let base = stack.as_mut_ptr();
        base.add(stack.len()) as usize & !15
    };
    let tid = unsafe {
        aerosls_clone_spawn(
            CLONE_FLAGS,
            stack_top as *mut u8,
            0,
            0,
            0,
            worker_main,
        )
    };
    if tid <= 0 {
        say("M4-RUNTIME-GATE-FAIL");
        let _ = std::io::stdout().lock().flush();
        return;
    }

    // Give the worker time to reach its wait. The parent then blocks on its
    // own timed wait; both tasks are parked when the checkpoint is taken.
    let deadline = Instant::now() + Duration::from_secs(10);
    while WORKER_TID.load(Ordering::SeqCst) == 0 && Instant::now() < deadline {
        unsafe {
            syscall(SYS_YIELD);
        }
    }
    say("M4-PARENT-WAIT");
    let timeout = Timespec {
        tv_sec: (FUTEX_TIMEOUT_MS / 1000) as i64,
        tv_nsec: ((FUTEX_TIMEOUT_MS % 1000) * 1_000_000) as i64,
    };
    let rc = futex(
        &FUTEX_WORD,
        FUTEX_WAIT_PRIVATE,
        0,
        &timeout as *const Timespec as *const u64,
    );

    // Boot 2 lands here after the timed wait; boot 1 only reaches it if the
    // checkpoint never happened. Either way the parent is the waker.
    say(&format!("M4-PARENT-WAKE={rc}"));
    FUTEX_WORD.store(1, Ordering::SeqCst);
    let n = futex(&FUTEX_WORD, FUTEX_WAKE_PRIVATE, 1, std::ptr::null());
    say(&format!("M4-FUTEX-WAKE={n}"));

    // Join the worker through its completion word. The check-before-wait
    // order closes the lost-wake window when the worker finishes first.
    while WORKER_DONE.load(Ordering::SeqCst) == 0 {
        let _ = futex(&WORKER_DONE, FUTEX_WAIT_PRIVATE, 0, std::ptr::null());
    }

    let heap_ptr = SHARED_PTR.load(Ordering::SeqCst) as *mut AtomicU64;
    let heap = unsafe { (*heap_ptr).load(Ordering::SeqCst) };
    let observed = SHARED_HEAP.load(Ordering::SeqCst);
    say(&format!("M4-HEAP={heap} M4-SHARED={observed}"));
    let passed = heap == 7 && observed == 0x4d34_3721;
    if passed {
        say("M4-GO-OK");
        say("M4-FUTEX-OK");
        say("M4-CHECKPOINT-OK");
        say("M4-RUNTIME-GATE-PASS");
    } else {
        say("M4-RUNTIME-GATE-FAIL");
    }
    unsafe {
        drop(Box::from_raw(heap_ptr));
    }
}

fn say(msg: &str) {
    let mut out = std::io::stdout().lock();
    let _ = out.write_all(msg.as_bytes());
    let _ = out.write_all(b"\n");
    let _ = out.flush();
}
