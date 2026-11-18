//! m4_ckpt_probe.rs — M4.6's live thread-group checkpoint probe.
//!
//! The gate profile is the one live Linux tenant whose task name is already
//! in an `EnvCkptRecord`, so this profile replaces the E7 firstuser with a
//! fixture that creates a real two-thread LinuxThreadGroup and parks both
//! members in private futex waits. The checkpoint carries the shared address
//! space and both contexts; the replay's parent timeout wakes the parent,
//! which wakes the child's restored untimed waiter.
//!
//! The fixture is `fixtures/m4_ckpt_probe.flat`. Its parent waits for the
//! child's `M4.6-THREAD-OK` write and READY word, then writes
//! `M4.6-CKPT-READY` and parks on a timed futex; the child is already parked
//! on an untimed one. Both are in the same address space and group when the
//! harness triggers the checkpoint.
//!
//! Spawned only when the gate sidecar is built with `m4_ckpt_probe`; default
//! builds compile neither this module nor its spawn path.

use aerosls_proto::manifest::{
    pack_manifest, ArgEnv, Bootstrap, Budget, Cpu, Image, Limits,
    Manifest, ManifestCap, MAX_MANIFEST_CAPS,
};
use crate::linux::LINUX_DEFAULT_ENV;

static PROBE: &[u8] = include_bytes!("../fixtures/m4_ckpt_probe.flat");

pub const PROBE_NAME: &str = aerosls_proto::env_proto::M4_CKPT_TASK_NAME;
/// M5.3: the same fixture under the ordinary-tenant checkpoint identity.
pub const TENANT_PROBE_NAME: &str =
    aerosls_proto::env_proto::TENANT_CKPT_TASK_NAME;
pub const PROBE_RESERVE: u64 = 64 * 1024;

const IMAGE_VBASE: u64 = 0x4000_0000_0000; /* USER_PROC_CODE_BASE */
const HEAP_GUARD_BYTES: u64 = 4096;
const MEM_BUDGET_BYTES: u64 = 64 * 1024;
const STACK_BUDGET_BYTES: u64 = 16 * 1024;

/// The same shape `entry.rs` uses for the ordinary gate tenant, so the shared
/// `track` bookkeeping is unchanged.
pub type Spawned = crate::e7_firstuser::Spawned;

pub fn spawn<K: aerosls_proto::kabi::Kernel>(
    k: &K,
    budget_base: u64,
    budget_len: u64,
) -> Result<Spawned, i32> {
    spawn_named(k, budget_base, budget_len, PROBE_NAME)
}

/// M5.3: reuse the proven two-thread fixture in an ordinary tenant profile,
/// under the tenant checkpoint task name init registers.
pub fn spawn_tenant<K: aerosls_proto::kabi::Kernel>(
    k: &K,
    budget_base: u64,
    budget_len: u64,
) -> Result<Spawned, i32> {
    spawn_named(k, budget_base, budget_len, TENANT_PROBE_NAME)
}

fn spawn_named<K: aerosls_proto::kabi::Kernel>(
    k: &K,
    budget_base: u64,
    budget_len: u64,
    name: &str,
) -> Result<Spawned, i32> {
    if budget_len < PROBE_RESERVE + 0x1000 {
        return Err(-1);
    }
    let tail = budget_base + budget_len - PROBE_RESERVE;
    if PROBE.len() as u64 > PROBE_RESERVE {
        return Err(-1);
    }
    unsafe {
        core::ptr::copy_nonoverlapping(PROBE.as_ptr(), tail as *mut u8, PROBE.len());
    }

    let caps: [Option<ManifestCap<'_>>; MAX_MANIFEST_CAPS] =
        [None; MAX_MANIFEST_CAPS];
    let m = Manifest {
        version_major: 1,
        version_minor: 0,
        flags: 0,
        name: Some(name),
        personality: Some("m4.linux.v1"),
        image: Some(Image {
            offset: 0,
            size: PROBE.len() as u32,
            entry: 0,
        }),
        budget: Some(Budget {
            mem_bytes: MEM_BUDGET_BYTES,
            stack_bytes: STACK_BUDGET_BYTES as u32,
            heap_initial: 0,
        }),
        cpu: Some(Cpu { share: 10, preemptible: true }),
        limits: Some(Limits {
            // The fixture's parent + clone child, with room for the group's
            // leader and a messenger channel each.
            max_tasks: 3,
            max_fds: 0,
            max_channels: 6,
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
        interp: None,
        signature: None,
    };

    let blob = pack_manifest(&m, tail);

    let (ch_r, ch_w) = k.linux_spawn(&blob, 0)?;

    let n_img_pages = (PROBE.len() as u64 + 4095) / 4096;
    let stack_base = IMAGE_VBASE + n_img_pages * 4096 + HEAP_GUARD_BYTES;
    let stk_pages = (STACK_BUDGET_BYTES + 4095) / 4096;
    let heap_base = stack_base + stk_pages * 4096;
    let heap_limit = heap_base + ((MEM_BUDGET_BYTES + 4095) / 4096) * 4096;

    Ok(Spawned { ch_r, ch_w, heap_base, heap_limit })
}
