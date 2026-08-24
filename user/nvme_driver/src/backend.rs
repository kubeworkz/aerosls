//! The storage abstraction the RD_* server talks to: a `BlockBackend`.
//!
//! The server (`server.rs`) implements the whole protocol — handshake,
//! frame parsing, grant rights/size checks, geometry checks — without caring
//! what is behind the sectors. Two implementations exist:
//!
//!   * `SimBackend` — an arena-backed region standing in for storage on the
//!     host (the fake kernel's storage arena in the integration tests; a
//!     leaked box in unit tests). Byte-for-byte RAM semantics, the same way
//!     the ramdisk driver serves its storage cap.
//!   * `nvme::NvmeDevice` (feature `target`) — the real controller: MMIO
//!     doorbells, PRP lists, DMA. See `nvme.rs`.
//!
//! The protocol's sector unit is the 512-byte logical block (the IDL's
//! `total_sectors`, and NVMe's own LBA unit); the real backend maps that to
//! 4 KiB pages (8 sectors) internally. Per-request I/O is bounded by
//! `MAX_SECTORS` (64 sectors = 32 KiB), matching `aerosls_proto::MAX_IO`.

use crate::copy::copy_blocks;
use aerosls_proto::BLOCK_SIZE;

/// The sector size the protocol reports and operates in. NVMe logical blocks
/// are 512 bytes (the C driver's own arithmetic assumes this); the IDL's
/// `BlockInfo.sector_size` is fixed at 512 in v1.
pub const SECTOR_SIZE: u32 = BLOCK_SIZE;
/// Per-request sector bound (the RD_* protocol's `MAX_IO`).
pub const MAX_SECTORS: u32 = aerosls_proto::MAX_IO;

/// Backend-level I/O failures, mapped to `RD_ERR_*` codes by the server.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum BackendErr {
    /// Backing store failure (e.g. an NVMe status-code error, or the device
    /// never came up).
    Io,
    /// Device busy.
    Busy,
    /// Out of DMA/queue memory.
    NoMem,
    /// Write to a read-only device.
    Ro,
}

/// A durable direct map of the whole device, for the zero-copy `RD_MAP`
/// path. Only devices whose storage is directly addressable provide one
/// (RAM-backed sim/ramdisk); a real NVMe device is reachable only through
/// DMA commands, so it returns `None` and clients use READ/WRITE.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct MapGrant {
    /// A MEM cap in the driver's table covering the region.
    pub slot: u32,
    pub base: u64,
    pub len: u64,
    /// Whether the grant is writable (driver storage cap rights).
    pub writable: bool,
}

/// A block storage device the server can serve sectors from.
pub trait BlockBackend {
    /// Total addressable sectors (512-byte logical blocks).
    fn total_sectors(&self) -> u64;
    /// Whether the device refuses writes (reported in INFO flags and
    /// enforced before WRITE).
    fn read_only(&self) -> bool;

    /// Read `sectors` sectors starting at `lba` into the buffer at
    /// `dst_addr` (kernel-minted grant, `dst_len` bytes, already validated
    /// by the server to cover `sectors * 512`).
    fn read(&mut self, lba: u64, sectors: u32, dst_addr: u64, dst_len: usize)
        -> Result<(), BackendErr>;

    /// Write `sectors` sectors from the buffer at `src_addr`.
    fn write(
        &mut self,
        lba: u64,
        sectors: u32,
        src_addr: u64,
        src_len: usize,
    ) -> Result<(), BackendErr>;

    /// Durability barrier (NVMe NVM Flush). A completed `write` is only
    /// durable against power loss once this returns.
    fn flush(&mut self) -> Result<(), BackendErr>;

    /// `Some` when the device can be directly mapped (zero-copy path).
    fn map(&self) -> Option<MapGrant>;
}

/// Arena-backed RAM stand-in for storage, used by every host test. It is the
/// sidecar analogue of the ramdisk driver's storage-cap view: `base`/`len`
/// describe a region in the shared address space, and sector access is a
/// bounded byte copy in and out of it.
#[derive(Clone, Copy, Debug)]
pub struct SimBackend {
    base: u64,
    len: u64,
    writable: bool,
    /// When `Some`, RD_MAP replies carry a persist grant over the region via
    /// this driver-table MEM cap. `None` models the real NVMe device (not
    /// directly addressable).
    map_slot: Option<u32>,
}

impl SimBackend {
    pub fn new(base: u64, len: u64, writable: bool) -> SimBackend {
        SimBackend {
            base,
            len,
            writable,
            map_slot: None,
        }
    }

