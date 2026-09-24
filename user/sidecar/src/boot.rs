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
//!    console]`). Init's fork children inherit that stdio. When the BIB said
//!    this sidecar's console is an ENVIRONMENT console (v3's identity words
//!    plus the `ENV_CONSOLE` flag), it then announces its own index and pid
//!    on that console — E6's in-band identity, and the first bytes of the
//!    environment's own stream.
//! 5. if the manifest declared a network channel, stores the channel handle
//!    for socket I/O (the net client is created lazily on first use).
//!
//! `boot` is generic over `Kernel` + `BufferAlloc`, so the identical code
//! runs against the host fake kernel (`aerosls-kernel-sim`, see
//! `tests/boot_tests.rs`) and the real ABI in the sidecar image
//! (`src/entry.rs`, behind the `target` feature).

use alloc::sync::Arc;

use aerosls_blockcache::{BlockCache, BufferAlloc};
use aerosls_procmgr::{ProcManager, Program};
use aerosls_proto::bootinfo::BootInfo;
use aerosls_proto::kabi::{CAP_CHAN, CAP_CHAN_W, CAP_MEM, Kernel};
use aerosls_proto::kwrap::{AWrap, KWrap};
use aerosls_vfs::{CharNode, Errno, Vfs, O_RDWR};

use crate::applets;
use crate::net_client::NetClient;

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
    /// `None` when no ramdisk driver is wired (console-only mode).
    /// The ramdisk channel's CHAN_W (send) and CHAN_R (receive) endpoints.
    /// The kernel enforces CAP_TYPE_CHAN_W for send and CAP_TYPE_CHAN_R for recv.
    pub ramdisk_chan_w: Option<u32>,
    pub ramdisk_chan_r: Option<u32>,
    /// The network driver channel (`network`), if the manifest declared
    /// one. The POSIX sidecar connects a `NetClient` on this endpoint
    /// for socket I/O.  CHAN_W (send) and CHAN_R (recv) endpoints.
    pub net_chan_w: Option<u32>,
    pub net_chan_r: Option<u32>,
    /// POSIX-Environments E6: this sidecar's OWN identity `(index, pid)` — the
    /// pair to announce on its own console — or `None` when there is none to
    /// announce: a pre-v3 BIB, the host constructor below, or a sidecar whose
    /// console is not an environment console (only a console the attach surface
    /// serves has an identity to state; see bootinfo's `ENV_CONSOLE` flag).
    /// `boot()` announces it on that console before init runs: an environment's
    /// attached stream must carry the environment's own account of which
    /// environment it is, because nothing else in the stream can — the cap
    /// table names capabilities ("console", "budget"), never the sidecar.
    pub identity: Option<(u32, u32)>,
}

/// The marker the identity announcement starts with. One constant, so the
/// sidecar that writes it and the E6 attach guard that asserts it
/// (tests/env_console_attach_check.sh, which greps this literal) cannot drift.
pub const IDENTITY_MARKER: &str = "[env-id]";

impl BootCaps {
    /// Resolve the caps from a parsed Boot Info Block, by name and type.
    pub fn from_bib(bib: &BootInfo) -> Result<BootCaps, BootErr> {
        let budget = bib
            .find_cap(CAP_MEM, "budget")
            .ok_or(BootErr::MissingCap("budget"))?;
        let ramdisk_r = bib.find_cap(CAP_CHAN, "ramdisk");
        let ramdisk_w = bib.find_cap(CAP_CHAN_W, "ramdisk");
        let console = bib.find_cap(CAP_CHAN, "console");
        let net_r = bib.find_cap(CAP_CHAN, "network");
        let net_w = bib.find_cap(CAP_CHAN_W, "network");
        Ok(BootCaps {
            budget_slot: budget.slot,
            budget_base: budget.base,
            budget_len: budget.len,
            console_chan: console.map(|c| c.slot),
            ramdisk_chan_w: ramdisk_w.map(|r| r.slot),
            ramdisk_chan_r: ramdisk_r.map(|r| r.slot),
            net_chan_w: net_w.map(|n| n.slot),
            net_chan_r: net_r.map(|n| n.slot),
            identity: bib.identity(),
        })
    }

    /// Give the sidecar an identity the way a BIB that carries
    /// `BOOT_INFO_FLAG_ENV_CONSOLE` does, for a caller (host test) that stands
    /// the sidecar up without one. `boot()` announces it.
    pub fn with_identity(mut self, index: u32, pid: u32) -> BootCaps {
        self.identity = Some((index, pid));
        self
    }

