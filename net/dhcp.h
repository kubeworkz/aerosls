#ifndef DHCP_H
#define DHCP_H

// Attempt to acquire an IP address via DHCP.
// Sends DISCOVER, waits for OFFER, sends REQUEST, waits for ACK.
// On success: net_my_ip, net_gw_ip, and net_subnet_mask (Navigator-Parity
// Gap Roadmap Phase 5a) are updated.
// On timeout (no server responds): falls back to KERNEL_STATIC_IP from config.h.
// Blocks for at most ~5 seconds (timer-driven, not a busy-spin).
void dhcp_start(void);

// Navigator-Parity Gap Roadmap Phase 5a: reports whether the DHCP client
// completed a real lease (OFFER + ACK) versus falling back to the static
// config.h defaults. Used by GET /api/network/status to report an honest
// "dhcp_bound" flag rather than always claiming the IP/gateway/subnet came
// from a live lease.
int dhcp_is_bound(void);

#endif /* DHCP_H */
