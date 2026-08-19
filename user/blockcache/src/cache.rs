//! The block cache — the ramdisk protocol *client* inside the POSIX sidecar.
//!
//! Implements the client side of `docs/AeroSLS-Ramdisk-Driver-Implementation-Plan-v0.1.md`
//! and the device half of the respawn state machine
//! (`docs/AeroSLS-Driver-Respawn-Spec-Decision-v0.1.md` §4.1):
//!
//! - **Handshake**: the first message on the endpoint is `RD_INFO` (the
//!   driver's implicit-handshake rule); the reply fixes the device geometry.
//! - **`RD_READ`/`RD_WRITE`**: one block per request (matching the Phase 2
//!   §3.4 walkthrough), with a transient MEM grant of a client-owned buffer
//!   — W-only for read buffers (the driver writes into them), R-only for
//!   write buffers (the driver reads from them). The kernel mints, bounds,
//!   and auto-revokes the grants; the driver re-checks rights and size.
//! - **`RD_MAP`**: a durable view of the whole device, used as the Phase 2
//!   §5.2 fast path for whole-file reads.
//! - **Stale handling**: a close event (peer dead / protocol) or a failed
//!   send flips the device to `STALE`; every subsequent op fails immediately
//!   and the mapped view is dropped ("drop on stale, always", respawn §6).
//!   Driver *error replies* (`RD_ERR_*`) do **not** stale the device — the
//!   device is alive, the request was bad.
//!
//! The cache is single-threaded and synchronous: at most one request is
//! outstanding on the endpoint by construction, which is exactly the
//! window=1 contract the kernel enforces (capability-layer spec §4.3).

use crate::copy;
use aerosls_proto::kabi::{GrantedCap, Kernel, RecvResult, SendCap, TIMEOUT_NONE};
use aerosls_proto::*;

/// Number of direct-mapped cache slots (blocks). The cache pool in the Phase
/// 2 budget map is 2 MiB (4096 × 512 B); v1 caches a fraction of that — the
/// geometry is a tuning knob, the protocol is not.
pub const NCACHE: usize = 8;

/// Device geometry as reported by the `RD_INFO` handshake.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct DeviceInfo {
    pub block_size: u32,
    pub blocks: u64,
    pub read_only: bool,
}

/// The device state (respawn decision §4.1). `Stale` is terminal for the
/// block cache; recovery is the respawn layer's job.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum State {
    Live,
    Stale { reason: u16, detail: u32 },
}

/// Client-side errors.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Error {
    /// The device is known dead (a close event was seen, or a send failed).
    /// `reason`/`detail` are the close-reason code and detail where known.
    Stale { reason: u16, detail: u32 },
    /// The driver replied with an `RD_ERR_*` status. The device is alive.
    Status(u16),
    /// Kernel-level failure (revoked, shutdown, ...).
    Kernel(i32),
    /// The driver sent something malformed or unexpected — a protocol
    /// violation. The device is treated as unreliable.
    Protocol,
}

/// A durable view of the whole device from `RD_MAP`: a persist MEM grant
/// minted by the kernel from the driver's storage cap, with lineage — it
/// dies with the driver (deep revocation), which is why the cache drops it
/// on stale.
///
/// Reads are raw memory copies (the shared address space); **valid only
/// while the device is `Live`**. The VFS must re-query `BlockCache::view()`
/// after a remount rather than hold a view across stale (respawn §6).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct MappedView {
    /// The persist grant's handle in the client table (revocation check).
    pub handle: u32,
    /// Base of the storage region in the shared address space.
    pub base: u64,
    /// Length of the region.
    pub len: u64,
    /// Rights the kernel actually minted (R for a read-only device).
    pub rights: u8,
}

impl MappedView {
    /// Read `dst.len()` bytes at `lba` directly from the view. The device
    /// must be `Live` (enforced by the block cache, not here).
    pub fn read(&self, lba: u64, dst: &mut [u8]) -> Result<(), Error> {
        let off = lba
            .checked_mul(BLOCK_SIZE as u64)
            .ok_or(Error::Protocol)?;
        let end = off
            .checked_add(dst.len() as u64)
            .ok_or(Error::Protocol)?;
        if end > self.len {
            return Err(Error::Status(RD_ERR_RANGE));
        }
        // Safety: `self.base` is the kernel-minted view; the range is
        // checked above against `self.len`.
        unsafe { copy::copy_from(self.base + off, dst) };
        Ok(())
    }
}

