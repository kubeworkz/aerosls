#include <stdint.h>
#include "e1000.h"
#include "../kernel/boot_params.h"   /* nicN= role assignment */
#include "net.h"
#include "../kernel/kernel_io.h"

// Mark a physical address range as uncacheable via MTRR variable range register.
// MTRR type 0 = UC (Uncacheable). Size must be a power of 2 and >= 4KB.
static void mtrr_set_uc(uint64_t base, uint64_t size) {
    // Disable caches + flush with WBINVD while we change MTRRs
    uint64_t cr0;
    __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
    // Set CD (bit 30) and clear NW (bit 29) to disable caching
    uint64_t cr0_new = (cr0 | (1ULL<<30)) & ~(1ULL<<29);
    __asm__ volatile("mov %0, %%cr0" :: "r"(cr0_new) : "memory");
    __asm__ volatile("wbinvd");

    // Disable all MTRRs temporarily (clear MTRRdefType.E bit 11)
    uint32_t mdef_lo, mdef_hi;
    __asm__ volatile("rdmsr" : "=a"(mdef_lo), "=d"(mdef_hi) : "c"(0x2FF));
    __asm__ volatile("wrmsr" :: "c"(0x2FF), "a"(mdef_lo & ~(1U<<11)), "d"(mdef_hi));

    // Program MTRRphysBase0 (0x200) and MTRRphysMask0 (0x201)
    uint64_t phys_base = (base & ~0xFFFULL) | 0;  // type=0 (UC)
    uint64_t phys_mask = (~(size - 1) & 0x000FFFFFFFFFF000ULL) | (1ULL << 11);
    __asm__ volatile("wrmsr" :: "c"(0x200),
                     "a"((uint32_t)phys_base), "d"((uint32_t)(phys_base>>32)));
    __asm__ volatile("wrmsr" :: "c"(0x201),
                     "a"((uint32_t)phys_mask), "d"((uint32_t)(phys_mask>>32)));

    // Re-enable MTRRs
    __asm__ volatile("wrmsr" :: "c"(0x2FF), "a"(mdef_lo | (1U<<11)), "d"(mdef_hi));
    __asm__ volatile("wbinvd");
    // Restore caching
    __asm__ volatile("mov %0, %%cr0" :: "r"(cr0) : "memory");
}


/* ─── Per-interface state ──────────────────────────────────────────────────
 * Multi-NIC Plan Phase 1. Every field below was a file-scope global, which
 * is why this driver could only ever bind ONE card: a second -device e1000
 * had nowhere to keep its rings and sat dead on the PCI bus. Collecting
 * them into a struct is the whole of Phase 1 -- there is still exactly one
 * instance and no behaviour change, so the existing suite is the gate.
 *
 * Cost, since it was previously guessed at rather than measured: 128
 * descriptors x 2048 B of buffers = 256 KiB, plus 4 KiB of descriptors, so
 * ~260 KiB per interface. On a 1 GiB node that is 0.2%. The array size is
 * not a memory trade-off worth agonising over.
 */
struct E1000Nic {
    uint64_t mmio_base;
    uint8_t  pci_slot;      /* for PCI config writes */
    uint8_t  present;       /* 0 until e1000_nic_bind() has run on this slot */
    uint8_t  roles;         /* bitmask of NicRole -- one NIC may hold both */
    MACAddr  mac;           /* this interface's own, from its EEPROM */
    uint16_t tx_tail;
    /* rx_tail starts at RING_SIZE-1 so the first poll checks descriptor 0.
     * The e1000 fills from RDH=0; we give it back descriptors as we
     * consume them. */
    uint16_t rx_tail;
    struct E1000TxDesc tx_ring[E1000_RING_SIZE] __attribute__((aligned(16)));
    struct E1000RxDesc rx_ring[E1000_RING_SIZE] __attribute__((aligned(16)));
    uint8_t  rx_bufs[E1000_RING_SIZE][E1000_RX_BUF_SIZE] __attribute__((aligned(16)));
};

static struct E1000Nic e1000_nics[E1000_MAX_NICS];

/* nic0() lived here through Phases 1-2 as the single-interface accessor.
 * Phase 3 removed its last caller: e1000_init() now takes an index, and
 * everything else resolves by role. Deleted rather than left as a
 * convenience nobody uses. */

