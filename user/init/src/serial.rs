//! `SYS_SLS_SERIAL_WRITE` (165): one line straight to the kernel serial log.
//!
//! This module is NOT behind the `target` feature (unlike `entry`, which only
//! exists on the real machine), so every build — the sidecar binary and every
//! host build that links the crate — has exactly one copy of the write. On the
//! target it issues the syscall; everywhere else it is a no-op, so a module
//! that wants to log needs neither `entry` nor a second copy of the asm.
//!
//! The console channel coalesces and can drop a line; the kernel's serial TX
//! lock (with loopback deferral) is the drop-proof one, and it is the same path
//! the tenants' own `[POSIX] aero state=` lines use. That is why the evidence
//! lines guards parse ride this path rather than the console.
//!
//! The clobber list is load-bearing: the kernel's syscall path uses every
//! caller-saved register and does not restore them, so each one is declared
//! clobbered here. Two copies of this block drifting apart is exactly the bug
//! this module exists to make impossible.

/// Send one line to the kernel serial log. The kernel handler is a strlen, so
/// the buffer keeps an explicit NUL after the text; a line longer than 255
/// bytes is truncated rather than split across two writes.
#[cfg(all(feature = "target", target_arch = "x86_64", target_os = "none"))]
pub(crate) fn serial_log(msg: &str) {
    let mut buf = [0u8; 256];
    let n = msg.len().min(255);
    buf[..n].copy_from_slice(&msg.as_bytes()[..n]);
    unsafe {
        core::arch::asm!(
            "syscall",
            inlateout("rax") 165u64 => _,
            inlateout("rdi") buf.as_ptr() => _,
            // The kernel's syscall path uses every caller-saved register and
            // does not restore them; declare them all clobbered so the
            // compiler cannot keep a stale value across this call.
            lateout("rcx") _, lateout("r11") _, lateout("rsi") _,
            lateout("rdx") _, lateout("r8") _, lateout("r9") _,
            lateout("r10") _,
            options(nostack),
        );
    }
}

/// Host builds have no kernel to write to: the test harness and the sim read
/// the manager's own return values, not a serial log.
#[cfg(not(all(feature = "target", target_arch = "x86_64", target_os = "none")))]
pub(crate) fn serial_log(_msg: &str) {}