/// Buffer acquisition for request grants.
///
/// Every `RD_READ`/`RD_WRITE` needs a client-owned buffer to grant the
/// driver (W-only for reads, R-only for writes). The allocator owns how
/// those buffers come to be: on the real sidecar they are carved from the
/// budget MEM cap (a single reusable request buffer suffices — window=1);
/// in tests they are fake-kernel regions.
pub trait BufferAlloc {
    /// Allocate `len` bytes of client-owned memory. Returns the grant
    /// descriptor describing the region (rights held; the caller requests a
    /// subset on the wire) and the region's base address.
    fn alloc(&mut self, len: usize) -> Result<(SendCap, u64), i32>;
}

#[derive(Clone, Copy, Debug)]
struct Slot {
    lba: u64,
    valid: bool,
    data: [u8; BLOCK_SIZE as usize],
}

impl Slot {
    const fn new() -> Slot {
        Slot {
            lba: 0,
            valid: false,
            data: [0u8; BLOCK_SIZE as usize],
        }
    }
}

/// The block cache. Generic over `K: Kernel` (host tests use the fake,
/// the sidecar image uses the real ABI) and `A: BufferAlloc`.
pub struct BlockCache<K: Kernel, A: BufferAlloc> {
    k: K,
    chan: u32,
    alloc: A,
    next_tag: u32,
    info: DeviceInfo,
    state: State,
    view: Option<MappedView>,
    slots: [Slot; NCACHE],
}

impl<K: Kernel, A: BufferAlloc> BlockCache<K, A> {
    /// Connect and handshake: `RD_INFO` must be the first message on the
    /// endpoint (the driver's implicit-handshake rule); on success the
    /// device is `Live` with its geometry fixed.
    pub fn connect(k: K, chan: u32, alloc: A) -> Result<Self, Error> {
        let mut bc = BlockCache {
            k,
            chan,
            alloc,
            next_tag: 1,
            info: DeviceInfo {
                block_size: 0,
                blocks: 0,
                read_only: false,
            },
            state: State::Live,
            view: None,
            slots: [Slot::new(); NCACHE],
        };
        let tag = bc.next_tag();
        bc.k
            .send(
                bc.chan,
                tag,
                0,
                &RdFrame::new(RD_INFO, false).encode(),
                &[],
                TIMEOUT_NONE,
            )
            .map_err(|e| bc.kernel_fail(e))?;
        let mut buf = [0u8; 64];
        let mut caps = [GrantedCap::default(); 1];
        let (rr, frame) = bc.recv_reply(tag, &mut buf, &mut caps, RD_INFO)?;
        if rr.n_caps != 0 {
            // RD_INFO must never carry caps.
            return Err(bc.proto_fail());
        }
        if frame.is_error() {
            let (status, _) = parse_status_body(&buf[16..rr.len]).ok_or_else(|| bc.proto_fail())?;
            return Err(Error::Status(status));
        }
        let (block_size, blocks, flags) =
            parse_info_body(&buf[16..rr.len]).ok_or_else(|| bc.proto_fail())?;
        if block_size != BLOCK_SIZE || blocks == 0 {
            // Protocol v1 fixes the block size; a zero-block device is
            // unusable.
            return Err(bc.proto_fail());
        }
        bc.info = DeviceInfo {
            block_size,
            blocks,
            read_only: flags & 1 != 0,
        };
        Ok(bc)
    }

    pub fn state(&self) -> State {
        self.state
    }

    pub fn info(&self) -> DeviceInfo {
        self.info
    }

    pub fn blocks(&self) -> u64 {
        self.info.blocks
    }

    /// The mapped view, if one was established and the device is still
    /// `Live`. `None` once stale (the view is dropped on stale).
    pub fn view(&self) -> Option<MappedView> {
        self.view
    }