uint64_t e1000_tx_no_route = 0;

/* Freestanding: no libc. Per-file helper, matching this tree's convention
 * (p_memcpy, sr_streq, wl_strcpy, mh_streq, bp_key_matches). */
static int e1000_streq(const char* a, const char* b) {
    while (*a && *b) { if (*a != *b) return 0; a++; b++; }
    return *a == *b;
}

void e1000_nic_bind(int idx, uint64_t mmio_base, uint8_t pci_slot,
                    uint8_t roles, const MACAddr* mac) {
    if (idx < 0 || idx >= E1000_MAX_NICS) return;
    struct E1000Nic* n = &e1000_nics[idx];
    n->mmio_base = mmio_base;
    n->pci_slot  = pci_slot;
    n->roles     = roles;
    n->tx_tail   = 0;
    n->rx_tail   = E1000_RING_SIZE - 1;
    if (mac) n->mac = *mac;
    n->present   = 1;
}

int e1000_nic_count(void) {
    int c = 0;
    for (int i = 0; i < E1000_MAX_NICS; i++) if (e1000_nics[i].present) c++;
    return c;
}

int e1000_nic_idx_for_slot(uint8_t pci_slot) {
    for (int i = 0; i < E1000_MAX_NICS; i++)
        if (e1000_nics[i].present && e1000_nics[i].pci_slot == pci_slot)
            return i;
    return -1;
}

const MACAddr* e1000_nic_mac(int idx) {
    if (idx < 0 || idx >= E1000_MAX_NICS || !e1000_nics[idx].present) return 0;
    return &e1000_nics[idx].mac;
}

/* Test-only: forget every binding. The driver has no unbind path in
 * production -- interfaces are found once at boot and stay -- but a test
 * that cannot return to "nothing bound" cannot check that the role lookup
 * FAILS rather than defaulting to interface 0. */
void e1000_nics_reset_for_test(void) {
    /* Clears the whole entry, not just `present`. Leaving the MAC and roles
     * behind made this a half-reset: a later bind() that passed no address
     * would silently inherit the previous test's, and a mutation deleting
     * e1000_nic_set_mac()'s store survived because of it. A reset that
     * leaves state behind is worse than no reset -- it makes tests pass
     * for the wrong reason. */
    for (int i = 0; i < E1000_MAX_NICS; i++) {
        struct E1000Nic* n = &e1000_nics[i];
        n->present   = 0;
        n->roles     = (uint8_t)NIC_ROLE_NONE;
        n->mmio_base = 0;
        n->pci_slot  = 0;
        n->tx_tail   = 0;
        n->rx_tail   = E1000_RING_SIZE - 1;
        for (int b = 0; b < 6; b++) n->mac.b[b] = 0;
    }
}

void e1000_nic_set_mac(int idx, const MACAddr* mac) {
    if (idx < 0 || idx >= E1000_MAX_NICS || !mac) return;
    struct E1000Nic* n = &e1000_nics[idx];
    n->mac = *mac;

    /* Only the MANAGEMENT interface publishes its address as the stack's
     * global one. net_my_mac is what ARP puts in its replies and what the
     * IP path stamps into every frame -- with two cards both writing it,
     * whichever initialised last would win, and the management NIC could
     * end up announcing the cluster NIC's address. The cluster interface
     * has no IP and needs no entry here; its identity lives in n->mac,
     * which is what the self-echo guard reads.
     *
     * Split out of e1000_init() so this rule is reachable from a host
     * test -- inside e1000_init() it sat behind MMIO and a mutation
     * removing the gate survived the whole suite. */
    if (n->roles & (uint8_t)NIC_ROLE_MGMT) net_my_mac = n->mac;
}

/* "mgmt" | "cluster" | "both" | "none" -> a role bitmask.
 *
 * Returns 0 for anything else. An unrecognised name must NOT fall through
 * to a default: a typo like nic0=mgnt silently becoming management is how
 * DSPP ends up on the wrong wire, and the whole point of naming roles
 * explicitly is that they are checkable. */
