//! m4_thread_probe.rs — M4.2's live proof that the clone/thread-create path
//! is real on a boot.
//!
//! The fixture (`fixtures/m4_thread_probe.S`) is a Linux-personality task.
//! It issues raw clone(56), the child writes `M4-THREAD-OK` through its own
//! tracked messenger and publishes a shared stack word, then faults; the
//! parent observes the word, writes `M4-THREAD-PARENT-OK`, and exits. The
//! serial log is the evidence: child marker before parent marker can only
//! happen if the child ran in the SAME address space, resumed at the parent's
//! post-syscall RIP, and the sidecar tracked its channel.
//!
//! Spawned only by the initial POSIX sidecar when `M4_THREAD_PROBE=1` is
//! passed to the ISO build; default builds compile none of this module.

use aerosls_proto::manifest::{
    pack_manifest, ArgEnv, Bootstrap, Budget, Cpu, Image, Limits,
    Manifest, ManifestCap, MAX_MANIFEST_CAPS,
};
use crate::linux::LINUX_DEFAULT_ENV;

static PROBE: &[u8] = include_bytes!("../fixtures/m4_thread_probe.flat");

pub const PROBE_NAME: &str = "m4.thread.probe.0";
pub const PROBE_RESERVE: u64 = 64 * 1024;

const IMAGE_VBASE: u64 = 0x4000_0000_0000; /* USER_PROC_CODE_BASE */
const HEAP_GUARD_BYTES: u64 = 4096;
const MEM_BUDGET_BYTES: u64 = 64 * 1024;
const STACK_BUDGET_BYTES: u64 = 16 * 1024;

pub struct Spawned {
    pub ch_r: u32,
    pub ch_w: u32,
    pub heap_base: u64,
    pub heap_limit: u64,
}

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
            max_tasks: 2,
            max_fds: 0,
            max_channels: 4,
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
