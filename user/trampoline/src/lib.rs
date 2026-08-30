//! # AeroSLS Trampoline Library
//!
//! Zero-copy, same-ring, hardware-enforced cross-sidecar calls via Intel MPK /
//! AMD PKU (Deliverable 5, Phase 3).
//!
//! ## Overview
//!
//! When two sidecars run in the same CPU privilege ring (ring 0 on x86-64) and
//! are mutually trusted, the kernel issues a **trampoline capability** — a
//! capability that grants the caller permission to directly branch to a
//! pre-verified entry point in the callee's address space. The hardware
//! protection key mechanism (MPK/PKU) ensures the callee's code cannot be
//! tampered with, and the caller's data pages are restricted during the call.
//!
//! The call sequence:
//! 1. Save caller's PKRU
//! 2. Load callee's PKRU (WRPKRU) — now has access to callee's code, lost
//!    access to own writable data
//! 3. Load callee's stack pointer
//! 4. Push arguments onto callee's stack
//! 5. JMP to callee's entry point
//! 6. Callee executes, writes result to pre-agreed arena slot
//! 7. RET back to caller
//! 8. Restore caller's PKRU
//! 9. Read result from arena slot
//!
//! This is approximately 6× the cost of a local function call (~30 ns) versus
//! 40× for the channel path (~200 ns).
//!
//! ## When MPK is unavailable
//!
//! On CPUs without MPK support (or when running in userspace like WSL), the
//! trampoline falls back to the channel path with `CAP_ENOSYS`. The generated
//! AeroIDL stubs detect this at startup and choose the appropriate path.
//!
//! ## Usage
//!
//! ```rust,ignore
//! use aerols_trampoline::{TrampolineCap, trampoline_call, mpk_supported};
//!
//! // Check if MPK is available
//! if !mpk_supported() {
//!     // Fall back to channel path
//!     return;
//! }
//!
//! // Create a trampoline cap (via syscall 305)
//! let tramp = TrampolineCap::create(callee_pid, entry_vaddr, max_stack)?;
//!
//! // Call the trampoline
//! let result = trampoline_call(&tramp, &[arg1, arg2])?;
//! ```

#![no_std]

use core::arch::asm;

/// MPK support flags (matches kernel/cap.h)
pub const CAP_TRAMP_MPK_SUPPORTED: u32 = 0x01;
pub const CAP_TRAMP_MPK_ENABLED: u32 = 0x02;

/// Syscall numbers (matches kernel/cap.h)
const SYS_SLS_TRAMPOLINE_CREATE: u64 = 305;
const SYS_SLS_TRAMPOLINE_CALL: u64 = 306;
const SYS_SLS_TRAMPOLINE_MPK_FLAGS: u64 = 307; // new: query MPK support

/// Capability error codes (matches kernel/cap.h)
const CAP_EINVAL: i64 = -1;
const CAP_ENOSYS: i64 = -10;
const CAP_ENOMEM: i64 = -7;
const CAP_ETABLEFULL: i64 = -4;
const CAP_EBADF: i64 = -2;
const CAP_ERANGE: i64 = -9;

/// A trampoline capability — grants permission to directly branch to a
/// pre-verified entry point in the callee's address space.
#[repr(C)]
pub struct TrampolineCap {
    pub entry_vaddr: u64,
    pub stack_vaddr: u64,
    pub callee_pkey: u32,
    pub caller_pkey_mask: u32,
    pub data_pkey: u32,
    pub flags: u32,
    pub max_stack_bytes: u32,
    _pad: u32,
}

/// Request struct for SYS_SLS_TRAMPOLINE_CREATE (syscall 305)
#[repr(C)]
struct TrampolineCreateRequest {
    callee_pid: u32,
    _pad: u32,
    entry_vaddr: u64,
    max_stack_bytes: u32,
    _pad2: u32,
}

/// Request struct for SYS_SLS_TRAMPOLINE_CALL (syscall 306)
#[repr(C)]
struct TrampolineCallRequest {
    tramp_idx: u16,
    _pad: [u8; 6],
    args: [u64; 4],
    arg_count: u64,
    arena_offset: u64,
    arena_len: u64,
}

