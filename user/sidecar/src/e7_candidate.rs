//! e7_candidate.rs — E7 increment 7 (AeroSLS-Linux-ABI-Shim-Design-v0.1.md
//! §5.1/§5.2): the SAME kernel create driven over a REAL ELF64 image, so a
//! boot measures exactly how far an unmodified image format gets instead of
//! an argument about it.
//!
//! The payload is `fixtures/e7_candidate.elf` — the probe source
//! (e7_linux_probe.S) linked as a static, non-PIE x86-64 ELF with the
//! committed build script's byte-exact output (fixtures/build_candidate.sh).
//! That makes the two children a CONTROLLED PAIR: identical machine code,
//! identical syscall sequence, only the IMAGE FORMAT differs —
//!
//!   * the .flat probe (e7_probe.rs): the flat mapper copies the blob at
//!     USER_PROC_CODE_BASE and enters at manifest `image_entry`;
//!   * this child: the magic dispatches to the ONE ELF mapper (§5.1), the
//!     segments land at their LINKED p_vaddr (0x3ff000 headers, 0x400000
//!     R+E text), the entry is e_entry, and the §5.2 stack image reports
//!     the image's own AT_PHDR/AT_PHENT/AT_PHNUM — the values a libc
//!     start-up reads before it executes one line of its own code.
//!
//! The serial log is the measurement: every `[E7]` line this child earns
//! (through the same linux::service answers) is a seam that held for a real
//! ELF, and the first MISSING line names where it stopped — §10.12 records
//! the run. Like the probe, a failure here is logged, never fatal: the
//! measurement is a measurement, not a dependency of this sidecar's boot.
//!
//! Payload placement is the probe's, deliberately cheap: the ELF is
//! embedded in THIS binary (include_bytes) and planted at the budget
//! region's last 64 KiB just before the spawn. The budget cap is
//! identity-mapped RW in our address space, and the kernel's create COPIES
//! the image synchronously while `linux_spawn` is on the stack — the bytes
//! only have to be valid until that syscall returns. The probe planted the
//! same address first; its copy is already in its own frames by the time
//! this file overwrites the tail.

use aerosls_proto::manifest::{
    build_manifest, crc32, ArgEnv, Bootstrap, Budget, Cpu, Image, Limits,
    Manifest, ManifestCap, HEADER_LEN, MAX_MANIFEST_CAPS,
};
use crate::linux::LINUX_DEFAULT_ENV;

/// The census candidate payload: `fixtures/e7_linux_probe.S` linked as an
/// ET_EXEC ELF64 (see build_candidate.sh for the exact, committed build
/// line). §5.1's gate: ET_EXEC only — an ET_DYN image is refused by name,
/// never loaded with a bias.
static CANDIDATE: &[u8] = include_bytes!("../fixtures/e7_candidate.elf");

/// The child's name — registered at create (the manifest's NAME record),
/// which is how `linux::service`'s exit_group reap resolves it, exactly
/// like the probe's.
pub const CAND_NAME: &str = "e7.linux.cand.0";

/// Reserve at the TOP of the budget region (page-aligned: budget_base is
/// 4 KiB-aligned from the manifest's MEM cap, budget_len is a MiB count).
pub const CAND_RESERVE: u64 = 64 * 1024;

/// The child's layout, mirrored from the kernel's create-side arithmetic
/// (kernel/cap.c steps 5-6) — §5.4's budget span as three numbers both
/// sides derive from one record, the twin discipline of e7_probe.rs (and
/// guard-clause-pinned there). IMPORTANT: cap.c's stack_base uses
/// `m.image_size` — the MANIFEST's number — for BOTH image paths, so this
/// twin stays one truth whether the bytes are flat or ELF. For this
/// fixture size (one 4 KiB text page + one header page) that is the same
/// 2 image pages the probe has, so both children's spans coincide.
const IMAGE_VBASE: u64 = 0x4000_0000_0000; /* USER_PROC_CODE_BASE */
const HEAP_GUARD_BYTES: u64 = 4096;        /* cap.c: one guard page */
const MEM_BUDGET_BYTES: u64 = 64 * 1024;   /* the BUDGET record's mem_bytes */
/// 16 KiB — above increment 7's Linux floor (8 KiB: the BIB's stack-top
/// page plus at least one page of §5.2 stack image below it).
const STACK_BUDGET_BYTES: u64 = 16 * 1024;

