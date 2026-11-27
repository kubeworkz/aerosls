//! e7_go_gate.rs — the live TG-1 acceptance fixture
//! (`docs/AeroSLS-E7-Thread-Group-Gate-v0.1.md` G1.1–G1.8).
//!
//! This is the one Linux child that is a real Go runtime, not a native probe.
//! It is compiled into the sidecar only for the `e7_go_gate` test profile and
//! is spawned from the system POSIX sidecar (the environment that owns the
//! serial UART). The binary prints its own stable markers and performs:
//!
//!   GTG-START     before any thread work (G1.1)
//!   GTG-TIDS      the two locked OS-thread ids (G1.2)
//!   GTG-SHARED=1  the worker observed the main thread's store (G1.3)
//!   GTG-FUTEX     a real FUTEX_WAIT_PRIVATE / FUTEX_WAKE_PRIVATE handshake
//!                 completed (G1.4)
//!   GTG-COPY=     64 KiB copied through the file class (G1.5)
//!   GTG-END       the work half completed; exit_group follows (G1.7)
//!
//! The actual fixture source lives at `tools/go_thread_gate_live/` and the
//! committed ELF is its pinned static build. Placement follows
//! `e7_firstuser.rs`: the image is copied to the budget region's tail just
//! before `SYS_SLS_LINUX_SPAWN` (328), so the bytes are only read while the
//! create call is on the stack. A failure is logged, never fatal.

use aerosls_proto::manifest::{
    build_manifest, crc32, ArgEnv, Bootstrap, Budget, Cpu, Image, Limits,
    Manifest, ManifestCap, FLAG_GO_ARENA, HEADER_LEN, MAX_MANIFEST_CAPS,
};
use aerosls_proto::kabi::Kernel;
use aerosls_vfs::{
    BufferAlloc, Errno, Vfs, O_CREAT, O_RDWR, O_TRUNC,
};

use crate::linux::LINUX_DEFAULT_ENV;

/// The committed static Go fixture (`tools/go_thread_gate_live/`).
static GO_GATE: &[u8] = include_bytes!("../fixtures/go_thread_gate_live.elf");

pub const GO_GATE_NAME: &str = "e7.go.gate.0";
pub const GO_GATE_INPUT: &str = "/tmp/e7-go-gate/in.bin";
pub const GO_GATE_OUTPUT: &str = "/tmp/e7-go-gate/out.bin";

/// The Go binary is 2.3 MiB; reserve 3 MiB at the top of the sidecar's 8 MiB
/// budget. The child's heap span is a separate virtual range (below), so this
/// constant is only the image staging area.
pub const GO_GATE_RESERVE: u64 = 3 * 1024 * 1024;

/// A 64 KiB deterministic source, written in 4 KiB chunks.
const INPUT_BYTES: usize = 64 * 1024;
const INPUT_CHUNK: usize = 4096;

/// Go 1.25.1 reserves TWO 512 MiB anonymous arenas before `main` (measured
/// with `strace -e mmap` on the pinned fixture), plus 64 MiB chunks and the
/// small runtime maps. The span is virtual (PROT_NONE reservations allocate
/// no frames), so it must cover the runtime's address-space probe, not the
/// bytes it touches; 1 GiB left the second arena no room and Go died with
/// `runtime: out of memory: 4194304-byte block`.
const MEM_BUDGET_BYTES: u64 = 2 * 1024 * 1024 * 1024;
const STACK_BUDGET_BYTES: u64 = 256 * 1024;

/// Go 1.25.1 amd64's first arena hint. The kernel moves this child's stack
/// just below it (FLAG_GO_ARENA) so the heap span can start exactly here.
const GO_ARENA_BASE: u64 = 0xC000000000;

pub struct Spawned {
    pub ch_r: u32,
    pub ch_w: u32,
    pub heap_base: u64,
    pub heap_limit: u64,
}

/// Create the deterministic input file in the tenant's ramfs.
pub fn stage<K: Kernel, A: BufferAlloc>(
    vfs: &mut Vfs<K, A>,
    task: u32,
) -> Result<(), Errno> {
    match vfs.mkdir(task, "/tmp/e7-go-gate", 0o755) {
        Ok(()) | Err(Errno::EExist) => {}
        Err(e) => return Err(e),
    }
    let fd = vfs.open(task, GO_GATE_INPUT, O_CREAT | O_RDWR | O_TRUNC, 0o644)?;
    let chunk = [0x5Au8; INPUT_CHUNK];
    let mut written = 0usize;
    while written < INPUT_BYTES {
        vfs.write(task, fd, &chunk)?;
        written += INPUT_CHUNK;
    }
    vfs.close(task, fd)?;
    Ok(())
}

pub fn spawn<K: Kernel>(
    k: &K,
    budget_base: u64,
    budget_len: u64,
) -> Result<Spawned, i32> {
    if budget_len < GO_GATE_RESERVE + 0x1000 {
        return Err(-1);
    }
    let tail = budget_base + budget_len - GO_GATE_RESERVE;
    if GO_GATE.len() as u64 > GO_GATE_RESERVE {
        return Err(-1);
    }
    // Identity-mapped budget region: phys == vaddr for this reservation.
    unsafe {
        core::ptr::copy_nonoverlapping(GO_GATE.as_ptr(), tail as *mut u8, GO_GATE.len());
    }

    let caps: [Option<ManifestCap<'_>>; MAX_MANIFEST_CAPS] =
        [None; MAX_MANIFEST_CAPS];
    let m = Manifest {
        version_major: 1,
        version_minor: 0,
        flags: FLAG_GO_ARENA,
        name: Some(GO_GATE_NAME),
        personality: Some("e7.linux.v1"),
        image: Some(Image {
            offset: 0,
            size: GO_GATE.len() as u32,
            entry: 0, /* ignored for an ELF image; e_entry wins */
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
        argenv: Some(ArgEnv::new(
            &[GO_GATE_NAME, GO_GATE_INPUT, GO_GATE_OUTPUT],
            LINUX_DEFAULT_ENV,
        )),
        signature: None,
    };

    let mut blob = build_manifest(&m);
    blob.extend_from_slice(&tail.to_le_bytes());
    let total = blob.len() as u32;
    blob[16..20].copy_from_slice(&total.to_le_bytes());
    let crc = crc32(&blob[HEADER_LEN..]);
    blob[20..24].copy_from_slice(&crc.to_le_bytes());

    let (ch_r, ch_w) = k.linux_spawn(&blob, 0)?;

    /* cap.c's FLAG_GO_ARENA branch stamps the same base: stack below
     * GO_ARENA_BASE, heap at it, mem_bytes long. `track` hands the sidecar
     * this ledger, so both ends start the mmap cursor at the same limit. */
    let heap_base = GO_ARENA_BASE;
    let heap_limit = heap_base + ((MEM_BUDGET_BYTES + 4095) / 4096) * 4096;

    Ok(Spawned { ch_r, ch_w, heap_base, heap_limit })
}
