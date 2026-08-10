/* arch/arm64/mmu.c — M4c: the VMSAv8-64 MMU, enabled at EL1 (plan doc
 * §6 M4c, §10.187: "VMSAv8-64, nothing like Sv39: TCR_EL1 (IPS/T0SZ),
 * MAIR_EL1 (device + normal WBWA), TTBR0/TTBR1, a 4-level 4 KiB walk").
 *
 * Design, honest about its size. A 4-level (L0/L1/L2/L3) 4 KiB-granule
 * walk identity-maps the kernel: qemu -M virt DRAM at 0x40000000, the
 * image (0x40080000..0x415a0040 — 21.2 MiB: the M2 chain arrays make
 * BSS ~21 MiB) plus slack, so a 32 MiB region 0x40000000..0x42000000
 * gets 4 KiB pages (16 L3 tables). The PL011 at 0x09000000 gets its own
 * device page (MAIR attr 0, PXN) reached through the SAME L0[0]/L1[0]
 * chain — L2[72] — so the whole map is one genuine L0->L1->L2->L3 walk.
 * TTBR1 is deliberately left unused (the deferred user-space half); the
 * kernel lives entirely in the TTBR0 half, like the RV64 kernel's
 * identity map. 20 tables x 4 KiB = 80 KiB of BSS, inside the mapped
 * region.
 *
 * Why identity: the boot smoke's A64 words are position-independent
 * enough for direct execution (§10.187), and the kernel code, stack,
 * and tables all sit in the mapped region, so SCTLR_EL1.M can be set in
 * place with no VA jump. MAIR attr 0 = device-nGnRnE (UART), attr 1 =
 * Normal WBWA (kernel image + tables). TCR_EL1: T0SZ=16 (48-bit VA
 * space), IPS=40-bit (safe on both cortex-a53 and cortex-a57 — qemu
 * models 40/44-bit PA respectively), TG0=4 KiB. SCTLR gains M|C|I, so
 * the Normal region is genuinely cacheable and coherent (the SIMI code
 * buffer gets its own explicit dc/ic flush before execution regardless
 * — kernel_arm64.c arm64_flush_icache).
 *
 * The enable sequence follows the ARM ARM transition discipline:
 * table stores made visible (dsb ish), TTBR programmed, TLB
 * invalidated, then SCTLR.M with an isb — no translation is used
 * between the mrs and the isb after setting M. */
#include <stdint.h>

#include "arch/arm64/mmu.h"

/* ---- map geometry ---- */
#define KERNEL_MAP_BASE 0x40000000UL      /* qemu virt DRAM start */
#define KERNEL_MAP_SIZE (32UL * 1024 * 1024) /* 0x40000000..0x42000000 */
#define N_L3_TABLES     (KERNEL_MAP_SIZE / 0x200000UL) /* 16 x 2 MiB */
#define UART_PAGE       0x09000000UL      /* virt PL011 MMIO page */

/* ---- descriptor bits (VMSAv8-64) ----
 * Low two bits: 0b00 invalid, 0b01 block (L1/L2 only), 0b11 table
 * (L0/L1/L2) or page (L3). The two encodings must stay DISTINCT
 * constants — the first M4c walk bug was defining them both as 0b11,
 * which folded the walk's block/table checks together and let the
 * compiler dead-code-eliminate the L3 descent (§10.190). */
#define DESC_VALID      0b11UL            /* table (L0-L2) / page (L3) */
#define DESC_BLOCK      0b01UL            /* block (L1/L2, unused here) */
#define DESC_AF         (1UL << 10)       /* access flag */
#define DESC_ATTR0      (0UL << 2)        /* MAIR index 0: device */
#define DESC_ATTR1      (1UL << 2)        /* MAIR index 1: normal WBWA */
#define DESC_PXN        (1UL << 53)       /* no EL1 execute */
#define DESC_ADDRMASK   (~0xFFFUL)

/* ---- system register encodings ---- */
#define MAIR_DEVICE_NGNRNE 0x00UL         /* attr 0 */
#define MAIR_NORMAL_WBWA   0xFFUL         /* attr 1 */
#define MAIR_EL1_VAL       (MAIR_NORMAL_WBWA << 8 | MAIR_DEVICE_NGNRNE)

/* T0SZ=16 (48-bit VA) | TG0=4 KiB (00) | IPS=0b010 (40-bit PA). */
#define TCR_EL1_VAL       (16UL | (0b010UL << 32))

/* Page tables live in BSS (zeroed by boot_arm64.S), 4 KiB aligned for
 * TTBR0. In this identity-map kernel, (uintptr_t)&table == its physical
 * address — the table descriptors store exactly that. */
static uint64_t g_pg_l0[512] __attribute__((aligned(4096)));
static uint64_t g_pg_l1[512] __attribute__((aligned(4096)));
static uint64_t g_pg_l2[512] __attribute__((aligned(4096)));
static uint64_t g_pg_l3_kernel[N_L3_TABLES][512] __attribute__((aligned(4096)));
static uint64_t g_pg_l3_uart[512] __attribute__((aligned(4096)));

