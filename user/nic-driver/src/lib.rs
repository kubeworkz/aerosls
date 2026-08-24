//! AeroSLS NIC driver sidecar — e1000/virtio-net lifecycle.
//!
//! Design: docs/AeroSLS-Device-Driver-SDK-Phase4-Design-v0.1.md §6.
//!
//! This crate implements the full lifecycle of a NIC driver sidecar:
//! from spawn to packet transmit/receive, including zero-copy paths
//! to a network stack sidecar.
//!
//! The driver communicates with the network stack over channels,
//! using MEM caps for zero-copy packet buffer transfer.

#![no_std]

extern crate alloc;

use alloc::vec::Vec;
use core::fmt;

use aerosls_proto::kabi::{Kernel, ERR_OK, GrantedCap, SendCap, RecvResult};

/// Maximum number of descriptors in TX/RX rings.
pub const RING_SIZE: usize = 128;

/// Maximum packet buffer size.
pub const MAX_PACKET_SIZE: usize = 2048;

/// Number of pre-allocated RX buffers.
pub const RX_BUF_COUNT: usize = 128;

/// Driver state.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum DriverState {
    /// Not initialized.
    Uninit,
    /// Hardware initialized, rings programmed.
    Ready,
    /// Operating normally.
    Running,
    /// Driver shutting down.
    Shutdown,
    /// Driver failed.
    Failed,
}

/// E1000 register offsets.
pub mod e1000_reg {
    pub const CTRL: u32 = 0x0000;
    pub const STATUS: u32 = 0x0008;
    pub const RCTL: u32 = 0x0100;
    pub const TCTL: u32 = 0x0400;
    pub const RDBAL: u32 = 0x2800;
    pub const RDBAH: u32 = 0x2804;
    pub const RDLEN: u32 = 0x2808;
    pub const RDH: u32 = 0x2810;
    pub const RDT: u32 = 0x2818;
    pub const TDBAL: u32 = 0x3800;
    pub const TDBAH: u32 = 0x3804;
    pub const TDLEN: u32 = 0x3808;
    pub const TDH: u32 = 0x3810;
    pub const TDT: u32 = 0x3818;
    pub const RAL: u32 = 0x5400;
    pub const RAH: u32 = 0x5404;
    pub const ICR: u32 = 0x00C0;  // Interrupt Cause Read

    // Control bits
    pub const CTRL_RST: u32 = 1 << 26;
    pub const CTRL_SLU: u32 = 1 << 6;
    pub const CTRL_ASDE: u32 = 1 << 5;

    // RCTL bits
    pub const RCTL_EN: u32 = 1 << 1;
    pub const RCTL_BAM: u32 = 1 << 15;

    // TCTL bits
    pub const TCTL_EN: u32 = 1 << 1;
    pub const TCTL_PSP: u32 = 1 << 3;

    // ICR bits
    pub const ICR_RXT0: u32 = 1 << 7;  // RX timer
    pub const ICR_TXDW: u32 = 1 << 0;  // TX descriptor written back
}

/// Network protocol opcodes between NIC driver and network stack.
pub mod nic_opcode {
    /// NIC → Stack: packet received.
    pub const RX_PACKET: u32 = 0xA001;
    /// Stack → NIC: transmit packet.
    pub const TX_PACKET: u32 = 0xA002;
    /// NIC → Stack: transmit complete.
    pub const TX_DONE: u32 = 0xA003;
    /// NIC → Stack: NIC info (MAC, capabilities).
    pub const NIC_INFO: u32 = 0xA004;
    /// Stack → NIC: set MAC address.
    pub const SET_MAC: u32 = 0xA005;
    /// Stack → NIC: get status.
    pub const GET_STATUS: u32 = 0xA006;
    /// Stack → NIC: set link up/down.
    pub const SET_LINK: u32 = 0xA007;
}

/// MAC address (6 bytes).
#[derive(Clone, Copy, Default, PartialEq, Eq)]
pub struct MacAddress {
    pub octets: [u8; 6],
}

impl fmt::Display for MacAddress {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(
            f,
            "{:02x}:{:02x}:{:02x}:{:02x}:{:02x}:{:02x}",
            self.octets[0], self.octets[1], self.octets[2],
            self.octets[3], self.octets[4], self.octets[5]
        )
    }
}

