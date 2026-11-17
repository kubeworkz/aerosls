//! M4.9's real `std::thread` gate.
//!
//! `std::thread::spawn` reaches musl's `pthread_create`, which reserves the
//! stack with `mmap(PROT_NONE)` and then `mprotect`s the usable range. The
//! parent and worker park on the group's private futex with a timed parent
//! wait, so the M4.6 checkpoint can capture both contexts. After restore the
//! parent times out, wakes the worker, and `JoinHandle::join` completes.
//!
//! This is deliberately not the raw-clone M4.7 shape: the worker stack, TLS
//! and join are the ones the runtime itself allocates.

use std::io::Write;
use std::os::raw::c_long;
use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::Arc;
const SYS_FUTEX: c_long = 202;
const SYS_GETTID: c_long = 186;
const SYS_WRITE: c_long = 1;

const FUTEX_WAIT_PRIVATE: c_long = 128;
const FUTEX_WAKE_PRIVATE: c_long = 129;
const FUTEX_TIMEOUT_MS: u64 = 30_000;

static FUTEX_WORD: AtomicU64 = AtomicU64::new(0);
static WORKER_READY: AtomicU64 = AtomicU64::new(0);
static WORKER_DONE: AtomicU64 = AtomicU64::new(0);

unsafe extern "C" {
    fn syscall(num: c_long, ...) -> c_long;
}

fn futex(addr: *const AtomicU64, op: c_long, val: u64, timeout: *const Timespec) -> c_long {
    unsafe {
        syscall(
            SYS_FUTEX,
            addr as c_long,
            op,
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

fn say(msg: &str) {
    let mut out = std::io::stdout().lock();
    let _ = out.write_all(msg.as_bytes());
    let _ = out.write_all(b"\n");
    let _ = out.flush();
}

fn say_value(prefix: &[u8], value: u64) {
    write_bytes(prefix);
    write_u64(value);
    write_bytes(b"\n");
}

#[repr(C)]
struct Timespec {
    tv_sec: i64,
    tv_nsec: i64,
}

fn main() {
    say("STD-THREAD-START");

    let shared = Arc::new(AtomicU64::new(0));
    let worker_shared = Arc::clone(&shared);
    let handle = std::thread::spawn(move || {
        let tid = gettid();
        say_value(b"STD-WORKER-TID=", tid);
        write_bytes(b"STD-WORKER-FD-OK\n");
        worker_shared.fetch_add(7, Ordering::SeqCst);
        WORKER_READY.store(1, Ordering::SeqCst);
        say("STD-WORKER-READY");

        say("STD-WORKER-WAIT");
        let rc = futex(&FUTEX_WORD, FUTEX_WAIT_PRIVATE, 0, std::ptr::null());
        if rc == 0 {
            say("STD-WORKER-WOKE");
        }
        WORKER_DONE.store(1, Ordering::SeqCst);
        let _ = futex(&WORKER_DONE, FUTEX_WAKE_PRIVATE, 1, std::ptr::null());
    });

    // pthread_create has returned; wait for the worker to be about to park so
    // boot 1 actually captures two parked contexts.
    while WORKER_READY.load(Ordering::SeqCst) == 0 {
        std::hint::spin_loop();
    }
    say("STD-PARENT-WAIT");
    let timeout = Timespec {
        tv_sec: (FUTEX_TIMEOUT_MS / 1000) as i64,
        tv_nsec: ((FUTEX_TIMEOUT_MS % 1000) * 1_000_000) as i64,
    };
    let rc = futex(
        &FUTEX_WORD,
        FUTEX_WAIT_PRIVATE,
        0,
        &timeout as *const Timespec,
    );
    say(&format!("STD-PARENT-WAKE={rc}"));
    FUTEX_WORD.store(1, Ordering::SeqCst);
    let n = futex(&FUTEX_WORD, FUTEX_WAKE_PRIVATE, 1, std::ptr::null());
    say(&format!("STD-FUTEX-WAKE={n}"));

    handle.join().expect("std::thread join");

    let observed = shared.load(Ordering::SeqCst);
    say(&format!("STD-SHARED={observed} STD-WORKER-DONE={}",
                 WORKER_DONE.load(Ordering::SeqCst)));
    let passed = observed == 7 && WORKER_DONE.load(Ordering::SeqCst) == 1;
    if passed {
        say("STD-JOIN-OK");
        say("STD-THREAD-PASS");
    } else {
        say("STD-THREAD-FAIL");
    }
}
