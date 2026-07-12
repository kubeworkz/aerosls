#ifndef DHCP_H
#define DHCP_H

// Attempt to acquire an IP address via DHCP.
// Sends DISCOVER, waits for OFFER, sends REQUEST, waits for ACK.
// On success: net_my_ip and net_gw_ip are updated.
// On timeout (no server responds): falls back to KERNEL_STATIC_IP from config.h.
// Blocks for at most ~5 seconds (timer-driven, not a busy-spin).
void dhcp_start(void);

#endif /* DHCP_H */
