/* kernel/kernel_arm64.c — M4a/M4b/M4c/M5/M5.1: the minimal arm64 kernel
 * main.
 *
 * Gap Remediation SIMI Phase 10 / M4-M5.1 (docs/AeroSLS-SIMI-ARM-Backend-
 * Plan-v0.1.md §6): a self-contained qemu -M virt build whose boot
 * prints a banner, runs the kernel in the TTBR1 half under the
 * VMSAv8-64 MMU (M4c), translates and EXECUTES an embedded .tmo through
 * kernel/simi_arm.c (M4b), and — M5, this milestone — runs a translated
 * SIMI program in EL0 from a USER VA that the kernel's own tables do
 * not map (the TTBR0/TTBR1 split), then ends in a clean power-off via
 * PSCI SYSTEM_OFF (`smc #0`, the SBI_SRST analog; qemu exits rc=0).
 * M5.1 adds the activation cache (translate-on-first-use, §10.192):
 * the boot gate is four entries — EL1 direct (MISS), EL1 direct (HIT),
 * EL0 (HIT), EL0 (HIT) — one translation, all returning 42.
 *
 * The MMU is enabled by boot_arm64.S before this main runs: TTBR1
 * holds the kernel (high VAs), TTBR0 is parked on an all-invalid root,
 * VBAR_EL1 points at the vector table. The kernel therefore NEVER
 * dereferences a low VA — the PL011 is reached at its high VA
 * (uart_pl011.c), and the selfcheck proves the user VAs are
 * unmapped-to-kernel by walking the kernel root at one and reading 0.
 *
 * M5's EL0 excursion (the interesting part): the translated blob is
 * position-independent (its only baked absolutes are the scratch
 * pointer, which we pass as a USER VA, and r6's namepool pointer, which
 * the smoke never dereferences; its register frame is SP-relative), so
 * the same bytes run in EL0. We map the code buffer's physical page,
 * an 8 KiB stack, a scratch page, and a `svc #0` stub page into a
 * fresh TTBR0 tree (mmu.c) at USER_* VAs with AP=01 (EL1 RW + EL0 RW);
 * then set SP_EL0 = user stack top, x30 = the stub, ELR_EL1 = the
 * entry, SPSR_EL1 = EL0t, switch TTBR0, and eret. The blob runs, its
 * trampoline `br x30`s into the stub, `svc #0` traps to the vector
 * table's sync-lower-EL slot (boot_arm64.S), which saves the result
 * (still in t0/x9) to g_user_result and erets back to the continuation
 * address below. All kernel-side references are TTBR1, so the user
 * TTBR0 staying installed is harmless. The [M5] lines assert the
 * result (42) and both walk proofs.
 *
 * No libc, no FP/SIMD: compiled -mgeneral-regs-only, mirroring the x86
 * kernel's -mno-sse discipline. */
#include <stddef.h>
#include <stdint.h>

#include "arch/arm64/gic.h"
#include "arch/arm64/mmu.h"
#include "arch/arm64/uart_pl011.h"
#include "arm64_boot_smoke_tmo.h"
#include "simi_arm.h"

/* PSCI 0.2 function ids (smc #0 to the virt EL3 monitor). */
#define PSCI_FN_SYSTEM_OFF 0x84000008UL

/* The svc-from-EL0 handshake (boot_arm64.S arm64_svc_from_el0): the
 * handler stores the user result (x9) here and erets to the address
 * here, which arm64_el0_activate sets before the eret into EL0. */
uint64_t g_user_result;
uint64_t g_user_ret_addr;

/* The freestanding {memcpy, memset} pair the M2 chain struct copies in
 * simi_arm.c synthesize (§10.181). Byte loops: GCC recognizes the
 * function names as builtins but cannot turn their own definitions into
 * recursive calls, so these are self-contained (verified: the linked
 * ELF has zero undefined symbols). */
void *memcpy(void *dst, const void *src, unsigned long n)
{
    unsigned char *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;
    while (n--)
        *d++ = *s++;
    return dst;
}

void *memset(void *s, int c, unsigned long n)
{
    unsigned char *p = (unsigned char *)s;
    while (n--)
        *p++ = (unsigned char)c;
    return s;
}

