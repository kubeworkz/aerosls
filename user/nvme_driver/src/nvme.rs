//! The real NVMe backend (cargo feature `target`): a Rust port of
//! `drivers/nvme.c` (controller bring-up), `drivers/nvme_admin.c` (admin
//! command submission + Identify Namespace), and `drivers/nvme_io.c`
//! (I/O queues, PRP submission, flush). The channel/protocol layer above is
//! shared and host-tested; this module is the part that talks to hardware
//! and therefore cannot run on a host (the C driver's own header comment
//! says the same of nvme_io.c — no test there links it).
//!
//! What the sidecar changes vs. the C kernel driver:
//!
//!   * MMIO base comes from a capability, not a hardcoded BAR: the manifest
//!     grants `bar0` (the controller's BAR0 region) and `dma` (physical RAM
//!     for queues, the PRP list page, and bounce buffers). In the shared
//!     address-space MVP, `cap_info().base` IS the physical address, so
//!     registers are dereferenced directly and DMA addresses go into PRPs
//!     as-is — the same identity mapping the C driver relied on.
//!   * Queue/bounce frames come from the `dma` MEM region via `FramePool`
//!     instead of `allocate_physical_ram_frame()`.
//!   * The admin-path `cli`/`sti` around the completion poll is dropped:
//!     that was a boot-time-only safety measure against timer-ISR reentrancy
//!     in the kernel's global context. A sidecar has no such context; the
//!     poll loop uses the portable `core::hint::spin_loop()` and the same
//!     bounded timeout as the C code (500M iterations), so a wedged
//!     controller fails a request instead of hanging the sidecar forever.
//!   * Timeouts return a distinct out-of-band status (0xFF, like
//!     `nvme_io_submit_sync`), never a real NVMe status byte — the C admin
//!     path's "zero CQE on timeout" made a timeout look like success.
//!
//! Geometry: 512-byte logical blocks (the protocol's sector unit), issued to
//! the controller as 4 KiB pages (8 sectors). Per-request sector counts are
//! bounded by `MAX_SECTORS` (64) = at most 8 pages, so every transfer is
//! ONE command with a single PRP list — never more than
//! `NVME_MAX_PAGES_PER_XFER` (32).

use core::ptr::{read_volatile, write_volatile};

use crate::backend::{BackendErr, BlockBackend, MapGrant, MAX_SECTORS};
use crate::copy::copy_blocks;
use crate::framepool::FramePool;
use crate::prp::{build_prp, NVME_PAGE_SIZE, NVME_SECTORS_PER_PAGE};
use aerosls_proto::BLOCK_SIZE;

// ── controller constants (drivers/nvme.h, nvme_admin.h, nvme_io.h) ──────────

/// NVMe MMIO register offsets.
const REG_CAP: u64 = 0x0000;
const REG_CC: u64 = 0x0014;
const REG_CSTS: u64 = 0x001C;
const REG_AQA: u64 = 0x0024;
const REG_ASQ: u64 = 0x0028;
const REG_ACQ: u64 = 0x0030;

/// Admin queue size (entries), matching `ADMIN_QUEUE_SIZE`.
const ADMIN_QUEUE_SIZE: u32 = 64;
/// I/O queue id + size (drivers/nvme_io.h `NVME_IO_QUEUE_ID` / `_SIZE`).
const IO_QUEUE_ID: u32 = 1;
const IO_QUEUE_SIZE: u32 = 64;
/// Namespace this driver brings up (single-namespace scope, like the C code).
const NSID: u32 = 1;

/// Admin opcodes.
const ADMIN_CREATE_CQ: u8 = 0x05;
const ADMIN_CREATE_SQ: u8 = 0x01;
const ADMIN_IDENTIFY: u8 = 0x06;
/// NVM command opcodes.
const NVM_FLUSH: u8 = 0x00;
const NVM_WRITE: u8 = 0x01;
const NVM_READ: u8 = 0x02;

