//! The e1000 device core: register map, descriptor formats, the `Mmio`
//! accessor, the DMA/layout contract, and the driver bring-up + loopback
//! self-test. Host-testable: every register/bit constant and the driver
//! logic are exercised against the in-file `Model`, an e1000 emulation that
//! mirrors QEMU's `hw/net/e1000.c` semantics for exactly the registers this
//! driver touches (QEMU is the on-target verification environment, and the
//! kernel's own driver in `net/e1000.c` is the in-tree reference for the
//! bring-up sequence).
//!
//! Memory model (both established conventions, target-proven):
//! - MMIO registers are reached through the `Mmio` trait. On target this is
//!   `RealMmio` over the window `SYS_DEV_MMAP` returns (devtest proved the
//!   mediated map; device MMIO is NOT identity-accessible). In host tests it
//!   is the `Model`.
//! - DMA memory is a raw contiguous region (`Dma::base`). On target that is
//!   the driver's budget MEM region: sidecar MEM regions are
//!   identity-accessible at their physical base (the POSIX heap
//!   convention), so `base` is both the CPU access address and the physical
//!   address the NIC DMA-reads/writes. In host tests `base` is a real host
//!   buffer and the `Model` dereferences descriptor `buffer_addr` values as
//!   host pointers — the same "fake the address-space-shaped primitive,
//!   keep the logic real" precedent as the kernel's C host tests.

use core::hint::spin_loop;

// ─── Register map ───────────────────────────────────────────────────────────
// Offsets match net/e1000.h (the kernel driver) and QEMU's e1000_regs.h.
pub const REG_CTRL: u32 = 0x0000;
pub const REG_STATUS: u32 = 0x0008;
pub const REG_MDIC: u32 = 0x0020;
pub const REG_RCTL: u32 = 0x0100;
pub const REG_TCTL: u32 = 0x0400;
pub const REG_RDBAL: u32 = 0x2800;
pub const REG_RDBAH: u32 = 0x2804;
pub const REG_RDLEN: u32 = 0x2808;
pub const REG_RDH: u32 = 0x2810;
pub const REG_RDT: u32 = 0x2818;
pub const REG_TDBAL: u32 = 0x3800;
pub const REG_TDBAH: u32 = 0x3804;
pub const REG_TDLEN: u32 = 0x3808;
pub const REG_TDH: u32 = 0x3810;
pub const REG_TDT: u32 = 0x3818;
pub const REG_RAL0: u32 = 0x5400;
pub const REG_RAH0: u32 = 0x5404;

// ─── Bit fields (e1000.h / QEMU e1000.c / e1000_regs.h) ────────────────────
pub const CTRL_RST: u32 = 1 << 26; // device reset (self-clearing)
pub const CTRL_SLU: u32 = 1 << 6; // set link up
pub const CTRL_ASDE: u32 = 1 << 5; // auto-speed detection enable
pub const STATUS_LU: u32 = 1 << 1; // link up
pub const TCTL_EN: u32 = 1 << 1; // transmit enable
pub const TCTL_PSP: u32 = 1 << 3; // pad short packets
pub const RCTL_EN: u32 = 1 << 1; // receive enable
pub const RCTL_UPE: u32 = 1 << 3; // unicast promiscuous
pub const RCTL_MPE: u32 = 1 << 4; // multicast promiscuous
pub const RCTL_BAM: u32 = 1 << 15; // broadcast accept
pub const RAH0_AV: u32 = 1 << 31; // address valid

// MDIC (QEMU e1000_regs.h bit layout; QEMU is the write target). The
// OP field encodes read = 10 (bit 27) and write = 01 (bit 26) per the
// Intel 8254x SDM; QEMU decodes READ first, then WRITE (set_mdic in
// hw/net/e1000.c). QEMU refuses any PHY address != 1.
pub const MDIC_DATA_MASK: u32 = 0xffff;
pub const MDIC_REG_SHIFT: u32 = 16;
pub const MDIC_REG_MASK: u32 = 0x1f;
pub const MDIC_PHY_SHIFT: u32 = 21;
pub const MDIC_PHY_MASK: u32 = 0x1f;
pub const MDIC_OP_READ: u32 = 1 << 27;
pub const MDIC_OP_WRITE: u32 = 1 << 26;
pub const MDIC_READY: u32 = 1 << 28;

// MII (PHY) space — the model's PHY is the 82540EM's M88E1000 (QEMU).
pub const MII_BMCR: u16 = 0;
pub const MII_BMCR_LOOPBACK: u16 = 0x4000;
pub const MII_PHY_ADDR: u16 = 1; // QEMU refuses MDIC PHY # != 1

// TX descriptor command / status bits.
pub const TX_CMD_EOP: u8 = 1 << 0; // end of packet
pub const TX_CMD_IFCS: u8 = 1 << 1; // insert FCS
pub const TX_CMD_RS: u8 = 1 << 3; // report status (write DD back)
pub const DESC_DD: u8 = 1 << 0; // descriptor done

/// Self-test payload ethertype (the repo's DSPP ethertype, reused so the
/// loopback frame is unambiguous); the payload bytes are what the driver
/// validates.
pub const SELFTEST_ETYPE: u16 = 0x88B5;
pub const SELFTEST_MARKER: &[u8] = b"AeroSLS-e1000-SL!";