void mmu_enable_identity(void)
{
    /* ---- build the tables (MMU still off; C stores, plain memory) ---- */
    uint64_t l1_pa = (uint64_t)(uintptr_t)g_pg_l1;
    uint64_t l2_pa = (uint64_t)(uintptr_t)g_pg_l2;

    g_pg_l0[0] = l1_pa | DESC_VALID;
    /* 0x40000000 is exactly 1 GiB: the kernel image sits at L1 index 1
     * ([1 GiB, 2 GiB)), the UART at 0x09000000 at L1 index 0. Both
     * entries share ONE L2 table: L2[0..15] (kernel 32 MiB) and L2[72]
     * (UART page). Missing this boundary was the first M4c bug — a
     * level-1 translation fault on the isb right after setting
     * SCTLR_EL1.M (§10.190). */
    g_pg_l1[0] = l2_pa | DESC_VALID;
    g_pg_l1[1] = l2_pa | DESC_VALID;

    for (unsigned i = 0; i < N_L3_TABLES; i++)
        g_pg_l2[i] = (uint64_t)(uintptr_t)g_pg_l3_kernel[i] | DESC_VALID;
    g_pg_l2[(UART_PAGE >> 21) & 0x1FF] =
        (uint64_t)(uintptr_t)g_pg_l3_uart | DESC_VALID;

    /* 32 MiB of 4 KiB identity pages: normal WBWA (attr 1), AF, EL1 RW
     * (AP=00), executable (no PXN) — the kernel text must run. */
    for (unsigned i = 0; i < N_L3_TABLES; i++) {
        for (unsigned j = 0; j < 512; j++) {
            uint64_t pa = KERNEL_MAP_BASE + (uint64_t)i * 0x200000
                          + (uint64_t)j * 0x1000;
            g_pg_l3_kernel[i][j] = pa | DESC_VALID | DESC_AF | DESC_ATTR1;
        }
    }

    /* The UART page: device (attr 0), AF, EL1 RW, no execute (PXN). */
    g_pg_l3_uart[(UART_PAGE >> 12) & 0x1FF] =
        UART_PAGE | DESC_VALID | DESC_AF | DESC_ATTR0 | DESC_PXN;

    /* ---- program the regime and enable (ARM ARM transition order) ---- */
    uint64_t ttbr0 = (uint64_t)(uintptr_t)g_pg_l0;
    uint64_t sctlr;
    asm volatile(
        "dsb ish\n"          /* table stores visible to the walker */
        "isb\n"
        "msr mair_el1, %[mair]\n"
        "msr tcr_el1, %[tcr]\n"
        "msr ttbr0_el1, %[ttbr0]\n"
        "isb\n"
        "tlbi vmalle1\n"     /* no stale TLB entries for the region */
        "dsb ish\n"
        "isb\n"
        "mrs %[sctlr], sctlr_el1\n"
        "orr %[sctlr], %[sctlr], #1\n"        /* M: MMU on */
        "orr %[sctlr], %[sctlr], #(1 << 2)\n" /* C: data cache (WBWA) */
        "orr %[sctlr], %[sctlr], #(1 << 12)\n"/* I: instruction cache */
        "msr sctlr_el1, %[sctlr]\n"
        "isb\n"
        : [sctlr] "=&r"(sctlr)
        : [mair] "r"(MAIR_EL1_VAL), [tcr] "r"(TCR_EL1_VAL),
          [ttbr0] "r"(ttbr0)
        : "memory");
}

uint64_t mmu_walk_va(uint64_t va)
{
    const uint64_t *l0 = (const uint64_t *)(uintptr_t)g_pg_l0;
    uint64_t d0 = l0[(va >> 39) & 0x1FF];
    if ((d0 & 3) != DESC_VALID)
        return 0;
    const uint64_t *l1 = (const uint64_t *)(uintptr_t)(d0 & DESC_ADDRMASK);
    uint64_t d1 = l1[(va >> 30) & 0x1FF];
    if ((d1 & 3) == DESC_BLOCK)
        return d1;                        /* 1 GiB block (unused here) */
    if ((d1 & 3) != DESC_VALID)
        return 0;
    const uint64_t *l2 = (const uint64_t *)(uintptr_t)(d1 & DESC_ADDRMASK);
    uint64_t d2 = l2[(va >> 21) & 0x1FF];
    if ((d2 & 3) == DESC_BLOCK)
        return d2;                        /* 2 MiB block (unused here) */
    if ((d2 & 3) != DESC_VALID)
        return 0;
    const uint64_t *l3 = (const uint64_t *)(uintptr_t)(d2 & DESC_ADDRMASK);
    uint64_t d3 = l3[(va >> 12) & 0x1FF];
    return (d3 & 3) == DESC_VALID ? d3 : 0;  /* L3 page descriptor */
}