/// Bounded completion poll, ~500 ms at 1 GHz (500M iterations), matching
/// `NVME_ADMIN_TIMEOUT` / `NVME_IO_TIMEOUT` in the C driver.
const POLL_TIMEOUT: u64 = 500_000_000;
/// Out-of-band status returned on poll timeout — never a real NVMe status
/// code byte (mirrors nvme_io.c's 0xFF).
const TIMEOUT_STATUS: u16 = 0xFF;

/// Doorbell registers start at `mmio_base + 0x1000`; admin SQ=0, admin CQ=1,
/// I/O SQ 1=2, I/O CQ 1=3 — each `stride` bytes apart.
const DOORBELL_OFFSET: u64 = 0x1000;

// ── wire structs (64-byte command / 16-byte completion, as in nvme_admin.h) ─

/// The 64-byte NVMe command dword. `repr(C)`'s natural layout is exactly the
/// packed C layout (2+2+4, then 8-byte fields, then 6 u32s — no padding
/// either way), asserted below.
#[repr(C)]
#[derive(Clone, Copy, Default)]
struct NvmeCmd {
    opcode: u8,
    flags: u8,
    command_id: u16,
    nsid: u32,
    reserved0: u64,
    metadata: u64,
    prp1: u64,
    prp2: u64,
    cdw10: u32,
    cdw11: u32,
    cdw12: u32,
    cdw13: u32,
    cdw14: u32,
    cdw15: u32,
}

const _: () = assert!(core::mem::size_of::<NvmeCmd>() == 64);

/// The 16-byte NVMe completion queue entry. Bit 0 of `status` is the phase
/// tag; bits 15:1 carry the status code.
#[repr(C)]
#[derive(Clone, Copy, Default)]
struct NvmeCqe {
    result: u32,
    reserved: u32,
    sq_head: u16,
    sq_id: u16,
    command_id: u16,
    status: u16,
}

const _: () = assert!(core::mem::size_of::<NvmeCqe>() == 16);

// ── MMIO register access ─────────────────────────────────────────────────────

struct NvmeRegs {
    base: u64,
    /// Doorbell register stride in bytes (CAP bits 32:35 → 4 << stride).
    stride: u32,
}

impl NvmeRegs {
    fn read32(&self, off: u64) -> u32 {
        // Safety: `base` is the BAR0 MEM cap's base; `off` is a register
        // offset within BAR0 (all offsets here are well below the 8 KiB
        // cap size).
        unsafe { read_volatile((self.base + off) as *const u32) }
    }

    fn write32(&self, off: u64, v: u32) {
        // Safety: same region as read32.
        unsafe { write_volatile((self.base + off) as *mut u32, v) }
    }

    fn write64(&self, off: u64, v: u64) {
        // Safety: same region as read32.
        unsafe { write_volatile((self.base + off) as *mut u64, v) }
    }

    /// Ring doorbell `idx` (0 = admin SQ, 1 = admin CQ, 2/3 = I/O SQ/CQ).
    fn ring_doorbell(&self, idx: u32, value: u32) {
        self.write32(DOORBELL_OFFSET + idx as u64 * self.stride as u64, value);
    }
}

// ── the device ───────────────────────────────────────────────────────────────

/// The full NVMe controller state behind `BlockBackend`. Owns the queue
/// frames and PRP list page (allocated from the `dma` region) and the
/// submit-path bookkeeping (tails, heads, phase tags, command ids).
pub struct NvmeDevice {
    regs: NvmeRegs,
    // admin queue
    admin_sq: u64,
    admin_cq: u64,
    admin_sq_tail: u16,
    admin_cq_head: u16,
    admin_phase: u16,
    cmd_id: u16,
    // io queue
    io_sq: u64,
    io_cq: u64,
    io_sq_tail: u16,
    io_cq_head: u16,
    io_phase: u16,
    io_cmd_id: u16,
    /// One 4 KiB page holding up to 512 PRP list entries, reused by every
    /// transfer — safe because the driver is strictly synchronous (one
    /// command outstanding at a time), so no second transfer can be building
    /// a list while the controller is still reading this one.
    prp_list: u64,
    /// Frames for queues / PRP list / bounce buffers, carved from the `dma`
    /// MEM cap.
    pool: FramePool,
    /// From Identify Namespace (NCAP, in 512-byte logical blocks).
    total_sectors: u64,
    /// True after bring-up + I/O queue creation succeeded.
    ready: bool,
}

