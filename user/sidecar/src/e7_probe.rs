//! e7_probe.rs — the E7 boot proof: this POSIX sidecar spawns a
//! Linux-personality child (`SYS_SLS_LINUX_SPAWN`, 328) and thereby puts
//! AeroSLS-Linux-ABI-Shim-Design-v0.1.md §3.3's steps 5–7 on a real boot —
//! the forward, the park, the resume, the §5.7 map and this sidecar's own
//! answers, observed as `[E7]` lines on the serial log rather than argued
//! from source.
//!
//! Why here and not in init: §3.5 places the Linux task's single channel
//! with *its environment's POSIX sidecar*, and the creator tree (init →
//! …) already carries `sidecar_authority` in every process, so this sidecar
//! may spawn; the answers live in `linux::service`, whose wait-set arm
//! (`linux::append_wait`) is already wired into the entry loop below.
//!
//! Payload placement, deliberately cheap: `fixtures/e7_linux_probe.flat`
//! is embedded in THIS binary (include_bytes) and planted at the budget
//! region's last 64 KiB just before the spawn. The budget cap is
//! identity-mapped RW in our address space (`rust_entry` stands the heap
//! over `budget_base`), and the kernel's create COPIES the image
//! synchronously while `linux_spawn` is on the stack — so the bytes only
//! have to be valid until that syscall returns, long before the heap (which
//! starts a full 1 MiB + 3 MiB below/above) could reach the tail. This
//! keeps the payload out of the boot image, out of every manifest except
//! its own, and out of the sidecar-archive stamp machinery: one crate's
//! sources, exactly like every other increment.
//!
//! The child's table is minted from a manifest with NO capability records —
//! §3.5's property as a construction, not a claim: the probe holds exactly
//! one capability pair (its messenger channel back to this sidecar).

use aerosls_proto::manifest::{
    build_manifest, crc32, Bootstrap, Budget, Cpu, Image, Limits, Manifest,
    ManifestCap, HEADER_LEN, MAX_MANIFEST_CAPS,
};

/// The probe payload: `fixtures/e7_linux_probe.S`, built to a flat image
/// (one `.text`, `_start` at file offset 0 — the create's flat mapper
/// enters at `image_entry`, which is therefore 0). The committed .flat is
/// the byte-exact output of fixtures/build_probe.sh; the fixture's header
/// carries the exact build line.
static PROBE: &[u8] = include_bytes!("../fixtures/e7_linux_probe.flat");

/// The child's name — registered at create (the manifest's NAME record),
/// which is how `linux::service`'s exit_group reap resolves it through
/// `k.sidecar_pid(name, partition)` with the PARTITION_SYSTEM partition
/// this sidecar's boot tree lives in.
pub const PROBE_NAME: &str = "e7.linux.probe.0";

/// Reserve at the TOP of the budget region (page-aligned: budget_base is
/// 4 KiB-aligned from the manifest's MEM cap, budget_len is a MiB count).
pub const PROBE_RESERVE: u64 = 64 * 1024;

/// The caller's channel endpoints, as `linux::track` wants them.
pub struct Spawned {
    pub ch_r: u32,
    pub ch_w: u32,
}

/// Plant the payload at the budget tail and spawn it through the real
/// kernel path. Failures are returned, never fatal to this sidecar's boot:
/// the proof is a proof, not a dependency. (A too-small budget is the one
/// refusal that never reaches the kernel — there is nowhere to plant.)
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
    // Identity-mapped budget region: rust_entry already uses budget_base
    // as the heap's pointer, so this write is the same mapping the heap
    // itself lives on. The kernel reads these bytes as PHYSICAL at
    // image_kaddr — phys == vaddr holds for exactly this region.
    unsafe {
        core::ptr::copy_nonoverlapping(PROBE.as_ptr(), tail as *mut u8, PROBE.len());
    }

    // §3.5 as construction: zero capability records — the child's table
    // will hold only the messenger channel pair the create inserts.
    let caps: [Option<ManifestCap<'_>>; MAX_MANIFEST_CAPS] =
        [None; MAX_MANIFEST_CAPS];
    let m = Manifest {
        version_major: 1,
        version_minor: 0,
        flags: 0,
        name: Some(PROBE_NAME),
        personality: Some("e7.linux.v1"), /* informational (kernel/cap.c
                                           * treats the record as a tag) */
        image: Some(Image {
            offset: 0,                     /* image data starts at the tail */
            size: PROBE.len() as u32,
            entry: 0,                      /* _start is at file offset 0 */
        }),
        budget: Some(Budget {
            mem_bytes: 64 * 1024,         /* informational (copied to the BIB) */
            stack_bytes: 16 * 1024,       /* kernel floor is 4096 */
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
        signature: None,
    };

    // Packed blob + image_kaddr footer, with total_len and the body CRC
    // patched to cover records + footer — byte-for-byte the pattern the
    // init-side manifest builders use (user/init/src/dm_manifest.rs is the
    // format reference), because the kernel's parser is one parser.
    let mut blob = build_manifest(&m);
    blob.extend_from_slice(&tail.to_le_bytes());
    let total = blob.len() as u32;
    blob[16..20].copy_from_slice(&total.to_le_bytes());
    let crc = crc32(&blob[HEADER_LEN..]);
    blob[20..24].copy_from_slice(&crc.to_le_bytes());

    // target_partition 0 = inherit (this sidecar's — the boot tree's).
    let (ch_r, ch_w) = k.linux_spawn(&blob, 0)?;
    Ok(Spawned { ch_r, ch_w })
}
