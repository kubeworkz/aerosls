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
    self, EnvFrame, ENV_CREATE, ENV_DESTROY, ENV_ERR_FULL, ENV_ERR_INVAL, ENV_ERR_NOENT,
    ENV_ERR_NOMEM, ENV_ERR_PART, ENV_OK,
};
use aerosls_proto::kabi::{Kernel, ERR_NOMEM};
use alloc::vec::Vec;
use core::fmt::Write;

use crate::{posix_manifest, ramdisk_manifest};

/// Per-environment frame budgets (must match the manifest builders' heap
/// sizes). A POSIX environment draws POSIX_HEAP_FRAMES + RD_HEAP_FRAMES +
/// RD_STORAGE_FRAMES frames up front, plus the two sidecars' image/stack
/// frames the kernel charges to the partition on create. ALL of it is charged
/// to the environment's partition: the regions by `alloc_region_in` below
/// (E4 follow-on — before it, these 1344 frames were billed to init's
/// PARTITION_SYSTEM quota), the sidecar frames by `cap_create_sidecar_in`.
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
    /// The three private regions this environment owns, kept per-environment
    /// from E5: a destroy has to hand each of them back individually (E4 only
    /// ever needed the ramdisk heap's base at create time), so all three bases
    /// are part of the environment's identity now.
    pub rd_heap: u64,
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
    // Private regions from the frame pool, charged to the TARGET partition
    // (E4 follow-on): the environment's heap and storage must count against
    // the tenant's frame quota, not the environment manager's — otherwise a
    // tenant is not quota-bounded for its own storage and a refused placement
    // leaves the regions on the creator. alloc_region_in returns 0 on
    // exhaustion/denial; allocate all three before creating anything so a
    // shortfall fails cleanly, with nothing half-built.
    let rd_heap = k.alloc_region_in(RD_HEAP_FRAMES, 1, partition);
    let rd_storage = k.alloc_region_in(RD_STORAGE_FRAMES, 1, partition);
    let px_heap = k.alloc_region_in(POSIX_HEAP_FRAMES, 1, partition);
    if rd_heap == 0 || rd_storage == 0 || px_heap == 0 {
        return Err(ERR_NOMEM);
    }

    let rd_name = ramdisk_name(index);
    let px_name = posix_name(index);

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
        rd_heap,
        rd_storage,
        px_heap,
        ramdisk_r,
        ramdisk_w,
        posix_r,
        posix_w,
    })
}

/// The ramdisk sidecar's registry name for environment `index`. Built in one
/// place so the name registered at create and the name resolved at destroy
/// cannot drift apart — the destroy path finds a sidecar by exactly the name
/// create gave it. Names are partition-scoped (E2), so `index` alone
/// identifies the environment within its own partition.
fn ramdisk_name(index: u32) -> NameBuf {
    let mut n = NameBuf::new();
    let _ = write!(n, "drv.ramdisk.{}", index);
    n
}

/// The POSIX sidecar's registry name for environment `index` (see
/// `ramdisk_name`).
fn posix_name(index: u32) -> NameBuf {
    let mut n = NameBuf::new();
    let _ = write!(n, "aerosls.posix.{}", index);
    n
}

/// End an environment: kill its two sidecars by the names it registered, and
/// report the pids that were live (0 for one that was not).
///
/// Killing and releasing are separate steps on purpose. The kernel DEFERS a
/// kill whose target is RUNNING at the time (kernel/process.c marks it
/// `pending_teardown` and finishes at the next schedule, so it never frees
/// page tables the running CPU is using). Releasing an environment's heap
/// while a sidecar can still execute in it would be a use-after-free, so the
/// caller must confirm the sidecars are gone — `sidecar_pid` is that test —
/// before calling `reclaim_environment`.
///
/// The pids are resolved BEFORE the kill, because a kill of a parked sidecar
/// drops its registry entry inside the syscall: afterwards the name resolves
/// to nothing and the pid would be lost. They are kept in `Reclaim` so a
/// deferred teardown can tell "my sidecar is still running" from "my sidecar
/// died and a newer environment registered the same name".
pub fn kill_environment<K: Kernel>(k: &K, env: &Environment) -> (u32, u32) {
    let rd = ramdisk_name(env.index);
    let px = posix_name(env.index);
    let rd_pid = k.sidecar_pid(rd.as_str(), env.partition);
    let px_pid = k.sidecar_pid(px.as_str(), env.partition);
    if rd_pid != 0 {
        k.proc_kill(rd_pid);
    }
    if px_pid != 0 {
        k.proc_kill(px_pid);
    }
    (rd_pid, px_pid)
}