/// The caller's channel endpoints and the child's §5.4 span, as
/// `linux::track` wants them (the ledger is born with these).
pub struct Spawned {
    pub ch_r: u32,
    pub ch_w: u32,
    pub heap_base: u64,
    pub heap_limit: u64,
}

/// Plant the ELF at the budget tail and spawn it through the real kernel
/// path. Failures are returned, never fatal to this sidecar's boot.
pub fn spawn<K: aerosls_proto::kabi::Kernel>(
    k: &K,
    budget_base: u64,
    budget_len: u64,
) -> Result<Spawned, i32> {
    if budget_len < CAND_RESERVE + 0x1000 {
        return Err(-1);
    }
    let tail = budget_base + budget_len - CAND_RESERVE;
    if CANDIDATE.len() as u64 > CAND_RESERVE {
        return Err(-1);
    }
    // Identity-mapped budget region (the probe's note): the kernel reads
    // these bytes as PHYSICAL at image_kaddr — phys == vaddr holds for
    // exactly this region.
    unsafe {
        core::ptr::copy_nonoverlapping(CANDIDATE.as_ptr(), tail as *mut u8, CANDIDATE.len());
    }

    // §3.5 as construction: zero capability records — the child's table
    // will hold only the messenger channel pair the create inserts.
    let caps: [Option<ManifestCap<'_>>; MAX_MANIFEST_CAPS] =
        [None; MAX_MANIFEST_CAPS];
    let m = Manifest {
        version_major: 1,
        version_minor: 0,
        flags: 0,
        name: Some(CAND_NAME),
        personality: Some("e7.linux.v1"), /* informational (kernel/cap.c
                                           * treats the record as a tag) */
        image: Some(Image {
            offset: 0,                    /* image data starts at the tail */
            size: CANDIDATE.len() as u32,
            /* The manifest's entry is IGNORED for an ELF image: the magic
             * dispatched to the ELF mapper, whose entry is e_entry (§5.1 —
             * image format is an image property, never a personality
             * inference). The field must still be VALID (0 < size) because
             * manifest validation runs BEFORE the magic dispatch. */
            entry: 0,
        }),
        budget: Some(Budget {
            mem_bytes: MEM_BUDGET_BYTES,  /* stamped into the brk span */
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
        // §5.2's environment-crossing record: both halves cross here —
        // argv is v1's { name } spelled explicitly (byte-identical to the
        // create's fallback), and envp is this environment's default
        // (linux.rs) instead of the empty environment the create could
        // only hand before the record landed. The pair crosses the SAME
        // record, so image format stays its only difference.
        argenv: Some(ArgEnv::new(&[CAND_NAME], LINUX_DEFAULT_ENV)),
        signature: None,
    };

    // Packed blob + image_kaddr footer, with total_len and the body CRC
    // patched to cover records + footer — the probe's pattern exactly (one
    // parser in the kernel).
    let mut blob = build_manifest(&m);
    blob.extend_from_slice(&tail.to_le_bytes());
    let total = blob.len() as u32;
    blob[16..20].copy_from_slice(&total.to_le_bytes());
    let crc = crc32(&blob[HEADER_LEN..]);
    blob[20..24].copy_from_slice(&crc.to_le_bytes());

    // target_partition 0 = inherit (this sidecar's — the boot tree's).
    let (ch_r, ch_w) = k.linux_spawn(&blob, 0)?;

    // The §5.4 span, derived from the SAME manifest numbers the create
    // stamps from — the mirror of cap.c's steps 5-6, constants above.
    let n_img_pages = (CANDIDATE.len() as u64 + 4095) / 4096;
    let stack_base = IMAGE_VBASE + n_img_pages * 4096 + HEAP_GUARD_BYTES;
    let stk_pages = (STACK_BUDGET_BYTES + 4095) / 4096;
    let heap_base = stack_base + stk_pages * 4096;
    let heap_limit = heap_base + ((MEM_BUDGET_BYTES + 4095) / 4096) * 4096;

    Ok(Spawned { ch_r, ch_w, heap_base, heap_limit })
}
