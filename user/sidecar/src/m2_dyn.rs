//! m2_dyn.rs — M2's image-agnostic dynamic spawn.
//!
//! The M2a fixture is one committed pair; this module is the general route
//! every dynamic payload takes. The caller names a VFS path, and nothing
//! here knows the bytes in advance:
//!
//!   1. read the image from the VFS into the budget tail;
//!   2. parse its `PT_INTERP` (the last word on which loader it needs);
//!   3. read THAT path from the VFS — the loader lives in the rootfs `/lib`
//!      namespace, seeded at boot, exactly where the loader's own
//!      `DT_NEEDED` search looks;
//!   4. stage both page-aligned and pack a version-2 manifest whose
//!      `INTERP` record names the loader and the validated path.
//!
//! The kernel validates the manifest's path against the image's `PT_INTERP`,
//! maps the loader at `LINUX_INTERP_BASE`, sets `AT_BASE`, and enters the
//! loader. A version-1 manifest carrying a dynamic image is refused by the
//! kernel instead of being entered at `e_entry`.
//!
//! The probe's fixture (`m2_dyn_hello.elf`) is a raw-syscall ET_EXEC with
//! `PT_INTERP /lib/ld-musl-x86_64.so.1` and `DT_NEEDED libc.so`; it shares
//! no object with the M2a hello and prints `M2DYN-OK`.

use core::slice as core_slice;

use aerosls_proto::kabi::Kernel;
use aerosls_proto::manifest::{
    pack_manifest, ArgEnv, Bootstrap, Budget, Cpu, Image, Interp, Limits, Manifest,
    ManifestCap, MAX_MANIFEST_CAPS,
};
use aerosls_vfs::{BufferAlloc, Vfs};

use crate::linux::LINUX_DEFAULT_ENV;

static HELLO: &[u8] = include_bytes!("../fixtures/m2_dyn_hello.elf");

/// Rootfs path boot seeds the hello at.
pub const IMAGE_PATH: &str = "/tmp/m2.dyn.hello";
/// Rootfs path the image's `PT_INTERP` names; boot seeds it beside the
/// M2b DSO in the `/lib` ramfs.
pub const LOADER_PATH: &str = "/lib/ld-musl-x86_64.so.1";
pub const HELLO_NAME: &str = "m2.dyn.hello.0";

/// The staged pair's window at the budget tail. The loader is ~600 KiB; the
/// reserve leaves room for both plus page alignment.
const RESERVE: u64 = 2 * 1024 * 1024;
const MAX_IMAGE: u64 = 64 * 1024;
const MAX_LOADER: u64 = 1024 * 1024;
const MEM_BUDGET_BYTES: u64 = 256 * 1024;
const STACK_BUDGET_BYTES: u64 = 64 * 1024;
const IMAGE_VBASE: u64 = 0x4000_0000_0000; /* USER_PROC_CODE_BASE */
const HEAP_GUARD_BYTES: u64 = 4096;

pub struct Spawned {
    pub ch_r: u32,
    pub ch_w: u32,
    pub heap_base: u64,
    pub heap_limit: u64,
    pub image_len: u64,
    pub loader_len: u64,
}

fn align_up(n: u64, align: u64) -> Option<u64> {
    n.checked_add(align - 1).map(|v| v & !(align - 1))
}

/// Boot's copy of the payload. The bytes are only the rootfs seed; the spawn
/// reads them back through the VFS like any other file.
pub fn seed_bytes() -> &'static [u8] {
    HELLO
}

/// `PT_INTERP`'s path from an ELF64 image. Only the fields the kernel's own
/// `elf64_find_interp` checks are read here: magic, class, program-header
/// bounds, and the one `PT_INTERP` segment.
fn interp_path(image: &[u8]) -> Result<&str, i32> {
    const EHDR: usize = 64;
    const PHDR: usize = 56;
    if image.len() < EHDR || &image[0..4] != b"\x7fELF" || image[4] != 2 {
        return Err(-2);
    }
    let phoff = u64::from_le_bytes(image[0x20..0x28].try_into().unwrap());
    let phentsize = u16::from_le_bytes(image[0x36..0x38].try_into().unwrap()) as usize;
    let phnum = u16::from_le_bytes(image[0x38..0x3a].try_into().unwrap()) as usize;
    if phentsize < PHDR {
        return Err(-3);
    }
    for i in 0..phnum {
        let off = (phoff as usize).checked_add(i.checked_mul(phentsize).ok_or(-3)?)
            .ok_or(-3)?;
        let ph = image.get(off..off + PHDR).ok_or(-3)?;
        if u32::from_le_bytes(ph[0..4].try_into().unwrap()) != 3 {
            continue;
        }
        let seg_off = u64::from_le_bytes(ph[8..16].try_into().unwrap()) as usize;
        let seg_len = u64::from_le_bytes(ph[32..40].try_into().unwrap()) as usize;
        let bytes = image
            .get(seg_off..seg_off.checked_add(seg_len).ok_or(-3)?)
            .ok_or(-3)?;
        let nul = bytes.iter().position(|&b| b == 0).unwrap_or(bytes.len());
        let path = core::str::from_utf8(&bytes[..nul]).map_err(|_| -3)?;
        if path.is_empty() {
            return Err(-3);
        }
        return Ok(path);
    }
    Err(-4) /* not a dynamic image */
}

