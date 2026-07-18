#ifndef SLSOS_CONFIG_H
#define SLSOS_CONFIG_H

// ─── AeroSLS Kernel Configuration ────────────────────────────────────────────
// Edit this file to configure the kernel for a specific hardware target.
// All values here can be overridden at runtime (e.g. by a future DHCP client).

// Static IP address assigned to the primary network interface.
// Format: 4 bytes in network (big-endian) byte order packed into a uint32_t.
// On a little-endian host, byte[0] is stored in the LSB of the integer.
//
//   IP 10.0.2.15  → bytes [0x0A, 0x00, 0x02, 0x0F] → 0x0F02000AUL
//   IP 192.168.1.100 → bytes [0xC0, 0xA8, 0x01, 0x64] → 0x6401A8C0UL
//
// This default (10.0.2.15) matches QEMU user-mode networking.
// Phase H2 (DHCP) will replace this with a DHCP-acquired address at runtime.
#define KERNEL_STATIC_IP  0x0F02000AUL   // 10.0.2.15

// Default gateway for the subnet above (QEMU user-net gateway = 10.0.2.2).
#define KERNEL_STATIC_GW  0x0202000AUL   // 10.0.2.2

// Kernel HTTP server port (guest-side; QEMU forwards host:3001 → guest:3000)
#define KERNEL_HTTP_PORT  3000

#endif /* SLSOS_CONFIG_H */