    /// Read one block into `dst` (exactly `BLOCK_SIZE` bytes). Served from
    /// the cache when present; a miss issues `RD_READ` with a W-only
    /// transient grant of a fresh buffer, then copies the block in.
    pub fn read_block(&mut self, lba: u64, dst: &mut [u8; BLOCK_SIZE as usize]) -> Result<(), Error> {
        self.ensure_live()?;
        self.check_range(lba, 1)?;
        let idx = (lba % NCACHE as u64) as usize;
        if self.slots[idx].valid && self.slots[idx].lba == lba {
            dst.copy_from_slice(&self.slots[idx].data);
            return Ok(());
        }

        let (grant, base) = self
            .alloc
            .alloc(BLOCK_SIZE as usize)
            .map_err(Error::Kernel)?;
        let grant = SendCap {
            rights: W,
            ..grant
        };
        let tag = self.next_tag();
        let mut req = [0u8; 28];
        req[..16].copy_from_slice(&RdFrame::new(RD_READ, false).encode());
        req[16..28].copy_from_slice(&encode_rw_body(lba, 1));
        self.k
            .send(self.chan, tag, 0, &req, &[grant], TIMEOUT_NONE)
            .map_err(|e| self.kernel_fail(e))?;

        let mut buf = [0u8; ChanHeader::MAX_PAYLOAD];
        let mut caps = [GrantedCap::default(); 1];
        let (rr, frame) = self.recv_reply(tag, &mut buf, &mut caps, RD_READ)?;
        if rr.n_caps != 0 {
            return Err(self.proto_fail());
        }
        let (status, bytes) = self.status_of(&frame, &buf[16..rr.len])?;
        if status != RD_OK {
            // Driver error reply (e.g. RD_ERR_RANGE): device is alive.
            return Err(Error::Status(status));
        }
        if bytes != BLOCK_SIZE as u64 {
            return Err(self.proto_fail());
        }
        // The driver wrote the block into our buffer; copy it into the slot
        // (memmove semantics — in shared space the buffer could alias the
        // cache slot in principle).
        // Safety: `base` is the allocator's buffer; `BLOCK_SIZE` bytes.
        unsafe { copy::copy_from(base, &mut self.slots[idx].data) };
        self.slots[idx].lba = lba;
        self.slots[idx].valid = true;
        dst.copy_from_slice(&self.slots[idx].data);
        Ok(())
    }

    /// Read `n = dst.len() / BLOCK_SIZE` whole blocks starting at `lba`.
    /// v1 issues one single-block request per block (matching the Phase 2
    /// walkthrough); whole-file reads take the `RD_MAP` fast path instead.
    pub fn read(&mut self, lba: u64, dst: &mut [u8]) -> Result<(), Error> {
        self.ensure_live()?;
        let n = dst.len() / BLOCK_SIZE as usize;
        if n == 0 || dst.len() % BLOCK_SIZE as usize != 0 {
            return Err(Error::Status(RD_ERR_INVAL));
        }
        self.check_range(lba, n as u64)?;
        for i in 0..n {
            let mut block = [0u8; BLOCK_SIZE as usize];
            self.read_block(lba + i as u64, &mut block)?;
            let off = i * BLOCK_SIZE as usize;
            dst[off..off + BLOCK_SIZE as usize].copy_from_slice(&block);
        }
        Ok(())
    }

    /// Write one block from `src`. The buffer is granted R-only (the driver
    /// reads from it); on success the cache slot for `lba` is invalidated
    /// (the data changed under the cache; refetch on the next read).
    pub fn write_block(&mut self, lba: u64, src: &[u8; BLOCK_SIZE as usize]) -> Result<(), Error> {
        self.ensure_live()?;
        self.check_range(lba, 1)?;
        let (grant, base) = self
            .alloc
            .alloc(BLOCK_SIZE as usize)
            .map_err(Error::Kernel)?;
        // Safety: `base` is the allocator's buffer; `BLOCK_SIZE` bytes.
        unsafe { copy::copy_to(base, src) };
        let grant = SendCap {
            rights: R,
            ..grant
        };
        let tag = self.next_tag();
        let mut req = [0u8; 28];
        req[..16].copy_from_slice(&RdFrame::new(RD_WRITE, false).encode());
        req[16..28].copy_from_slice(&encode_rw_body(lba, 1));
        self.k
            .send(self.chan, tag, 0, &req, &[grant], TIMEOUT_NONE)
            .map_err(|e| self.kernel_fail(e))?;

        let mut buf = [0u8; ChanHeader::MAX_PAYLOAD];
        let mut caps = [GrantedCap::default(); 1];
        let (rr, frame) = self.recv_reply(tag, &mut buf, &mut caps, RD_WRITE)?;
        if rr.n_caps != 0 {
            return Err(self.proto_fail());
        }
        let (status, _bytes) = self.status_of(&frame, &buf[16..rr.len])?;
        if status != RD_OK {
            return Err(Error::Status(status));
        }
        self.slots[(lba % NCACHE as u64) as usize].valid = false;
        Ok(())
    }

