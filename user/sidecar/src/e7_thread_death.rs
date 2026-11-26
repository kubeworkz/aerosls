//! e7_thread_death.rs — E7 TG-1 increment I5b's two live arms.
//!
//! I5's glue was hooked on `exit(60)` only, so a thread that died any other
//! way never cleared its TID word. The host test and clause Z7 pin the shape;
//! THIS module supplies the end-to-end evidence, by spawning two children
//! through the real create path:
//!
//!   * `e7.tdeath.fault.0` (fixtures/e7_thread_fault_probe.S) — a cloned
//!     thread reads a PROT_NONE page and dies of a Ring-3 #PF while a sibling
//!     in its group is parked on its TID word. The fault path's group teardown
//!     must clear that word and wake the joiner (n=1), and walk the robust
//!     list the dead thread registered.
//!   * `e7.tdeath.group.0` (fixtures/e7_thread_groupexit_probe.S) — the leader
//!     asks for exit_group (231) while one thread still owns a CLEARTID word
//!     and a robust list and a second thread is parked on that word. The same
//!     clear, wake and walk must happen on the exit_group path.
//!
//! Both payloads are planted at the budget region's tail before their own
//! spawn (`e7_probe.rs`'s placement), so neither lives in the boot image.
//! tests/linux_thread_death_check.sh asserts the kernel's lines on a boot.

use aerosls_proto::manifest::{
    build_manifest, crc32, ArgEnv, Bootstrap, Budget, Cpu, Image, Limits,
    Manifest, ManifestCap, HEADER_LEN, MAX_MANIFEST_CAPS,
};
use crate::linux::LINUX_DEFAULT_ENV;

/// `fixtures/e7_thread_fault_probe.S` → one-.text flat, `_start` at offset 0.
static FAULT: &[u8] = include_bytes!("../fixtures/e7_thread_fault_probe.flat");
/// `fixtures/e7_thread_groupexit_probe.S`, same shape.
static GROUPEXIT: &[u8] = include_bytes!("../fixtures/e7_thread_groupexit_probe.flat");
/// `fixtures/e7_fault_handler_probe.S` — E7 increment 31's arm: the SAME
/// Ring-3 #PF as `FAULT`, but with a SIGSEGV handler installed, so the
/// kernel must deliver a frame instead of killing the thread.
static FAULT_HANDLER: &[u8] =
    include_bytes!("../fixtures/e7_fault_handler_probe.flat");
/// `fixtures/e7_fault_return_probe.S` — E7 increment 32's arm: the same #PF
/// with a handler, but this handler FIXES the fault in its ucontext and
/// returns through `rt_sigreturn(15)`, so the retried instruction runs.
static FAULT_RETURN: &[u8] =
    include_bytes!("../fixtures/e7_fault_return_probe.flat");
/// `fixtures/e7_fault_reveal_probe.S` — E7 increment 34's arm: SIGTERM is
/// blocked and pended BEFORE the fault, and the trap handler UNBLOCKS it, so
/// the sidecar's reveal must run inside that call and the task dies there.
static FAULT_REVEAL: &[u8] =
    include_bytes!("../fixtures/e7_fault_reveal_probe.flat");
/// `fixtures/e7_fault_abandon_probe.S` — E7 increment 35's arm: the trap
/// handler leaves its frame by a longjmp-style rsp pop instead of a 15, and
/// the first 14 afterwards must close the kernel's mask window (its answer,
/// and a 15 that must now be -EINVAL, both come from the sidecar again).
static FAULT_ABANDON: &[u8] =
    include_bytes!("../fixtures/e7_fault_abandon_probe.flat");

pub const FAULT_NAME: &str = "e7.tdeath.fault.0";
pub const GROUP_NAME: &str = "e7.tdeath.group.0";
pub const FAULT_HANDLER_NAME: &str = "e7.fault.handler.0";
pub const FAULT_RETURN_NAME: &str = "e7.fault.return.0";
/// E7 increment 34: a SIGTERM blocked before the fault, pending by the time
/// the handler runs, and UNBLOCKED from inside it — the reveal must run in
/// the handler, so the task dies there.
pub const FAULT_REVEAL_NAME: &str = "e7.fault.reveal.0";
/// E7 increment 35: the trap handler leaves its frame by longjmp; the first
/// rt_sigprocmask made above the abandoned frame must close the mask window.
pub const FAULT_ABANDON_NAME: &str = "e7.fault.abandon.0";

pub const PROBE_RESERVE: u64 = 64 * 1024;

const IMAGE_VBASE: u64 = 0x4000_0000_0000; /* USER_PROC_CODE_BASE */
const HEAP_GUARD_BYTES: u64 = 4096;        /* cap.c: one guard page */
const MEM_BUDGET_BYTES: u64 = 64 * 1024;   /* the BUDGET record's mem_bytes */
const STACK_BUDGET_BYTES: u64 = 16 * 1024; /* stack_bytes; kernel floor 4096 */

pub struct Spawned {
    pub ch_r: u32,
    pub ch_w: u32,
    pub heap_base: u64,
    pub heap_limit: u64,
}