int e1000_role_from_name(const char* s, uint8_t* out) {
    if (!s || !out) return 0;
    if (e1000_streq(s, "mgmt"))    { *out = (uint8_t)NIC_ROLE_MGMT;    return 1; }
    if (e1000_streq(s, "cluster")) { *out = (uint8_t)NIC_ROLE_CLUSTER; return 1; }
    if (e1000_streq(s, "both"))    { *out = (uint8_t)(NIC_ROLE_MGMT | NIC_ROLE_CLUSTER); return 1; }
    if (e1000_streq(s, "none"))    { *out = (uint8_t)NIC_ROLE_NONE;    return 1; }
    return 0;
}

int e1000_assign_roles(int count, const char* cmdline) {
    if (count <= 0) return 0;
    if (count > E1000_MAX_NICS) count = E1000_MAX_NICS;

    /* An explicit nicN= for ANY interface switches the whole assignment to
     * command-line mode. Mixing "some explicit, rest by order" would mean
     * an operator who names one interface silently gets a convention for
     * the other -- exactly the ambiguity the command line exists to remove.
     */
    int any_explicit = 0;
    for (int i = 0; i < count; i++) {
        char key[8], val[16];
        key[0]='n'; key[1]='i'; key[2]='c'; key[3]=(char)('0'+i); key[4]='\0';
        if (boot_params_find_str(cmdline, key, val, (int)sizeof(val))) { any_explicit = 1; break; }
    }

    if (!any_explicit) {
        e1000_assign_roles_by_order(count);
        return 0;
    }

    for (int i = 0; i < count; i++) {
        char key[8], val[16];
        key[0]='n'; key[1]='i'; key[2]='c'; key[3]=(char)('0'+i); key[4]='\0';
        uint8_t roles = (uint8_t)NIC_ROLE_NONE;

        if (!boot_params_find_str(cmdline, key, val, (int)sizeof(val))) {
            /* Named some but not this one. Left with no role, and said so:
             * an interface that transmits nothing is a real configuration,
             * but never an accidental one. */
            kernel_serial_printf("[E1000] nic%d has no %s= on the command line "
                                 "-- left with NO role.\n", i, key);
        } else if (!e1000_role_from_name(val, &roles)) {
            roles = (uint8_t)NIC_ROLE_NONE;
            kernel_serial_printf("[E1000] nic%d: '%s' is not a role "
                                 "(mgmt|cluster|both|none) -- left with NO role.\n",
                                 i, val);
        }
        e1000_nics[i].roles = roles;
    }
    return 1;
}

void e1000_assign_roles_by_order(int count) {
    if (count <= 0) return;
    if (count > E1000_MAX_NICS) count = E1000_MAX_NICS;

    if (count == 1) {
        /* One card carries everything. This is every node in existence
         * today, and the path that must behave exactly as it did before
         * multi-NIC support was added. */
        e1000_nics[0].roles = (uint8_t)(NIC_ROLE_MGMT | NIC_ROLE_CLUSTER);
        return;
    }

    /* First card management, second the cluster segment, matching the order
     * QEMU's -device arguments produce. A convention, not a discovery --
     * nothing in PCI config space says which wire a card is on. Phase 4
     * replaces this with an explicit `nicN=` on the kernel command line;
     * this stays as the fallback for when none is given. */
    e1000_nics[0].roles = (uint8_t)NIC_ROLE_MGMT;
    e1000_nics[1].roles = (uint8_t)NIC_ROLE_CLUSTER;
    for (int i = 2; i < count; i++) e1000_nics[i].roles = (uint8_t)NIC_ROLE_NONE;
}

int e1000_nic_by_role(NicRole role) {
    for (int i = 0; i < E1000_MAX_NICS; i++)
        if (e1000_nics[i].present && (e1000_nics[i].roles & (uint8_t)role))
            return i;
    return -1;
}

static inline volatile uint32_t* nic_reg(struct E1000Nic* n, uint32_t off) {
    return (volatile uint32_t*)(n->mmio_base + off);
}

// Force a 32-bit MMIO write using inline assembly to prevent the compiler
// from reordering or caching the write.
static inline void mmio_write32(uint64_t addr, uint32_t val) {
    __asm__ volatile("movl %0, (%1)" :: "r"(val), "r"((volatile uint32_t*)addr) : "memory");
}

