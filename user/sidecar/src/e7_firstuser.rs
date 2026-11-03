//! e7_firstuser.rs — the designated first user, finally on the machine
//! (AeroSLS-Linux-ABI-Shim-Design-v0.1.md §10.12's named boundary: "`aerobackup`'s
//! musl binary has still never executed under the shim").
//!
//! The payload is `fixtures/aerobackup.musl.elf` — `backup/aerobackup`'s own
//! release build for `x86_64-unknown-linux-musl`: a static, non-PIE ET_EXEC
//! ELF (Type EXEC, entry 0x4068f6, PT_LOADs linked low at 0x400000 exactly
//! like the candidate fixture), the tree's real backup operation designated in
//! `tools/linux_abi_candidate.conf`. Nothing about the binary is adapted for
//! the shim: it is the candidate's census target as built.
//!
//! What this run MEASURES, and what it cannot: argv keeps v1's SHAPE —
//! §5.2's environment-crossing record (`SIDECAR_TAG_ARGENV`) is landed, and
//! this child crosses `argv = { USER_NAME }` through it, so the binary is
//! invoked as `e7.linux.user.0` with NO flags: its `--storage/--retain`
//! operation cannot start here (a flag string invented at this call site
//! would be a new command line, not this census's measurement). The
//! record's envp half carries this environment's default environment
//! (`PATH=/bin`, `HOME=/root`, linux.rs), so a real libc's `getenv` is
//! answered off the §5.2 image instead of an empty environment — the
//! second half of "argv/envp are the environment's to define" (§5.2),
//! which the create previously had nothing to cross them on. The question
//! this child answers is therefore the honest first
//! one: does an unmodified real libc binary get through crt, TLS (§5.3), the
//! §5.2 stack image and its auxv, and where does it stop — a usage write and
//! a clean `exit_group`, the first `-ENOSYS` name (`openat`? `mmap`?
//! `clock_gettime`, which §5.5's conf already predicts musl needs and the
//! no-vDSO decision makes un-hidable), or a fault. The serial log is the
//! measurement: every `[E7] linux: num=` line this child earns names a
//! syscall that crossed the seam, and the first MISSING outcome names where
//! it stopped.
//!
//! Placement is the probe's and the candidate's, deliberately: the ELF is
//! embedded in THIS binary (include_bytes) and planted at the budget
//! region's last 1 MiB just before the spawn — bigger than the pair's 64 KiB
//! because this payload is 620,200 bytes, still only a tail of the POSIX
//! sidecar's 4 MiB budget. The kernel's create COPIES the image synchronously
//! while `linux_spawn` is on the stack (the same contract the pair relies
//! on): the bytes only have to be valid until that syscall returns, and this
//! spawn runs at boot, before the event loop's bump heap has grown anywhere
//! near the tail. A failure here is logged, never fatal — a measurement, not
//! a dependency of this sidecar's boot.

use aerosls_proto::manifest::{
    build_manifest, crc32, ArgEnv, Bootstrap, Budget, Cpu, FLAG_GATE, Image,
    Limits, Manifest, ManifestCap, HEADER_LEN, MAX_MANIFEST_CAPS,
};
use aerosls_proto::kabi::Kernel;
use aerosls_vfs::{
    BufferAlloc, Errno, Vfs, O_CREAT, O_RDWR, O_TRUNC, SEEK_SET,
};
use crate::linux::LINUX_DEFAULT_ENV;

/// The designated first user: `backup/aerobackup`'s static musl release
/// binary (see the module doc). §5.1's gate applies unchanged: ET_EXEC only —
/// and this build already is one, which is why the fixture is committed as
/// built rather than linked by a script like the probe pair.
static FIRSTUSER: &[u8] = include_bytes!("../fixtures/aerobackup.musl.elf");

/// The child's name — registered at create (the manifest's NAME record),
/// which is what `linux::service`'s exit_group reap and every `[E7] linux:`
/// line resolve against. It is ALSO the child's entire argv — crossed as
/// argv through §5.2's environment-crossing record, byte-identical to the
/// create's absent-record fallback.
pub const USER_NAME: &str = "e7.linux.user.0";

/// Reserve at the TOP of the budget region, sized to the payload: 1 MiB
/// covers the 620,200-byte ELF with slack, against the POSIX sidecar's 4 MiB
/// budget — the tail the pair does not occupy (they plant the same top and
/// their copies are already in their own frames by the time this file runs).
pub const USER_RESERVE: u64 = 1024 * 1024;