    /// Host/test constructor (the sim's client table has no budget cap and
    /// no console channel; the ramdisk endpoint is the first wired handle).
    pub fn new(
        budget_slot: u32,
        budget_base: u64,
        budget_len: u64,
        console_chan: Option<u32>,
        ramdisk_chan_w: Option<u32>,
        ramdisk_chan_r: Option<u32>,
        net_chan_w: Option<u32>,
        net_chan_r: Option<u32>,
    ) -> BootCaps {
        BootCaps {
            budget_slot,
            budget_base,
            budget_len,
            console_chan,
            ramdisk_chan_w,
            ramdisk_chan_r,
            net_chan_w,
            net_chan_r,
            // The sim's client table is built by hand, not from a BIB, so the
            // sidecar boots with no identity and stays quiet — which is also
            // what keeps every pre-E6 host test's console output unchanged.
            identity: None,
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
/// feed input / drain output (the ChanDev wiring point). If the manifest
/// declared a network channel, `net` holds the protocol client.
pub struct Booted<K: Kernel, A: BufferAlloc> {
    pub proc: ProcManager<K, A>,
    pub console: Arc<CharNode>,
    /// The network driver client (None if the manifest has no network
    /// channel). Applet-level socket I/O goes through this.
    pub net: Option<NetClient<K, A>>,
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
    // Wrap K and A in Arc so both BlockCache and NetClient can share
    // the same instance.
    let k_arc = Arc::new(k);
    let alloc_arc = Arc::new(aerosls_proto::Mutex::new(alloc));
    let k_wrap = KWrap(k_arc.clone());
    let alloc_wrap = AWrap(alloc_arc.clone());

    // 2. Device attach + handshake (RD_INFO) — only when a ramdisk driver
    //    is wired. Without one the sidecar boots in console-only mode.
    let mut vfs = Vfs::new();
    if let (Some(ramdisk_w), Some(ramdisk_r)) = (caps.ramdisk_chan_w, caps.ramdisk_chan_r) {
        let cache = BlockCache::connect(k_wrap, ramdisk_w, ramdisk_r, alloc_wrap)
            .map_err(|e| BootErr::Handshake(handshake_class(&e)))?;
        // 3. Mount the root aerofs image, formatting it first if the store is
        //    empty (a tenant environment's ramdisk starts as blank
        //    k_alloc_region memory — E3). The pre-seeded system rootfs has a
        //    valid superblock, so this is a plain mount there. The returned
        //    "was formatted" flag is unused for now (E6's per-env terminals
        //    will surface it); the E3 boot check proves isolation from the
        //    kernel's per-name sidecar registrations at distinct storage
        //    addresses instead.
        let _formatted = vfs.mount_aerofs_or_format("/", cache).map_err(BootErr::Mount)?;
    }
    vfs.mount_devfs("/dev", console.clone())
        .map_err(BootErr::Mount)?;
    vfs.mount_ramfs("/tmp").map_err(BootErr::Mount)?;

    // 4. System applets, with console stdio on fds 0,1,2.
    let mut proc = ProcManager::new(vfs);
    applets::register_default_applets(&mut proc);
    if caps.ramdisk_chan_w.is_some() {
        // Full mode: init reads /etc/init.rc and spawns children.
        proc.spawn_init(Program::new("init", applets::init));
    } else {
        // Console-only mode: skip the boot script runner; spawn the
        // interactive shell directly as PID 0 (the shell becomes init).
        proc.spawn_init(Program::new("sh", applets::sh));
    }
    for _ in 0..3 {
        proc.vfs
            .open(0, "/dev/console", O_RDWR, 0)
            .map_err(BootErr::Console)?;
    }

    // 4.5 The environment's own account of itself — E6's in-band identity.
    //
    // Written to the in-memory console (never to the kernel log: a diagnostic
    // is not the same claim) BEFORE the scheduler runs, so it is the first
    // thing init's stdio produces and an attach that has read any of this
    // environment's output has read this line. It is the one assertion an
    // environment can make that its console cannot be lied about — the kernel
    // writes this environment's bytes into this environment's console, and a
    // reader who is told a different index is looking at the wrong console.
    // Only an environment console has an identity to state: `caps.identity` is
    // `None` for a sidecar whose console is the kernel transcript (`aerosls.init.0`,
    // `drv.ramdisk.0`, and the system POSIX sidecar `aerosls.posix.0`). "Index 0"
    // is a real environment, so a non-announcing sidecar must be silent rather
    // than indistinguishable from environment 0.
    if let Some((index, pid)) = caps.identity {
        let line = alloc::format!("{} index={} pid={}\n", IDENTITY_MARKER, index, pid);
        console.write(line.as_bytes()).map_err(BootErr::Console)?;
    }

    // 5. Network client (optional): connect and handshake with the
    //    network driver when a net_chan was declared in the manifest.
    let net: Option<NetClient<K, A>> = match (caps.net_chan_w, caps.net_chan_r) {
        (Some(cw), Some(cr)) => {
            let nc = NetClient::new(KWrap(k_arc), cw, cr, AWrap(alloc_arc));
            // NOTE: NET_INFO handshake moved to entry.rs (binary crate)
            // to survive LTO — library-crate serial_trace calls are stripped.
            Some(nc)
        }
        _ => None,
    };



    Ok(Booted {
        proc,
        console,
        net,
    })
}

fn handshake_class(e: &aerosls_blockcache::Error) -> &'static str {
    match e {
        aerosls_blockcache::Error::Stale { .. } => "stale",
        aerosls_blockcache::Error::Status(_) => "status",
        aerosls_blockcache::Error::Kernel(_) => "kernel",
        aerosls_blockcache::Error::Protocol => "protocol",
    }
}