static inline uint32_t mmio_read32(uint64_t addr) {
    uint32_t v;
    __asm__ volatile("movl (%1), %0" : "=r"(v) : "r"((volatile uint32_t*)addr) : "memory");
    return v;
}

void e1000_init(int idx, uint64_t mmio_base, uint8_t pci_slot, uint8_t roles) {
    if (idx < 0 || idx >= E1000_MAX_NICS) return;
    struct E1000Nic* n = &e1000_nics[idx];
    e1000_nic_bind(idx, mmio_base, pci_slot, roles, 0);

    // Enable PCI Memory Space (bit 1) and Bus Master (bit 2) for this NIC.
    // Using the dynamically detected slot so the same binary works on any hardware.
    {
        extern uint32_t pci_read_config(uint8_t, uint8_t, uint8_t, uint8_t);
        extern void pci_write_config(uint8_t, uint8_t, uint8_t, uint8_t, uint32_t);
        uint32_t cmd = pci_read_config(0, pci_slot, 0, 0x04);
        cmd |= (1U << 1) | (1U << 2);
        pci_write_config(0, pci_slot, 0, 0x04, cmd);
    }

    // Mark the MMIO BAR0 region as uncacheable via MTRR.
    mtrr_set_uc(mmio_base & ~0xFFFULL, 0x20000ULL);

    // ── Link Up Setup ─────────────────────────────────────────────────────────
    #define E1000_CTRL_RST  (1U << 26)
    #define E1000_CTRL_SLU  (1U << 6)
    #define E1000_CTRL_ASDE (1U << 5)
    uint32_t ctrl = mmio_read32(mmio_base + E1000_REG_CTRL);
    ctrl = (ctrl & ~E1000_CTRL_RST) | E1000_CTRL_SLU | E1000_CTRL_ASDE;
    mmio_write32(mmio_base + E1000_REG_CTRL, ctrl);
    for (volatile int i = 0; i < 50000; i++) __asm__ volatile("pause");
    __asm__ volatile("wbinvd");

    // ── TX ring ──────────────────────────────────────────────────────────────
    for (int i = 0; i < E1000_RING_SIZE; i++) {
        n->tx_ring[i].buffer_addr = 0;
        n->tx_ring[i].status      = 0xF;  // mark all as done
    }
    *nic_reg(n, E1000_REG_TDBAL) = (uint32_t)(uint64_t)n->tx_ring;
    *nic_reg(n, E1000_REG_TDBAH) = (uint32_t)((uint64_t)n->tx_ring >> 32);
    *nic_reg(n, E1000_REG_TDLEN) = E1000_RING_SIZE * sizeof(struct E1000TxDesc);
    *nic_reg(n, E1000_REG_TDH)   = 0;
    *nic_reg(n, E1000_REG_TDT)   = 0;
    // CT (bits 11:4) = 0x0F, COLD (bits 21:12) = 0x040 (full-duplex)
    *nic_reg(n, E1000_REG_TCTL)  = E1000_TCTL_EN | E1000_TCTL_PSP
                          | (0x0FU << 4)   /* CT  */
                          | (0x040U << 12); /* COLD */

    // ── MAC from EEPROM ──────────────────────────────────────────────────────
    // RAL0/RAH0 are loaded from the NIC's EEPROM at power-on (or from the MAC
    // specified on the QEMU command line).  Read them now — before we write
    // anything to those registers. The ONLY publisher of the stack-wide
    // net_my_mac is e1000_nic_set_mac(): it fires for a NIC holding
    // NIC_ROLE_MGMT and ignores everyone else, so a second (cluster-only or
    // user-driver) card can never overwrite the node's IP identity. Roles are
    // assigned BEFORE e1000_init runs (kernel.c step 7), so the gate sees the
    // real role here. (The direct net_my_mac.b[] stores this block used to
    // make were a pre-role relic: they bypassed the gate, left b[5] zero, and
    // let a later card clobber the address.)
    {
        uint32_t ral = *nic_reg(n, E1000_REG_RAL0);
        uint32_t rah = *nic_reg(n, E1000_REG_RAH0);
        MACAddr eeprom;
        eeprom.b[0] = (uint8_t)(ral);
        eeprom.b[1] = (uint8_t)(ral >>  8);
        eeprom.b[2] = (uint8_t)(ral >> 16);
        eeprom.b[3] = (uint8_t)(ral >> 24);
        eeprom.b[4] = (uint8_t)(rah);
        eeprom.b[5] = (uint8_t)(rah >>  8);
        e1000_nic_set_mac(idx, &eeprom);

        kernel_serial_printf("[E1000] nic%d MAC %02x:%02x:%02x:%02x:%02x:%02x  roles:%s%s\n",
            idx, n->mac.b[0], n->mac.b[1], n->mac.b[2],
            n->mac.b[3], n->mac.b[4], n->mac.b[5],
            (roles & NIC_ROLE_MGMT)    ? " mgmt"    : "",
            (roles & NIC_ROLE_CLUSTER) ? " cluster" : "");
    }

    // ── RX ring ──────────────────────────────────────────────────────────────
    // Program the EEPROM MAC back into the receive address filter (slot 0),
    // setting the Address Valid (AV) bit so the NIC accepts frames for our MAC.
    /* n->mac, not net_my_mac: each card filters for its OWN address. */
    *nic_reg(n, E1000_REG_RAL0) = ((uint32_t)n->mac.b[0])
                         | ((uint32_t)n->mac.b[1] <<  8)
                         | ((uint32_t)n->mac.b[2] << 16)
                         | ((uint32_t)n->mac.b[3] << 24);
    *nic_reg(n, E1000_REG_RAH0) = ((uint32_t)n->mac.b[4])
                         | ((uint32_t)n->mac.b[5] <<  8)
                         | (1U << 31); // AV = Address Valid

    // Also enable unicast+multicast promiscuous so we never miss a packet
    // RCTL_UPE=bit3, RCTL_MPE=bit4 — harmless in addition to BAM
    for (int i = 0; i < E1000_RING_SIZE; i++) {
        n->rx_ring[i].buffer_addr = (uint64_t)n->rx_bufs[i];
        n->rx_ring[i].status      = 0;
    }
    *nic_reg(n, E1000_REG_RDBAL) = (uint32_t)(uint64_t)n->rx_ring;
    *nic_reg(n, E1000_REG_RDBAH) = (uint32_t)((uint64_t)n->rx_ring >> 32);
    *nic_reg(n, E1000_REG_RDLEN) = E1000_RING_SIZE * sizeof(struct E1000RxDesc);
    *nic_reg(n, E1000_REG_RDH)   = 0;
    *nic_reg(n, E1000_REG_RDT)   = E1000_RING_SIZE - 1;
    *nic_reg(n, E1000_REG_RCTL)  = E1000_RCTL_EN | E1000_RCTL_BAM | (1U<<3) | (1U<<4);
}

