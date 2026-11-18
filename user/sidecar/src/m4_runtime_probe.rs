//! m4_runtime_probe.rs — M4.7's live runtime gate.
//!
//! The fixture is `fixtures/thread_runtime_gate.elf`, a static non-PIE musl
//! build of `tools/thread_runtime_gate`. It creates a second kernel thread,
//! shares a heap object and the inherited console fd, and parks both members
//! on the group's private futex queue. The harness checkpoints the parked
//! group; boot 2 wakes the restored worker and the same parent joins it.
//!
//! Spawned only when the gate sidecar is built with `m4_runtime_probe`;
//! default builds compile neither this module nor its spawn path.

use aerosls_proto::manifest::{
    pack_manifest, ArgEnv, Bootstrap, Budget, Cpu, Image, Limits,
    Manifest, ManifestCap, MAX_MANIFEST_CAPS,
};
use crate::linux::LINUX_DEFAULT_ENV;

static PROBE: &[u8] = include_bytes!("../fixtures/thread_runtime_gate.elf");

pub const PROBE_NAME: &str = aerosls_proto::env_proto::LINUX_TASK_NAME;
pub const PROBE_RESERVE: u64 = 1024 * 1024;

const IMAGE_VBASE: u64 = 0x4000_0000_0000; /* USER_PROC_CODE_BASE */
const HEAP_GUARD_BYTES: u64 = 4096;
/* The fixture's 64 KiB worker stack travels in the image's RW segment; this
 * budget is the shared heap span. The span is virtual; only touched pages
 * are backed. */
const MEM_BUDGET_BYTES: u64 = 16 * 1024 * 1024;
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
            // parent + one Rust thread; the clone messenger pair per task.
            max_tasks: 4,
            max_fds: 4,
            max_channels: 8,
            max_open_files: 4,
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
