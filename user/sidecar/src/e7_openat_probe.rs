//! e7_openat_probe.rs — E7 TG-1 increment I4's native openat probe.
//!
//! The companion of `e7_thread_probe.rs` and `e7_futex_probe.rs`. This
//! sidecar spawns one more Linux-personality child through the REAL create
//! path; that child clones one sibling, and the SIBLING forwards openat(257)
//! and close(3) across the group's channel pair. The leader never forwards,
//! so the live boot measures exactly what I4 adds: a cloned thread parks on
//! the group's forward lease, the sidecar answers, and the reply is applied
//! to and wakes the CHILD (fd >= 3, close == 0).
//!
//! Payload placement is `e7_futex_probe.rs`'s, two 4 KiB slots lower than the
//! thread probe: all three probes are embedded in this binary and copied into
//! the budget region's tail before their spawn, so the copies must not
//! overlap.

use aerosls_proto::manifest::{
    build_manifest, crc32, ArgEnv, Bootstrap, Budget, Cpu, Image, Limits,
    Manifest, ManifestCap, HEADER_LEN, MAX_MANIFEST_CAPS,
};
use crate::linux::LINUX_DEFAULT_ENV;

/// `fixtures/e7_openat_probe.S`, built to a one-.text flat image whose
/// `_start` is at file offset 0. The committed .flat is the byte-exact output
/// of fixtures/build_openat_probe.sh.
static PROBE: &[u8] = include_bytes!("../fixtures/e7_openat_probe.flat");

/// The child's name — registered at create and resolved by the sidecar's
/// exit_group reap through `k.sidecar_pid(name, partition)`.
pub const PROBE_NAME: &str = "e7.openat.probe.0";

/// Reserve at the TOP of the budget region (page-aligned: budget_base is
/// 4 KiB-aligned from the manifest's MEM cap, budget_len is a MiB count).
/// Two pages lower than the thread probe's slot and one lower than the futex
/// probe's, so the three payload copies never overlap.
pub const PROBE_RESERVE: u64 = 64 * 1024;
const PROBE_SLOT_DROP: u64 = 0x2000;

/// The child's layout, mirrored from the kernel's create-side layout
/// (kernel/cap.c steps 5-6) and from the manifest THIS file authors — the
/// §5.4 budget span is three numbers both sides derive from one record.
const IMAGE_VBASE: u64 = 0x4000_0000_0000; /* USER_PROC_CODE_BASE */
const HEAP_GUARD_BYTES: u64 = 4096;        /* cap.c: one guard page */
const MEM_BUDGET_BYTES: u64 = 64 * 1024;   /* the BUDGET record's mem_bytes */
const STACK_BUDGET_BYTES: u64 = 16 * 1024; /* stack_bytes; kernel floor 4096 */

/// The caller's channel endpoints and the child's §5.4 span, as `linux::track`
/// wants them.
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
    if budget_len < PROBE_RESERVE + PROBE_SLOT_DROP + 0x1000 {
        return Err(-1);
    }
    let tail = budget_base + budget_len - PROBE_RESERVE - PROBE_SLOT_DROP;
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
        personality: Some("e7.linux.v1"),
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