fn spawn_one<K: aerosls_proto::kabi::Kernel>(
    k: &K,
    budget_base: u64,
    budget_len: u64,
    payload: &'static [u8],
    name: &'static str,
) -> Result<Spawned, i32> {
    if budget_len < PROBE_RESERVE + 0x1000 || payload.len() as u64 > PROBE_RESERVE {
        return Err(-1);
    }
    let tail = budget_base + budget_len - PROBE_RESERVE;
    unsafe {
        core::ptr::copy_nonoverlapping(payload.as_ptr(), tail as *mut u8, payload.len());
    }

    let caps: [Option<ManifestCap<'_>>; MAX_MANIFEST_CAPS] =
        [None; MAX_MANIFEST_CAPS];
    let m = Manifest {
        version_major: 1,
        version_minor: 0,
        flags: 0,
        name: Some(name),
        personality: Some("e7.linux.v1"),
        image: Some(Image {
            offset: 0,
            size: payload.len() as u32,
            entry: 0,
        }),
        budget: Some(Budget {
            mem_bytes: MEM_BUDGET_BYTES,
            stack_bytes: STACK_BUDGET_BYTES as u32,
            heap_initial: 0,
        }),
        cpu: Some(Cpu {
            share: 10,
            preemptible: true,
        }),
        limits: Some(Limits {
            max_tasks: 1,
            max_fds: 0,
            max_channels: 2,
            max_open_files: 0,
            chan_queue_depth: 4,
        }),
        caps,
        n_caps: 0,
        bootstrap: Some(Bootstrap {
            console: None,
            debug: None,
            log_level: 1,
        }),
        flags_value: Some(0),
        argenv: Some(ArgEnv::new(&[name], LINUX_DEFAULT_ENV)),
        signature: None,
    };

    let mut blob = build_manifest(&m);
    blob.extend_from_slice(&tail.to_le_bytes());
    let total = blob.len() as u32;
    blob[16..20].copy_from_slice(&total.to_le_bytes());
    let crc = crc32(&blob[HEADER_LEN..]);
    blob[20..24].copy_from_slice(&crc.to_le_bytes());

    let (ch_r, ch_w) = k.linux_spawn(&blob, 0)?;

    let n_img_pages = (payload.len() as u64 + 4095) / 4096;
    let stack_base = IMAGE_VBASE + n_img_pages * 4096 + HEAP_GUARD_BYTES;
    let stk_pages = (STACK_BUDGET_BYTES + 4095) / 4096;
    let heap_base = stack_base + stk_pages * 4096;
    let heap_limit = heap_base + ((MEM_BUDGET_BYTES + 4095) / 4096) * 4096;

    Ok(Spawned { ch_r, ch_w, heap_base, heap_limit })
}

/// The thread that dies of a #PF while its joiner is parked on its TID word.
pub fn spawn_fault<K: aerosls_proto::kabi::Kernel>(
    k: &K,
    budget_base: u64,
    budget_len: u64,
) -> Result<Spawned, i32> {
    spawn_one(k, budget_base, budget_len, FAULT, FAULT_NAME)
}

/// The group that goes down through exit_group with the same two words owed.
pub fn spawn_group<K: aerosls_proto::kabi::Kernel>(
    k: &K,
    budget_base: u64,
    budget_len: u64,
) -> Result<Spawned, i32> {
    spawn_one(k, budget_base, budget_len, GROUPEXIT, GROUP_NAME)
}

/// E7 increment 31: the fault that finds a handler — the same PROT_NONE read,
/// but SIGSEGV is installed, so the delivery must replace the kill.
pub fn spawn_fault_handler<K: aerosls_proto::kabi::Kernel>(
    k: &K,
    budget_base: u64,
    budget_len: u64,
) -> Result<Spawned, i32> {
    spawn_one(k, budget_base, budget_len, FAULT_HANDLER, FAULT_HANDLER_NAME)
}

/// E7 increment 32: the fault whose handler RETURNS — it rewrites the
/// interrupted r13 in its ucontext, and the kernel must resume the faulting
/// instruction with that fix in place.
pub fn spawn_fault_return<K: aerosls_proto::kabi::Kernel>(
    k: &K,
    budget_base: u64,
    budget_len: u64,
) -> Result<Spawned, i32> {
    spawn_one(k, budget_base, budget_len, FAULT_RETURN, FAULT_RETURN_NAME)
}

/// E7 increment 34: the fault whose handler UNBLOCKS a pended SIGTERM — the
/// reveal must run inside the handler (the sidecar's ladder), not after the
/// return, so the task dies inside the call it made.
pub fn spawn_fault_reveal<K: aerosls_proto::kabi::Kernel>(
    k: &K,
    budget_base: u64,
    budget_len: u64,
) -> Result<Spawned, i32> {
    spawn_one(k, budget_base, budget_len, FAULT_REVEAL, FAULT_REVEAL_NAME)
}

/// E7 increment 35: the fault whose handler LEAVES the frame by longjmp — the
/// first 14 above the frame must close the kernel's window (mask back to the
/// frame's recorded set, frame dropped), so the query answers the sidecar's
/// copy and a following 15 is refused instead of restoring a dead frame.
pub fn spawn_fault_abandon<K: aerosls_proto::kabi::Kernel>(
    k: &K,
    budget_base: u64,
    budget_len: u64,
) -> Result<Spawned, i32> {
    spawn_one(k, budget_base, budget_len, FAULT_ABANDON, FAULT_ABANDON_NAME)
}