/* The emitted-code buffer (M4b): 4 KiB, page-aligned since M5 (its
 * physical page is mapped into the user tables at USER_CODE_VA). The
 * smoke program emits ~1 KiB of native A64, so 4 KiB is ample;
 * overflow would trip the translator's out_cap. */
#define ARM64_SMOKE_CODE_BUF_SIZE 4096
static uint8_t g_smoke_code_buf[ARM64_SMOKE_CODE_BUF_SIZE]
    __attribute__((aligned(4096)));
static uint32_t g_code_len, g_entry_off;

/* M5.1 activation cache (§10.192, the x86 kernel/simi_translate.c
 * precedent, ISA doc §11): translate-on-first-use. The cache key is
 * object name + FNV-1a hash + .tmo byte size — re-uploading the same
 * name with different bytes is a MISS, not a stale HIT. The emitted
 * code lives in g_smoke_code_buf (one slot today — the array shape
 * generalizes, static-array discipline, no allocator). The register
 * frame is deliberately NOT cached: it is SP-relative, carved per entry
 * off the kernel stack (EL1) or a fresh user stack (EL0). */
#define ARM64_ACT_SLOTS      1
#define ARM64_ACT_NAME_LEN   24
struct Arm64Activation {
    char     name[ARM64_ACT_NAME_LEN];
    uint32_t content_hash;   /* FNV-1a of the .tmo bytes */
    uint32_t tmo_len;        /* .tmo byte size */
    uint32_t code_len;       /* translated A64 length */
    uint32_t entry_off;      /* entry offset within g_smoke_code_buf */
    uint32_t valid;
};
static struct Arm64Activation g_act[ARM64_ACT_SLOTS];
static uint32_t g_translate_count;   /* the M5.1 gate: exactly 1 at boot */
static uint32_t g_el0_excursions;    /* the M5.1 gate: exactly 2 at boot */

/* M5.2: the tick counter, bumped by the EL1h IRQ handler. Volatile —
 * written by arm64_tick_irq, read by the wait loop. */
static volatile uint32_t g_tick_count;
#define M52_TICK_TARGET 4            /* the M5.2 gate: [TICK 1..4], unbroken — 2
                                     * pended through the EL0 windows, 2
                                     * during idle (contention probe, §10.196) */
#define M53_TICK_TARGET 6            /* the M5.3 gate: [TICK 1..6], unbroken —
                                     * TICK 5 physical + TICK 6 nested virtual
                                     * (the M5.3 nesting phase, §10.197) */

static void print_u64_dec(uint64_t v);

/* M5.2: the EL1h IRQ handler (boot_arm64.S arm64_irq_el1h calls this
 * with caller-saved regs saved). Acknowledge; if spurious (1023) there
 * is nothing to EOIR; re-arm the timer BEFORE the EOIR (minimize the
 * window a tick could be missed — the RISC-V discipline); bump the
 * counter; print [TICK N]; end. */
void arm64_tick_irq(void)
{
    uint32_t intid = gic_iar();
    if (intid == 1023)
        return;
    if (intid == GIC_VIRT_TIMER_INTID) {
        /* M5.3 nested tick: the virtual timer (INTID 27, priority 0x00)
         * preempted the physical handler. One-shot: disarm it FIRST so
         * the level deasserts before the EOIR (else the line re-asserts
         * and storms); no re-arm — the nesting is a single, bounded
         * event. The EL1h IRQ entry (boot_arm64.S) saved/restored the
         * caller-saved set, so the outer handler's frame is untouched. */
        arm_vtimer_disarm();
        g_tick_count++;
        uart_puts("[TICK ");
        print_u64_dec(g_tick_count);
        uart_puts("]\\r\\n");
        gic_eoir(intid);
        return;
    }
    /* Physical timer path (INTID 30, priority 0x80). */
    arm_timer_arm();
    g_tick_count++;
    uart_puts("[TICK ");
    print_u64_dec(g_tick_count);
    uart_puts("]\\r\\n");
    if (g_tick_count == 5) {
        /* M5.3 nesting window (§10.197): on the FIFTH physical tick —
         * the M5.3 phase runs after the M5.2 gate (TICK 1..4), so the
         * M5.2 assertions stay byte-identical. Arm the virtual timer
         * (10 ms) and unmask IRQs; the virtual fire preempts THIS
         * handler (a DIFFERENT INTID at a higher priority — the
         * same-PPI case is impossible, GICv2 running-priority: an
         * interrupt cannot preempt its own active instance, and the
         * spike showed qemu's lax re-entry corrupts the GIC). The
         * window is a short, bounded spin (10 ms) well under the
         * 100 ms physical period, so no physical fire can nest too. */
        arm_vtimer_arm();
        uart_puts("[M5.3] nesting window: virtual timer armed (10 ms), "
                  "IRQs unmasked...\\r\\n");
        asm volatile("msr daifclr, #2" ::: "memory");   /* IRQ unmask (I) */
        uint64_t now, end;
        asm volatile("mrs %0, cntpct_el0" : "=r"(end));
        end += arm_timer_cntfrq() / 100;   /* 10 ms window */
        do {
            asm volatile("mrs %0, cntpct_el0" : "=r"(now));
        } while (now < end);
        asm volatile("msr daifset, #2" ::: "memory");   /* re-mask */
        uart_puts("[M5.3] nested: virtual timer preempted the physical "
                  "handler\\r\\n");
    }
    gic_eoir(intid);
}