    /// Write `n = src.len() / BLOCK_SIZE` whole blocks starting at `lba`.
    pub fn write(&mut self, lba: u64, src: &[u8]) -> Result<(), Error> {
        self.ensure_live()?;
        let n = src.len() / BLOCK_SIZE as usize;
        if n == 0 || src.len() % BLOCK_SIZE as usize != 0 {
            return Err(Error::Status(RD_ERR_INVAL));
        }
        self.check_range(lba, n as u64)?;
        for i in 0..n {
            let off = i * BLOCK_SIZE as usize;
            let block: [u8; BLOCK_SIZE as usize] = src[off..off + BLOCK_SIZE as usize]
                .try_into()
                .expect("slice length is BLOCK_SIZE");
            self.write_block(lba + i as u64, &block)?;
        }
        Ok(())
    }

    /// `RD_FLUSH` (a no-op for the RAM backend; the surface is stable for a
    /// future cached backend).
    pub fn flush(&mut self) -> Result<(), Error> {
        self.ensure_live()?;
        let tag = self.next_tag();
        self.k
            .send(
                self.chan,
                tag,
                0,
                &RdFrame::new(RD_FLUSH, false).encode(),
                &[],
                TIMEOUT_NONE,
            )
            .map_err(|e| self.kernel_fail(e))?;
        let mut buf = [0u8; 64];
        let mut caps = [GrantedCap::default(); 1];
        let (rr, frame) = self.recv_reply(tag, &mut buf, &mut caps, RD_FLUSH)?;
        if rr.n_caps != 0 {
            return Err(self.proto_fail());
        }
        let (status, _) = self.status_of(&frame, &buf[16..rr.len])?;
        if status != RD_OK {
            return Err(Error::Status(status));
        }
        Ok(())
    }

    /// Observe a close event already queued on the endpoint — the
    /// respawn-decision §5 steps 1–3 path where the kernel delivered the
    /// close while no request was in flight (the sidecar event loop would
    /// call this on wake; here the VFS polls before every operation). If a
    /// close is queued it is consumed and the device marked stale ("drop on
    /// stale, always"); a cache hit must never come from a dead device.
    /// `Ok(())` otherwise (no event, or an MSG/NEW_CHANNEL the cache does
    /// not act on).
    pub fn poll_dead(&mut self) -> Result<(), Error> {
        match self.k.poll(self.chan).map_err(|e| self.kernel_fail(e))? {
            CH_KIND_CLOSE => {
                let mut buf = [0u8; 64];
                let mut caps = [GrantedCap::default(); 1];
                let rr = self
                    .k
                    .recv(self.chan, &mut buf, &mut caps)
                    .map_err(|e| self.kernel_fail(e))?;
                let (reason, detail) =
                    parse_close_body(&buf[..rr.len]).unwrap_or((CLOSE_PEER, 0));
                Err(self.close_fail(reason, detail))
            }
            _ => Ok(()),
        }
    }

    /// `RD_MAP`: establish a durable view of the whole device. The reply
    /// carries a persist MEM grant derived from the driver's storage cap;
    /// the view is stored and returned. It dies with the driver (lineage),
    /// and the cache drops it when the device goes stale.
    pub fn map(&mut self) -> Result<MappedView, Error> {
        self.ensure_live()?;
        let tag = self.next_tag();
        self.k
            .send(
                self.chan,
                tag,
                0,
                &RdFrame::new(RD_MAP, false).encode(),
                &[],
                TIMEOUT_NONE,
            )
            .map_err(|e| self.kernel_fail(e))?;
        let mut buf = [0u8; 64];
        let mut caps = [GrantedCap::default(); 1];
        let (rr, frame) = self.recv_reply(tag, &mut buf, &mut caps, RD_MAP)?;
        let (status, _) = self.status_of(&frame, &buf[16..rr.len])?;
        if status != RD_OK {
            return Err(Error::Status(status));
        }
        let g = caps
            .get(0)
            .filter(|_| rr.n_caps == 1)
            .ok_or_else(|| self.proto_fail())?;
        if g.rights & R == 0 || g.len < BLOCK_SIZE as u64 {
            return Err(self.proto_fail());
        }
        let view = MappedView {
            handle: g.handle,
            base: g.base,
            len: g.len,
            rights: g.rights,
        };
        self.view = Some(view);
        Ok(view)
    }