uint8_t e1000_nic_roles(int idx) {
    if (idx < 0 || idx >= E1000_MAX_NICS) return (uint8_t)NIC_ROLE_NONE;
    return e1000_nics[idx].roles;
}

/* ─── Driver-ownership handoff ───────────────────────────────────────────────
 * A NIC whose assigned role is NONE belongs to a user driver sidecar
 * (drv.e1000.0, spawned by the DM): the kernel enables PCI memory space +
 * bus mastering and marks the MMIO BAR uncacheable so the driver's DMA
 * works, then NEVER touches the device again — no CTRL writes, no ring
 * programming, no RX poll. The driver maps BAR0 via SYS_DEV_MMAP and owns
 * every register from reset on. kernel/kernel.c calls this after role
 * assignment (grub passes nic0=both nic1=none; enumeration order is the
 * fallback) instead of e1000_init() for role-less NICs. */
void e1000_driver_handoff(int idx, uint64_t mmio_base, uint8_t pci_slot) {
    if (idx < 0 || idx >= E1000_MAX_NICS) return;
    struct E1000Nic* n = &e1000_nics[idx];
    e1000_nic_bind(idx, mmio_base, pci_slot, (uint8_t)NIC_ROLE_NONE, 0);

    /* Enable PCI Memory Space (bit 1) and Bus Master (bit 2) so the user
     * driver's DMA works — the same pair e1000_init() sets for the
     * kernel-owned NICs. The role-less NIC is otherwise left completely
     * alone. */
    {
        extern uint32_t pci_read_config(uint8_t, uint8_t, uint8_t, uint8_t);
        extern void pci_write_config(uint8_t, uint8_t, uint8_t, uint8_t, uint32_t);
        uint32_t cmd = pci_read_config(0, pci_slot, 0, 0x04);
        cmd |= (1U << 1) | (1U << 2);
        pci_write_config(0, pci_slot, 0, 0x04, cmd);
    }

    /* Same uncacheable marking e1000_init() applies — device MMIO must not
     * be cached on real hardware. */
    mtrr_set_uc(mmio_base & ~0xFFFULL, 0x20000ULL);

    kernel_serial_printf(
        "[E1000] nic%d MMIO 0x%llx handed to user driver (role: none)\n",
        idx, (unsigned long long)mmio_base);
}

