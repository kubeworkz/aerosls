//! m2a_interp.rs — M2a's sidecar-supplied musl interpreter.
//!
//! The payload pair is committed under `fixtures/`:
//!   - `m2a_hello.elf` — an ET_EXEC, dynamically linked musl hello that
//!     proves itself with `write(1, "M2A-OK\n", 7)` and then calls
//!     `_exit(0)` — no `puts`, no stdio flush, no `writev`;
//!   - `ld-musl-x86_64.so.1` — the matching musl dynamic loader (ET_DYN);
//!   - `libm2a.so` — M2b's second DSO. Boot seeds it into a ramfs mounted at
//!     `/lib`, so the loader's `DT_NEEDED` search finds it there instead of
//!     stopping at ENOENT.
//!
//! Both are copied into the POSIX sidecar's budget tail, page-aligned. The
//! manifest is version 2: its footer names the main image, its `INTERP`
//! record names the loader and the validation-only path. The kernel maps the
//! main image at its linked address, maps the loader at
//! `LINUX_INTERP_BASE`, sets `AT_BASE` to that base, and enters the loader
//! (not the main image) — the loader then transfers to the main `AT_ENTRY`.
//!
//! The demo is ONE-SHOT PER BOOT: `first_boot()` reads the kernel's boot
//! generation for this sidecar name, which the kernel keeps per (partition,
//! name) for the whole kernel session and never clears at teardown. Init's
//! watchdog respawn of the E3 POSIX sidecar reads 1, so it skips the spawn
//! instead of re-running it after the irqtest has filled the task table
//! (which would log `CAP_ETABLEFULL` for a proof that already passed).

use aerosls_proto::kabi::Kernel;
use aerosls_proto::manifest::{
    pack_manifest, ArgEnv, Bootstrap, Budget, Cpu, Image, Interp, Limits, Manifest,
    ManifestCap, MAX_MANIFEST_CAPS,
};
use crate::linux::LINUX_DEFAULT_ENV;

extern "C" {
    /// `SYS_SLS_BOOT_GEN` (319): 0 on this sidecar NAME's first boot in this
    /// kernel session, 1+ after init's watchdog respawned it.
    fn k_boot_gen() -> u64;
}

/// This sidecar instance's boot generation (0 = first, 1+ = a watchdog
/// respawn). The demo runs only at 0: the kernel's counter is boot-scoped,
/// not sidecar-scoped, so it survives the POSIX teardown that the E3
/// irqtest's driver-death phase causes — a sidecar-local `static` would not.
pub fn boot_generation() -> u32 {
    unsafe { k_boot_gen() as u32 }
}

static HELLO: &[u8] = include_bytes!("../fixtures/m2a_hello.elf");
static INTERP: &[u8] = include_bytes!("../fixtures/ld-musl-x86_64.so.1");
/// M2b: the DSO the loader's `/lib` search must find. Boot seeds these bytes
/// into a ramfs at `/lib`; the bytes are the probe's own `libm2a.so`.
pub static M2A_LIB: &[u8] = include_bytes!("../fixtures/libm2a.so");

pub const M2A_NAME: &str = "m2a.linux.hello.0";
pub const M2A_INTERP_PATH: &str = "/lib/ld-musl-x86_64.so.1";
pub const M2A_LIB_PATH: &str = "/lib/libm2a.so";
const M2A_RESERVE: u64 = 1024 * 1024;
const MEM_BUDGET_BYTES: u64 = 256 * 1024;
const STACK_BUDGET_BYTES: u64 = 64 * 1024;
const IMAGE_VBASE: u64 = 0x4000_0000_0000; /* USER_PROC_CODE_BASE */
const HEAP_GUARD_BYTES: u64 = 4096;

pub struct Spawned {
    pub ch_r: u32,
    pub ch_w: u32,
    pub heap_base: u64,
    pub heap_limit: u64,
}

fn align_up(n: u64, align: u64) -> Option<u64> {
    n.checked_add(align - 1).map(|v| v & !(align - 1))
}

/// Plant the pair at the budget tail and spawn the dynamic child. Failures
/// are returned, never fatal to this sidecar's boot.
pub fn spawn<K: Kernel>(
    k: &K,
    budget_base: u64,
    budget_len: u64,
) -> Result<Spawned, i32> {
    let main_kaddr = budget_base
        .checked_add(budget_len)
        .and_then(|end| end.checked_sub(M2A_RESERVE))
        .ok_or(-1)?;
    let main_span = align_up(HELLO.len() as u64, 4096).ok_or(-1)?;
    let interp_kaddr = main_kaddr.checked_add(main_span).ok_or(-1)?;
    let interp_end = interp_kaddr
        .checked_add(INTERP.len() as u64)
        .ok_or(-1)?;
    if interp_end > budget_base + budget_len {
        return Err(-1);
    }
    if HELLO.len() as u64 > M2A_RESERVE || INTERP.len() as u64 > M2A_RESERVE {
        return Err(-1);
    }

    /* The budget region is identity-mapped; the kernel reads these bytes as
     * physical at the footer/INTERP addresses. The copy is synchronous: it
     * is complete before `linux_spawn` returns. */
    unsafe {
        core::ptr::copy_nonoverlapping(
            HELLO.as_ptr(), main_kaddr as *mut u8, HELLO.len());
        core::ptr::copy_nonoverlapping(
            INTERP.as_ptr(), interp_kaddr as *mut u8, INTERP.len());
    }

    let caps: [Option<ManifestCap<'_>>; MAX_MANIFEST_CAPS] =
        [None; MAX_MANIFEST_CAPS];
    let m = Manifest {
        version_major: 2,
        version_minor: 0,
        flags: 0,
        name: Some(M2A_NAME),
        personality: Some("m2a.linux.v1"),
        image: Some(Image {
            offset: 0,
            size: HELLO.len() as u32,
            entry: 0, /* ignored for an ELF; valid because 0 < size */
        }),
        interp: Some(Interp {
            image_size: INTERP.len() as u32,
            image_kaddr: interp_kaddr,
            path: M2A_INTERP_PATH,
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
        argenv: Some(ArgEnv::new(&[M2A_NAME], LINUX_DEFAULT_ENV)),
        signature: None,
    };
    let blob = pack_manifest(&m, main_kaddr);

    /* target_partition 0 = inherit (this sidecar's). */
    let (ch_r, ch_w) = k.linux_spawn(&blob, 0)?;

    let n_img_pages = (HELLO.len() as u64 + 4095) / 4096;
    let stack_base = IMAGE_VBASE + n_img_pages * 4096 + HEAP_GUARD_BYTES;
    let stk_pages = (STACK_BUDGET_BYTES + 4095) / 4096;
    let heap_base = stack_base + stk_pages * 4096;
    let heap_limit = heap_base + ((MEM_BUDGET_BYTES + 4095) / 4096) * 4096;
    Ok(Spawned { ch_r, ch_w, heap_base, heap_limit })
}
