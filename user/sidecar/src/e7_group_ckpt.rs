//! e7_group_ckpt.rs — TG-2d's live two-thread checkpoint workload.
//!
//! Built only with the `e7_group_ckpt` feature. The gate environment's POSIX
//! sidecar stages a deterministic 64 KiB input and spawns the freestanding
//! fixture `fixtures/e7_group_ckpt.elf` with FLAG_GATE. That fixture creates a
//! raw sibling, both wait on one futex word, and the kernel prints its
//! `[E7-GATE] group barrier:` line once every member is parked. The live check
//! checkpoints on that line, reboots, and reads the post-restore wake/copy
//! markers back from the same two threads.
//!
//! The task name is the record's Linux leader slot (`e7.gate.user.0`), not a
//! second invented name: the environment manager already registered that slot
//! before the gate tenant existed.

use aerosls_proto::manifest::{
    build_manifest, crc32, ArgEnv, Bootstrap, Budget, Cpu, Image, Limits,
    Manifest, ManifestCap, FLAG_GATE, HEADER_LEN, MAX_MANIFEST_CAPS,
};
use aerosls_proto::kabi::Kernel;
use aerosls_vfs::{
    BufferAlloc, Errno, Vfs, O_CREAT, O_RDWR, O_TRUNC,
};

use crate::linux::LINUX_DEFAULT_ENV;

static GROUP_CKPT: &[u8] = include_bytes!("../fixtures/e7_group_ckpt.elf");

pub const GROUP_TASK_NAME: &str = aerosls_proto::env_proto::LINUX_TASK_NAME;
pub const GROUP_INPUT: &str = "/tmp/e7-group-ckpt/in.bin";
pub const GROUP_OUTPUT: &str = "/tmp/e7-group-ckpt/out.bin";

/// The 10 KiB fixture needs only a tail slot; 256 KiB leaves slack without
/// moving the sidecar's working heap.
pub const GROUP_RESERVE: u64 = 256 * 1024;
const INPUT_BYTES: usize = 64 * 1024;
const INPUT_CHUNK: usize = 4096;
const INPUT_BYTE: u8 = 0x5A;

const IMAGE_VBASE: u64 = 0x4000_0000_0000;
const HEAP_GUARD_BYTES: u64 = 4096;
const MEM_BUDGET_BYTES: u64 = 64 * 1024 * 1024;
const STACK_BUDGET_BYTES: u64 = 128 * 1024;

pub struct Spawned {
    pub ch_r: u32,
    pub ch_w: u32,
    pub heap_base: u64,
    pub heap_limit: u64,
}

pub fn stage<K: Kernel, A: BufferAlloc>(
    vfs: &mut Vfs<K, A>,
    task: u32,
) -> Result<(), Errno> {
    match vfs.mkdir(task, "/tmp/e7-group-ckpt", 0o755) {
        Ok(()) | Err(Errno::EExist) => {}
        Err(e) => return Err(e),
    }
    let fd = vfs.open(task, GROUP_INPUT, O_CREAT | O_RDWR | O_TRUNC, 0o644)?;
    let chunk = [INPUT_BYTE; INPUT_CHUNK];
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
    if budget_len < GROUP_RESERVE + 0x1000 {
        return Err(-1);
    }
    let tail = budget_base + budget_len - GROUP_RESERVE;
    if GROUP_CKPT.len() as u64 > GROUP_RESERVE {
        return Err(-1);
    }
    unsafe {
        core::ptr::copy_nonoverlapping(
            GROUP_CKPT.as_ptr(),
            tail as *mut u8,
            GROUP_CKPT.len(),
        );
    }

    let caps: [Option<ManifestCap<'_>>; MAX_MANIFEST_CAPS] =
        [None; MAX_MANIFEST_CAPS];
    let m = Manifest {
        version_major: 1,
        version_minor: 0,
        flags: FLAG_GATE,
        name: Some(GROUP_TASK_NAME),
        personality: Some("e7.linux.v1"),
        image: Some(Image {
            offset: 0,
            size: GROUP_CKPT.len() as u32,
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
        argenv: Some(ArgEnv::new(
            &[GROUP_TASK_NAME, GROUP_INPUT, GROUP_OUTPUT],
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

    let n_img_pages = (GROUP_CKPT.len() as u64 + 4095) / 4096;
    let stack_base = IMAGE_VBASE + n_img_pages * 4096 + HEAP_GUARD_BYTES;
    let stk_pages = (STACK_BUDGET_BYTES + 4095) / 4096;
    let heap_base = stack_base + stk_pages * 4096;
    let heap_limit =
        heap_base + ((MEM_BUDGET_BYTES + 4095) / 4096) * 4096;

    Ok(Spawned {
        ch_r,
        ch_w,
        heap_base,
        heap_limit,
    })
}