    /// Enable the zero-copy `RD_MAP` reply over `slot` (a driver-table MEM
    /// cap covering `[base, base+len)`).
    pub fn with_map_slot(mut self, slot: u32) -> SimBackend {
        self.map_slot = Some(slot);
        self
    }
}

impl BlockBackend for SimBackend {
    fn total_sectors(&self) -> u64 {
        self.len / BLOCK_SIZE as u64
    }

    fn read_only(&self) -> bool {
        !self.writable
    }

    fn read(
        &mut self,
        lba: u64,
        sectors: u32,
        dst_addr: u64,
        dst_len: usize,
    ) -> Result<(), BackendErr> {
        let bytes = sectors as usize * BLOCK_SIZE as usize;
        debug_assert!(dst_len >= bytes);
        // Safety: `src` is inside the backend's region (`lba + sectors` was
        // validated by the server against total_sectors); `dst` is inside a
        // kernel-minted grant the server checked covers `bytes`.
        unsafe {
            copy_blocks(
                (self.base + lba * BLOCK_SIZE as u64) as *const u8,
                dst_addr as *mut u8,
                bytes,
            );
        }
        Ok(())
    }

    fn write(
        &mut self,
        lba: u64,
        sectors: u32,
        src_addr: u64,
        src_len: usize,
    ) -> Result<(), BackendErr> {
        if !self.writable {
            return Err(BackendErr::Ro);
        }
        let bytes = sectors as usize * BLOCK_SIZE as usize;
        debug_assert!(src_len >= bytes);
        // Safety: `dst` is inside the backend's region; `src` is inside a
        // kernel-minted grant the server checked covers `bytes`.
        unsafe {
            copy_blocks(
                src_addr as *const u8,
                (self.base + lba * BLOCK_SIZE as u64) as *mut u8,
                bytes,
            );
        }
        Ok(())
    }

    fn flush(&mut self) -> Result<(), BackendErr> {
        // RAM: nothing to flush (mirrors the ramdisk's no-op).
        Ok(())
    }

    fn map(&self) -> Option<MapGrant> {
        self.map_slot.map(|slot| MapGrant {
            slot,
            base: self.base,
            len: self.len,
            writable: self.writable,
        })
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use aerosls_proto::R;
    use aerosls_proto::W;

    /// A leaky arena (stable address, never freed — fine for tests).
    fn arena(bytes: usize) -> (u64, u64) {
        let b = Box::leak(vec![0u8; bytes].into_boxed_slice());
        (b.as_ptr() as u64, b.len() as u64)
    }

    #[test]
    fn geometry() {
        let (base, len) = arena(8 * BLOCK_SIZE as usize);
        let d = SimBackend::new(base, len, true);
        assert_eq!(d.total_sectors(), 8);
        assert!(!d.read_only());
        let ro = SimBackend::new(base, len, false);
        assert!(ro.read_only());
    }

    #[test]
    fn read_write_roundtrip() {
        let (base, len) = arena(16 * BLOCK_SIZE as usize);
        let mut d = SimBackend::new(base, len, true);
        let (buf, _) = arena(4 * BLOCK_SIZE as usize);
        let pattern: Vec<u8> = (0..(4 * BLOCK_SIZE as usize)).map(|i| (i % 251) as u8).collect();
        unsafe {
            copy_blocks(pattern.as_ptr(), buf as *mut u8, pattern.len());
        }
        d.write(2, 4, buf, pattern.len()).unwrap();
        // Read it back into a fresh buffer and compare.
        let (out, out_len) = arena(4 * BLOCK_SIZE as usize);
        d.read(2, 4, out, out_len as usize).unwrap();
        let got: Vec<u8> = unsafe {
            core::slice::from_raw_parts(out as *const u8, out_len as usize).to_vec()
        };
        assert_eq!(got, pattern);
    }

    #[test]
    fn write_rejected_on_read_only() {
        let (base, len) = arena(8 * BLOCK_SIZE as usize);
        let mut d = SimBackend::new(base, len, false);
        let (buf, _) = arena(512);
        assert_eq!(d.write(0, 1, buf, 512), Err(BackendErr::Ro));
    }

    #[test]
    fn map_grant() {
        let (base, len) = arena(8 * BLOCK_SIZE as usize);
        let d = SimBackend::new(base, len, true).with_map_slot(7);
        let m = d.map().unwrap();
        assert_eq!(
            m,
            MapGrant {
                slot: 7,
                base,
                len,
                writable: true,
            }
        );
        // No map slot → None (the real NVMe behavior).
        let d2 = SimBackend::new(base, len, true);
        assert_eq!(d2.map(), None);
    }

    #[test]
    fn rights_bits_are_sane() {
        // Sanity anchor for the server's grant checks.
        assert_ne!(R, 0);
        assert_ne!(W, 0);
    }
}