    // ── internals ─────────────────────────────────────────────────────────────

    fn next_tag(&mut self) -> u32 {
        let t = self.next_tag;
        self.next_tag = self.next_tag.wrapping_add(1);
        t
    }

    fn ensure_live(&self) -> Result<(), Error> {
        match self.state {
            State::Live => Ok(()),
            State::Stale { reason, detail } => Err(Error::Stale { reason, detail }),
        }
    }

    fn check_range(&self, lba: u64, n: u64) -> Result<(), Error> {
        let end = lba.checked_add(n).ok_or(Error::Status(RD_ERR_RANGE))?;
        if end > self.info.blocks {
            return Err(Error::Status(RD_ERR_RANGE));
        }
        Ok(())
    }

    /// Receive the reply to the outstanding request and validate the
    /// envelope: it must be an `F_REPLY` MSG echoing our tag, with a
    /// well-formed frame of the expected type. Anything else — a close event
    /// (peer died), a driver-initiated request, a mismatched tag, garbage —
    /// is a device-level failure and stales the cache.
    fn recv_reply(
        &mut self,
        tag: u32,
        buf: &mut [u8],
        caps: &mut [GrantedCap],
        expect_ty: u16,
    ) -> Result<(RecvResult, RdFrame), Error> {
        let rr = self
            .k
            .recv(self.chan, buf, caps)
            .map_err(|e| self.kernel_fail(e))?;
        match rr.kind {
            CH_KIND_MSG => {
                if rr.tag != tag || (rr.flags & F_REPLY) == 0 {
                    return Err(self.proto_fail());
                }
                let frame = RdFrame::parse(&buf[..rr.len]).ok_or_else(|| self.proto_fail())?;
                if frame.magic != RD_MAGIC
                    || frame.version != RD_VERSION
                    || frame.ty != expect_ty
                {
                    return Err(self.proto_fail());
                }
                Ok((rr, frame))
            }
            CH_KIND_CLOSE => {
                let (reason, detail) =
                    parse_close_body(&buf[..rr.len]).unwrap_or((CLOSE_PEER, 0));
                Err(self.close_fail(reason, detail))
            }
            // NEW_CHANNEL on a data endpoint, or anything else: kernel bug.
            _ => Err(self.proto_fail()),
        }
    }

    /// Parse the status body of a validated reply. The driver sets the error
    /// flag iff `status != RD_OK`; a disagreement is an inconsistent driver
    /// (protocol violation → stale). A truncated body is likewise a protocol
    /// violation. A clean `RD_ERR_*` reply is returned as-is — the device is
    /// alive, the request was bad.
    fn status_of(&mut self, frame: &RdFrame, body: &[u8]) -> Result<(u16, u64), Error> {
        let (status, bytes) = parse_status_body(body).ok_or_else(|| self.proto_fail())?;
        if frame.is_error() != (status != RD_OK) {
            return Err(self.proto_fail());
        }
        Ok((status, bytes))
    }

    /// The device is gone (close event, failed send, or kernel error):
    /// mark stale, drop the mapped view ("drop on stale, always"), and
    /// surface the failure.
    fn close_fail(&mut self, reason: u16, detail: u32) -> Error {
        self.mark_stale(reason, detail);
        Error::Stale { reason, detail }
    }

    fn kernel_fail(&mut self, e: i32) -> Error {
        self.mark_stale(CLOSE_PEER, 0);
        Error::Kernel(e)
    }

    /// A protocol violation by the driver: the device is unreliable.
    fn proto_fail(&mut self) -> Error {
        self.mark_stale(CLOSE_PROTO, 0);
        Error::Protocol
    }

    fn mark_stale(&mut self, reason: u16, detail: u32) {
        self.state = State::Stale { reason, detail };
        self.view = None;
    }
}