// ─── Descriptor formats (16 bytes each; net/e1000.h + QEMU) ─────────────────
#[derive(Clone, Copy, Default)]
#[repr(C)]
pub struct TxDesc {
    pub buffer_addr: u64,
    pub length: u16,
    pub cso: u8,
    pub cmd: u8,
    pub status: u8,
    pub css: u8,
    pub special: u16,
}

#[derive(Clone, Copy, Default)]
#[repr(C)]
pub struct RxDesc {
    pub buffer_addr: u64,
    pub length: u16,
    pub checksum: u16,
    pub status: u8,
    pub errors: u8,
    pub special: u16,
}

// ─── Ring / DMA layout ──────────────────────────────────────────────────────
/// Descriptor slots per direction. 16 slots is ample for the loopback
/// self-test and keeps the DMA footprint small (see `DMA_REGION_BYTES`).
pub const RING_N: usize = 16;
/// Per-RX-slot buffer size — QEMU delivers into 2048-byte buffers with
/// RCTL_BSIZE clear (matches the kernel driver's E1000_RX_BUF_SIZE).
pub const RX_BUF_BYTES: usize = 2048;

/// Offsets inside the DMA region.
pub const TX_DESC_OFF: u64 = 0x0000; // RING_N * 16 = 0x100
pub const RX_DESC_OFF: u64 = 0x0100;
pub const TX_FRAME_OFF: u64 = 0x0200; // the self-test TX frame buffer
pub const RX_BUF0_OFF: u64 = 0x1000; // RING_N * 2048 = 0x8000
/// Whole-region size: ~35 KiB — well inside a 64 KiB+ budget cap.
pub const DMA_REGION_BYTES: u64 = RX_BUF0_OFF + (RING_N as u64 * RX_BUF_BYTES as u64);

/// Maximum iterations for a bounded device poll (register + descriptor).
/// Each loop is a `spin_loop` (microseconds at worst on TCG); the caps keep
/// a dead device from wedging the driver — a timeout is an error, not a
/// hang.
pub const POLL_LIMIT: u32 = 4_000_000;

/// Outer iterations of the RX wait loop below. Each outer iteration scans
/// the whole RX ring, burns `RX_WAIT_SPIN` `pause`s, and performs one MMIO
/// read. The poll exists to catch the loopback frame QEMU delivers for a
/// TX that lands AFTER its ~1000 ms post-RCTL grace (a TX during the grace
/// is dropped outright — see `run_selftest_retry`), so a bounded wait per
/// attempt is all the driver needs: the frame arrives within the first few
/// outer iterations of the post-grace attempt (the very first DD scan
/// finds it), and when the attempt is still inside the grace the bound
/// burns ~1 s of guest time before NoRxFrame lets the retry re-TX. Sized
/// deliberately small: the retry ladder has 8 attempts and the smoke
/// window is 180 s, so each dropped attempt must cost only ~1-2 s of
/// guest time — under TCG a 200k-iteration poll is tens of seconds, which
/// starves the ladder into a single slow attempt. Small is also what the
/// host model's `drop_rx` retry tests need to stay fast.
pub const RX_WAIT_OUTER: u32 = 30_000;
/// Pause burst per outer iteration (see `RX_WAIT_OUTER`).
pub const RX_WAIT_SPIN: u32 = 128;

// ─── MMIO access ────────────────────────────────────────────────────────────
/// 32-bit register access. `rd`/`wr` must be volatile-observable (device
/// MMIO); the implementor decides how (raw volatile ops on target, model
/// state in host tests).
pub trait Mmio {
    fn rd(&self, reg: u32) -> u32;
    fn wr(&self, reg: u32, val: u32);
}

/// The real window: a user-half virtual address `SYS_DEV_MMAP` returned for
/// the `nic0.bar0` DEV cap. Device MMIO only — never dereference without
/// mapping. Constructed exactly once in `entry.rs` from the mmap result.
pub struct RealMmio {
    base: *mut u8,
}

impl RealMmio {
    /// # Safety
    /// `vaddr` must be a live mapping of the e1000 MMIO BAR0 (from
    /// `k_dev_mmap`), valid for the driver's lifetime.
    pub unsafe fn new(vaddr: u64) -> Self {
        RealMmio {
            base: vaddr as usize as *mut u8,
        }
    }
}

impl Mmio for RealMmio {
    fn rd(&self, reg: u32) -> u32 {
        unsafe { core::ptr::read_volatile(self.base.add(reg as usize) as *const u32) }
    }
    fn wr(&self, reg: u32, val: u32) {
        unsafe { core::ptr::write_volatile(self.base.add(reg as usize) as *mut u32, val) }
    }
}

// ─── DMA region access ──────────────────────────────────────────────────────
/// A raw DMA region: `base` is the physical address of the region start
/// (== the CPU access address on target; == a real host buffer in tests).
#[derive(Clone, Copy)]
pub struct Dma {
    pub base: u64,
}

