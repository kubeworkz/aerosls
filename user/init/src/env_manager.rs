//! POSIX-Environments E4: the environment manager.
//!
//! Creates one tenant POSIX environment **in a target partition** — its own
//! ramdisk driver and POSIX sidecar, each placed in that partition via
//! `SYS_SLS_CREATE_SIDECAR`'s `target_partition` (E4 part 1) over private
//! frame-pool regions. This generalises E3's system-partition tenant spawn
//! (`entry::spawn_e3_tenant_envs`) to an arbitrary partition; the ENV_CREATE
//! request path — init's dispatch loop and the control plane — drives it in
//! later E4 parts. Keeping manifest construction here, in Rust, avoids a
//! second manifest builder in kernel C (roadmap §7).

use aerosls_proto::env_proto::{
    self, EnvFrame, ENV_CREATE, ENV_DESTROY, ENV_ERR_FULL, ENV_ERR_INVAL, ENV_ERR_NOMEM,
    ENV_ERR_PART, ENV_ERR_UNSUPP, ENV_OK,
};
use aerosls_proto::kabi::{Kernel, ERR_NOMEM};
use alloc::vec::Vec;
use core::fmt::Write;

use crate::{posix_manifest, ramdisk_manifest};

/// Per-environment frame budgets (must match the manifest builders' heap
/// sizes). A POSIX environment draws POSIX_HEAP_FRAMES + RD_HEAP_FRAMES +
/// RD_STORAGE_FRAMES frames up front, plus the two sidecars' image/stack
/// frames the kernel charges to the partition on create.
pub const POSIX_HEAP_FRAMES: u64 = 1024; // 4 MiB — build_posix_manifest_tenant heap
pub const RD_HEAP_FRAMES: u64 = 64; //     256 KiB — build_ramdisk_manifest_named heap
pub const RD_STORAGE_FRAMES: u64 = 256; // 1 MiB — the private block device
const RD_STORAGE_BYTES: u64 = RD_STORAGE_FRAMES * 4096;

/// The shared, read-only sidecar images an environment instantiates (the boot
/// image's `ramdisk.image` / `posix.image` MEM caps — each sidecar gets its
/// own copy of the code; only heap and storage are per-instance, principle 2).
#[derive(Clone, Copy)]
pub struct EnvImages {
    pub ramdisk_kaddr: u64,
    pub ramdisk_size: u32,
    pub posix_kaddr: u64,
    pub posix_size: u32,
}

/// A created environment: the partition it lives in, its `index` (identity),
/// the private regions it owns, and init's messenger endpoints to its ramdisk
/// and POSIX sidecars.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Environment {
    pub partition: u32,
    pub index: u32,
    pub rd_storage: u64,
    pub px_heap: u64,
    pub ramdisk_r: u32,
    pub ramdisk_w: u32,
    pub posix_r: u32,
    pub posix_w: u32,
}

/// Format a sidecar name (`<prefix>.<index>`) into a small stack buffer —
/// names are a short prefix plus a decimal index, so 32 bytes is ample and no
/// heap allocation is needed for the identity.
struct NameBuf {
    buf: [u8; 32],
    len: usize,
}

impl NameBuf {
    fn new() -> NameBuf {
        NameBuf { buf: [0; 32], len: 0 }
    }
    fn as_str(&self) -> &str {
        core::str::from_utf8(&self.buf[..self.len]).unwrap_or("")
    }
}

impl Write for NameBuf {
    fn write_str(&mut self, s: &str) -> core::fmt::Result {
        let b = s.as_bytes();
        if self.len + b.len() > self.buf.len() {
            return Err(core::fmt::Error);
        }
        self.buf[self.len..self.len + b.len()].copy_from_slice(b);
        self.len += b.len();
        Ok(())
    }
}