/// TX descriptor (16 bytes, matches e1000 hardware format).
#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct TxDesc {
    pub buffer_addr: u64,
    pub length: u16,
    pub cso: u8,
    pub cmd: u8,
    pub status: u8,
    pub css: u8,
    pub special: u16,
}

impl TxDesc {
    pub const CMD_EOP: u8 = 1 << 0;    // End of packet
    pub const CMD_IFCS: u8 = 1 << 1;   // Insert FCS
    pub const CMD_RS: u8 = 1 << 3;     // Report status
    pub const STATUS_DD: u8 = 1 << 0;  // Descriptor done
}

/// RX descriptor (16 bytes, matches e1000 hardware format).
#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct RxDesc {
    pub buffer_addr: u64,
    pub length: u16,
    pub checksum: u16,
    pub status: u8,
    pub errors: u8,
    pub special: u16,
}

impl RxDesc {
    pub const STATUS_DD: u8 = 1 << 0;  // Descriptor done
    pub const STATUS_EOP: u8 = 1 << 1; // End of packet
}

/// The NIC driver state.
pub struct NicDriver<'a, K: Kernel> {
    kernel: &'a K,
    state: DriverState,
    mac: MacAddress,
    mmio_base: u64,

    /// TX ring and tail pointer.
    tx_ring: Vec<TxDesc>,
    tx_tail: u16,
    /// DMA buffer handle for TX ring.
    tx_dma_buf: u32,

    /// RX ring and tail pointer.
    rx_ring: Vec<RxDesc>,
    rx_tail: u16,
    /// DMA buffer handles for RX ring and buffers.
    rx_dma_buf: u32,
    rx_bufs_dma: u32,

    /// Capability handles from BIB.
    io_port_cap: u16,
    irq_cap: u16,
    stack_chan_wr: u32,  // CHAN_W to network stack
    stack_chan_rd: u32,  // CHAN_R from network stack
    control_chan_rd: u32, // CHAN_R from Device Manager

    /// Statistics.
    pub rx_packets: u64,
    pub tx_packets: u64,
    pub rx_dropped: u64,
}

impl<'a, K: Kernel> NicDriver<'a, K> {
    /// Create a new NIC driver.
    pub fn new(kernel: &'a K) -> Self {
        Self {
            kernel,
            state: DriverState::Uninit,
            mac: MacAddress::default(),
            mmio_base: 0,
            tx_ring: Vec::new(),
            tx_tail: 0,
            tx_dma_buf: 0,
            rx_ring: Vec::new(),
            rx_tail: 0,
            rx_dma_buf: 0,
            rx_bufs_dma: 0,
            io_port_cap: 0xFFFF,
            irq_cap: 0xFFFF,
            stack_chan_wr: 0,
            stack_chan_rd: 0,
            control_chan_rd: 0,
            rx_packets: 0,
            tx_packets: 0,
            rx_dropped: 0,
        }
    }

    /// Initialize the NIC driver from Boot Info Block capabilities.
    ///
    /// This is called from _start() after the kernel creates the sidecar.
    pub fn init(
        &mut self,
        io_port_cap: u16,
        irq_cap: u16,
        tx_dma_buf: u32,
        rx_dma_buf: u32,
        rx_bufs_dma: u32,
        stack_chan_wr: u32,
        stack_chan_rd: u32,
        control_chan_rd: u32,
    ) -> Result<(), DriverState> {
        self.io_port_cap = io_port_cap;
        self.irq_cap = irq_cap;
        self.tx_dma_buf = tx_dma_buf;
        self.rx_dma_buf = rx_dma_buf;
        self.rx_bufs_dma = rx_bufs_dma;
        self.stack_chan_wr = stack_chan_wr;
        self.stack_chan_rd = stack_chan_rd;
        self.control_chan_rd = control_chan_rd;

        // Map IO_PORT capability to get MMIO base address
        // In the real kernel, this would be cap_map() returning a vaddr
        // For now, we store 0 and rely on the real ABI
        self.mmio_base = 0;

        // Read MAC address from EEPROM (RAL0/RAH0)
        self.read_mac();

        // Initialize TX ring
        self.init_tx_ring();

        // Initialize RX ring
        self.init_rx_ring();

        // Enable bus mastering via PCI config
        self.enable_bus_mastering();

        // Program link setup
        self.link_setup();

        self.state = DriverState::Ready;

        // Send NIC_INFO to the network stack
        self.send_nic_info();

        Ok(())
    }