/// Increment 19's gate profile: the real command line, byte for byte the
/// one `tools/linux_abi_candidate.conf` designates. Nine entries including
/// argv[0]; MAX_ARGV is 12 so this fits with headroom.
/// The gate task's KERNEL name, distinct from the system POSIX's first user
/// of the same argv[0]. The exit path reaps by name; two live tasks sharing
/// `e7.linux.user.0` let one sidecar's reap find the other's child.
/// E7 increment 20: one constant for both halves of the record — the spawn
/// here and the registration the kernel stores (user/proto's env_proto). A
/// drift between them is a record whose Linux slot the stamp can never fill.
pub const GATE_TASK_NAME: &str = aerosls_proto::env_proto::LINUX_TASK_NAME;
pub const GATE_STORAGE: &str = "/tmp/e7-census/sls_storage.img";
pub const GATE_BACKUP_DIR: &str = "/tmp/e7-census/backups";
pub const GATE_TIER_DIR: &str = "/tmp/e7-census/backups/hourly";
pub const GATE_ARGV: [&str; 9] = [
    USER_NAME,
    "--storage",
    GATE_STORAGE,
    "--backup-dir",
    GATE_BACKUP_DIR,
    "--kind",
    "hourly",
    "--retain",
    "4",
];

/// The staged source: 512 KiB, a 4 KiB non-zero marker at offset 0 and
/// another at 256 KiB, zeros elsewhere. copy_sparse's 64 KiB chunk turns
/// the six zero chunks into holes and counts exactly 6.
pub const GATE_IMAGE_BYTES: u64 = 512 * 1024;
const GATE_MARKER_BYTES: usize = 4096;
const GATE_MARKER_OFFSET: u64 = 256 * 1024;
const GATE_MARKER: u8 = 0xA5;

/// Six old backups, one per day before the pinned epoch. The binary's
/// retention is newest-by-mtime; these names also make the tie-break
/// deterministic when the ramfs mtimes are equal.
pub const GATE_OLD_NAMES: [&str; 6] = [
    "sls_storage-20260101-000000.img",
    "sls_storage-20260102-000000.img",
    "sls_storage-20260103-000000.img",
    "sls_storage-20260104-000000.img",
    "sls_storage-20260105-000000.img",
    "sls_storage-20260106-000000.img",
];

/// The child's layout, mirrored from the kernel's create-side arithmetic
/// (kernel/cap.c steps 5-6) — the twin discipline of e7_probe.rs and
/// e7_candidate.rs. cap.c's stack_base uses `m.image_size` for BOTH image
/// paths, so this twin is one truth regardless of where the ELF's segments
/// land: the linked text at 0x400000 and the stack/heap spans above
/// USER_PROC_CODE_BASE are different regions of one address space.
const IMAGE_VBASE: u64 = 0x4000_0000_0000; /* USER_PROC_CODE_BASE */
const HEAP_GUARD_BYTES: u64 = 4096;        /* cap.c: one guard page */
/// A real libc child's brk span, 4x the probe pair's: musl's startup and a
/// Rust binary's early allocations are the first honest callers of
/// increment 6's brk, and a 64 KiB denial echo would end the measurement
/// before it says anything interesting about the binary.
const MEM_BUDGET_BYTES: u64 = 256 * 1024;
/// 64 KiB — the POSIX sidecar's own stack class, well above increment 7's
/// 8 KiB floor: musl crt plus a Rust entry frame is the first stack whose
/// depth nobody measured (§11's Q2 — this child's survival is data for it).
const STACK_BUDGET_BYTES: u64 = 64 * 1024;

/// The caller's channel endpoints and the child's §5.4 span, as
/// `linux::track` wants them (the ledger is born with these).
pub struct Spawned {
    pub ch_r: u32,
    pub ch_w: u32,
    pub heap_base: u64,
    pub heap_limit: u64,
}

/// Plant the first user at the budget tail and spawn it through the real
/// kernel path. Failures are returned, never fatal to this sidecar's boot.
pub fn spawn<K: Kernel>(
    k: &K,
    budget_base: u64,
    budget_len: u64,
) -> Result<Spawned, i32> {
    spawn_with(k, budget_base, budget_len, USER_NAME, &[USER_NAME], false)
}

/// The gate profile's spawn: the SAME fixture and placement, with the
/// designated command line instead of v1's `{ name }`. Separate from
/// `spawn` so the system POSIX's measurement keeps its old shape.
pub fn spawn_gate<K: Kernel>(
    k: &K,
    budget_base: u64,
    budget_len: u64,
) -> Result<Spawned, i32> {
    spawn_with(k, budget_base, budget_len, GATE_TASK_NAME, &GATE_ARGV, true)
}

