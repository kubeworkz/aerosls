//! m4_hardening_probe.rs — M4.8's live clone3/requeue/robust/signal gate.
//!
//! The fixture is `fixtures/m4_hardening_gate.elf`, a static non-PIE musl
//! build of `tools/m4_hardening_gate`. It creates two kernel threads through
//! clone3(435), requeues a parent waiter from A to B, exits a thread with a
//! robust futex list, and tgkills a parked sibling by TGID/TID. It ends with
//! a group-directed kill, so the existing exit_group sweep is the final
//! evidence.
//!
//! Spawned only by the initial POSIX sidecar when `M4_HARDENING_PROBE=1` is
//! passed to the ISO build; default builds compile neither this module nor
//! its spawn path.

use aerosls_proto::manifest::{
    build_manifest, crc32, ArgEnv, Bootstrap, Budget, Cpu, Image, Limits,
    Manifest, ManifestCap, HEADER_LEN, MAX_MANIFEST_CAPS,
};
use crate::linux::LINUX_DEFAULT_ENV;

static PROBE: &[u8] = include_bytes!("../fixtures/m4_hardening_gate.elf");

/* The gate profile's third task slot. The name is the kernel spawn stamp's
 * identity, and `entry.rs` tracks the gate task under the same constant. */
pub const PROBE_NAME: &str = aerosls_proto::env_proto::LINUX_TASK_NAME;
pub const PROBE_RESERVE: u64 = 1024 * 1024;

const IMAGE_VBASE: u64 = 0x4000_0000_0000; /* USER_PROC_CODE_BASE */
const HEAP_GUARD_BYTES: u64 = 4096;
const MEM_BUDGET_BYTES: u64 = 8 * 1024 * 1024;
const STACK_BUDGET_BYTES: u64 = 64 * 1024;

pub type Spawned = crate::e7_firstuser::Spawned;

pub fn spawn<K: aerosls_proto::kabi::Kernel>(
    k: &K,
    budget_base: u64,
    budget_len: u64,
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
        name: Some(PROBE_NAME),
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
            max_tasks: 4,
            max_fds: 0,
            max_channels: 8,
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
        argenv: Some(ArgEnv::new(&[PROBE_NAME], LINUX_DEFAULT_ENV)),
        signature: None,
    };

    let mut blob = build_manifest(&m);
    blob.extend_from_slice(&tail.to_le_bytes());
    let total = blob.len() as u32;
    blob[16..20].copy_from_slice(&total.to_le_bytes());
    let crc = crc32(&blob[HEADER_LEN..]);
    blob[20..24].copy_from_slice(&crc.to_le_bytes());

    let (ch_r, ch_w) = k.linux_spawn(&blob, 0)?;

    let n_img_pages = (PROBE.len() as u64 + 4095) / 4096;
    let stack_base = IMAGE_VBASE + n_img_pages * 4096 + HEAP_GUARD_BYTES;
    let stk_pages = (STACK_BUDGET_BYTES + 4095) / 4096;
    let heap_base = stack_base + stk_pages * 4096;
    let heap_limit = heap_base + ((MEM_BUDGET_BYTES + 4095) / 4096) * 4096;

    Ok(Spawned { ch_r, ch_w, heap_base, heap_limit })
}