impl NvmeDevice {
    /// Bring the controller up: disable, program admin queues, enable, query
    /// capacity, create the I/O queue pair. `bar0` is the BAR0 MMIO base
    /// (from the `bar0` MEM cap); `dma_base`/`dma_len` is the `dma` MEM cap.
    pub fn new(bar0: u64, dma_base: u64, dma_len: u64) -> Result<NvmeDevice, BackendErr> {
        let mut pool = FramePool::new();
        pool.init(dma_base, dma_len);

        // Doorbell stride from CAP bits 32:35 (drivers/nvme.c).
        let cap = unsafe { read_volatile((bar0 + REG_CAP) as *const u64) };
        let stride = 4u32 << ((cap >> 32) & 0xF);

        let mut dev = NvmeDevice {
            regs: NvmeRegs { base: bar0, stride },
            admin_sq: 0,
            admin_cq: 0,
            admin_sq_tail: 0,
            admin_cq_head: 0,
            admin_phase: 1,
            cmd_id: 0,
            io_sq: 0,
            io_cq: 0,
            io_sq_tail: 0,
            io_cq_head: 0,
            io_phase: 1,
            io_cmd_id: 0x80, // high range avoids any admin clash
            prp_list: 0,
            pool,
            total_sectors: 0,
            ready: false,
        };
        dev.bring_up()?;
        Ok(dev)
    }

    /// Controller bring-up — port of `init_nvme_controller()` (drivers/nvme.c)
    /// followed by Identify Namespace and the I/O queue pair.
    fn bring_up(&mut self) -> Result<(), BackendErr> {
        // 1. If the controller is active, shut it down so we can program the
        //    admin queue structures.
        let mut cc = self.regs.read32(REG_CC);
        if cc & (1 << 0) != 0 {
            cc &= !(1 << 0); // clear EN
            self.regs.write32(REG_CC, cc);
        }
        while self.regs.read32(REG_CSTS) & (1 << 0) != 0 {
            core::hint::spin_loop(); // wait RDY low (disabled)
        }

        // 2. Allocate + zero the admin queue frames.
        let sq = self.pool.alloc_page().ok_or(BackendErr::NoMem)?;
        let cq = self.pool.alloc_page().ok_or(BackendErr::NoMem)?;
        self.zero_page(sq);
        self.zero_page(cq);
        self.admin_sq = sq;
        self.admin_cq = cq;

        // 3. Program AQA (queue sizes, 0-indexed) and the queue bases.
        let aqa = ((ADMIN_QUEUE_SIZE - 1) << 16) | (ADMIN_QUEUE_SIZE - 1);
        self.regs.write32(REG_AQA, aqa);
        self.regs.write64(REG_ASQ, sq);
        self.regs.write64(REG_ACQ, cq);

        // 4. Enable: I/O command set NVM, 4 KiB page size, 64-byte SQ entries
        //    (SQES=6), 16-byte CQ entries (CQES=4), EN=1 — exactly the C
        //    driver's `(4 << 20) | (6 << 16) | (0 << 7) | (0 << 4) | (1 << 0)`.
        cc = (4 << 20) | (6 << 16) | (1 << 0);
        self.regs.write32(REG_CC, cc);
        while self.regs.read32(REG_CSTS) & (1 << 0) == 0 {
            core::hint::spin_loop(); // wait RDY high (ready)
        }

        // 5. Capacity from Identify Namespace (NCAP logical blocks).
        self.total_sectors = self.identify_namespace(NSID)?;

        // 6. I/O queue pair (Create CQ / Create SQ) + PRP list page — port of
        //    `nvme_io_init()` (drivers/nvme_io.c).
        self.io_queue_init()?;

        self.ready = true;
        Ok(())
    }

