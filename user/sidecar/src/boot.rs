//! The POSIX sidecar bootstrap (Phase 2 §6 — deliverable 5).
//!
//! This is the dependency-ordered sequence every sidecar start follows; the
//! ordering is the whole bootstrap contract — no step touches a facility
//! the previous step hasn't stood up:
//!
//! 1. the caller hands us the initial caps (from the Boot Info Block, which
//!    the kernel filled from the manifest — §6.1), the console, and the
//!    request-buffer allocator;
//! 2. `boot()` connects the block cache to the ramdisk channel and
//!    handshakes (`RD_INFO` — the device geometry is fixed here);
//! 3. mounts `/` (aerofs-lite, superblock validated through the cache),
//!    `/dev` (the console + `/dev/null`), and `/tmp` (ramfs);
//! 4. installs the built-in applet registry, spawns init — the boot script
//!    runner (`applets::init`, which executes `/etc/init.rc`, one forked
//!    child per command) — as task 0's program, and opens `/dev/console` on
//!    its stdio (fds 0,1,2 — the design's `stdio: [console, console,
//!    console]`). Init's fork children inherit that stdio.
//!
//! `boot` is generic over `Kernel` + `BufferAlloc`, so the identical code
//! runs against the host fake kernel (`aerosls-kernel-sim`, see
//! `tests/boot_tests.rs`) and the real ABI in the sidecar image
//! (`src/entry.rs`, behind the `target` feature).

use alloc::sync::Arc;

use aerosls_blockcache::{BlockCache, BufferAlloc};
use aerosls_procmgr::{ProcManager, Program};
use aerosls_proto::bootinfo::BootInfo;
use aerosls_proto::kabi::{CAP_CHAN, CAP_MEM, Kernel};
use aerosls_vfs::{CharNode, Errno, Vfs, O_RDWR};

use crate::applets;

/// The initial caps the bootstrap needs, by manifest name (the kernel built
/// the table from the manifest in record order; the BIB reports the same
/// order — §2.2, §6.1).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct BootCaps {
    /// The budget MEM cap (`budget`): the heap region (claimed by the entry
    /// point; v1 heap is reserved, so the host build passes zeros), plus the
    /// cap's table slot (the `BudgetAlloc` grants sub-regions of it).
    pub budget_slot: u32,
    pub budget_base: u64,
    pub budget_len: u64,
    /// The kernel-console channel (`console`), if the manifest declared
    /// one. Recorded here as the ChanDev wiring point: v1 uses the
    /// in-memory console, and the channel plugs into that slot without the
    /// VFS changing.
    pub console_chan: Option<u32>,
    /// The ramdisk channel (`ramdisk`) — the block cache's endpoint.
    pub ramdisk_chan: u32,
}

impl BootCaps {
    /// Resolve the caps from a parsed Boot Info Block, by name and type.
    pub fn from_bib(bib: &BootInfo) -> Result<BootCaps, BootErr> {
        let budget = bib
            .find_cap(CAP_MEM, "budget")
            .ok_or(BootErr::MissingCap("budget"))?;
        let ramdisk = bib
            .find_cap(CAP_CHAN, "ramdisk")
            .ok_or(BootErr::MissingCap("ramdisk"))?;
        let console = bib.find_cap(CAP_CHAN, "console");
        Ok(BootCaps {
            budget_slot: budget.slot,
            budget_base: budget.base,
            budget_len: budget.len,
            console_chan: console.map(|c| c.slot),
            ramdisk_chan: ramdisk.slot,
        })
    }

    /// Host/test constructor (the sim's client table has no budget cap and
    /// no console channel; the ramdisk endpoint is the first wired handle).
    pub fn new(
        budget_slot: u32,
        budget_base: u64,
        budget_len: u64,
        console_chan: Option<u32>,
        ramdisk_chan: u32,
    ) -> BootCaps {
        BootCaps {
            budget_slot,
            budget_base,
            budget_len,
            console_chan,
            ramdisk_chan,
        }
    }
}

/// Bootstrap failures. Everything is a "cannot stand the sidecar up" — the
/// caller (the entry point) logs and, in a healthy kernel, this only
/// happens on a misbuilt manifest/BIB or a dead device.
#[derive(Debug, PartialEq, Eq)]
pub enum BootErr {
    /// The Boot Info Block itself failed to parse.
    Bib(aerosls_proto::bootinfo::BootErr),
    /// A required initial cap is absent (bad manifest/BIB).
    MissingCap(&'static str),
    /// The device handshake failed (driver dead, protocol violation, ...).
    /// The static str names the block-cache error class.
    Handshake(&'static str),
    /// A mount failed.
    Mount(Errno),
    /// The console stdio open failed.
    Console(Errno),
}

/// The result of a successful boot: the proc manager (which owns the VFS,
/// which owns the block cache) plus the console handle, so the caller can
/// feed input / drain output (the ChanDev wiring point).
pub struct Booted<K: Kernel, A: BufferAlloc> {
    pub proc: ProcManager<K, A>,
    pub console: Arc<CharNode>,
}

impl<K: Kernel, A: BufferAlloc> Booted<K, A> {
    /// Run the scheduler until quiet (test convenience). Returns the number
    /// of steps run.
    pub fn run(&mut self, max_steps: usize) -> usize {
        self.proc.run_until_quiet(max_steps)
    }
}

/// Boot the POSIX sidecar: handshake with the ramdisk driver, mount `/`,
/// `/dev`, and `/tmp`, install the built-in applet registry, then spawn
/// init — the boot script runner — with console stdio. See the module docs
/// for the ordering contract.
pub fn boot<K: Kernel, A: BufferAlloc>(
    k: K,
    caps: &BootCaps,
    console: Arc<CharNode>,
    alloc: A,
) -> Result<Booted<K, A>, BootErr> {
    // 2. Device attach + handshake (RD_INFO). The device geometry is fixed
    //    here; the driver thread must be serving.
    let cache = BlockCache::connect(k, caps.ramdisk_chan, alloc)
        .map_err(|e| BootErr::Handshake(handshake_class(&e)))?;

    // 3. Mounts, in order (path resolution is longest-prefix; /dev and /tmp
    //    shadow the root mount).
    let mut vfs = Vfs::new();
    vfs.mount_aerofs("/", cache).map_err(BootErr::Mount)?;
    vfs.mount_devfs("/dev", console.clone())
        .map_err(BootErr::Mount)?;
    vfs.mount_ramfs("/tmp").map_err(BootErr::Mount)?;

    // 4. System applets + init (the boot script runner), with console
    //    stdio on fds 0,1,2. Init's fork children inherit the stdio.
    let mut proc = ProcManager::new(vfs);
    applets::register_default_applets(&mut proc);
    proc.spawn_init(Program::new("init", applets::init));
    for _ in 0..3 {
        proc.vfs
            .open(0, "/dev/console", O_RDWR, 0)
            .map_err(BootErr::Console)?;
    }

    Ok(Booted { proc, console })
}

fn handshake_class(e: &aerosls_blockcache::Error) -> &'static str {
    match e {
        aerosls_blockcache::Error::Stale { .. } => "stale",
        aerosls_blockcache::Error::Status(_) => "status",
        aerosls_blockcache::Error::Kernel(_) => "kernel",
        aerosls_blockcache::Error::Protocol => "protocol",
    }
}