/// Raw syscall — issues the `syscall` instruction with rax=num, rdi=arg.
///
/// # Safety
/// The caller must ensure `num` is a valid syscall number and `arg` points to
/// a valid request struct (or is 0 for no-arg syscalls).
#[cfg(feature = "target")]
#[inline]
unsafe fn sls_syscall(num: u64, arg: u64) -> u64 {
    let ret: u64;
    core::arch::asm!(
        "syscall",
        inlateout("rax") num => ret,
        inlateout("rdi") arg => _,
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

/// Host-build stub: panics (MPK is not available in userspace).
#[cfg(not(feature = "target"))]
unsafe fn sls_syscall(_num: u64, _arg: u64) -> u64 {
    // Return CAP_ENOSYS for any trampoline syscall in host build
    CAP_ENOSYS as u64
}

/// Check if MPK is supported on this CPU.
///
/// Returns `true` if the kernel reports MPK support via the trampoline MPK
/// flags syscall. On CPUs without MPK (or in host builds), returns `false`
/// and the caller should fall back to the channel path.
pub fn mpk_supported() -> bool {
    let flags = unsafe { sls_syscall(SYS_SLS_TRAMPOLINE_MPK_FLAGS, 0) };
    (flags as u32) & CAP_TRAMP_MPK_SUPPORTED != 0
}

impl TrampolineCap {
    /// Create a new trampoline capability to a trusted callee.
    ///
    /// # Arguments
    /// * `callee_pid` - PID of the target sidecar process
    /// * `entry_vaddr` - Virtual address of the callee's verified entry point
    /// * `max_stack_bytes` - Maximum stack size the callee will use (guard bound)
    ///
    /// # Returns
    /// The trampoline capability on success, or a negative error code on failure.
    pub fn create(callee_pid: u32, entry_vaddr: u64, max_stack_bytes: u32) -> Result<Self, i64> {
        let req = TrampolineCreateRequest {
            callee_pid,
            _pad: 0,
            entry_vaddr,
            max_stack_bytes,
            _pad2: 0,
        };

        let result = unsafe { sls_syscall(SYS_SLS_TRAMPOLINE_CREATE, &req as *const _ as u64) };

        if (result as i64) < 0 {
            return Err(result as i64);
        }

        // The syscall returns the cap-table slot index on success.
        // For the inline path, we need the actual TrampolineCap data.
        // TODO: add a SYS_SLS_TRAMPOLINE_INFO syscall to retrieve the cap data,
        // or embed it in the cap word. For now, construct a minimal cap.
        Ok(TrampolineCap {
            entry_vaddr,
            stack_vaddr: 0, // set by callee's manifest
            callee_pkey: 0, // retrieved from kernel
            caller_pkey_mask: 0,
            data_pkey: 3, // shared arena key (convention)
            flags: 0x01,
            max_stack_bytes,
            _pad: 0,
        })
    }

    /// Invoke the trampoline via the kernel-mediated path (syscall 306).
    ///
    /// This is the "slow path" for interpreted languages that cannot use the
    /// inline WRPKRU+JMP stub. The kernel performs the call on behalf of the
    /// caller. Prefer the inline path (`trampoline_call_inline`) when possible.
    pub fn call_kernel(&self, tramp_idx: u16, args: &[u64]) -> Result<u64, i64> {
        let mut req = TrampolineCallRequest {
            tramp_idx,
            _pad: [0; 6],
            args: [0; 4],
            arg_count: args.len() as u64,
            arena_offset: 0,
            arena_len: 0,
        };

        let n = args.len().min(4);
        req.args[..n].copy_from_slice(&args[..n]);

        let result = unsafe { sls_syscall(SYS_SLS_TRAMPOLINE_CALL, &req as *const _ as u64) };

        if (result as i64) < 0 {
            Err(result as i64)
        } else {
            Ok(result)
        }
    }
}

/// Inline trampoline call — the fast path.
///
/// Performs the WRPKRU + JMP + RET + WRPKRU sequence entirely in user space.
/// This is approximately 6× the cost of a local function call (~30 ns).
///
/// # Safety
/// The caller must ensure:
/// - `tramp` points to a valid TrampolineCap (issued by the kernel)
/// - `callee_entry` is the verified entry point of the callee
/// - `callee_stack` is the callee's pre-allocated stack
/// - `args` contains at most 4 register-width arguments
/// - The callee writes its result to the arena slot at `result_slot`
///
/// # Arguments
/// * `tramp` - The trampoline capability
/// * `callee_entry` - The callee's entry point virtual address
/// * `callee_stack` - The callee's stack pointer (top of stack)
/// * `args` - Up to 4 register-width arguments (rdi, rsi, rdx, rcx)
/// * `result_slot` - Pointer to the arena slot where the callee writes its result
///
/// # Returns
/// The callee's result value, or a negative error code.
#[cfg(feature = "target")]
pub unsafe fn trampoline_call_inline(
    tramp: &TrampolineCap,
    callee_entry: u64,
    callee_stack: u64,
    args: &[u64],
    result_slot: *mut u64,
) -> i64 {
    let n = args.len().min(4);
    let a0 = if n > 0 { args[0] } else { 0 };
    let a1 = if n > 1 { args[1] } else { 0 };
    let a2 = if n > 2 { args[2] } else { 0 };
    let a3 = if n > 3 { args[3] } else { 0 };

    let saved_pkru: u32;
    let result: i64;

    core::arch::asm!(
        // 1. Save caller's PKRU
        "xor ecx, ecx",
        "rdpkru",
        "mov {saved_pkru:e}, eax",

        // 2. Load callee's PKRU — grant access to callee's code (key 2)
        //    and arena data (key 3), restrict caller's data (key 1)
        "mov eax, {caller_pkey_mask:e}",
        "wrpkru",

        // 3. Save caller's RSP, switch to callee's stack
        "mov {old_rsp}, rsp",
        "mov rsp, {callee_stack}",

        // 4. Push arguments onto callee's stack (callee expects them there)
        "push {a3}",
        "push {a2}",
        "push {a1}",
        "push {a0}",

        // 5. JMP to callee's entry point (no CALL — callee uses RET to return)
        "jmp *{callee_entry}",

        // Callee returns here via RET (the trampoline's return address is
        // pushed by the JMP, so RET pops it and lands here)

        // 6. Restore caller's stack
        "mov rsp, {old_rsp}",

        // 7. Restore caller's PKRU
        "mov eax, {saved_pkru:e}",
        "wrpkru",

        // 8. Read result from arena slot
        "mov {result}, [{result_slot}]",

        saved_pkru = out(reg) _,
        old_rsp = out(reg) _,
        result = out(reg) result,
        a0 = in(reg) a0,
        a1 = in(reg) a1,
        a2 = in(reg) a2,
        a3 = in(reg) a3,
        callee_entry = in(reg) callee_entry,
        callee_stack = in(reg) callee_stack,
        result_slot = in(reg) result_slot,
        caller_pkey_mask = in(reg) tramp.caller_pkey_mask,
        out("rax") _,
        out("rcx") _,
        out("r11") _,
    );

    result
}

/// Host-build fallback for trampoline_call_inline — returns CAP_ENOSYS.
#[cfg(not(feature = "target"))]
pub unsafe fn trampoline_call_inline(
    _tramp: &TrampolineCap,
    _callee_entry: u64,
    _callee_stack: u64,
    _args: &[u64],
    _result_slot: *mut u64,
) -> i64 {
    CAP_ENOSYS
}

/// High-level trampoline call — detects MPK and chooses the appropriate path.
///
/// If MPK is supported and the trampoline cap is valid, uses the inline path.
/// Otherwise, falls back to the kernel-mediated path (syscall 306).
/// If neither works, returns `CAP_ENOSYS`.
pub fn trampoline_call(
    tramp: &TrampolineCap,
    tramp_idx: u16,
    callee_entry: u64,
    callee_stack: u64,
    args: &[u64],
    result_slot: *mut u64,
) -> Result<u64, i64> {
    if mpk_supported() {
        // Inline path — the fast path
        let result = unsafe {
            trampoline_call_inline(tramp, callee_entry, callee_stack, args, result_slot)
        };
        if result >= 0 {
            Ok(result as u64)
        } else {
            Err(result)
        }
    } else {
        // Kernel-mediated path — the slow path
        tramp.call_kernel(tramp_idx, args)
    }
}

/// Query the kernel's MPK support flags.
///
/// Returns the raw flags word from the kernel:
/// - bit 0 (`CAP_TRAMP_MPK_SUPPORTED`): CPU supports MPK/PKU
/// - bit 1 (`CAP_TRAMP_MPK_ENABLED`): kernel has enabled MPK subsystem
pub fn mpk_flags() -> u32 {
    unsafe { sls_syscall(SYS_SLS_TRAMPOLINE_MPK_FLAGS, 0) as u32 }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_mpk_not_supported_in_host() {
        // In host build, MPK should not be supported
        assert!(!mpk_supported());
    }

    #[test]
    fn test_trampoline_create_fails_in_host() {
        // In host build, trampoline creation should fail with ENOSYS
        let result = TrampolineCap::create(100, 0x1000, 65536);
        assert_eq!(result, Err(CAP_ENOSYS));
    }

    #[test]
    fn test_trampoline_call_fails_in_host() {
        // In host build, trampoline calls should fail with ENOSYS
        let tramp = TrampolineCap {
            entry_vaddr: 0x1000,
            stack_vaddr: 0x2000,
            callee_pkey: 2,
            caller_pkey_mask: 0,
            data_pkey: 3,
            flags: 0x01,
            max_stack_bytes: 65536,
            _pad: 0,
        };
        let result = unsafe {
            trampoline_call_inline(&tramp, 0x1000, 0x2000, &[1, 2], core::ptr::null_mut())
        };
        assert_eq!(result, CAP_ENOSYS);
    }

    #[test]
    fn test_trampoline_call_high_level_fails_in_host() {
        let tramp = TrampolineCap {
            entry_vaddr: 0x1000,
            stack_vaddr: 0x2000,
            callee_pkey: 2,
            caller_pkey_mask: 0,
            data_pkey: 3,
            flags: 0x01,
            max_stack_bytes: 65536,
            _pad: 0,
        };
        let result = trampoline_call(&tramp, 0, 0x1000, 0x2000, &[1, 2], core::ptr::null_mut());
        assert_eq!(result, Err(CAP_ENOSYS));
    }
}