/// Release an environment's three private regions back to its OWN partition —
/// the same partition `alloc_region_in` charged, so the tenant's frame usage
/// returns exactly to where the create left it. Only call this once both
/// sidecars are provably gone (see `kill_environment`). Returns true when all
/// three releases succeeded; a region whose frames were already free is not an
/// error (the frame pool skips them), so a retry after a partial failure is
/// safe.
pub fn reclaim_environment<K: Kernel>(k: &K, env: &Environment) -> bool {
    let mut ok = true;
    ok &= k.free_region_in(env.rd_heap, RD_HEAP_FRAMES, env.partition);
    ok &= k.free_region_in(env.rd_storage, RD_STORAGE_FRAMES, env.partition);
    ok &= k.free_region_in(env.px_heap, POSIX_HEAP_FRAMES, env.partition);
    ok
}

/// An environment whose sidecars were killed, together with the pids that were
/// live when they were. `pending` is non-zero only for the window between a
/// deferred kill and the schedule tick that finishes it.
pub struct Reclaim {
    pub env: Environment,
    pub ramdisk_pid: u32,
    pub posix_pid: u32,
}

/// Is the sidecar we killed under `pid` gone? True when it was never live
/// (pid 0 — nothing to wait for), when the name no longer resolves at all, or
/// when it resolves to a DIFFERENT pid, which means our sidecar's entry was
/// dropped and the name now belongs to a newer environment. That last case is
/// why the pid is remembered: reclamation must never wait on — or worse, act
/// on — a process that merely inherited the name.
fn sidecar_gone<K: Kernel>(k: &K, name: &str, partition: u32, pid: u32) -> bool {
    if pid == 0 {
        return true;
    }
    let now = k.sidecar_pid(name, partition);
    now == 0 || now != pid
}

/// Finish one environment's teardown: release its regions if both sidecars are
/// really gone, otherwise leave everything alone and report that it is not
/// done. Never partially releases, and never re-kills — the kill has already
/// been issued; this only waits for the kernel to finish it.
pub fn try_reclaim<K: Kernel>(k: &K, r: &Reclaim) -> bool {
    let rd = ramdisk_name(r.env.index);
    let px = posix_name(r.env.index);
    if !sidecar_gone(k, rd.as_str(), r.env.partition, r.ramdisk_pid)
        || !sidecar_gone(k, px.as_str(), r.env.partition, r.posix_pid)
    {
        return false;
    }
    reclaim_environment(k, &r.env)
}

/// The environment manager's state: the images every environment instantiates,
/// the environments created so far (each with its assigned id, kept for
/// `ENV_DESTROY` — E5), the ones whose teardown the kernel has not finished
/// yet, and a bound on how many may coexist. init owns one of these and drives
/// it from the ENV request channel (E4 part 3).
pub struct EnvManager {
    images: EnvImages,
    envs: Vec<(u32, Environment)>,
    /// Environments killed but not yet reclaimed. Drained by `reap`, which
    /// every request runs first, so a deferred teardown lands within a tick of
    /// the kill. Empty at rest.
    reclaiming: Vec<Reclaim>,
    next_id: u32,
    max_envs: usize,
}

impl EnvManager {
    pub fn new(images: EnvImages, max_envs: usize) -> EnvManager {
        EnvManager {
            images,
            envs: Vec::new(),
            reclaiming: Vec::new(),
            next_id: 1,
            max_envs,
        }
    }