    /// Port of `nvme_identify_namespace()` (drivers/nvme_admin.c): CNS=1,
    /// read NCAP (logical blocks) from the returned 4 KiB data structure.
    /// Assumes 512-byte logical blocks, same as the C driver.
    fn identify_namespace(&mut self, nsid: u32) -> Result<u64, BackendErr> {
        let scratch = self.pool.alloc_page().ok_or(BackendErr::NoMem)?;
        self.zero_page(scratch);

        let mut cmd = NvmeCmd::default();
        cmd.opcode = ADMIN_IDENTIFY;
        cmd.nsid = nsid;
        cmd.prp1 = scratch;
        cmd.cdw10 = 0x0000_0001; // CNS=1 (Identify Namespace)
        if self.submit_admin(&mut cmd) != 0 {
            return Err(BackendErr::Io);
        }
        // NCAP at byte offset 8 (NVMe spec, Identify Namespace structure).
        let ncap = unsafe { read_volatile((scratch + 8) as *const u64) };
        Ok(ncap)
    }

    /// Port of `nvme_io_init()`: Create I/O CQ (admin 0x05), Create I/O SQ
    /// (admin 0x01), then allocate the shared PRP list page.
    fn io_queue_init(&mut self) -> Result<(), BackendErr> {
        let sq = self.pool.alloc_page().ok_or(BackendErr::NoMem)?;
        let cq = self.pool.alloc_page().ok_or(BackendErr::NoMem)?;
        self.zero_page(sq);
        self.zero_page(cq);

        // Create CQ (qid 1, size 64, physically contiguous, no interrupts).
        let mut c = NvmeCmd::default();
        c.opcode = ADMIN_CREATE_CQ;
        c.prp1 = cq;
        c.cdw10 = ((IO_QUEUE_SIZE - 1) << 16) | IO_QUEUE_ID;
        c.cdw11 = 0x0000_0001;
        if self.submit_admin(&mut c) != 0 {
            return Err(BackendErr::Io);
        }

        // Create SQ (qid 1, CQID=1 in cdw11 bits 31:16, PC=1).
        let mut s = NvmeCmd::default();
        s.opcode = ADMIN_CREATE_SQ;
        s.prp1 = sq;
        s.cdw10 = ((IO_QUEUE_SIZE - 1) << 16) | IO_QUEUE_ID;
        s.cdw11 = (IO_QUEUE_ID << 16) | 0x0000_0001;
        if self.submit_admin(&mut s) != 0 {
            return Err(BackendErr::Io);
        }

        self.io_sq = sq;
        self.io_cq = cq;

        // PRP list page — failure is fatal here (every transfer > 2 pages
        // needs it, and the server can issue up to 8).
        let pl = self.pool.alloc_page().ok_or(BackendErr::NoMem)?;
        self.zero_page(pl);
        self.prp_list = pl;
        Ok(())
    }

    /// Submit one admin command and poll for completion — port of
    /// `nvme_submit_admin_cmd()` (drivers/nvme_admin.c), minus the cli/sti
    /// (see module docs). Returns the status code (0 = success), or
    /// `TIMEOUT_STATUS` on a bounded poll timeout.
    fn submit_admin(&mut self, cmd: &mut NvmeCmd) -> u16 {
        cmd.command_id = self.cmd_id;
        self.cmd_id = self.cmd_id.wrapping_add(1);

        let sq = self.admin_sq as *mut NvmeCmd;
        // Safety: `admin_sq_tail < ADMIN_QUEUE_SIZE`; the frame is DMA-able
        // memory the controller reads concurrently — volatile write.
        unsafe { write_volatile(sq.add(self.admin_sq_tail as usize), *cmd) };
        self.admin_sq_tail = (self.admin_sq_tail + 1) % ADMIN_QUEUE_SIZE as u16;
        self.regs.ring_doorbell(0, self.admin_sq_tail as u32);

        let cq = self.admin_cq as *const NvmeCqe;
        let head = self.admin_cq_head;
        let cqe = Self::poll_completion(cq, head, &mut self.admin_phase);
        let status = cqe.map(|c| c.status).unwrap_or(TIMEOUT_STATUS);

        if cqe.is_some() {
            self.admin_cq_head = (self.admin_cq_head + 1) % ADMIN_QUEUE_SIZE as u16;
            self.regs.ring_doorbell(1, self.admin_cq_head as u32);
        }
        (status >> 1) & 0xFF
    }

