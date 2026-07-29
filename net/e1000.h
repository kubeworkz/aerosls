#ifndef E1000_H
#define E1000_H

#include "net.h"   /* MACAddr */

#include <stdint.h>

#define E1000_REG_CTRL    0x0000
#define E1000_REG_STATUS  0x0008
#define E1000_REG_RDBAL   0x2800
#define E1000_REG_RDBAH   0x2804
#define E1000_REG_RDLEN   0x2808
#define E1000_REG_RDH     0x2810
#define E1000_REG_RDT     0x2818
#define E1000_REG_RCTL    0x0100   // Receive Control
#define E1000_REG_TDBAL   0x3800
#define E1000_REG_TDBAH   0x3804
#define E1000_REG_TDLEN   0x3808
#define E1000_REG_TDH     0x3810
#define E1000_REG_TDT     0x3818
#define E1000_REG_TCTL    0x0400   // Transmit Control
#define E1000_REG_RAL0    0x5400   // Receive Address Low 0
#define E1000_REG_RAH0    0x5404   // Receive Address High 0

#define E1000_RCTL_EN     (1<<1)
#define E1000_RCTL_BAM    (1<<15)  // Broadcast Accept
#define E1000_RCTL_BSIZE  0        // 2048-byte buffers
#define E1000_TCTL_EN     (1<<1)
#define E1000_TCTL_PSP    (1<<3)

struct E1000TxDesc {
    uint64_t buffer_addr;
    uint16_t length;
    uint8_t  cso;
    uint8_t  cmd;
    uint8_t  status;
    uint8_t  css;
    uint16_t special;
} __attribute__((packed));

struct E1000RxDesc {
    uint64_t buffer_addr;
    uint16_t length;
    uint16_t checksum;
    uint8_t  status;
    uint8_t  errors;
    uint16_t special;
} __attribute__((packed));

/* How many interfaces the driver can bind. Phase 1 uses exactly one; the
 * bound exists now so the array type is settled before Phase 3 enumerates.
 * ~260 KiB each (see net/e1000.c), so this is cheap. */
#define E1000_MAX_NICS    2

#define E1000_RING_SIZE   128
#define E1000_RX_BUF_SIZE 2048


/*
 * Bring up interface `idx` and bind it with `roles`.
 *
 * Phase 3 gained the index and the roles. The MAC is read from THIS card's
 * EEPROM into its own slot -- previously every call overwrote one global,
 * which is fine with one card and silently wrong with two.
 */
void e1000_init(int idx, uint64_t mmio_base, uint8_t pci_slot, uint8_t roles);
/* ─── Interface roles ──────────────────────────────────────────────────────
 * Multi-NIC Plan Phase 2. Which interface a frame leaves by is NOT a routing
 * decision here: the two traffic classes are disjoint by protocol. Nothing in
 * net/dspp.c mentions IP, and nothing in ipv4/arp/udp/tcp mentions
 * ETHERTYPE_DSPP. So the caller already knows what kind of traffic it is
 * emitting, and says so -- no route table, no longest-prefix match.
 *
 * A BITMASK, not a single value, because that is what makes single-NIC boots
 * unchanged: one interface holds BOTH roles and answers every lookup. Every
 * node in existence today is single-NIC, so that path has to stay identical.
 */
typedef enum {
    NIC_ROLE_NONE    = 0,
    NIC_ROLE_MGMT    = 1u << 0,   /* IP: HTTP, ARP, TCP, UDP, DHCP */
    NIC_ROLE_CLUSTER = 1u << 1,   /* DSPP only: L2 broadcast, no IP */
} NicRole;

/*
 * Transmit on whichever interface holds `role`.
 *
 * Replaces e1000_transmit_packet(). The rename is deliberate: every one of
 * the four call sites had to be visited anyway, and a same-named function
 * that silently picked an interface would be the kind of default that is
 * wrong without anyone noticing.
 *
 * If no interface holds the role the frame is DROPPED and counted
 * (e1000_tx_no_route). Silently succeeding would make a misconfigured
 * cluster look healthy while nothing left the box.
 */
void e1000_transmit(NicRole role, void* physical_buffer, uint16_t size);

/* Frames dropped because no interface held the requested role. */
extern uint64_t e1000_tx_no_route;

/* Bind interface `idx` without touching hardware -- pure state.
 *
 * Split out of e1000_init() so the role logic is reachable from a host test:
 * everything else in e1000.c performs MMIO, which faults in a host process.
 * Same split as boot_params' parser and console.c's line editor.
 */
void e1000_nic_bind(int idx, uint64_t mmio_base, uint8_t pci_slot,
                    uint8_t roles, const MACAddr* mac);

/* How many interfaces are bound. */
int e1000_nic_count(void);

/* Interface `idx`'s own MAC, or NULL if `idx` is not bound. The self-echo
 * guard needs THIS rather than a single global: a frame from interface A
 * must be dropped on A and delivered on B. */
const MACAddr* e1000_nic_mac(int idx);

/* Test-only: forget every binding, so a test can check that the role
 * lookup fails rather than defaulting to interface 0. Production never
 * unbinds -- interfaces are found once at boot. */
void e1000_nics_reset_for_test(void);

/*
 * Record interface `idx`'s hardware address.
 *
 * If that interface holds NIC_ROLE_MGMT, its address ALSO becomes the
 * stack's global net_my_mac -- what ARP answers with and what the IP path
 * stamps on every frame. A cluster-only interface must not publish there,
 * or whichever card initialised last would decide the node's IP identity.
 */
void e1000_nic_set_mac(int idx, const MACAddr* mac);

/*
 * Assign roles, preferring an explicit `nicN=` on the kernel command line
 * and falling back to enumeration order when none is given.
 *
 * Returns 1 if the command line decided it, 0 if the fallback did. An
 * explicit `nicN=` for ANY interface switches the whole assignment to
 * command-line mode: naming one and inheriting a convention for the rest
 * is precisely the ambiguity being removed.
 *
 * Names: mgmt | cluster | both | none. Anything else leaves that interface
 * with NO role and logs -- a typo must not resolve to a plausible default.
 */
int e1000_assign_roles(int count, const char* cmdline);

/* "mgmt"|"cluster"|"both"|"none" -> bitmask. 1 on success, 0 if the name is
 * not one of those. Pure; exposed for testing. */
int e1000_role_from_name(const char* s, uint8_t* out);

/*
 * Assign roles to the `count` interfaces the boot scan found.
 *
 * Called AFTER the scan rather than during it, because a single card has to
 * hold both roles and that cannot be known until the count is.
 */
void e1000_assign_roles_by_order(int count);

/* Index of the interface holding `role`, or -1. */
int e1000_nic_by_role(NicRole role);
void e1000_poll_rx(void);   // drain receive ring; dispatches to net_rx_dispatch

#endif // E1000_H