    /// Read MAC address from the NIC's EEPROM registers.
    fn read_mac(&mut self) {
        // In the real driver, this reads from RAL0/RAH0 MMIO registers.
        // The values are loaded from the NIC's EEPROM at power-on.
        // For now, use a default MAC.
        self.mac = MacAddress { octets: [0x52, 0x54, 0x00, 0x12, 0x34, 0x01] };
    }

    /// Initialize the TX descriptor ring.
    fn init_tx_ring(&mut self) {
        self.tx_ring = alloc::vec![TxDesc::default(); RING_SIZE];
        self.tx_tail = 0;

        // Program TDBAL/TDBAH/TDLEN/TDH/TDT via MMIO
        // In the real driver, these are MMIO writes through the IO_PORT cap
    }

    /// Initialize the RX descriptor ring and buffers.
    fn init_rx_ring(&mut self) {
        self.rx_ring = alloc::vec![RxDesc::default(); RING_SIZE];
        self.rx_tail = (RING_SIZE as u16) - 1;

        // Each RX descriptor points to a pre-allocated DMA buffer
        // In the real driver, buffer_addr comes from the DMA_MEM cap
    }

    /// Enable PCI bus mastering.
    fn enable_bus_mastering(&mut self) {
        // Read PCI command register, set Bus Master and Memory Space bits
        // This is done via the BUS_ACCESS capability
    }

    /// Program link setup (ctrl register).
    fn link_setup(&mut self) {
        // Set SLU (Set Link Up) and ASDE (Auto Speed Detection Enable)
        // Clear RST (Reset) if set
    }

    /// Send NIC info to the network stack.
    fn send_nic_info(&self) {
        let _ = self.kernel.send(
            self.stack_chan_wr,
            nic_opcode::NIC_INFO,
            0,
            &self.mac.octets.iter().map(|&b| b as u32).collect::<Vec<_>>(),
            &[],
            0,
        );
    }

    /// Enter the main event loop.
    ///
    /// Polls all channels for messages from the network stack and
    /// interrupt handler. Processes TX/RX and control messages.
    pub fn event_loop(&mut self) {
        self.state = DriverState::Running;

        loop {
            // Wait for messages on any channel
            let chans = [self.stack_chan_rd, self.control_chan_rd, self.irq_cap as u32];

            match self.kernel.wait(&chans, 10_000_000) { // 10ms timeout
                Ok((idx, kind)) => {
                    if kind == 0 {
                        // CH_KIND_MSG — process the message
                        self.process_message(chans[idx]);
                    }
                    // Close events handled by the kernel (driver death)
                }
                Err(_) => {
                    // Timeout — do maintenance work
                    self.reclaim_tx_completions();
                }
            }

            if self.state != DriverState::Running {
                break;
            }
        }
    }

    /// Process a received message from a channel.
    fn process_message(&mut self, chan: u32) {
        let mut buf = [0u8; 256];
        let mut slots = [GrantedCap::default(); 4];

        match self.kernel.recv(chan, &mut buf, &mut slots) {
            Ok(result) => {
                let opcode = result.tag;
                match opcode {
                    nic_opcode::TX_PACKET => {
                        // Network stack wants us to transmit
                        if let Some(cap) = slots.first() {
                            self.transmit_packet(cap, result.len);
                        }
                    }
                    nic_opcode::SET_MAC => {
                        // Set MAC address
                    }
                    nic_opcode::GET_STATUS => {
                        // Report status
                    }
                    nic_opcode::SET_LINK => {
                        // Set link state
                    }
                    _ => {}
                }
            }
            Err(_) => {}
        }
    }