void e1000_transmit(NicRole role, void* physical_buffer, uint16_t size) {
    int idx = e1000_nic_by_role(role);
    if (idx < 0) {
        /* No interface holds this role. Drop and count rather than fall back
         * to "whichever exists": sending cluster traffic out a management
         * NIC would leak DSPP onto the wrong segment, and sending IP out the
         * cluster NIC would go nowhere. A visible counter beats either. */
        e1000_tx_no_route++;
        return;
    }
    struct E1000Nic* n = &e1000_nics[idx];
    // NOTE: We do NOT disable interrupts here.  The e1000 TX descriptor ring
    // is written atomically (TDT update is a single 32-bit MMIO write) and
    // the timer ISR only reads RX descriptors via e1000_poll_rx — it never
    // touches TX.  Holding cli would block the LAPIC timer, preventing ARP
    // replies from being processed during the TX wait, causing a deadlock when
    // the SYN-ACK needs ARP for the gateway and the ARP reply never arrives.

    struct E1000TxDesc* desc = &n->tx_ring[n->tx_tail];
    desc->buffer_addr = (uint64_t)physical_buffer;
    desc->length      = size;
    desc->cmd         = (1 << 0) | (1 << 1) | (1 << 3); // EOP + IFCS + RS
    desc->status      = 0;

    n->tx_tail = (uint16_t)((n->tx_tail + 1) % E1000_RING_SIZE);
    *nic_reg(n, E1000_REG_TDT) = n->tx_tail;

    uint32_t tx_timeout = 0;
    while (!(desc->status & 0x01)) {
        if (++tx_timeout > 2000000) break;
        __asm__ volatile("pause");
    }
}

void e1000_poll_rx(void) {
    // Re-entrancy guard: the timer ISR calls us, but tcp_handle_segment called
    // from within us may trigger another TX which fires another timer tick.
    // A static flag prevents recursive/reentrant calls from corrupting state.
    static volatile uint8_t _in_poll = 0;
    if (_in_poll) return;
    _in_poll = 1;

    for (int idx = 0; idx < E1000_MAX_NICS; idx++) {
    struct E1000Nic* n = &e1000_nics[idx];
    if (!n->present) continue;
    /* A user-driver NIC (role: none) has no kernel-programmed rings — the
     * kernel never polls it, because the driver owns every register.
     * Polling an unprogrammed ring would read garbage descriptors and
     * could even race the driver's own ring setup. */
    if (!(n->roles & ((uint8_t)NIC_ROLE_MGMT | (uint8_t)NIC_ROLE_CLUSTER)))
        continue;
    for (;;) {
        uint16_t next = (uint16_t)((n->rx_tail + 1) % E1000_RING_SIZE);
        struct E1000RxDesc* desc = &n->rx_ring[next];

        // Check DD (descriptor done) bit — hardware sets this when the
        // descriptor has been filled with a received packet.
        if (!(desc->status & 0x01)) break;  // no more completed descriptors

        // Pass frame to the network stack
        /* Which interface it arrived on travels with the frame: the
         * self-echo guard has to compare against THIS NIC's MAC, not a
         * single global one. */
        net_rx_dispatch(n->rx_bufs[next], desc->length, idx);

        // Return descriptor to hardware and advance our tail pointer
        desc->status = 0;
        *nic_reg(n, E1000_REG_RDT) = next;
        n->rx_tail = next;
    }
    }
    _in_poll = 0;
}