    /// Submit one I/O command and poll — port of `nvme_io_submit_sync()`
    /// (drivers/nvme_io.c). Same contract as `submit_admin`.
    fn submit_io(&mut self, cmd: &mut NvmeCmd) -> u16 {
        cmd.command_id = self.io_cmd_id;
        self.io_cmd_id = self.io_cmd_id.wrapping_add(1);

        let sq = self.io_sq as *mut NvmeCmd;
        // Safety: as submit_admin.
        unsafe { write_volatile(sq.add(self.io_sq_tail as usize), *cmd) };
        self.io_sq_tail = (self.io_sq_tail + 1) % IO_QUEUE_SIZE as u16;
        self.regs.ring_doorbell(2, self.io_sq_tail as u32);

        let cq = self.io_cq as *const NvmeCqe;
        let head = self.io_cq_head;
        let cqe = Self::poll_completion(cq, head, &mut self.io_phase);
        let status = cqe.map(|c| c.status).unwrap_or(TIMEOUT_STATUS);

        if cqe.is_some() {
            self.io_cq_head = (self.io_cq_head + 1) % IO_QUEUE_SIZE as u16;
            if self.io_cq_head == 0 {
                self.io_phase ^= 1;
            }
            self.regs.ring_doorbell(3, self.io_cq_head as u32);
        }
        (status >> 1) & 0xFF
    }

    /// Poll one completion-queue slot for the expected phase tag, bounded by
    /// `POLL_TIMEOUT`. Returns `None` on timeout (the phase tag never
    /// flipped). The C code's `pause()` loop is `core::hint::spin_loop()`.
    //
    // A free function on purpose: the callers hold `cq`/`head` as copies and
    // pass `&mut self.<phase>`, which would conflict with a `&self` receiver.
    fn poll_completion(
        cq: *const NvmeCqe,
        head: u16,
        phase: &mut u16,
    ) -> Option<NvmeCqe> {
        let mut deadline = POLL_TIMEOUT;
        loop {
            // Safety: `head < queue size`; the frame is DMA-able memory the
            // controller writes concurrently — volatile read.
            let cqe = unsafe { read_volatile(cq.add(head as usize)) };
            if cqe.status & 0x1 == *phase {
                return Some(cqe);
            }
            core::hint::spin_loop();
            if deadline == 0 {
                return None;
            }
            deadline -= 1;
        }
    }

    /// Zero a 4 KiB DMA frame.
    fn zero_page(&self, addr: u64) {
        // Safety: addr is a pool frame (valid, writable, 4 KiB).
        unsafe {
            let p = addr as *mut u32;
            for i in 0..(NVME_PAGE_SIZE as usize / 4) {
                write_volatile(p.add(i), 0);
            }
        }
    }

    /// The PRP list page as a mutable slice (512 entries).
    fn prp_list_mut(&mut self) -> &mut [u64] {
        // Safety: prp_list is a pool frame (valid, writable, 4 KiB = 512 u64s).
        unsafe { core::slice::from_raw_parts_mut(self.prp_list as *mut u64, 512) }
    }