    /// The environments created so far, as `(env_id, environment)`.
    pub fn environments(&self) -> &[(u32, Environment)] {
        &self.envs
    }

    /// Environments whose teardown is still outstanding — zero except in the
    /// window between a kill of a RUNNING sidecar and the schedule tick that
    /// finishes its teardown.
    pub fn pending_reclaims(&self) -> usize {
        self.reclaiming.len()
    }

    /// Finish any teardown the kernel deferred, and return how many are still
    /// outstanding. Idempotent and free when nothing is pending, so every
    /// request runs it first: the environments that need it were killed on an
    /// earlier request, and this is the tick that releases their frames.
    pub fn reap<K: Kernel>(&mut self, k: &K) -> usize {
        if self.reclaiming.is_empty() {
            return 0;
        }
        let pending = core::mem::take(&mut self.reclaiming);
        for r in pending {
            if !try_reclaim(k, &r) {
                self.reclaiming.push(r);
            }
        }
        self.reclaiming.len()
    }

    /// Handle one ENV request (`env_proto`): parse the frame + body, act, and
    /// write the reply — an `EnvFrame` echoing the request type plus a uniform
    /// `{status, env_id, partition}` body — into `reply`, returning its length
    /// (0 if `reply` is too small). A malformed request is answered
    /// `ENV_ERR_INVAL`; a create is placed in the requested partition via
    /// `create_environment` (so the kernel's E4 gate authorises the placement).
    pub fn handle_request<K: Kernel>(&mut self, k: &K, req: &[u8], reply: &mut [u8]) -> usize {
        // Finish any teardown the kernel deferred on an earlier request before
        // serving this one, so a destroyed environment's frames come back
        // within a tick of the kill. Idempotent and free when nothing is
        // pending.
        self.reap(k);
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
            ENV_DESTROY => {
                let env_id = match env_proto::parse_destroy_body(body) {
                    Some(v) => v,
                    None => return self.reply(reply, ENV_DESTROY, ENV_ERR_INVAL, 0, 0),
                };
                // An unknown id is reported as such rather than silently
                // succeeding: "destroyed" and "there was nothing there" are
                // different answers to a caller, and this is the only way it
                // can tell them apart.
                let pos = match self.envs.iter().position(|(id, _)| *id == env_id) {
                    Some(p) => p,
                    None => return self.reply(reply, ENV_DESTROY, ENV_ERR_NOENT, env_id, 0),
                };
                let (_, env) = self.envs.remove(pos);
                let partition = env.partition;
                let (ramdisk_pid, posix_pid) = kill_environment(k, &env);
                let pending = Reclaim { env, ramdisk_pid, posix_pid };
                // Usually the environment's sidecars are parked (an idle
                // environment is a blocked process), so the kill tears them
                // down inside the syscall and this releases the frames right
                // now. When they are not — a sidecar RUNNING at the moment of
                // the kill — the kernel defers the teardown, the regions stay
                // taken, and `reap` releases them on a later request. Both
                // answers are ENV_OK: the environment IS being ended; only its
                // memory accounting is a tick behind.
                if !try_reclaim(k, &pending) {
                    self.reclaiming.push(pending);
                }
                self.reply(reply, ENV_DESTROY, ENV_OK, env_id, partition)
            }
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

        // Three private regions were allocated with the environment budgets —
        // and each of them charged to the TARGET partition. This is the
        // assertion that bites when the charging reverts to the caller: an
        // `alloc_region(...)` call here would record partition 0 ("the
        // caller's") for all three while the region sizes stayed identical.
        let regions = k.regions();
        assert_eq!(regions.len(), 3);
        assert_eq!(regions[0], (RD_HEAP_FRAMES, 1, 5));
        assert_eq!(regions[1], (RD_STORAGE_FRAMES, 1, 5));
        assert_eq!(regions[2], (POSIX_HEAP_FRAMES, 1, 5));
        assert!(regions.iter().all(|r| r.2 == env.partition),
                "every environment region is charged to the environment's partition");

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

    fn destroy_req(env_id: u32) -> alloc::vec::Vec<u8> {
        let mut r = EnvFrame::new(ENV_DESTROY, false).encode().to_vec();
        r.extend_from_slice(&env_proto::encode_destroy_body(env_id));
        r
    }

    #[test]
    fn env_manager_rejects_malformed_requests() {
        let k = SimKernel::new();
        let mut mgr = EnvManager::new(images(), 8);
        let mut reply = [0u8; 64];
        // A too-short / foreign frame is refused, and nothing is created.
        let n = mgr.handle_request(&k, &[0u8; 4], &mut reply);
        assert_eq!(reply_status(&reply, n).0, ENV_ERR_INVAL);
        // A destroy carrying no env_id is refused too, and touches nothing.
        let dn = mgr.handle_request(&k, &EnvFrame::new(ENV_DESTROY, false).encode(), &mut reply);
        assert_eq!(reply_status(&reply, dn).0, ENV_ERR_INVAL);
        assert_eq!(k.created().len(), 0);
        assert_eq!(k.killed_pids().len(), 0);
    }

    /// E5: destroying an environment kills both sidecars, drops both registry
    /// names, and hands all three regions back to the TENANT partition.
    #[test]
    fn destroy_kills_both_sidecars_and_releases_every_region() {
        let k = SimKernel::new();
        let mut mgr = EnvManager::new(images(), 8);
        let mut reply = [0u8; 64];
        let n = mgr.handle_request(&k, &create_req(6, 2), &mut reply);
        let (status, env_id, _) = reply_status(&reply, n);
        assert_eq!(status, ENV_OK);
        assert_eq!(k.sidecar_count(), 2, "both sidecars registered on create");
        let env = mgr.environments()[0].1;

        let dn = mgr.handle_request(&k, &destroy_req(env_id), &mut reply);
        let (dstatus, denv, dpart) = reply_status(&reply, dn);
        assert_eq!(dstatus, ENV_OK);
        assert_eq!(denv, env_id);
        assert_eq!(dpart, 6, "the reply names the environment's own partition");
        assert!(mgr.environments().is_empty(), "the table entry is gone");
        assert_eq!(mgr.pending_reclaims(), 0, "an idle environment reclaims inline");

        // Both sidecars were killed, and both names left the registry — the
        // kernel drops a sidecar's entry as the first step of its teardown.
        assert_eq!(k.killed_pids().len(), 2);
        assert_eq!(k.sidecar_count(), 0);
        assert_eq!(k.sidecar_pid("aerosls.posix.2", 6), 0);
        assert_eq!(k.sidecar_pid("drv.ramdisk.2", 6), 0);

        // All three regions came back, each charged to the tenant partition.
        // This is the assertion that bites if the release reverts to the
        // caller's partition (the E4 charging bug, at destroy time).
        let freed = k.freed_regions();
        assert_eq!(freed.len(), 3);
        assert_eq!(freed[0], (env.rd_heap, RD_HEAP_FRAMES, 6));
        assert_eq!(freed[1], (env.rd_storage, RD_STORAGE_FRAMES, 6));
        assert_eq!(freed[2], (env.px_heap, POSIX_HEAP_FRAMES, 6));
        assert!(freed.iter().all(|(_, _, p)| *p == env.partition));
    }

    /// E5: an unknown env_id is reported as such, not silently "succeeded".
    #[test]
    fn destroy_unknown_environment_reports_noent() {
        let k = SimKernel::new();
        let mut mgr = EnvManager::new(images(), 8);
        let mut reply = [0u8; 64];
        let dn = mgr.handle_request(&k, &destroy_req(7), &mut reply);
        assert_eq!(reply_status(&reply, dn).0, ENV_ERR_NOENT);
        assert_eq!(k.killed_pids().len(), 0, "nothing was killed");
        assert!(k.freed_regions().is_empty(), "and nothing was released");
    }

    /// E5's ordering rule, as a negative control: when the kernel DEFERS the
    /// kill (the target was RUNNING, so its teardown runs at the next
    /// schedule), the environment's frames must NOT be released. Releasing
    /// them while a sidecar can still execute in that heap is the
    /// use-after-free the deferral exists to avoid.
    #[test]
    fn destroy_defers_reclamation_while_a_sidecar_is_still_alive() {
        let k = SimKernel::new();
        let mut mgr = EnvManager::new(images(), 8);
        let mut reply = [0u8; 64];
        let n = mgr.handle_request(&k, &create_req(6, 2), &mut reply);
        let env_id = reply_status(&reply, n).1;
        let env = mgr.environments()[0].1;

        // Both sidecars are RUNNING at the moment of the kill.
        k.set_kill_deferred(true);
        let dn = mgr.handle_request(&k, &destroy_req(env_id), &mut reply);
        assert_eq!(reply_status(&reply, dn).0, ENV_OK,
                   "the environment is still ended, and the caller is told so");
        assert!(mgr.environments().is_empty(), "its table entry is released");
        assert_eq!(mgr.pending_reclaims(), 1, "but its memory is not");
        assert!(k.freed_regions().is_empty(),
                "NOTHING may be released while a sidecar can still run");
        assert_eq!(k.sidecar_count(), 2, "the deferred kill leaves them registered");

        // The next schedule finishes the teardown; the next request reaps.
        k.complete_deferred_kill();
        let n2 = mgr.handle_request(&k, &create_req(6, 3), &mut reply);
        assert_eq!(reply_status(&reply, n2).0, ENV_OK);
        assert_eq!(mgr.pending_reclaims(), 0, "the deferred reclaim completed");
        let freed = k.freed_regions();
        assert_eq!(freed.len(), 3, "the released regions are the destroyed one's");
        assert_eq!(freed[0], (env.rd_heap, RD_HEAP_FRAMES, 6));
        assert_eq!(freed[1], (env.rd_storage, RD_STORAGE_FRAMES, 6));
        assert_eq!(freed[2], (env.px_heap, POSIX_HEAP_FRAMES, 6));
    }

    /// E5's leak test, the shape the roadmap's verification plan calls for: N
    /// create → destroy cycles where N exceeds both PROC_MAX (16) and
    /// SIDECAR_REGISTRY_MAX (16). The table holds exactly ONE environment at a
    /// time, so every cycle after the first can only succeed if the previous
    /// destroy really released its slot — and the registry, the table and the
    /// released-frame ledger all advance by exactly the per-cycle amount, with
    /// nothing left live behind them.
    #[test]
    fn recycle_cycles_leak_no_slots_registry_entries_or_frames() {
        let k = SimKernel::new();
        let mut mgr = EnvManager::new(images(), 1);
        let mut reply = [0u8; 64];
        const CYCLES: u32 = 24; // > PROC_MAX 16 and > SIDECAR_REGISTRY_MAX 16
        for cycle in 0..CYCLES {
            let n = mgr.handle_request(&k, &create_req(6, cycle + 1), &mut reply);
            let (status, env_id, _) = reply_status(&reply, n);
            assert_eq!(status, ENV_OK, "cycle {}: create fits the single slot", cycle);
            assert_eq!(k.sidecar_count(), 2, "cycle {}: two sidecars live", cycle);

            let dn = mgr.handle_request(&k, &destroy_req(env_id), &mut reply);
            assert_eq!(reply_status(&reply, dn).0, ENV_OK, "cycle {}: destroy", cycle);
            assert_eq!(mgr.pending_reclaims(), 0, "cycle {}: reclaimed inline", cycle);
            assert_eq!(k.sidecar_count(), 0, "cycle {}: registry back to empty", cycle);
            assert!(mgr.environments().is_empty(), "cycle {}: slot released", cycle);
            // Exactly two kills and three releases per cycle — the ledger
            // grows by the cycle's own work and nothing else.
            assert_eq!(k.killed_pids().len() as u32, 2 * (cycle + 1));
            assert_eq!(k.freed_regions().len() as u32, 3 * (cycle + 1));
        }
        assert_eq!(k.sidecar_count(), 0, "no registry entry survives the loop");
        assert_eq!(mgr.pending_reclaims(), 0, "and nothing is left half-torn-down");
    }
}