/// Create one POSIX environment identified by `index` **in `partition`**:
/// allocate its private ramdisk heap + storage and POSIX heap from the frame
/// pool, then create its ramdisk driver (`drv.ramdisk.<index>`) and POSIX
/// sidecar (`aerosls.posix.<index>`, wired to that ramdisk) in `partition`.
/// The names are partition-scoped (E2), so environments in different
/// partitions may share an index. Returns the environment's handles, `ERR_NOMEM`
/// if the frame pool cannot back the regions, or the kernel's error if a create
/// is refused (e.g. the partition is absent/paused, or the caller is not
/// PARTITION_SYSTEM — enforced by cap_create_sidecar_in).
pub fn create_environment<K: Kernel>(
    k: &K,
    partition: u32,
    index: u32,
    images: &EnvImages,
) -> Result<Environment, i32> {
    // Private regions from the frame pool. alloc_region returns 0 on
    // exhaustion/denial; allocate all three before creating anything so a
    // shortfall fails cleanly, with nothing half-built.
    let rd_heap = k.alloc_region(RD_HEAP_FRAMES, 1);
    let rd_storage = k.alloc_region(RD_STORAGE_FRAMES, 1);
    let px_heap = k.alloc_region(POSIX_HEAP_FRAMES, 1);
    if rd_heap == 0 || rd_storage == 0 || px_heap == 0 {
        return Err(ERR_NOMEM);
    }

    let mut rd_name = NameBuf::new();
    let _ = write!(rd_name, "drv.ramdisk.{}", index);
    let mut px_name = NameBuf::new();
    let _ = write!(px_name, "aerosls.posix.{}", index);

    // Tenant ramdisk: its own name + private R|W storage (0x3, so the POSIX
    // sidecar can format the empty region on first mount — E3), placed in the
    // target partition.
    let rd_manifest = ramdisk_manifest::build_ramdisk_manifest_named(
        rd_name.as_str(),
        images.ramdisk_kaddr,
        images.ramdisk_size,
        rd_heap,
        rd_storage,
        RD_STORAGE_BYTES,
        0x3,
    );
    let (ramdisk_r, ramdisk_w) = k.create_sidecar_in(&rd_manifest, partition)?;

    // Tenant POSIX: tenant profile (no hardware/network — E2), wired to THIS
    // environment's ramdisk, placed in the same partition.
    let px_manifest = posix_manifest::build_posix_manifest_tenant(
        px_name.as_str(),
        rd_name.as_str(),
        images.posix_kaddr,
        images.posix_size,
        px_heap,
    );
    let (posix_r, posix_w) = k.create_sidecar_in(&px_manifest, partition)?;

    Ok(Environment {
        partition,
        index,
        rd_storage,
        px_heap,
        ramdisk_r,
        ramdisk_w,
        posix_r,
        posix_w,
    })
}

/// The environment manager's state: the images every environment instantiates,
/// the environments created so far (each with its assigned id, kept for a
/// future `ENV_DESTROY` — E5), and a bound on how many may coexist. init owns
/// one of these and drives it from the ENV request channel (E4 part 3).
pub struct EnvManager {
    images: EnvImages,
    envs: Vec<(u32, Environment)>,
    next_id: u32,
    max_envs: usize,
}

impl EnvManager {
    pub fn new(images: EnvImages, max_envs: usize) -> EnvManager {
        EnvManager { images, envs: Vec::new(), next_id: 1, max_envs }
    }

    /// The environments created so far, as `(env_id, environment)`.
    pub fn environments(&self) -> &[(u32, Environment)] {
        &self.envs
    }

    /// Handle one ENV request (`env_proto`): parse the frame + body, act, and
    /// write the reply — an `EnvFrame` echoing the request type plus a uniform
    /// `{status, env_id, partition}` body — into `reply`, returning its length
    /// (0 if `reply` is too small). A malformed request is answered
    /// `ENV_ERR_INVAL`; a create is placed in the requested partition via
    /// `create_environment` (so the kernel's E4 gate authorises the placement).
    pub fn handle_request<K: Kernel>(&mut self, k: &K, req: &[u8], reply: &mut [u8]) -> usize {
        let frame = match EnvFrame::parse(req) {
            Some(f) => f,
            None => return self.reply(reply, ENV_CREATE, ENV_ERR_INVAL, 0, 0),
        };
        let body = &req[EnvFrame::SIZE..];
        match frame.ty {
            ENV_CREATE => {
                let (partition, index) = match env_proto::parse_create_body(body) {
                    Some(v) => v,
                    None => return self.reply(reply, ENV_CREATE, ENV_ERR_INVAL, 0, 0),
                };
                if self.envs.len() >= self.max_envs {
                    return self.reply(reply, ENV_CREATE, ENV_ERR_FULL, 0, partition);
                }
                match create_environment(k, partition, index, &self.images) {
                    Ok(env) => {
                        let id = self.next_id;
                        self.next_id = self.next_id.wrapping_add(1);
                        self.envs.push((id, env));
                        self.reply(reply, ENV_CREATE, ENV_OK, id, partition)
                    }
                    Err(ERR_NOMEM) => self.reply(reply, ENV_CREATE, ENV_ERR_NOMEM, 0, partition),
                    // Any other kernel error is a refused placement: an absent or
                    // paused partition, or a caller the E4 gate denied.
                    Err(_) => self.reply(reply, ENV_CREATE, ENV_ERR_PART, 0, partition),
                }
            }
            // Leak-free destroy is E5; recognise the opcode so the control plane
            // gets a structured "not yet" instead of a dropped request.
            ENV_DESTROY => self.reply(reply, ENV_DESTROY, ENV_ERR_UNSUPP, 0, 0),
            other => self.reply(reply, other, ENV_ERR_INVAL, 0, 0),
        }
    }