    /// One READ or WRITE command covering up to 8 pages (MAX_SECTORS=64).
    ///
    /// Zero-copy path: when the grant is 4 KiB-aligned, the buffer's own
    /// physical address goes straight into the PRP list — no copy, exactly
    /// the "zero copies where possible" design (Phase 5 §3). When it is not
    /// (a kernel-minted sub-region can start anywhere), the data bounces
    /// through page-aligned frames from the `dma` pool: the controller DMAs
    /// there, and one bounded copy moves the payload — never more than
    /// 32 KiB.
    fn transfer(
        &mut self,
        lba: u64,
        sectors: u32,
        buf_addr: u64,
        opcode: u8,
    ) -> Result<(), BackendErr> {
        if !self.ready {
            return Err(BackendErr::Io);
        }
        if sectors == 0 {
            return Ok(());
        }
        debug_assert!(sectors <= MAX_SECTORS);
        // At most 8 pages: one command with a single PRP list, always under
        // NVME_MAX_PAGES_PER_XFER (32).
        let pages = sectors.div_ceil(NVME_SECTORS_PER_PAGE);
        let bytes = sectors as u64 * BLOCK_SIZE as u64;

        let (prp1, prp2);
        let bounce;

        if buf_addr & (NVME_PAGE_SIZE as u64 - 1) == 0 {
            // Zero-copy: the buffer is already page-aligned and (in the
            // identity-mapped MVP) physically contiguous for its length.
            bounce = None;
            let (p1, p2) = {
                let mut list = self.prp_list_mut();
                build_prp(buf_addr, pages, &mut list).map_err(|_| BackendErr::Io)?
            };
            prp1 = p1;
            prp2 = p2;
        } else {
            // Bounce: page-aligned contiguous run from the DMA pool.
            let b = self.pool.alloc_pages(pages).ok_or(BackendErr::NoMem)?;
            if opcode == NVM_WRITE {
                // Safety: `bytes` <= 32 KiB and both ranges were validated by
                // the server (grant size) / pool (frame bounds).
                unsafe { copy_blocks(buf_addr as *const u8, b as *mut u8, bytes as usize) };
            }
            bounce = Some(b);
            let (p1, p2) = {
                let mut list = self.prp_list_mut();
                build_prp(b, pages, &mut list).map_err(|_| BackendErr::Io)?
            };
            prp1 = p1;
            prp2 = p2;
        }

        let mut cmd = NvmeCmd::default();
        cmd.opcode = opcode;
        cmd.nsid = NSID;
        cmd.prp1 = prp1;
        cmd.prp2 = prp2;
        cmd.cdw10 = (lba & 0xFFFF_FFFF) as u32;
        cmd.cdw11 = (lba >> 32) as u32;
        cmd.cdw12 = sectors - 1; // NLB is 0-based
        let status = self.submit_io(&mut cmd);
        if status != 0 {
            return Err(BackendErr::Io);
        }

        if let Some(b) = bounce {
            if opcode == NVM_READ {
                // Safety: as above.
                unsafe { copy_blocks(b as *const u8, buf_addr as *mut u8, bytes as usize) };
            }
        }
        Ok(())
    }
}

impl BlockBackend for NvmeDevice {
    fn total_sectors(&self) -> u64 {
        self.total_sectors
    }

    fn read_only(&self) -> bool {
        false // NVM is writable; write authority is the controller's, not a cap's
    }

    fn read(
        &mut self,
        lba: u64,
        sectors: u32,
        dst_addr: u64,
        _dst_len: usize,
    ) -> Result<(), BackendErr> {
        self.transfer(lba, sectors, dst_addr, NVM_READ)
    }

    fn write(
        &mut self,
        lba: u64,
        sectors: u32,
        src_addr: u64,
        _src_len: usize,
    ) -> Result<(), BackendErr> {
        self.transfer(lba, sectors, src_addr, NVM_WRITE)
    }

    fn flush(&mut self) -> Result<(), BackendErr> {
        if !self.ready {
            return Err(BackendErr::Io);
        }
        // NVM Flush: commits the controller's volatile write cache to media.
        // Until this returns, a completed write is only durable against
        // process/kernel restart, NOT power loss.
        let mut cmd = NvmeCmd::default();
        cmd.opcode = NVM_FLUSH;
        cmd.nsid = NSID;
        if self.submit_io(&mut cmd) != 0 {
            Err(BackendErr::Io)
        } else {
            Ok(())
        }
    }

    fn map(&self) -> Option<MapGrant> {
        // NVMe storage is reachable only through DMA commands, never a
        // directly addressable region: no zero-copy map exists, so RD_MAP
        // answers NOMEM and clients use READ/WRITE (Phase 5 §3).
        None
    }
}