fn spawn_with<K: Kernel>(
    k: &K,
    budget_base: u64,
    budget_len: u64,
    task_name: &str,
    argv: &[&str],
    gate: bool,
) -> Result<Spawned, i32> {
    if budget_len < USER_RESERVE + 0x1000 {
        return Err(-1);
    }
    let tail = budget_base + budget_len - USER_RESERVE;
    if FIRSTUSER.len() as u64 > USER_RESERVE {
        return Err(-1);
    }
    // Identity-mapped budget region (the probe's note): the kernel reads
    // these bytes as PHYSICAL at image_kaddr — phys == vaddr holds for
    // exactly this region.
    unsafe {
        core::ptr::copy_nonoverlapping(FIRSTUSER.as_ptr(), tail as *mut u8, FIRSTUSER.len());
    }

    // §3.5 as construction: zero capability records — the child's table
    // will hold only the messenger channel pair the create inserts.
    let caps: [Option<ManifestCap<'_>>; MAX_MANIFEST_CAPS] =
        [None; MAX_MANIFEST_CAPS];
    let m = Manifest {
        version_major: 1,
        version_minor: 0,
        /* Increment 21: the gate profile's bit crosses on the CHILD's own
         * manifest now, not just the environment sidecar's. The kernel's
         * create stamps proc_table's gate_profile from this header (cap.c);
         * the same bit is what the BIB then carries, so the sidecar's profile
         * and the trap path's stop are one selection, not two. The system
         * firstuser (spawn_with's `gate == false`) keeps a zero header. */
        flags: if gate { FLAG_GATE } else { 0 },
        name: Some(task_name),
        personality: Some("e7.linux.v1"), /* informational (kernel/cap.c
                                           * treats the record as a tag) */
        image: Some(Image {
            offset: 0,                    /* image data starts at the tail */
            size: FIRSTUSER.len() as u32,
            /* The manifest's entry is IGNORED for an ELF image: the magic
             * dispatched to the ELF mapper, whose entry is e_entry (§5.1).
             * The field must still be VALID (0 < size) because manifest
             * validation runs BEFORE the magic dispatch. */
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
        // argv is the CALLER's line (v1's { name } for the system spawn,
        // the designated command for the gate profile), and envp is this
        // environment's default (linux.rs) instead of the empty
        // environment the create could only hand before the record landed.
        argenv: Some(ArgEnv::new(argv, LINUX_DEFAULT_ENV)),
        signature: None,
    };

    // Packed blob + image_kaddr footer, with total_len and the body CRC
    // patched to cover records + footer — one parser in the kernel, the
    // pair's pattern exactly.
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
    let n_img_pages = (FIRSTUSER.len() as u64 + 4095) / 4096;
    let stack_base = IMAGE_VBASE + n_img_pages * 4096 + HEAP_GUARD_BYTES;
    let stk_pages = (STACK_BUDGET_BYTES + 4095) / 4096;
    let heap_base = stack_base + stk_pages * 4096;
    let heap_limit = heap_base + ((MEM_BUDGET_BYTES + 4095) / 4096) * 4096;

    Ok(Spawned { ch_r, ch_w, heap_base, heap_limit })
}

/// Stage the gate's source image and the six old backups in this tenant's
/// VFS before the first user is allowed to start. `/tmp` is the tenant's
/// ramfs; the paths are absolute, so task 0's cwd is irrelevant. The
/// image is created at its full length first, then the two markers are
/// written, which is why the copy sees four zero chunks on each side of
/// the 256 KiB marker.
pub fn stage_gate<K: Kernel, A: BufferAlloc>(
    vfs: &mut Vfs<K, A>,
    task: u32,
) -> Result<(), Errno> {
    mkdir_if_missing(vfs, task, "/tmp/e7-census")?;
    mkdir_if_missing(vfs, task, GATE_BACKUP_DIR)?;
    mkdir_if_missing(vfs, task, GATE_TIER_DIR)?;

    let fd = vfs.open(task, GATE_STORAGE, O_CREAT | O_RDWR | O_TRUNC, 0o644)?;
    vfs.truncate_at(task, fd, GATE_IMAGE_BYTES)?;
    let marker = [GATE_MARKER; GATE_MARKER_BYTES];
    vfs.write(task, fd, &marker)?;
    vfs.lseek(task, fd, GATE_MARKER_OFFSET as i64, SEEK_SET)?;
    vfs.write(task, fd, &marker)?;
    vfs.close(task, fd)?;

    for name in GATE_OLD_NAMES {
        let path = alloc::format!("{}/{}", GATE_TIER_DIR, name);
        let fd = vfs.open(task, &path, O_CREAT | O_RDWR | O_TRUNC, 0o644)?;
        vfs.write(task, fd, name.as_bytes())?;
        vfs.write(task, fd, b"
")?;
        vfs.close(task, fd)?;
    }
    Ok(())
}

fn mkdir_if_missing<K: Kernel, A: BufferAlloc>(
    vfs: &mut Vfs<K, A>,
    task: u32,
    path: &str,
) -> Result<(), Errno> {
    match vfs.mkdir(task, path, 0o755) {
        Ok(()) | Err(Errno::EExist) => Ok(()),
        Err(e) => Err(e),
    }
}