    fn reply(&self, reply: &mut [u8], ty: u16, status: u16, env_id: u32, partition: u32) -> usize {
        let hdr = EnvFrame::new(ty, status != ENV_OK).encode();
        let body = env_proto::encode_reply_body(status, env_id, partition);
        let n = hdr.len() + body.len();
        if reply.len() < n {
            return 0;
        }
        reply[..hdr.len()].copy_from_slice(&hdr);
        reply[hdr.len()..n].copy_from_slice(&body);
        n
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::sim::SimKernel;
    use aerosls_proto::manifest::{parse_manifest, CapKind};

    fn images() -> EnvImages {
        EnvImages {
            ramdisk_kaddr: 0x3000_0000,
            ramdisk_size: 0x8000,
            posix_kaddr: 0x3100_0000,
            posix_size: 0x40000,
        }
    }

    #[test]
    fn creates_ramdisk_and_posix_in_the_target_partition() {
        let k = SimKernel::new();
        let env = create_environment(&k, 5, 3, &images()).expect("environment created");

        assert_eq!(env.partition, 5);
        assert_eq!(env.index, 3);

        // Three private regions were allocated with the environment budgets.
        let regions = k.regions();
        assert_eq!(regions.len(), 3);
        assert_eq!(regions[0], (RD_HEAP_FRAMES, 1));
        assert_eq!(regions[1], (RD_STORAGE_FRAMES, 1));
        assert_eq!(regions[2], (POSIX_HEAP_FRAMES, 1));

        // Two sidecars were created, BOTH targeting partition 5.
        let created = k.created();
        assert_eq!(created.len(), 2);
        assert_eq!(created[0].1, 5, "ramdisk targets the requested partition");
        assert_eq!(created[1].1, 5, "POSIX targets the requested partition");

        // The ramdisk manifest carries this environment's name and its own
        // private, writable storage region.
        let rd = parse_manifest(&created[0].0).unwrap();
        assert_eq!(rd.name, Some("drv.ramdisk.3"));
        let storage = rd.find_cap("storage").expect("storage cap");
        assert_eq!(storage.rights, 0x3);
        assert!(matches!(storage.kind,
            CapKind::Mem { base, .. } if base == env.rd_storage));

        // The POSIX manifest carries this environment's name, its heap, and a
        // ramdisk channel wired to THIS environment's ramdisk (partition-scoped
        // resolution by E2 lets the index repeat across partitions).
        let px = parse_manifest(&created[1].0).unwrap();
        assert_eq!(px.name, Some("aerosls.posix.3"));
        assert_eq!(px.find_cap("ramdisk").unwrap().kind,
                   CapKind::Chan { peer: Some("drv.ramdisk.3"), flags: 0 });
        // The tenant profile carries no hardware/network caps.
        for absent in ["network", "uart", "irq.timer.0", "nic0.bar0"] {
            assert!(px.find_cap(absent).is_none());
        }
    }

    #[test]
    fn two_environments_get_distinct_names_and_storage() {
        let k = SimKernel::new();
        let a = create_environment(&k, 5, 1, &images()).unwrap();
        let b = create_environment(&k, 7, 1, &images()).unwrap();

        // Same index (1) in different partitions — distinct storage regions,
        // and each sidecar targets its own partition.
        assert_ne!(a.rd_storage, b.rd_storage);
        let created = k.created();
        assert_eq!(created.len(), 4);
        assert_eq!(created[0].1, 5);
        assert_eq!(created[2].1, 7);
        // Both name their ramdisk "drv.ramdisk.1" — the collision is resolved
        // by the partition, not the name (E2 scoped registry).
        assert_eq!(parse_manifest(&created[0].0).unwrap().name, Some("drv.ramdisk.1"));
        assert_eq!(parse_manifest(&created[2].0).unwrap().name, Some("drv.ramdisk.1"));
    }

    #[test]
    fn region_exhaustion_fails_before_creating_anything() {
        // A sim that hands out no regions (alloc_region == 0) makes the create
        // fail with ERR_NOMEM and spawn nothing.
        let k = SimKernel::new();
        k.set_regions_exhausted(true);
        assert_eq!(create_environment(&k, 5, 1, &images()), Err(ERR_NOMEM));
        assert_eq!(k.created().len(), 0);
    }

    // ── EnvManager dispatch: the ENV_* request path ─────────────────────────
    fn create_req(partition: u32, index: u32) -> alloc::vec::Vec<u8> {
        let mut r = EnvFrame::new(ENV_CREATE, false).encode().to_vec();
        r.extend_from_slice(&env_proto::encode_create_body(partition, index));
        r
    }

    fn reply_status(reply: &[u8], n: usize) -> (u16, u32, u32) {
        env_proto::parse_reply_body(&reply[EnvFrame::SIZE..n]).expect("reply body parses")
    }

    #[test]
    fn env_manager_creates_on_env_create_and_tracks_it() {
        let k = SimKernel::new();
        let mut mgr = EnvManager::new(images(), 8);
        let mut reply = [0u8; 64];
        let n = mgr.handle_request(&k, &create_req(6, 2), &mut reply);
        let (status, env_id, partition) = reply_status(&reply, n);
        assert_eq!(status, ENV_OK);
        assert_eq!(env_id, 1); // the first environment
        assert_eq!(partition, 6);
        // The manager tracks it, and BOTH sidecars were placed in partition 6.
        assert_eq!(mgr.environments().len(), 1);
        assert_eq!(mgr.environments()[0].0, 1);
        assert_eq!(mgr.environments()[0].1.partition, 6);
        let created = k.created();
        assert_eq!(created.len(), 2);
        assert!(created.iter().all(|(_, p)| *p == 6));
    }

    #[test]
    fn env_manager_assigns_increasing_ids() {
        let k = SimKernel::new();
        let mut mgr = EnvManager::new(images(), 8);
        let mut reply = [0u8; 64];
        let n1 = mgr.handle_request(&k, &create_req(6, 1), &mut reply);
        assert_eq!(reply_status(&reply, n1).1, 1);
        let n2 = mgr.handle_request(&k, &create_req(7, 1), &mut reply);
        assert_eq!(reply_status(&reply, n2).1, 2);
        assert_eq!(mgr.environments().len(), 2);
    }

    #[test]
    fn env_manager_refuses_when_full() {
        let k = SimKernel::new();
        let mut mgr = EnvManager::new(images(), 1); // room for one
        let mut reply = [0u8; 64];
        mgr.handle_request(&k, &create_req(6, 1), &mut reply);
        let n = mgr.handle_request(&k, &create_req(7, 1), &mut reply);
        assert_eq!(reply_status(&reply, n).0, ENV_ERR_FULL);
        assert_eq!(mgr.environments().len(), 1);
    }

    #[test]
    fn env_manager_reports_region_exhaustion() {
        let k = SimKernel::new();
        k.set_regions_exhausted(true);
        let mut mgr = EnvManager::new(images(), 8);
        let mut reply = [0u8; 64];
        let n = mgr.handle_request(&k, &create_req(6, 1), &mut reply);
        assert_eq!(reply_status(&reply, n).0, ENV_ERR_NOMEM);
        assert_eq!(mgr.environments().len(), 0);
    }

    #[test]
    fn env_manager_rejects_malformed_and_defers_destroy() {
        let k = SimKernel::new();
        let mut mgr = EnvManager::new(images(), 8);
        let mut reply = [0u8; 64];
        // A too-short / foreign frame is refused, and nothing is created.
        let n = mgr.handle_request(&k, &[0u8; 4], &mut reply);
        assert_eq!(reply_status(&reply, n).0, ENV_ERR_INVAL);
        // ENV_DESTROY is recognised but deferred to E5.
        let mut dreq = EnvFrame::new(ENV_DESTROY, false).encode().to_vec();
        dreq.extend_from_slice(&env_proto::encode_destroy_body(1));
        let dn = mgr.handle_request(&k, &dreq, &mut reply);
        assert_eq!(reply_status(&reply, dn).0, ENV_ERR_UNSUPP);
        assert_eq!(k.created().len(), 0);
    }
}
