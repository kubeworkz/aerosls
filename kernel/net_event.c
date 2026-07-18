#include "net_event.h"
#include "../net/e1000.h"
#include "kernel_io.h"

// net_poll_tick — called from timer_irq_handler() on every ~10 ms tick
//
// Drains the e1000 receive descriptor ring.  Each completed descriptor
// delivers a raw Ethernet frame to net_rx_dispatch(), which dispatches
// ARP → arp_handle_packet, IPv4/TCP → tcp_handle_segment, etc.
//
// Threads waiting in net_event_hlt_wait() wake on this same interrupt
// and recheck their TCP condition (rbuf_used, connection state) in their
// calling loop.  No explicit "ready" flags are needed — the TCP receive
// ring buffer IS the synchronisation point.
void net_poll_tick(void) {
    // Guard: skip if the NIC has not been initialised yet
    if (!e1000_mmio_base) return;

    e1000_poll_rx();
}
