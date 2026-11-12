//! m1_net_probe.rs — M1's live Linux TCP round trip.
//!
//! The child is `fixtures/m1_net_probe.flat`: raw x86-64 Linux syscalls, no
//! libc, mapped by the same kernel create path as the E7 probe. Its socket
//! calls travel the M1 bridge the same way every other Linux syscall does —
//! the child forwards to THIS sidecar, and `linux::service` answers through
//! the sidecar's own `network` Chan cap. That is deliberately the tenant
//! shape: the child has zero capability records of its own, and only the
//! sidecar's manifest decides whether a network exists.
//!
//! Spawned only when the `m1_net_probe` feature is compiled in and the
//! environment carries both network channel endpoints; the live image is
//! built with `M1_NET_PROBE=1` so no shipped profile grows this task.
//!
//! The payload placement and budget arithmetic are the E7 probe's, copied
//! rather than shared: both sides derive the §5.4 span from the manifest,
//! and the kernel's create is the one that stamps the child's copy.

use aerosls_proto::manifest::{
    build_manifest, crc32, ArgEnv, Bootstrap, Budget, Cpu, Image, Limits,
    Manifest, ManifestCap, HEADER_LEN, MAX_MANIFEST_CAPS,
};
use crate::linux::LINUX_DEFAULT_ENV;

static PROBE: &[u8] = include_bytes!("../fixtures/m1_net_probe.flat");

/// The child's registry name — how the sidecar's exit_group reap resolves it.
pub const PROBE_NAME: &str = "m1.net.probe.0";

/// Plant at the top of the parent's budget region, the E7 probe's reserve.
pub const PROBE_RESERVE: u64 = 64 * 1024;

/// The child's layout, mirrored from kernel/cap.c steps 5-6 (the E7 probe's
/// constants, with a larger fd limit for the round trip's three sockets).
const IMAGE_VBASE: u64 = 0x4000_0000_0000; /* USER_PROC_CODE_BASE */
const HEAP_GUARD_BYTES: u64 = 4096;        /* cap.c: one guard page */
const MEM_BUDGET_BYTES: u64 = 64 * 1024;   /* the BUDGET record's mem_bytes */
const STACK_BUDGET_BYTES: u64 = 16 * 1024; /* stack_bytes */

pub struct Spawned {
    pub ch_r: u32,
    pub ch_w: u32,
    pub heap_base: u64,
    pub heap_limit: u64,
}

/// Plant the flat probe at the budget tail and spawn it through the real
/// kernel path. The child's manifest has zero capability records: the
/// kernel inserts only the messenger channel pair.
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
        personality: Some("m1.linux.net.v1"),
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
            max_fds: 8,          /* client, listener, accepted peer */
            max_channels: 2,
            max_open_files: 8,
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