impl Dma {
    pub fn at(&self, off: u64) -> *mut u8 {
        (self.base as usize + off as usize) as *mut u8
    }
    /// Byte-slice view of an offset range — the CPU side of the DMA memory.
    pub fn slice(&self, off: u64, len: usize) -> &mut [u8] {
        unsafe { core::slice::from_raw_parts_mut(self.at(off), len) }
    }
    pub fn tx_desc(&self, i: usize) -> &mut TxDesc {
        unsafe { &mut *(self.at(TX_DESC_OFF + (i as u64 * 16)) as *mut TxDesc) }
    }
    pub fn rx_desc(&self, i: usize) -> &mut RxDesc {
        unsafe { &mut *(self.at(RX_DESC_OFF + (i as u64 * 16)) as *mut RxDesc) }
    }
}

// ─── Errors ─────────────────────────────────────────────────────────────────
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum E1000Error {
    /// Budget region too small for the ring/buffer layout.
    DmaRegionTooSmall { have: u64, need: u64 },
    /// Link never came up (STATUS.LU) within the poll bound.
    LinkDown,
    /// A bounded device poll (MDIC busy / descriptor DD) never completed.
    Timeout(&'static str),
    /// PHY/MDIC programming failed (wrong PHY, no READY, ...).
    Mdic(&'static str),
    /// The loopback frame never came back through the RX ring.
    NoRxFrame,
    /// The received frame failed byte validation.
    RxMismatch { rx_len: u16, expected: u16 },
}

impl core::fmt::Display for E1000Error {
    fn fmt(&self, f: &mut core::fmt::Formatter<'_>) -> core::fmt::Result {
        match self {
            E1000Error::DmaRegionTooSmall { have, need } => {
                write!(f, "DMA region {have} bytes < needed {need}")
            }
            E1000Error::LinkDown => write!(f, "link never came up (STATUS.LU)"),
            E1000Error::Timeout(w) => write!(f, "device poll timed out: {w}"),
            E1000Error::Mdic(w) => write!(f, "MDIC error: {w}"),
            E1000Error::NoRxFrame => write!(f, "no RX frame after TX completed"),
            E1000Error::RxMismatch { rx_len, expected } => {
                write!(f, "RX frame {rx_len} bytes != expected {expected}")
            }
        }
    }
}

// ─── The driver ─────────────────────────────────────────────────────────────
/// The e1000 device, accessed through `M`. Stateless with respect to the
/// ring (descriptor indices live in the DMA memory / registers), so one
/// instance can drive the whole lifecycle.
pub struct Device<M: Mmio> {
    mmio: M,
}

impl<M: Mmio> Device<M> {
    pub fn new(mmio: M) -> Self {
        Device { mmio }
    }

    pub fn mmio(&self) -> &M {
        &self.mmio
    }

    /// The MAC the NIC loaded into RAL0/RAH0 at reset (QEMU preloads the
    /// `-device e1000,mac=` value; real parts load their EEPROM/flash).
    pub fn mac(&self) -> [u8; 6] {
        let ral = self.mmio.rd(REG_RAL0);
        let rah = self.mmio.rd(REG_RAH0);
        [
            ral as u8,
            (ral >> 8) as u8,
            (ral >> 16) as u8,
            (ral >> 24) as u8,
            rah as u8,
            (rah >> 8) as u8,
        ]
    }

    pub fn link_up(&self) -> bool {
        self.mmio.rd(REG_STATUS) & STATUS_LU != 0
    }

    /// The kernel driver's link-up write (net/e1000.c): assert SLU/ASDE,
    /// clear RST. Harmless when already up (QEMU's reset CTRL already has
    /// SLU) and required on parts that come out of reset with it clear.
    pub fn ctrl_link_up(&self) {
        let v = self.mmio.rd(REG_CTRL);
        self.mmio
            .wr(REG_CTRL, (v & !CTRL_RST) | CTRL_SLU | CTRL_ASDE);
    }

    /// Program RAL0/RAH0 (receive-address filter slot 0) + Address Valid.
    pub fn set_mac_filter(&self, mac: &[u8; 6]) {
        self.mmio.wr(
            REG_RAL0,
            (mac[0] as u32)
                | ((mac[1] as u32) << 8)
                | ((mac[2] as u32) << 16)
                | ((mac[3] as u32) << 24),
        );
        self.mmio.wr(
            REG_RAH0,
            (mac[4] as u32) | ((mac[5] as u32) << 8) | RAH0_AV,
        );
    }

    /// One MDIC transaction (write op). Polls READY, bounded. QEMU's PHY
    /// lives at address 1.
    pub fn mdic_write(&self, phy: u16, reg: u16, data: u16) -> Result<(), E1000Error> {
        if phy != MII_PHY_ADDR {
            return Err(E1000Error::Mdic("PHY address is not 1"));
        }
        let val = MDIC_OP_WRITE
            | ((phy as u32 & MDIC_PHY_MASK) << MDIC_PHY_SHIFT)
            | ((reg as u32 & MDIC_REG_MASK) << MDIC_REG_SHIFT)
            | (data as u32 & MDIC_DATA_MASK);
        self.mmio.wr(REG_MDIC, val);
        for _ in 0..POLL_LIMIT {
            if self.mmio.rd(REG_MDIC) & MDIC_READY != 0 {
                return Ok(());
            }
            spin_loop();
        }
        Err(E1000Error::Timeout("MDIC READY"))
    }

    /// PHY loopback: with MII_BMCR_LOOPBACK set, QEMU's model routes every
    /// transmitted frame straight back into the RX path (`e1000_send_packet`
    /// does `if (phy[MII_BMCR] & LOOPBACK) qemu_receive_packet(...)`). This
    /// is the on-model proof of the full TX→wire→RX data path with no peer.
    pub fn phy_loopback(&self, on: bool) -> Result<(), E1000Error> {
        let v = if on { MII_BMCR_LOOPBACK } else { 0 };
        self.mdic_write(MII_PHY_ADDR, MII_BMCR, v)
    }

    /// Program the TX ring (net/e1000.c sequence): descriptor base/len,
    /// head=tail=0, and TCTL with EN|PSP + CT/COLD defaults.
    pub fn setup_tx(&self, dma: &Dma) {
        let tx_desc_phys = dma.base + TX_DESC_OFF;
        self.mmio.wr(REG_TDBAL, tx_desc_phys as u32);
        self.mmio.wr(REG_TDBAH, (tx_desc_phys >> 32) as u32);
        self.mmio.wr(REG_TDLEN, (RING_N * 16) as u32);
        self.mmio.wr(REG_TDH, 0);
        self.mmio.wr(REG_TDT, 0);
        self.mmio.wr(
            REG_TCTL,
            TCTL_EN | TCTL_PSP | (0x0Fu32 << 4) /* CT */ | (0x040u32 << 12) /* COLD */,
        );
    }

    /// Program the RX ring: point every descriptor at its buffer, clear
    /// status, then RDBAL/len + RDH=0/RDT=n-1 + RCTL (EN|BAM|UPE|MPE) —
    /// the kernel driver's exact enablement.
    pub fn setup_rx(&self, dma: &Dma) {
        for i in 0..RING_N {
            let d = dma.rx_desc(i);
            d.buffer_addr = dma.base + RX_BUF0_OFF + (i as u64 * RX_BUF_BYTES as u64);
            d.status = 0;
            d.length = 0;
        }
        let rx_desc_phys = dma.base + RX_DESC_OFF;
        self.mmio.wr(REG_RDBAL, rx_desc_phys as u32);
        self.mmio.wr(REG_RDBAH, (rx_desc_phys >> 32) as u32);
        self.mmio.wr(REG_RDLEN, (RING_N * 16) as u32);
        self.mmio.wr(REG_RDH, 0);
        self.mmio.wr(REG_RDT, (RING_N - 1) as u32);
        self.mmio
            .wr(REG_RCTL, RCTL_EN | RCTL_BAM | RCTL_UPE | RCTL_MPE);
    }

    /// Submit one frame (already in DMA memory at `frame_off`) on descriptor
    /// 0: publish bytes, then advance TDT — the MMIO write that makes the
    /// NIC DMA the descriptor and transmit.
    pub fn tx_submit(&self, dma: &Dma, frame_off: u64, len: u16) {
        let d = dma.tx_desc(0);
        d.buffer_addr = dma.base + frame_off;
        d.length = len;
        d.cmd = TX_CMD_EOP | TX_CMD_IFCS | TX_CMD_RS;
        d.status = 0;
        // Descriptor + frame bytes must be visible before the device reads
        // them on the TDT poke (x86 stores are ordered; the fence pins the
        // intent and costs nothing on TCG).
        core::sync::atomic::fence(core::sync::atomic::Ordering::SeqCst);
        self.mmio.wr(REG_TDT, 1);
    }

    /// Wait (bounded) for the TX descriptor's DD bit — the NIC's DMA
    /// write-back that the frame left the descriptor ring.
    pub fn tx_wait_dd(&self, dma: &Dma) -> Result<(), E1000Error> {
        for _ in 0..POLL_LIMIT {
            if dma.tx_desc(0).status & DESC_DD != 0 {
                return Ok(());
            }
            spin_loop();
        }
        Err(E1000Error::Timeout("TX descriptor DD"))
    }

    /// Wait for the loopback frame to appear on the RX ring. Each outer
    /// iteration re-scans every descriptor and burns a small pause burst;
    /// the one MMIO read per outer keeps the wait observable to the device
    /// model and paces the loop against guest time. Returns the slot index
    /// and the completed frame's byte length as soon as a DD write-back
    /// appears, or NoRxFrame when the bound elapses (the caller's retry
    /// re-transmits after QEMU's RX grace has expired).
    fn rx_poll(&self, dma: &Dma) -> Result<(usize, u16), E1000Error> {
        for _ in 0..RX_WAIT_OUTER {
            for i in 0..RING_N {
                if dma.rx_desc(i).status & DESC_DD != 0 {
                    return Ok((i, dma.rx_desc(i).length));
                }
            }
            for _ in 0..RX_WAIT_SPIN {
                spin_loop();
            }
            // MMIO read: side-effect-free; keeps the poll paced and the
            // device model engaged while the ring is idle.
            let _ = self.mmio.rd(REG_STATUS);
        }
        Err(E1000Error::NoRxFrame)
    }

    /// Clear a consumed RX descriptor back to hardware ownership.
    fn rx_release(&self, dma: &Dma, slot: usize) {
        dma.rx_desc(slot).status = 0;
        self.mmio.wr(REG_RDT, slot as u32);
    }

    /// Polled RX drain: release every completed descriptor, returning how
    /// many frames were dropped (v1 has no consumer — the network
    /// data-plane milestone gives frames somewhere to go). Draining is
    /// still required so a live NIC never stalls its DMA engine.
    pub fn rx_drain(&self, dma: &Dma) -> usize {
        let mut n = 0;
        for i in 0..RING_N {
            if dma.rx_desc(i).status & DESC_DD != 0 {
                self.rx_release(dma, i);
                n += 1;
            }
        }
        n
    }
}

/// Outcome of a successful loopback self-test.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct SelftestOut {
    /// The NIC's MAC the test ran against.
    pub mac: [u8; 6],
    /// Bytes transmitted (and validated on return).
    pub frame_len: u16,
    /// RX slot the frame landed in.
    pub rx_slot: usize,
}

/// Bring the link up, set the MAC filter, enable PHY loopback, zero the
/// descriptor area, and program both rings. This is where RCTL is written
/// — the write that starts QEMU's 1000 ms RX-delivery grace window — so it
/// runs exactly once per selftest: a retry must never re-arm that window
/// by re-enabling RX. Idempotent with respect to repeated TX attempts
/// (see `tx_round_trip`).
fn bring_up_rings<M: Mmio>(dev: &Device<M>, dma: &Dma, mac: &[u8; 6]) -> Result<(), E1000Error> {
    dev.ctrl_link_up();
    for _ in 0..POLL_LIMIT {
        if dev.link_up() {
            break;
        }
        spin_loop();
    }
    if !dev.link_up() {
        return Err(E1000Error::LinkDown);
    }

    dev.set_mac_filter(mac);
    dev.phy_loopback(true)?;

    dma.slice(TX_DESC_OFF, RING_N * 16).fill(0);
    dma.slice(RX_DESC_OFF, RING_N * 16).fill(0);
    dev.setup_tx(dma);
    dev.setup_rx(dma);
    Ok(())
}

/// One TX→RX round trip: (re)arm the TX ring head/tail (so descriptor 0
/// can be reused after a previous attempt advanced TDH), build and submit
/// the loopback frame, wait for its DD write-back, poll RX, validate every
/// received byte, release the slot, and restore the PHY. `setup_tx` here
/// is the retry-safe part: it resets TDH/TDT but never touches RCTL, so it
/// does not restart QEMU's RX grace window.
fn tx_round_trip<M: Mmio>(
    dev: &Device<M>,
    dma: &Dma,
    mac: &[u8; 6],
) -> Result<SelftestOut, E1000Error> {
    dev.setup_tx(dma);

    // Build the frame: eth dst=our MAC, src=our MAC, SELFTEST_ETYPE,
    // marker payload (30 bytes — a legal short frame; QEMU pads nothing on
    // the TX side and the RX side tolerates whatever the net layer does).
    let frame_len = 14 + SELFTEST_MARKER.len();
    {
        let f = dma.slice(TX_FRAME_OFF, frame_len);
        f[0..6].copy_from_slice(mac);
        f[6..12].copy_from_slice(mac);
        f[12..14].copy_from_slice(&SELFTEST_ETYPE.to_le_bytes());
        f[14..].copy_from_slice(SELFTEST_MARKER);
    }

    dev.tx_submit(dma, TX_FRAME_OFF, frame_len as u16);
    dev.tx_wait_dd(dma)?;

    // The frame should come straight back on RX slot 0 (the hardware head).
    let (slot, rx_len) = dev.rx_poll(dma)?;
    // desc.length must at least cover the frame; its upper bound varies
    // with the model: QEMU's desc.length includes the CRC it appends on
    // the RX side (e1000x_fcs_len = 4 without RCTL.SECRC) and short frames
    // may be padded by the net layer (net_client_needs_padding). The byte
    // compare below only looks at the first `frame_len` bytes, so accept
    // any length in frame_len..frame_len+64 (a full 60-byte pad + CRC).
    if rx_len < frame_len as u16 || rx_len > frame_len as u16 + 64 {
        return Err(E1000Error::RxMismatch {
            rx_len,
            expected: frame_len as u16,
        });
    }
    let sent = dma.slice(TX_FRAME_OFF, frame_len);
    let got = dma.slice(RX_BUF0_OFF + (slot as u64 * RX_BUF_BYTES as u64), frame_len);
    if got != sent {
        return Err(E1000Error::RxMismatch {
            rx_len,
            expected: frame_len as u16,
        });
    }
    dev.rx_release(dma, slot);
    dev.phy_loopback(false)?;

    Ok(SelftestOut {
        mac: *mac,
        frame_len: frame_len as u16,
        rx_slot: slot,
    })
}

/// Run the bring-up + PHY-loopback self-test end to end, once:
/// link-up → MAC filter → rings → transmit a frame to our own MAC with PHY
/// loopback set → the NIC routes it back into the RX ring → validate every
/// received byte matches what was sent → restore loopback.
pub fn run_selftest<M: Mmio>(
    dev: &Device<M>,
    dma: &Dma,
    mac: &[u8; 6],
) -> Result<SelftestOut, E1000Error> {
    debug_assert!(dma.base != 0);
    bring_up_rings(dev, dma, mac)?;
    tx_round_trip(dev, dma, mac)
}

/// QEMU arms a 1000 ms `flush_queue_timer` on every RCTL write, and while
/// it is pending every loopback delivery is silently DROPPED (the net
/// layer's receive path refuses the frame — nothing is queued for later).
/// So a TX that lands inside the window is lost outright, and the only way
/// to get a frame onto the RX ring is to transmit again once the window
/// has expired (≈1 s of QEMU wall time after RX-enable). Under the smoke's
/// `-accel tcg,thread=multi` QEMU timers fire against real time, so the
/// grace reliably expires ~1 s after `bring_up_rings` regardless of what
/// the guest does.
///
/// `run_selftest` is the single-shot attempt and races that window.
/// `run_selftest_retry` is the on-target entry: `bring_up_rings` exactly
/// once (so retries never re-arm the window via RCTL), then up to
/// `SELFTEST_MAX_ATTEMPTS` TX→RX round trips with a caller-supplied
/// `settle` burn between them. Each attempt re-transmits — the bounded
/// `rx_poll` catches a delivery the instant one lands, and a dropped
/// in-grace attempt costs only its poll bound before the settle paces the
/// next TX past the window. Only `NoRxFrame` is retried — a `RxMismatch`
/// means the bytes were wrong, which a retry cannot fix. The host model
/// exercises the retry path deterministically via its `drop_rx` gate (no
/// wall clock needed in tests).
pub const SELFTEST_MAX_ATTEMPTS: u32 = 8;

pub fn run_selftest_retry<M: Mmio>(
    dev: &Device<M>,
    dma: &Dma,
    mac: &[u8; 6],
    mut settle: impl FnMut(),
) -> Result<SelftestOut, E1000Error> {
    bring_up_rings(dev, dma, mac)?;
    let mut last = E1000Error::NoRxFrame;
    for _ in 0..SELFTEST_MAX_ATTEMPTS {
        match tx_round_trip(dev, dma, mac) {
            Ok(out) => return Ok(out),
            Err(E1000Error::NoRxFrame) => {
                last = E1000Error::NoRxFrame;
                settle();
            }
            Err(e) => return Err(e),
        }
    }
    Err(last)
}

// ═══ Host model + tests (QEMU hw/net/e1000.c semantics, register subset) ════
#[cfg(test)]
mod tests {
    use super::*;
    use std::cell::{Cell, RefCell};
    use std::collections::HashMap;

    /// QEMU-faithful e1000 model for exactly the registers/descriptor flow
    /// this driver uses:
    /// - reset preloads RAL0/RAH0 with the MAC (+AV) and STATUS.LU;
    /// - a TDT write runs QEMU's `start_xmit` when TCTL.EN (walks TDH→TDT,
    ///   reads each legacy descriptor from the DMA region — `buffer_addr`
    ///   values are host pointers in tests — and writebacks DD on RS);
    /// - with PHY loopback set (MII_BMCR bit 0x4000, written via MDIC), the
    ///   frame is delivered back into the RX path, which runs from RDH with
    ///   per-descriptor copies into the rx buffers (like `e1000_receive_iov`);
    /// - MDIC writes decode phy/reg/data and land in the PHY register file
    ///   (BMCR gets QEMU's `set_phy_ctrl` masking).
    ///
    /// Interior mutability: the `Mmio` trait takes `&self`, mirroring how a
    /// register file is inherently shared state.
    struct Model {
        regs: RefCell<HashMap<u32, u32>>,
        phy: RefCell<[u16; 32]>,
        /// When set, the RX copy has its payload marker byte flipped — for
        /// the corruption-detection test.
        corrupt_rx: Cell<bool>,
        /// While > 0, loopback deliveries are silently dropped (decrementing
        /// each time) — mirroring QEMU's post-RCTL 1000 ms flush_queue_timer
        /// grace, deterministically. Exercises the retry wrapper.
        drop_rx: Cell<u32>,
    }

    impl Model {
        fn new(mac: [u8; 6]) -> Self {
            let m = Model {
                regs: RefCell::new(HashMap::new()),
                phy: RefCell::new([0; 32]),
                corrupt_rx: Cell::new(false),
                drop_rx: Cell::new(0),
            };
            m.set_reg(
                REG_RAL0,
                mac[0] as u32
                    | ((mac[1] as u32) << 8)
                    | ((mac[2] as u32) << 16)
                    | ((mac[3] as u32) << 24),
            );
            m.set_reg(REG_RAH0, (mac[4] as u32) | ((mac[5] as u32) << 8) | RAH0_AV);
            m.set_reg(REG_STATUS, STATUS_LU | (1u32 << 31)); // QEMU's reset STATUS
            m
        }

        fn reg(&self, r: u32) -> u32 {
            self.regs.borrow().get(&r).copied().unwrap_or(0)
        }
        fn set_reg(&self, r: u32, v: u32) {
            self.regs.borrow_mut().insert(r, v);
        }
        fn phy_reg(&self, r: u16) -> u16 {
            self.phy.borrow()[r as usize]
        }

        fn tx_desc_base(&self) -> u64 {
            (self.reg(REG_TDBAL) as u64) | ((self.reg(REG_TDBAH) as u64) << 32)
        }
        fn rx_desc_base(&self) -> u64 {
            (self.reg(REG_RDBAL) as u64) | ((self.reg(REG_RDBAH) as u64) << 32)
        }

        /// QEMU start_xmit: TCTL.EN gate, then walk TDH → TDT.
        fn run_tx(&self) {
            if self.reg(REG_TCTL) & TCTL_EN == 0 {
                return;
            }
            let base = self.tx_desc_base();
            let mut tdh = self.reg(REG_TDH);
            let tdt = self.reg(REG_TDT);
            let tlen = self.reg(REG_TDLEN);
            let mut guard = 0u32;
            while tdh != tdt && guard < 256 {
                let d = (base + tdh as u64 * 16) as *mut TxDesc;
                let desc = unsafe { core::ptr::read(d) };
                if desc.cmd & TX_CMD_RS != 0 {
                    // txdesc_writeback: status DD bit in the descriptor.
                    unsafe { (*d).status |= DESC_DD }
                }
                if desc.cmd & TX_CMD_EOP != 0 {
                    // e1000_send_packet: PHY loopback routes TX into RX.
                    if self.phy_reg(MII_BMCR) & MII_BMCR_LOOPBACK != 0 {
                        let frame: &[u8] = unsafe {
                            core::slice::from_raw_parts(
                                desc.buffer_addr as *const u8,
                                desc.length as usize,
                            )
                        };
                        // QEMU's receive grace: drops while the post-RCTL
                        // flush timer is pending (modeled as a count).
                        if self.drop_rx.get() > 0 {
                            self.drop_rx.set(self.drop_rx.get() - 1);
                        } else {
                            self.deliver_rx(frame);
                        }
                    }
                }
                tdh += 1;
                if (tdh as u64) * 16 >= tlen as u64 {
                    tdh = 0;
                }
                guard += 1;
            }
            self.set_reg(REG_TDH, tdh);
        }

        /// QEMU e1000_receive_iov for our single-descriptor case: from RDH,
        /// copy into the descriptor's buffer, write back length/EOP/DD.
        fn deliver_rx(&self, frame: &[u8]) {
            if self.reg(REG_RCTL) & RCTL_EN == 0 {
                return;
            }
            let rdh = self.reg(REG_RDH);
            let rdt = self.reg(REG_RDT);
            let rlen = self.reg(REG_RDLEN);
            if rdh == rdt {
                return; // no buffers
            }
            let d = (self.rx_desc_base() + rdh as u64 * 16) as *mut RxDesc;
            let desc = unsafe { core::ptr::read(d) };
            if desc.buffer_addr == 0 {
                return;
            }
            let copy = frame.len().min(RX_BUF_BYTES);
            unsafe {
                core::ptr::copy_nonoverlapping(
                    frame.as_ptr(),
                    desc.buffer_addr as *mut u8,
                    copy,
                );
            }
            if self.corrupt_rx.get() && copy > 20 {
                // Flip one payload byte so the driver's compare must fail.
                unsafe {
                    let p = desc.buffer_addr as *mut u8;
                    *p.add(20) ^= 0xff;
                }
            }
            unsafe {
                let mut wd = desc;
                wd.length = copy as u16;
                wd.status |= 0x2; // EOP
                core::ptr::write(d, wd);
                (*d).status |= DESC_DD; // DD written last, like QEMU's store
            }
            let mut nrdh = rdh + 1;
            if (nrdh as u64) * 16 >= rlen as u64 {
                nrdh = 0;
            }
            self.set_reg(REG_RDH, nrdh);
        }
    }

    impl Mmio for Model {
        fn rd(&self, reg: u32) -> u32 {
            self.reg(reg)
        }
        fn wr(&self, reg: u32, val: u32) {
            match reg {
                REG_TDT => {
                    self.set_reg(reg, val);
                    self.run_tx();
                }
                REG_MDIC => {
                    let data = val & MDIC_DATA_MASK;
                    let raddr = ((val >> MDIC_REG_SHIFT) & MDIC_REG_MASK) as usize;
                    let pnum = ((val >> MDIC_PHY_SHIFT) & MDIC_PHY_MASK) as usize;
                    if pnum == MII_PHY_ADDR as usize {
                        if val & MDIC_OP_READ != 0 {
                            let v = (val & !MDIC_DATA_MASK) | (self.phy.borrow()[raddr] as u32);
                            self.set_reg(reg, v);
                        } else if val & MDIC_OP_WRITE != 0 {
                            let mut phy = self.phy.borrow_mut();
                            if raddr == MII_BMCR as usize {
                                // QEMU set_phy_ctrl: clear reserved + the
                                // self-clearing bits.
                                phy[raddr] = (data as u16) & !(0x3f | 0x8000 | 0x0200);
                            } else {
                                phy[raddr] = data as u16;
                            }
                            drop(phy);
                            self.set_reg(reg, val | MDIC_READY);
                        }
                    } else {
                        self.set_reg(reg, val | MDIC_READY);
                    }
                }
                _ => self.set_reg(reg, val),
            }
        }
    }

    /// A host DMA region: `base` is the aligned address of a real buffer.
    fn dma_host() -> (Dma, Vec<u8>) {
        let mut buf = vec![0u8; DMA_REGION_BYTES as usize + 64];
        let p = buf.as_mut_ptr() as usize;
        let aligned = (p + 15) & !15usize;
        (Dma { base: aligned as u64 }, buf)
    }

    const TEST_MAC: [u8; 6] = [0x52, 0x54, 0x00, 0x12, 0x34, 0x99];

    #[test]
    fn mac_reads_back_ral_rah() {
        let dev = Device::new(Model::new(TEST_MAC));
        assert_eq!(dev.mac(), TEST_MAC);
        assert!(dev.link_up(), "QEMU reset STATUS carries LU");
    }

    #[test]
    fn selftest_round_trips_a_frame_through_both_rings() {
        let (dma, _hold) = dma_host();
        let dev = Device::new(Model::new(TEST_MAC));
        let out = run_selftest(&dev, &dma, &TEST_MAC).expect("selftest passed");
        assert_eq!(out.mac, TEST_MAC);
        assert_eq!(out.frame_len as usize, 14 + SELFTEST_MARKER.len());
        assert_eq!(out.rx_slot, 0, "first frame lands on RX slot 0");

        // The RX buffer actually carries the sent bytes (the model copied
        // them through the descriptor's buffer_addr).
        let sent = dma.slice(TX_FRAME_OFF, out.frame_len as usize);
        let got = dma.slice(RX_BUF0_OFF, out.frame_len as usize);
        assert_eq!(got, sent, "RX buffer holds the exact transmitted frame");
    }

    #[test]
    fn selftest_fails_when_rx_never_enables() {
        struct NoRx(Model);
        impl Mmio for NoRx {
            fn rd(&self, reg: u32) -> u32 {
                self.0.rd(reg)
            }
            fn wr(&self, reg: u32, val: u32) {
                if reg == REG_RCTL {
                    return; // swallow receive-enable: nothing can be received
                }
                self.0.wr(reg, val);
            }
        }
        let (dma, _hold) = dma_host();
        let dev = Device::new(NoRx(Model::new(TEST_MAC)));
        match run_selftest(&dev, &dma, &TEST_MAC) {
            Err(E1000Error::NoRxFrame) => {}
            other => panic!("expected NoRxFrame, got {other:?}"),
        }
    }

    #[test]
    fn single_shot_selftest_fails_when_delivery_is_dropped() {
        // QEMU drops deliveries during its post-RCTL grace; the single-shot
        // attempt must report NoRxFrame so the retry wrapper can fire.
        let m = Model::new(TEST_MAC);
        m.drop_rx.set(1);
        let (dma, _hold) = dma_host();
        let dev = Device::new(m);
        match run_selftest(&dev, &dma, &TEST_MAC) {
            Err(E1000Error::NoRxFrame) => {}
            other => panic!("expected NoRxFrame, got {other:?}"),
        }
    }

    #[test]
    fn retry_selftest_survives_dropped_deliveries() {
        // Drop the first two loopback deliveries (QEMU's ~1 s grace across
        // two fast attempts); the retry wrapper must eventually land the
        // frame and report a clean round-trip.
        let m = Model::new(TEST_MAC);
        m.drop_rx.set(2);
        let (dma, _hold) = dma_host();
        let dev = Device::new(m);
        let out = run_selftest_retry(&dev, &dma, &TEST_MAC, || {}).expect("retried selftest passed");
        assert_eq!(out.frame_len as usize, 14 + SELFTEST_MARKER.len());
        assert_eq!(dev.mmio().drop_rx.get(), 0, "both drops were consumed");
    }

    #[test]
    fn selftest_fails_without_phy_loopback() {
        // Loopback never set: TX completes but the frame goes to the
        // (absent) wire and nothing returns — proving PASS depended on the
        // loopback path actually being exercised.
        struct NoLoopback(Model);
        impl Mmio for NoLoopback {
            fn rd(&self, reg: u32) -> u32 {
                self.0.rd(reg)
            }
            fn wr(&self, reg: u32, val: u32) {
                if reg == REG_MDIC && val & MDIC_OP_WRITE != 0 {
                    // Complete the MDIC transaction but do NOT touch the PHY.
                    self.0.set_reg(reg, val | MDIC_READY);
                    return;
                }
                self.0.wr(reg, val);
            }
        }
        let (dma, _hold) = dma_host();
        let dev = Device::new(NoLoopback(Model::new(TEST_MAC)));
        match run_selftest(&dev, &dma, &TEST_MAC) {
            Err(E1000Error::NoRxFrame) => {}
            other => panic!("expected NoRxFrame, got {other:?}"),
        }
    }

    #[test]
    fn corrupted_rx_frame_is_detected() {
        let m = Model::new(TEST_MAC);
        m.corrupt_rx.set(true);
        let (dma, _hold) = dma_host();
        let dev = Device::new(m);
        match run_selftest(&dev, &dma, &TEST_MAC) {
            Err(E1000Error::RxMismatch { .. }) => {}
            other => panic!("expected RxMismatch, got {other:?}"),
        }
    }

    #[test]
    fn mdic_phy_loopback_round_trip() {
        // The Mmio trait takes &self, so the model lives inside the Device;
        // read the PHY state back through the same window the driver wrote.
        let dev = Device::new(Model::new(TEST_MAC));
        dev.phy_loopback(true).expect("loopback on");
        assert_ne!(
            dev.mmio().phy_reg(MII_BMCR) & MII_BMCR_LOOPBACK,
            0,
            "PHY BMCR loopback bit set through MDIC"
        );
        dev.phy_loopback(false).expect("loopback off");
        assert_eq!(dev.mmio().phy_reg(MII_BMCR) & MII_BMCR_LOOPBACK, 0);
    }
}