/// Read a whole VFS file into `dst` (identity-mapped budget tail). Returns
/// the byte count; a short final read is EOF, never an error.
fn read_vfs<K: Kernel, A: BufferAlloc>(
    vfs: &mut Vfs<K, A>,
    task: u32,
    path: &str,
    dst: u64,
    cap: u64,
) -> Result<u64, i32> {
    let fd = vfs.open(task, path, 0, 0).map_err(|_| -5)?;
    let mut chunk = [0u8; 4096];
    let mut off = 0u64;
    loop {
        let want = (cap - off).min(chunk.len() as u64) as usize;
        if want == 0 {
            vfs.close(task, fd).ok();
            return Err(-6); /* larger than the staging cap */
        }
        match vfs.read_at(task, fd, off, &mut chunk[..want]) {
            Ok(0) => break,
            Ok(n) => {
                unsafe {
                    core::ptr::copy_nonoverlapping(
                        chunk.as_ptr(),
                        (dst + off) as *mut u8,
                        n,
                    );
                }
                off += n as u64;
            }
            Err(_) => {
                vfs.close(task, fd).ok();
                return Err(-7);
            }
        }
    }
    vfs.close(task, fd).ok();
    Ok(off)
}

/// Stage `image_path` and the loader its `PT_INTERP` names, then spawn it.
/// Failures are returned to the caller, never fatal to the sidecar's boot.
pub fn spawn<K: Kernel, A: BufferAlloc>(
    k: &K,
    vfs: &mut Vfs<K, A>,
    image_path: &str,
    name: &str,
    budget_base: u64,
    budget_len: u64,
) -> Result<Spawned, i32> {
    let main_kaddr = budget_base
        .checked_add(budget_len)
        .and_then(|end| end.checked_sub(RESERVE))
        .ok_or(-1)?;
    let vt = vfs.add_task().map_err(|_| -8)?;

    /* 1. the image, straight from the rootfs into its staging window. */
    let image_len = read_vfs(vfs, vt, image_path, main_kaddr, MAX_IMAGE)?;
    let image = unsafe {
        core_slice::from_raw_parts(main_kaddr as *const u8, image_len as usize)
    };

    /* 2. what loader does it name? */
    let interp = interp_path(image)?;

    /* 3. that loader, from the same VFS namespace. */
    let interp_kaddr = main_kaddr
        .checked_add(align_up(image_len, 4096).ok_or(-1)?)
        .ok_or(-1)?;
    let loader_len = read_vfs(vfs, vt, interp, interp_kaddr, MAX_LOADER)?;
    if interp_kaddr
        .checked_add(loader_len)
        .map_or(true, |end| end > budget_base + budget_len)
    {
        return Err(-1);
    }

    let caps: [Option<ManifestCap<'_>>; MAX_MANIFEST_CAPS] = [None; MAX_MANIFEST_CAPS];
    let m = Manifest {
        version_major: 2,
        version_minor: 0,
        flags: 0,
        name: Some(name),
        personality: Some("m2a.linux.v1"),
        image: Some(Image {
            offset: 0,
            size: image_len as u32,
            entry: 0, /* ignored for an ELF; valid because 0 < size */
        }),
        interp: Some(Interp {
            image_size: loader_len as u32,
            image_kaddr: interp_kaddr,
            path: interp,
        }),
        budget: Some(Budget {
            mem_bytes: MEM_BUDGET_BYTES,
            stack_bytes: STACK_BUDGET_BYTES as u32,
            heap_initial: 0,
        }),
        cpu: Some(Cpu { share: 10, preemptible: true }),
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
        argenv: Some(ArgEnv::new(&[name], LINUX_DEFAULT_ENV)),
        signature: None,
    };
    let blob = pack_manifest(&m, main_kaddr);

    /* target_partition 0 = inherit (this sidecar's). */
    let (ch_r, ch_w) = k.linux_spawn(&blob, 0)?;

    let n_img_pages = (image_len + 4095) / 4096;
    let stack_base = IMAGE_VBASE + n_img_pages * 4096 + HEAP_GUARD_BYTES;
    let stk_pages = (STACK_BUDGET_BYTES + 4095) / 4096;
    let heap_base = stack_base + stk_pages * 4096;
    let heap_limit = heap_base + ((MEM_BUDGET_BYTES + 4095) / 4096) * 4096;
    Ok(Spawned {
        ch_r,
        ch_w,
        heap_base,
        heap_limit,
        image_len,
        loader_len,
    })
}
