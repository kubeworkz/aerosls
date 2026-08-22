//! The raw syscall layer — one instruction, one convention.
//!
//! The kernel's entry stub (`kernel/process.c`) dispatches on `rax` and
//! passes `rdi` as the single opaque argument (a pointer to a request
//! struct). This is exactly user/libsls/sls.h's `_sls_syscall`:
//!
//! ```c
//! static inline uint64_t _sls_syscall(uint64_t num, void *arg) {
//!     uint64_t ret;
//!     __asm__ volatile ("mov %1, %%rdi\n\tsyscall" : "=a"(ret) : "r"(arg), "0"(num)
//!                       : "rcx","r11","rdi","rsi","rdx","r8","r9","r10","memory");
//!     return ret;
//! }
//! ```
//!
//! The C version loads `rdi` itself because GCC may keep a live value in
//! RDI across the syscall and the kernel's `.unknown_syscall` path destroys
//! it. Rust's `in("rdi")` operand is safe by construction: the compiler
//! treats input registers as clobbered after the `asm!`, so nothing live is
//! left there.

/// Issue syscall `num` with the single request-struct pointer `arg`.
///
/// Returns `rax` (0 / a positive result on success, a negative `CAP_E*` as
/// a u64 on failure — the do_syscall ABI widens negatives through u64).
#[cfg(feature = "target")]
#[inline]
pub unsafe fn sls_syscall(num: u64, arg: u64) -> u64 {
    let ret: u64;
    core::arch::asm!(
        "syscall",
        inlateout("rax") num => ret,
        in("rdi") arg,
        lateout("rcx") _,
        lateout("r11") _,
        lateout("rsi") _,
        lateout("rdx") _,
        lateout("r8") _,
        lateout("r9") _,
        lateout("r10") _,
        options(nostack),
    );
    ret
}

// ── Host build (tests): a swappable fake, never a real syscall ─────────────
//
// Sidecar crates follow the repo convention of defaulting to the host build
// so `cargo test` links a fake kernel instead of the real ABI. Here the
// fake is a single function the test harness installs; `set_fake_syscall`
// is how tests point the runtime at their in-process kernel sim.

// The fake plumbing needs `std` (a Mutex) and exists only for tests: the
// crate's own unit tests and external integration tests behind the
// `host-fake` feature. A real sidecar build (`target`) is `no_std` and never
// compiles this.
#[cfg(all(not(feature = "target"), any(test, feature = "host-fake")))]
type SyscallFn = fn(u64, u64) -> u64;

#[cfg(all(not(feature = "target"), any(test, feature = "host-fake")))]
static FAKE_SYSCALL: std::sync::Mutex<Option<SyscallFn>> = std::sync::Mutex::new(None);

/// Install (or clear, with `None`) the host-build fake syscall handler.
/// Tests call this before driving the runtime; the runtime calls
/// `sls_syscall` which routes here. Returns the previously installed
/// handler so tests can restore it.
#[cfg(all(not(feature = "target"), any(test, feature = "host-fake")))]
pub fn set_fake_syscall(f: Option<SyscallFn>) -> Option<SyscallFn> {
    let mut g = FAKE_SYSCALL.lock().unwrap();
    let old = *g;
    *g = f;
    old
}

/// Host-build `sls_syscall`: route through the installed fake. With no fake
/// installed this is a programming error (the mock runtime's panic-on-call
/// behavior).
#[cfg(not(feature = "target"))]
pub unsafe fn sls_syscall(num: u64, _arg: u64) -> u64 {
    // Route through the fake when one is installed: the crate's own tests
    // (`test`) or external integration tests (the `host-fake` feature).
    #[cfg(any(test, feature = "host-fake"))]
    {
        let f = *FAKE_SYSCALL.lock().unwrap();
        if let Some(f) = f {
            return f(num, _arg);
        }
    }
    panic!("aerosls: syscall {num} invoked in host build with no fake installed")
}