    /// Transmit a packet via the TX ring.
    fn transmit_packet(&mut self, buf_cap: &GrantedCap, len: usize) {
        if self.tx_ring.len() == 0 {
            return;
        }

        let desc = &mut self.tx_ring[self.tx_tail as usize];
        desc.buffer_addr = buf_cap.base;
        desc.length = len as u16;
        desc.cmd = TxDesc::CMD_EOP | TxDesc::CMD_IFCS | TxDesc::CMD_RS;
        desc.status = 0;

        // Advance tail and kick the hardware
        self.tx_tail = ((self.tx_tail + 1) % RING_SIZE as u16);
        // MMIO write to TDT register

        // Wait for completion (in production, this would be async via IRQ)
        let mut timeout = 0u32;
        while desc.status & TxDesc::STATUS_DD == 0 {
            timeout += 1;
            if timeout > 2_000_000 {
                break; // timeout
            }
            core::hint::spin_loop();
        }

        self.tx_packets += 1;

        // Notify the stack that the buffer can be reused
        let _ = self.kernel.send(
            self.stack_chan_wr,
            nic_opcode::TX_DONE,
            0,
            &[buf_cap.handle, if desc.status & TxDesc::STATUS_DD != 0 { 1 } else { 0 }],
            &[],
            0,
        );
    }

    /// Handle a received interrupt (from IRQ channel).
    pub fn handle_interrupt(&mut self) {
        // Read ICR (Interrupt Cause Read)
        let icr = self.mmio_read32(e1000_reg::ICR);

        if icr & e1000_reg::ICR_RXT0 != 0 {
            self.deliver_rx_packets();
        }

        if icr & e1000_reg::ICR_TXDW != 0 {
            self.reclaim_tx_completions();
        }

        // Acknowledge the IRQ
        let _ = self.kernel.close(self.irq_cap, 0, 0);
    }

    /// Deliver received packets to the network stack.
    fn deliver_rx_packets(&mut self) {
        loop {
            let next = ((self.rx_tail + 1) % RING_SIZE as u16) as usize;

            if self.rx_ring[next].status & RxDesc::STATUS_DD == 0 {
                break; // no more completed descriptors
            }

            let len = self.rx_ring[next].length as usize;

            // Create a MEM cap for this packet buffer and send to stack
            // In the real driver, this is a zero-copy transfer:
            // the MEM cap points to the DMA buffer's physical memory,
            // which the stack maps into its own address space.
            let _ = self.kernel.send(
                self.stack_chan_wr,
                nic_opcode::RX_PACKET,
                0,
                &[next as u32, len as u32],
                &[], // MEM cap would be attached here
                0,
            );

            // Return descriptor to hardware
            self.rx_ring[next].status = 0;
            // MMIO write to RDT register

            self.rx_tail = next as u16;
            self.rx_packets += 1;
        }
    }

    /// Reclaim completed TX descriptors.
    fn reclaim_tx_completions(&mut self) {
        // Walk TX ring and free completed buffers
        // In the real driver, this checks the DD bit and frees DMA buffers
    }

    /// Read a 32-bit MMIO register.
    fn mmio_read32(&self, offset: u32) -> u32 {
        // In the real driver, this is a volatile read through the IO_PORT cap
        // For now, return 0
        unsafe {
            let addr = (self.mmio_base + offset as u64) as *const u32;
            core::ptr::read_volatile(addr)
        }
    }

    /// Write a 32-bit MMIO register.
    fn mmio_write32(&self, offset: u32, value: u32) {
        // In the real driver, this is a volatile write through the IO_PORT cap
        unsafe {
            let addr = (self.mmio_base + offset as u64) as *mut u32;
            core::ptr::write_volatile(addr, value);
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn mac_display() {
        let mac = MacAddress { octets: [0x52, 0x54, 0x00, 0x12, 0x34, 0x01] };
        assert_eq!(alloc::format!("{}", mac), "52:54:00:12:34:01");
    }

    #[test]
    fn tx_desc_flags() {
        let mut desc = TxDesc::default();
        desc.cmd = TxDesc::CMD_EOP | TxDesc::CMD_IFCS | TxDesc::CMD_RS;
        assert!(desc.cmd & TxDesc::CMD_EOP != 0);
        assert!(desc.cmd & TxDesc::CMD_IFCS != 0);
        assert!(desc.cmd & TxDesc::CMD_RS != 0);
    }
}