static uint32_t fnv1a32(const uint8_t *p, uint32_t n)
{
    uint32_t h = 2166136261u;
    uint32_t i;
    for (i = 0; i < n; i++) {
        h ^= p[i];
        h *= 16777619u;
    }
    return h;
}

static int arm64_streq(const char *a, const char *b)
{
    while (*a && *b && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

static void print_u64(uint64_t v)
{
    /* Minimal hex print for the [SIMI]/[M5] lines (no libc). */
    char buf[17];
    int i;
    for (i = 15; i >= 0; i--) {
        unsigned nib = (unsigned)(v & 0xF);
        buf[i] = (char)(nib < 10 ? '0' + nib : 'a' + nib - 10);
        v >>= 4;
    }
    buf[16] = '\0';
    uart_puts("0x");
    uart_puts(buf);
}

static void print_u64_dec(uint64_t v)
{
    /* Decimal print for the [TICK N] lines (no libc). */
    char buf[21];
    int i = 20;
    buf[i] = '\0';
    if (v == 0) {
        uart_puts("0");
        return;
    }
    while (v) {
        buf[--i] = (char)('0' + (v % 10));
        v /= 10;
    }
    uart_puts(buf + i);
}

/* M4c: the translated code is DATA until it runs; the I-cache must be
 * invalidated (and the D-cache line cleaned to the point of
 * unification) before the first execute, or real silicon can run stale
 * bytes. qemu TCG is coherent, but this is the honest hardware hygiene
 * the JIT harness's __builtin___clear_cache provides (M3). dc cvau + ic
 * ivau per 64-byte line, then the barriers. */
static void arm64_flush_icache(uintptr_t addr, size_t len)
{
    uintptr_t start = addr & ~(uintptr_t)63;
    uintptr_t end = (addr + len + 63) & ~(uintptr_t)63;
    uintptr_t p;
    for (p = start; p < end; p += 64)
        asm volatile("dc cvau, %0" ::"r"(p) : "memory");
    asm volatile("dsb ish" ::: "memory");
    for (p = start; p < end; p += 64)
        asm volatile("ic ivau, %0" ::"r"(p) : "memory");
    asm volatile("dsb ish\n isb" ::: "memory");
}

/* M5 selfcheck, split in two: the kernel tree first (runs before the
 * user map exists), the user-tree proofs inside arm64_el0_activate. */
static void mmu_selfcheck_kernel(void)
{
    uint64_t d_img = mmu_walk_kernel(0xFFFF000040080000ULL);
    uint64_t d_uart = mmu_walk_kernel(0xFFFF000009000000ULL);

    uart_puts("[M5] TTBR1 kernel walk(0xFFFF000040080000) block attr=");
    uart_putc('0' + (char)((d_img >> 2) & 7));
    uart_puts("\\r\\n");
    uart_puts("[M5] TTBR1 kernel walk(0xFFFF000009000000) L3 attr=");
    uart_putc('0' + (char)((d_uart >> 2) & 7));
    uart_puts((d_uart & (1UL << 53)) ? " pxn=1 (device)\\r\\n" : " pxn=0\\r\\n");
}

static void mmu_selfcheck_user(void)
{
    /* The "unmapped-to-kernel" proof: the SAME VA that the user tree
     * maps for EL0 is a translation fault in the kernel's own tables. */
    uint64_t root = mmu_user_root_va();
    uart_puts("[DBG] user root=");
    print_u64(root);
    uart_puts(" L0[0]=");
    print_u64(((const uint64_t *)(uintptr_t)root)[0]);
    uart_puts("\r\n");
    uint64_t d_kern = mmu_walk_kernel(USER_CODE_VA);
    uint64_t d_user = mmu_walk(root, USER_CODE_VA);

    uart_puts("[M5] kernel walk(0x10000000) = ");
    uart_puts(d_kern ? "MAPPED (bug!)" : "0 (unmapped-to-kernel)");
    uart_puts("\\r\\n");
    uart_puts("[M5] user walk(0x10000000) L3=");
    print_u64(d_user);
    uart_puts(" attr=");
    uart_putc('0' + (char)((d_user >> 2) & 7));
    uart_puts(" ap=");
    uart_putc('0' + (char)((d_user >> 6) & 3));   /* AP is bits [7:6], not [9:8] */
    uart_puts((d_user & (1UL << 54)) ? " uxn=1\\r\\n" : " uxn=0 (el0-exec)\\r\\n");
}

/* M5.1: the activation cache core (§10.192). MISS: translate the .tmo
 * into g_smoke_code_buf, flush the I-cache, record the slot, increment
 * g_translate_count. HIT (name+hash+size all match): skip the
 * translator entirely, reuse entry_off/len, no re-flush (bytes
 * unchanged). Returns TX_AR_OK or the translator's error (printed). */
static int arm64_activate(const char *name, const uint8_t *tmo, uint32_t tmo_len)
{
    struct Arm64Activation *act = NULL;
    int i;
    for (i = 0; i < ARM64_ACT_SLOTS; i++)
        if (g_act[i].valid && arm64_streq(g_act[i].name, name)) {
            act = &g_act[i];
            break;
        }

    uint32_t hash = fnv1a32(tmo, tmo_len);
    if (act && act->content_hash == hash && act->tmo_len == tmo_len) {
        /* HIT — no translation, no flush, no counter bump. */
        g_code_len = act->code_len;
        g_entry_off = act->entry_off;
        uart_puts("[SIMI] activation cache HIT (reusing entry_off=");
        print_u64(g_entry_off);
        uart_puts(")\\r\\n");
        return TX_AR_OK;
    }

    /* MISS — translate, flush, record. */
    uart_puts("[SIMI] translating ");
    uart_puts(name);
    uart_puts(".tmo with kernel/simi_arm.c...\\r\\n");
    uint32_t len = 0, entry_off = 0;
    int rc = simi_arm_translate(tmo, tmo_len, g_smoke_code_buf,
                                ARM64_SMOKE_CODE_BUF_SIZE,
                                "main", 0, 0, 0, 0, &len, &entry_off);
    if (rc != TX_AR_OK) {
        uart_puts("[SIMI] translate FAILED, rc=");
        print_u64((uint64_t)(unsigned)rc);
        uart_puts(" (");
        uart_puts(simi_arm_strerror(rc));
        uart_puts(")\\r\\n");
        return rc;
    }
    g_code_len = len;
    g_entry_off = entry_off;
    arm64_flush_icache((uintptr_t)g_smoke_code_buf, (size_t)len);
    g_translate_count++;
    uart_puts("[SIMI] activation cache MISS (translated ");
    print_u64(len);
    uart_puts(" bytes)\\r\\n");

    if (!act) {
        for (i = 0; i < ARM64_ACT_SLOTS; i++)
            if (!g_act[i].valid) {
                act = &g_act[i];
                break;
            }
    }
    if (act) {
        for (i = 0; i < ARM64_ACT_NAME_LEN - 1 && name[i]; i++)
            act->name[i] = name[i];
        act->name[i] = '\0';
        act->content_hash = hash;
        act->tmo_len = tmo_len;
        act->code_len = len;
        act->entry_off = entry_off;
        act->valid = 1;
    }
    return TX_AR_OK;
}

/* M4b path, now cache-aware (M5.1): activate the embedded smoke (MISS
 * on the first call, HIT on the second) and call the entry as a real
 * function — the M4b gate (the kernel leg's LINK + call). The
 * translated program's result rides in t0 (x9), NOT x0: X_T0 is
 * simi_arm.c's primary working register and carries the RET result; the
 * trampoline is a normal callable A64 subroutine, so a plain `blr`
 * works and returns with x9 holding the result — exactly what
 * simi_arm_jit.c's `blr x0; mov x0, x9; ret` stub reads (M3, §10.178).
 * The call happens in inline asm with x9 declared live so the compiler
 * cannot reuse it; the value is copied out of x9 immediately. */
static void arm64_el1_entry(void)
{
    int rc = arm64_activate("arm64_boot_smoke", g_arm64_boot_smoke_tmo,
                            g_arm64_boot_smoke_tmo_len);
    if (rc != TX_AR_OK)
        return;
    uart_puts("[SIMI] calling entry directly...\\r\\n");

    typedef int64_t (*SimiEntryFn)(void);
    SimiEntryFn fn = (SimiEntryFn)(uintptr_t)(g_smoke_code_buf + g_entry_off);

    register uint64_t result __asm__("x9");
    __asm__ volatile(
        "blr %[addr]\n"
        : "+r"(result)
        : [addr] "r"((unsigned long)(uintptr_t)fn)
        : "x0", "x1", "x2", "x3", "x4", "x5", "x6", "x7", "x8",
          "x10", "x11", "x12", "x29", "x30", "memory");
    uint64_t code = result;   /* captured from x9 before anything can clobber it */

    uart_puts("[SIMI] entry returned (real machine code executed) -- result=");
    print_u64(code);
    uart_puts(" (expected 0x2a = 42)\\r\\n");
}

/* M5 continuation: the svc handler erets here (boot_arm64.S reads
 * g_user_ret_addr). A real function entry — deliberately NOT a
 * computed-goto label: the first M5 attempt used `&&after_eret` and GCC
 * placed that label at the TOP of the inlined function body, so the
 * eret re-ran the whole EL0 excursion and the clobbered register file
 * faulted on a bare physical address (§10.191). A function entry needs
 * no pre-eret register state and no valid LR (noreturn: PSCI powers the
 * machine off before any epilogue could run). */
static void psci_system_off(void);
static void arm64_el0_done(void) __attribute__((noreturn));
static void arm64_el0_activate(void) __attribute__((noreturn));

/* M5/M5.1: run the translated program in EL0 from USER_CODE_VA — a VA
 * the kernel's own tables do not map. Activates through the cache (a
 * HIT after the EL1 entries), builds the user TTBR0 tree, writes the
 * svc stub, erets into the blob with x30 = stub. The blob's trampoline
 * `br x30`s into the stub, `svc #0` traps to arm64_svc_from_el0, which
 * stores x9 (the result) and erets into arm64_el0_done — this function
 * NEVER returns. g_el0_excursions counts the erets into EL0 (the M5.1
 * gate wants exactly two). */
static void arm64_el0_activate(void)
{
    g_el0_excursions++;
    int rc = arm64_activate("arm64_boot_smoke", g_arm64_boot_smoke_tmo,
                            g_arm64_boot_smoke_tmo_len);
    if (rc != TX_AR_OK) {
        uart_puts("[SIMI] EL0 entry aborted (activate failed)\\r\\n");
        for (;;)
            ;
    }

    /* The stub page (mmu.c BSS): one `svc #0` word, then flush — it is
     * written by the kernel and executed by EL0. */
    *(volatile uint32_t *)mmu_user_stub_addr() = 0xD4000001UL; /* svc #0 */
    arm64_flush_icache((uintptr_t)mmu_user_stub_addr(), 4);

    /* Map the code buffer's physical page (the cached one, already
     * flushed at kernel VA) plus the stack/stub/scratch backing pages
     * into a FRESH user tree — only the code page is shared. */
    uint64_t code_pa = (uint64_t)(uintptr_t)g_smoke_code_buf - KERNEL_VIRT_OFF;
    mmu_build_user(code_pa);
    /* The EL0 alias is a distinct VA from the kernel VA; flush it too
     * (dc cvau / ic ivau per line) so real silicon sees the cached bytes
     * through this alias (no-op on qemu TCG, PIPT on real ARMv8). */
    arm64_flush_icache(USER_CODE_VA, (size_t)g_code_len);
    uart_puts("[M5] user TTBR0 tree built: code@0x10000000 stack@0x10001000 "
              "stub@0x10004000 scratch@0x10005000\\r\\n");
    mmu_selfcheck_user();

    /* The handshake: the svc handler erets to this address (boot_arm64.S
     * reads g_user_ret_addr, restores EL1h, erets). A real function
     * entry, not a computed-goto label — see arm64_el0_done above. */
    g_user_result = 0;
    g_user_ret_addr = (uint64_t)(uintptr_t)arm64_el0_done;

    uint64_t root = mmu_user_root_phys();
    uint64_t spsr = 0x3c0;   /* EL0t, DAIF masked */
    uint64_t code_va = USER_CODE_VA + (uint64_t)g_entry_off;

    uart_puts("[M5] eret into EL0...\\r\\n");
    asm volatile(
        "msr ttbr0_el1, %[root]\n\t"
        "isb\n\t"
        "tlbi vmalle1\n\t"
        "dsb ish\n\t"
        "isb\n\t"
        "msr sp_el0, %[usp]\n\t"
        "mov x30, %[stub]\n\t"
        "msr elr_el1, %[code]\n\t"
        "msr spsr_el1, %[spsr]\n\t"
        "eret\n\t"
        :
        : [root] "r"(root), [usp] "r"(USER_STACK_TOP_VA),
          [stub] "r"(USER_STUB_VA), [code] "r"(code_va), [spsr] "r"(spsr)
        : "x30", "memory");
    __builtin_unreachable();
}

/* M5.2: arm the timer, unmask IRQs (msr daifclr #4 clears the I bit),
 * idle until the target tick count fires — each tick prints [TICK N]
 * and re-arms (the [TICK 2] re-arm proof) — then re-mask. The ticks
 * fire strictly AFTER the last EL0 result line, so the M5.1 counts are
 * untouched by construction. The busy-wait reads the volatile counter
 * each iteration (the IRQ handler bumps it). */
static void arm64_wait_ticks(uint32_t target)
{
    /* The timer was armed in main before the EL0 excursions (§10.196)
     * and the handler re-arms on every tick, so there is nothing to arm
     * here. Self-contained masking: unmask IRQs for the idle wait (the
     * M5.3 phase calls this a SECOND time after the M5.2 gate re-masked,
     * so the mask state must not be assumed from the caller) and
     * re-mask before returning. Each tick pended through an EL0 window
     * was taken exactly once on the continuation's return. */
    asm volatile("msr daifclr, #2" ::: "memory");   /* IRQ unmask (I) */
    uart_puts("[M5.2] waiting for ");
    print_u64_dec(target);
    uart_puts(" ticks total (GICv2, CNTP PPI 14, 100 ms period)...\\r\\n");
    while (g_tick_count < target)
        ;
    asm volatile("msr daifset, #2" ::: "memory");   /* re-mask */
    uart_puts("[M5.2] tick gate reached: ");
    print_u64_dec(g_tick_count);
    uart_puts(" ticks, unbroken run\\r\\n");
}

/* The EL0 excursion's continuation (defined after psci_system_off):
 * entered via the svc handler's eret with TTBR0 still the user tables
 * and the kernel SP (SP_EL1 never changed across the excursion — the
 * eret into EL0 switched to SP_EL0). Prints the result; if this is the
 * first excursion, launches the SECOND EL0 excursion (the M5.1 gate:
 * two EL0 entries, both cache HITs, one translation total); after the
 * second, runs the M5.2 tick gate, then powers off. */
static void arm64_el0_done(void)
{
    /* M5.2 contention probe (§10.196): the svc handler returns with DAIF
     * masked (SPSR 0x3c5), but a tick that fired during the EL0 window
     * is pending RIGHT NOW. Unmask so it is taken exactly once on this
     * return — a level-sensitive GIC line that deasserts (the handler's
     * re-arm) before being taken is silently dropped, the exact
     * lost-tick failure the probe exists to prove absent. Then RE-MASK
     * immediately (the M5.3 determinism fix, §10.197): the continuation
     * prints and the next excursion's setup must run masked, so the
     * next physical fire pends deterministically and is taken at the
     * next daifclr boundary (the next el0_done or the wait phase) —
     * every tick's position is pinned, independent of UART timing. */
    asm volatile("msr daifclr, #2" ::: "memory");   /* take the pended tick */
    asm volatile("msr daifset, #2" ::: "memory");   /* re-mask for the prints */
    uart_puts("[M5] returned from EL0 -- user result=");
    print_u64(g_user_result);
    uart_puts(" (expected 0x2a = 42)\\r\\n");
    if (g_el0_excursions < 2)
        arm64_el0_activate();   /* second EL0 excursion (HIT) */
    arm64_wait_ticks(M52_TICK_TARGET);
    /* M5.3 (§10.197): the nesting probe — a SEPARATE phase after the
     * M5.2 gate, so the M5.2 assertions stay byte-identical. Wait for
     * the next physical tick (TICK 5); its handler opens the nesting
     * window and the virtual timer preempts it (TICK 6), then the gate
     * closes at 6. The window's handler ends well inside the 100 ms
     * physical period, so no seventh tick can land before the PSCI. */
    uart_puts("[M5.3] nesting probe: arming the virtual timer for the "
              "next physical tick (TICK 5)...\\r\\n");
    arm64_wait_ticks(M53_TICK_TARGET);
    uart_puts("[M4] issuing PSCI SYSTEM_OFF\\r\\n");
    psci_system_off();
    for (;;)
        ;
}

static void psci_system_off(void)
{
    register uint64_t x0 asm("x0") = PSCI_FN_SYSTEM_OFF;
    asm volatile("smc #0" : : "r"(x0) : "memory");
    /* Not reached when the monitor honors the call. */
    for (;;)
        ;
}

static void print_el(void)
{
    uint64_t el;
    asm volatile("mrs %0, CurrentEL" : "=r"(el));
    uart_puts("[M4] exception level: EL");
    uart_putc('0' + (char)((el >> 2) & 3));
    uart_puts("\\r\\n");
}

void kernel_arm64_main(void)
{
    /* The MMU is already on — boot_arm64.S enabled it: TTBR1 kernel
     * (high VAs), TTBR0 parked on an all-invalid root, VBAR_EL1 set.
     * The banner below is translated output from the first word. */
    uart_init();
    uart_puts("AeroSLS ARM64 Node Kernel Online!\\r\\n");
    uart_puts("[M5] TTBR1 kernel: VMA=0xFFFF000040080000 LMA=0x40080000, "
              "TTBR0 empty (kernel is TTBR1-pure)\\r\\n");
    print_el();
    mmu_selfcheck_kernel();
    /* M5.2: the GIC and the generic timer (CNTFRQ read) are init once,
     * before any entry; the timer is armed when the wait phase starts
     * (arm64_wait_ticks), so no tick can fire during the entries. */
    gic_init();
    arm_timer_init();
    /* M5.1 gate (four entries, one translation, all 42): the first EL1
     * entry is the cache MISS (translate + call); the second EL1 entry
     * and both EL0 excursions are HITs (reuse, no retranslation). The
     * EL0 path never returns — arm64_el0_done launches the second EL0
     * excursion, then runs the M5.2 tick gate, then PSCI SYSTEM_OFF. */
    arm64_el1_entry();
    arm64_el1_entry();
    /* M5.2 contention probe (§10.196): arm the timer BEFORE the EL0
     * excursions. The EL0 program is a 1e8-iteration loop, so a 100 ms
     * tick fires DURING each EL0 window, pends against the EL0 SPSR's I
     * mask (0x3c0), and is taken exactly once when the continuation
     * clears I on return — the [TICK N] lines interleave with the
     * excursion logs, and the unbroken 1..4 stream is the zero-lost-tick
     * proof. IRQs stay masked here; the unmask happens at the
     * continuation entry (arm64_el0_done). */
    arm_timer_arm();
    uart_puts("[M5.2] contention probe: 100 ms ticks armed before the EL0 "
              "excursions (they pend through each EL0 window)\\r\\n");
    arm64_el0_activate();
    /* Unreachable: arm64_el0_activate is noreturn. */
    for (;;)
        ;
}
