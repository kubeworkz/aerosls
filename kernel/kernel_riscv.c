/*
 * kernel_riscv.c — AeroSLS RISC-V supervisor entry point.
 *
 * Gap Remediation SIMI Phase 9 (sub-phase 9d): extended from a 10-line
 * boot banner + wfi loop into a real, if deliberately minimal, boot-time
 * smoke test proving simi_riscv_translate()'s output is directly
 * callable as real machine code inside THIS freestanding kernel binary,
 * with a genuine ecall-based syscall round trip through the new
 * arch/riscv/trap_riscv.{S,c} trap-entry mechanism. See
 * AeroSLS-SIMI-ISA-v0.1.md §16 Phase 9 for the full scope statement and
 * an honest accounting of what this is and is not (no process table, no
 * user/kernel privilege separation, no Sv39 paging enabled -- this all
 * still runs at the kernel's own S-mode privilege level, on the boot
 * stack's own physical memory, exactly like the banner-and-halt code it
 * replaces did).
 */
#include "../include/sls_mmu.h"
#include "simi_riscv.h"
#include "../arch/riscv/trap_riscv.h"
#include "../arch/riscv/plic.h"
#include "../arch/riscv/sbi.h"   /* sbi_arm_timer — Phase 9k periodic tick */
#include "../arch/riscv/context_riscv.h"   /* RvTask + perform_riscv_context_switch — Design B part 3 */
#include "rv64_boot_smoke_tmo.h"
#include "rv64_float_smoke_a_tmo.h"
#include "rv64_float_smoke_b_tmo.h"

extern void sbi_putchar(char c);

static void rv_boot_print(const char* s) {
    while (*s) sbi_putchar(*s++);
}
static void rv_boot_print_hex_rc(int rc) {
    /* rc is always small and non-negative (a TX_RV_ERR_* enum value) --
     * a single decimal digit is enough for every error code this
     * translator can return today. */
    sbi_putchar((char)('0' + (rc % 10)));
}
static void rv_boot_print_hex64(uint64_t v) {
    /* Full 16-digit lowercase hex, 0x-prefixed -- the smoke asserts
     * exact bit patterns (+inf, 20.0), so a readable full-width dump is
     * worth the 17 bytes of buffer. */
    static const char hexd[] = "0123456789abcdef";
    char buf[17];
    for (int i = 0; i < 16; i++) { buf[15 - i] = hexd[v & 0xf]; v >>= 4; }
    buf[16] = '\0';
    rv_boot_print("0x");
    rv_boot_print(buf);
}
static void rv_boot_print_udec64(uint64_t v) {
    char buf[20];
    int i = 0;
    if (v == 0) { sbi_putchar('0'); return; }
    while (v > 0 && i < 20) { buf[i++] = (char)('0' + (v % 10)); v /= 10; }
    while (i > 0) sbi_putchar(buf[--i]);
}

/* Dedicated trap-handling stack for hart 0 — deliberately separate from
 * bsp_stack_top (arch/riscv/boot_riscv.S), per trap_riscv.h's own
 * requirement that riscv_trap_init()'s kernel_stack_top not alias
 * whatever stack the caller is already running on. 8 KiB, matching the
 * x86 side's kernel_syscall_stack sizing (arch/x86/user_paging.c). */
static uint8_t g_hart0_trap_stack[8192] __attribute__((aligned(16)));
static struct RvPerHartData g_hart0_data;

/* Output buffer for the smoke-test translation. The embedded program is
 * tiny (80-byte .tmo, ~870 bytes of translated code per the host
 * toolchain's own simi-riscv-verify run — see rv64_boot_smoke_tmo.h) —
 * one page is generous headroom, matching this project's established
 * "static buffer, no general-purpose allocator" convention (kernel/
 * simi_translate.c's g_simi_code_buf, kernel/loader.c's ServiceBinary). */
#define RV64_SMOKE_CODE_BUF_SIZE 4096
static uint8_t g_smoke_code_buf[RV64_SMOKE_CODE_BUF_SIZE] __attribute__((aligned(16)));

/* Gap Remediation SIMI Phase 9 (sub-phases 9d/9f): translate the embedded
 * rv64_boot_smoke.simi program and call it directly as a real function —
 * the exact same verification idea tools/simi/simi_riscv_verify.c already
 * uses on the host side (a call into freshly emitted machine code, with
 * the result read from t0), just now running inside the real freestanding
 * kernel binary instead of a host test harness. scratch_ptr/rt_resolve_fn/
 * rt_objsize_fn/rt_objtype_fn are all 0 -- this program never touches r7,
 * r6, RESOLVE, OBJSIZE, or OBJTYPE, so nothing dereferences them. */
/* Design B part 2 (ISA doc §16 Phase 16 audit addendum): translate and
 * call an embedded .tmo, returning the value the RV translator leaves in
 * t0 (its documented return register, simi_riscv.c's OP_RET). Shared by
 * the float smoke below (three runs); the boot smoke keeps its own
 * inline copy (its inline asm is heavily commented for the ebreak
 * syscall that follows). */
static uint64_t rv64_translate_and_call(const uint8_t* tmo, uint32_t tmo_len) {
    uint32_t len = 0, entry_off = 0;
    int rc = simi_riscv_translate(tmo, tmo_len, g_smoke_code_buf,
                                  RV64_SMOKE_CODE_BUF_SIZE,
                                  "main", 0, 0, 0, 0, &len, &entry_off);
    if (rc != TX_RV_OK) {
        rv_boot_print("[SIMI] translate FAILED, rc=");
        rv_boot_print_hex_rc(rc);
        rv_boot_print("\n");
        return 0xDEADBEEFDEADBEEFULL;
    }
    typedef int64_t (*SimiEntryFn)(void);
    SimiEntryFn fn = (SimiEntryFn)(uintptr_t)(g_smoke_code_buf + entry_off);
    register uint64_t result __asm__("t0");
    __asm__ volatile(
        "jalr ra, 0(%[addr])\n"
        : "+r"(result)
        : [addr] "r"((unsigned long)(uintptr_t)fn)
        : "ra", "memory");
    return result;
}

/* Design B part 2 (ISA doc §16 Phase 16 audit addendum): the FS lazy-
 * save on REAL hardware. The kernel deliberately disables FP (FS=Off)
 * before each owner's run; the first FP instruction of the translated
 * program traps scause=2, riscv_trap_dispatch_common's code==2 branch
 * saves the previous owner's live state into its fp_save row, loads the
 * new owner's, and re-executes. The assertions (all printed, CI-
 * greppable):
 *   A = 1.5/0.0 -> +inf (0x7ff0000000000000) with fcsr DZ set  -- and
 *       that exact state round-trips: after B evicts A, fp_save[0] must
 *       hold f10=+inf AND DZ (the 33rd slot) -- the lazy save captures
 *       more than the f-registers.
 *   B = 10.0+10.0 -> 20.0 (0x4034000000000000), flag-free fcsr -- the
 *       CONTRAST owner: B's saved state (evicted explicitly) must come
 *       back clean (fcsr=0), proving each owner's row holds ITS state,
 *       not A's.
 *   A again (the reload path): A's state was saved at B's eviction,
 *       so A's second run must trap AGAIN and get +inf BACK from
 *       fp_save[0] -- the full two-owner round trip.
 * Three lazy-saves total (A#1's cold-start, B, A#2), each logged as
 * "[FP] lazy-save: owner X -> Y". */
static void rv64_float_smoke_test(void) __attribute__((unused));
static void rv64_float_smoke_test(void) {
    struct RvPerHartData* phd = &g_hart0_data;
    uint64_t fs_off_mask = 3ULL << 13;   /* sstatus.FS bits 14:13 = Off */
    rv_boot_print("[FP-SMOKE] two-owner FP lazy-save round-trip (Design B part 2)...\n");

    /* Reset the owner registry: both rows zeroed, owner A (0) current. */
    for (int i = 0; i < 33; i++) { phd->fp_save[0][i] = 0; phd->fp_save[1][i] = 0; }
    phd->fp_owner = 0;
    phd->fp_current = 0;

    /* Owner A: 1.5/0.0 = +inf, DZ. FS=Off so A's first FP instruction
     * traps (cold start: the handler saves/loads row 0, both zeros). */
    __asm__ volatile("csrc sstatus, %0" : : "r"(fs_off_mask) : "memory");
    uint64_t rA = rv64_translate_and_call(g_rv64_float_smoke_a_tmo,
                                          g_rv64_float_smoke_a_tmo_len);
    rv_boot_print("[FP-SMOKE] owner A run: ");
    rv_boot_print_hex64(rA);
    rv_boot_print(rA == 0x7ff0000000000000ULL ? " = +inf PASS\n"
                                             : " != +inf FAIL\n");

    /* Switch to owner B: mark the owner, disarm FP. A's LIVE state stays
     * in the f-registers (no eager save) -- B's first FP instruction
     * traps and the handler writes A's state into fp_save[0]. */
    phd->fp_owner = 1;
    __asm__ volatile("csrc sstatus, %0" : : "r"(fs_off_mask) : "memory");
    uint64_t rB = rv64_translate_and_call(g_rv64_float_smoke_b_tmo,
                                          g_rv64_float_smoke_b_tmo_len);
    rv_boot_print("[FP-SMOKE] owner B run: ");
    rv_boot_print_hex64(rB);
    rv_boot_print(rB == 0x4034000000000000ULL ? " = 20.0 PASS\n"
                                              : " != 20.0 FAIL\n");

    /* A was evicted into fp_save[0] by B's lazy-save trap: assert the
     * saved row holds A's DISTINCT state (f10=+inf, fcsr DZ set). */
    int a_saved = (phd->fp_save[0][10] == 0x7ff0000000000000ULL) &&
                  ((phd->fp_save[0][32] & 8) != 0);
    rv_boot_print("[FP-SMOKE] A evicted into fp_save[0]: f10=");
    rv_boot_print_hex64(phd->fp_save[0][10]);
    rv_boot_print(", fcsr DZ=");
    rv_boot_print((phd->fp_save[0][32] & 8) ? "1" : "0");
    rv_boot_print(a_saved ? " PASS\n" : " FAIL\n");

    /* Evict B explicitly (FP is still Dirty from B's run) and assert its
     * row is the CONTRAST: 20.0 with a CLEAN fcsr -- not A's state. */
    fp_save_all(&phd->fp_save[1][0]);
    int b_saved = (phd->fp_save[1][10] == 0x4034000000000000ULL) &&
                  (phd->fp_save[1][32] == 0);
    rv_boot_print("[FP-SMOKE] B evicted: fp_save[1] f10=");
    rv_boot_print_hex64(phd->fp_save[1][10]);
    rv_boot_print(", fcsr=0 ");
    rv_boot_print(b_saved ? "PASS\n" : "FAIL\n");

    /* Back to owner A (the reload path): A's state was saved at B's
     * eviction, so A's second run must trap again and get +inf BACK
     * from fp_save[0] -- the round trip closes. */
    phd->fp_owner = 0;
    __asm__ volatile("csrc sstatus, %0" : : "r"(fs_off_mask) : "memory");
    uint64_t rA2 = rv64_translate_and_call(g_rv64_float_smoke_a_tmo,
                                           g_rv64_float_smoke_a_tmo_len);
    rv_boot_print("[FP-SMOKE] owner A reload run: ");
    rv_boot_print_hex64(rA2);
    rv_boot_print(rA2 == 0x7ff0000000000000ULL ? " = +inf PASS\n"
                                               : " != +inf FAIL\n");

    int all_ok = (rA == 0x7ff0000000000000ULL) && a_saved &&
                 (rB == 0x4034000000000000ULL) && b_saved &&
                 (rA2 == 0x7ff0000000000000ULL);
    rv_boot_print(all_ok ? "[FP-SMOKE] round-trip: ALL PASS\n"
                         : "[FP-SMOKE] round-trip: FAIL\n");
}

/* ─── Design B part 3 (ISA doc §16 Phase 16 audit addendum): the
 * two-task FP round-robin — the context switch finally gets real
 * callers. perform_riscv_context_switch (arch/riscv/context_riscv.S)
 * has been dead code since Phase 9c and GPR-only (the audit, Finding 2:
 * "the context switch is GPR-only AND dead code"). This section gives
 * it its first callers and teaches the switch to save and restore the
 * fp_save owner rows: rv_task_switch_fp saves the LIVE owner's state,
 * loads the incoming task's row, folds the registry, disarms FP, then
 * performs the coroutine switch. The tick marks each slice boundary
 * (g_rv_slice_expired, set in sbi.c), so the round-robin is tick-
 * cadenced like Phase 9l's modeled table — but with REAL register
 * contexts and REAL per-task FP state. */
static struct RvTask g_rv_main_ctx;    /* the boot context (suspended while tasks run) */
static struct RvTask g_rv_task_a;
static struct RvTask g_rv_task_b;

/* The time-slice boundary marker (context_riscv.h): written by the tick
 * handler (sbi.c), polled+cleared by the running task at its slice
 * boundary. Defined here — the tasks are this file's feature. */
volatile uint64_t g_rv_slice_expired;

/* Lay a task's initial stack frame: the switch's restore pops 14
 * registers (ra, s0-s11, tp) off the incoming sp and `ret`s — so the
 * fabricated frame has ra = the task entry and zeros elsewhere. */
static void rv_task_init(struct RvTask* t, void (*fn)(void), uint64_t owner,
                         const char* name) {
    uint64_t* sp = (uint64_t*)(t->stack + sizeof(t->stack));
    sp -= 14;
    sp[0] = (uint64_t)(uintptr_t)fn;   /* ra: the task entry */
    for (int i = 1; i < 14; i++) sp[i] = 0;
    t->rsp = (uint64_t)(uintptr_t)sp;
    t->fp_owner = owner & 1;
    t->name = name;
    t->slices = 0;
}

/* The FP-aware task switch — the switch's only intended caller. Saves
 * the live owner's FP state (whoever's is in the registers) into its
 * fp_save row, loads the incoming task's row, folds the owner registry,
 * disarms FP (so the incoming task's FIRST FP instruction lazy-saves —
 * proving the eager load and the trap path agree), then performs the
 * GPR coroutine switch. When the switch later returns into this task,
 * the switch that left it here already loaded THIS task's state — no FP
 * work happens on resume. All FP instructions here are the sanctioned
 * fp_save_all/fp_load_all plumbing (rv64_fp_census gate). */
static void rv_task_switch_fp(struct RvTask* cur, struct RvTask* next) {
    struct RvPerHartData* phd = &g_hart0_data;
    uint64_t sst;
    __asm__ volatile("csrr %0, sstatus" : "=r"(sst));
    /* FS=Dirty first: an fsd with FS=Off would itself trap. */
    __asm__ volatile("csrw sstatus, %0" : : "r"(sst | (3ULL << 13)) : "memory");
    uint64_t live = phd->fp_current & 1;
    fp_save_all(&phd->fp_save[live][0]);
    uint64_t no = next->fp_owner & 1;
    fp_load_all(&phd->fp_save[no][0]);
    phd->fp_owner = no;
    phd->fp_current = no;
    /* FS=Off: the incoming task's first FP instruction lazy-saves. */
    __asm__ volatile("csrc sstatus, %0" : : "r"(3ULL << 13) : "memory");
    perform_riscv_context_switch(cur, next);
    /* Resumed later: this task's state was loaded by the switch that
     * left it here; FS is Off again (that switch disarmed it). */
}

/* The FP work of the two tasks — the ONLY FP instructions outside the
 * fp_save_all/fp_load_all plumbing, and the rv64_fp_census allow-list
 * sanctions exactly these two functions (an injected fadd.d in any
 * other kernel function still fails the gate). Each slice does one
 * fadd.d accumulating the task's step into f10, reads it back with
 * fmv.x.d, asserts the exact bit pattern (the per-slice expected
 * constants are precomputed double(s) bit patterns — no FP conversion
 * needed in C), then waits for the tick's slice-boundary flag and
 * yields to the partner task. Because every switch disarms FP, each
 * slice's first fadd.d traps and lazy-saves — one trap per slice. */
#define RV_TASK_SLICES 5

static void rv_fp_task_a(void);
static void rv_fp_task_b(void);

static void rv_fp_task_a(void) {
    /* Task A's accumulator: f10 += 1.0 per slice -> 1.0..5.0. The step
     * constant rides in a GPR and is materialized INSIDE the asm via
     * fmv.d.x into scratch ft0: the compiler therefore sees no FP value
     * live in this function, so it cannot hoist a loop-invariant double
     * into callee-saved fs0 (which would put a compiler-generated fld in
     * the epilogue AFTER the handback switch disarms FS -- the spurious
     * 11th lazy-save that broke the per-slice trap count). The only FP
     * instructions in this function are the asm blocks themselves. */
    static const uint64_t exp[RV_TASK_SLICES] = {
        0x3FF0000000000000ULL, /* 1.0 */
        0x4000000000000000ULL, /* 2.0 */
        0x4008000000000000ULL, /* 3.0 */
        0x4010000000000000ULL, /* 4.0 */
        0x4014000000000000ULL, /* 5.0 */
    };
    uint64_t step = 0x3FF0000000000000ULL;   /* 1.0, as bits */
    for (int s = 0; s < RV_TASK_SLICES; s++) {
        uint64_t got;
        __asm__ volatile(
            "fmv.d.x ft0, %1\n\t"
            "fadd.d f10, f10, ft0\n\t"
            "fmv.x.d %0, f10\n\t"
            : "=r"(got)
            : "r"(step)
            : "ft0", "f10", "memory");
        g_rv_task_a.slices++;
        rv_boot_print("[TASK] fp-a slice ");
        rv_boot_print_udec64((uint64_t)(s + 1));
        rv_boot_print(": ");
        rv_boot_print_hex64(got);
        rv_boot_print(got == exp[s] ? " PASS\n" : " FAIL\n");
        while (!g_rv_slice_expired) { __asm__ volatile("wfi"); }
        g_rv_slice_expired = 0;
        rv_task_switch_fp(&g_rv_task_a, &g_rv_task_b);
    }
    /* All slices done: hand back to the boot context. */
    rv_task_switch_fp(&g_rv_task_a, &g_rv_main_ctx);
}

static void rv_fp_task_b(void) {
    /* Task B's accumulator: f10 += 0.5 per slice -> 0.5..2.5 — the
     * CONTRAST owner: after every switch back, B's f10 must be ITS own
     * value, never A's 1.0/2.0/... — that is the round-trip proof. Same
     * GPR-step discipline as task A (see above): no FP value lives in
     * this function outside the asm blocks, so no compiler-generated
     * FP spill runs after the disarm. */
    static const uint64_t exp[RV_TASK_SLICES] = {
        0x3FE0000000000000ULL, /* 0.5 */
        0x3FF0000000000000ULL, /* 1.0 */
        0x3FF8000000000000ULL, /* 1.5 */
        0x4000000000000000ULL, /* 2.0 */
        0x4004000000000000ULL, /* 2.5 */
    };
    uint64_t step = 0x3FE0000000000000ULL;   /* 0.5, as bits */
    for (int s = 0; s < RV_TASK_SLICES; s++) {
        uint64_t got;
        __asm__ volatile(
            "fmv.d.x ft0, %1\n\t"
            "fadd.d f10, f10, ft0\n\t"
            "fmv.x.d %0, f10\n\t"
            : "=r"(got)
            : "r"(step)
            : "ft0", "f10", "memory");
        g_rv_task_b.slices++;
        rv_boot_print("[TASK] fp-b slice ");
        rv_boot_print_udec64((uint64_t)(s + 1));
        rv_boot_print(": ");
        rv_boot_print_hex64(got);
        rv_boot_print(got == exp[s] ? " PASS\n" : " FAIL\n");
        while (!g_rv_slice_expired) { __asm__ volatile("wfi"); }
        g_rv_slice_expired = 0;
        rv_task_switch_fp(&g_rv_task_b, &g_rv_task_a);
    }
    rv_task_switch_fp(&g_rv_task_b, &g_rv_main_ctx);
}

/* The demo driver: reset the FP registry (the float smoke left row 0 =
 * +inf and row 1 = 20.0), initialize the two tasks, run the round-robin
 * at a fast 100ms tick cadence, then assert the FINAL fp_save rows and
 * the lazy-save delta (one per task-slice = 2*RV_TASK_SLICES = 10). */
static void rv_fp_round_robin_demo(void) __attribute__((unused)); /* the echo build never calls it (wfi loop) */
static void rv_fp_round_robin_demo(void) {
    struct RvPerHartData* phd = &g_hart0_data;
    rv_boot_print("[TASK] two-task FP round-robin (real register contexts, Design B part 3)...\n");
    for (int i = 0; i < 33; i++) { phd->fp_save[0][i] = 0; phd->fp_save[1][i] = 0; }
    phd->fp_owner = 0;
    phd->fp_current = 0;
    /* Make the registry honest: fp_current=0 claims "the physical FP
     * registers hold owner 0's state" — but the smoke test left owner 0's
     * +inf physically in the regs. Materialize the zeroed row into the
     * real registers (sanctioned plumbing) so task A's first fadd starts
     * from 0.0, not the smoke's stale +inf. */
    uint64_t sst;
    __asm__ volatile("csrr %0, sstatus" : "=r"(sst));
    __asm__ volatile("csrw sstatus, %0" : : "r"(sst | (3ULL << 13)) : "memory");
    fp_load_all(&phd->fp_save[0][0]);
    rv_task_init(&g_rv_task_a, rv_fp_task_a, 0, "fp-a");
    rv_task_init(&g_rv_task_b, rv_fp_task_b, 1, "fp-b");
    uint64_t lazy_before = g_fp_lazy_count;
    sbi_set_tick_period(1000000UL);   /* 100ms slices for the demo */
    __asm__ volatile("csrc sstatus, %0" : : "r"(3ULL << 13) : "memory");
    rv_task_switch_fp(&g_rv_main_ctx, &g_rv_task_a);
    /* Back here when task A finishes its slices and hands control back. */
    sbi_set_tick_period(0);
    uint64_t lazy_delta = g_fp_lazy_count - lazy_before;
    int rows_ok = (phd->fp_save[0][10] == 0x4014000000000000ULL) &&  /* A: 5.0 */
                  (phd->fp_save[1][10] == 0x4004000000000000ULL);    /* B: 2.5 */
    int lazy_ok = (lazy_delta == 2 * RV_TASK_SLICES);
    rv_boot_print("[TASK] fp_save rows: A=");
    rv_boot_print_hex64(phd->fp_save[0][10]);
    rv_boot_print(" B=");
    rv_boot_print_hex64(phd->fp_save[1][10]);
    rv_boot_print(rows_ok ? " PASS;" : " FAIL;");
    rv_boot_print(" lazy-saves during demo: ");
    rv_boot_print_udec64(lazy_delta);
    rv_boot_print(lazy_ok ? " PASS\n" : " FAIL\n");
    rv_boot_print(rows_ok && lazy_ok ? "[TASK] round-robin: ALL PASS\n"
                                     : "[TASK] round-robin: FAIL\n");
}

static void rv64_boot_smoke_test(void) __attribute__((unused));
static void rv64_boot_smoke_test(void) {
    rv_boot_print("[SIMI] RV64 boot smoke test: translating rv64_boot_smoke.tmo...\n");

    uint32_t len = 0, entry_off = 0;
    int rc = simi_riscv_translate(g_rv64_boot_smoke_tmo, g_rv64_boot_smoke_tmo_len,
                                   g_smoke_code_buf, RV64_SMOKE_CODE_BUF_SIZE,
                                   "main", 0, 0, 0, 0, &len, &entry_off);
    if (rc != TX_RV_OK) {
        rv_boot_print("[SIMI] translate FAILED, rc=");
        rv_boot_print_hex_rc(rc);
        rv_boot_print("\n");
        return;
    }

    rv_boot_print("[SIMI] translated OK, calling entry directly...\n");

    typedef int64_t (*SimiEntryFn)(void);
    SimiEntryFn fn = (SimiEntryFn)(uintptr_t)(g_smoke_code_buf + entry_off);

    /* The translated program's result rides in t0 (x5), NOT a0: that is
     * the RV translator's documented return convention (simi_riscv.c's
     * OP_RET loads r0 into t0 -- "r0 is the return-value register" --
     * and the trampoline's epilogue returns with t0 untouched; the host
     * verifier reads the same register, simi_riscv_verify.c's cpu.x[5]).
     * A plain C call would read a0, which the trampoline never sets, so
     * the call happens in inline asm with ra as the link register (the
     * trampoline saves/restores its incoming ra itself) and t0 declared
     * live so the compiler cannot reuse it; the value is copied out of
     * t0 immediately, before any intervening C call can clobber it. */
    register uint64_t result __asm__("t0");
    __asm__ volatile(
        "jalr ra, 0(%[addr])\n"
        : "+r"(result)
        : [addr] "r"((unsigned long)(uintptr_t)fn)
        : "ra", "memory");
    uint64_t code = result;   /* captured from t0 before anything can clobber it */

    rv_boot_print("[SIMI] entry returned (real machine code executed) -- issuing "
                  "RV_SYS_EXIT (a7=164, a0=code) via ebreak, the delegated "
                  "syscall trigger...\n");

    /* The SIMI syscall fixture on real hardware (Phase 9f): the
     * translated program's return value (42) becomes the RV_SYS_EXIT
     * syscall ABI -- a7 = 164 (matching kernel/process.h's SYS_SLS_EXIT),
     * a0 = exit code -- and the syscall is triggered through ebreak
     * (exception 3), the one exception OpenSBI's default MEDELEG
     * delegates to S-mode. (Exception 9, ecall from S-mode, is NOT
     * delegated -- an ecall carrying this ABI bounces as a failed SBI
     * call and never reaches our stvec handler.) riscv_trap_entry
     * (arch/riscv/trap_riscv.S) saves every GPR including a7/a0,
     * riscv_trap_dispatch() (arch/riscv/trap_riscv.c) routes scause=3
     * with a7==RV_SYS_EXIT to riscv_syscall_dispatch(), which reports
     * the exit code and powers the machine off via OpenSBI's SBI_SRST
     * extension -- control never returns, and QEMU exits rc=0. This is
     * the same trap path the pre-9f ebreak smoke exercised, now carrying
     * the real syscall instead of halting on an unhandled exception. */
    /* The ebreak must be the 4-byte (non-compressed) form: the handler
     * advances sepc by exactly 4 (trap_riscv.c's code==3 branch), and
     * with the C extension enabled the plain `ebreak` mnemonic assembles
     * to the 2-byte compressed 0x9002 -- mepc+4 would then land
     * mid-instruction. .option norvc pins the 32-bit 0x00100073, keeping
     * the +4 resume exact (the bare-metal trap-entry fixture in
     * tools/simi/tests/rv_trap_entry/ caught this same bug by actually
     * executing the restore path, see ISA doc §16 Phase 9h). */
    __asm__ volatile(
        "mv a0, %0\n"
        "li a7, %1\n"
        ".option norvc\n"
        "ebreak\n"
        ".option rvc\n"
        : : "r"(code), "i"(RV_SYS_EXIT) : "a0", "a7", "memory");

    /* Unreachable in practice (the syscall powers the machine off), but
     * stated explicitly rather than left as fallthrough into whatever
     * code happens to follow. */
    rv_boot_print("[SIMI] unexpected: RV_SYS_EXIT returned control.\n");
}

void kernel_riscv_main(unsigned long hart_id, unsigned long fdt) {
    (void)hart_id;
    (void)fdt;
    const char* msg = "AeroSLS RISC-V Supervisor Node Kernel Online!";
    for(int i = 0; msg[i] != '\0'; i++) sbi_putchar(msg[i]);
    sbi_putchar('\n');

    /* Design A (ISA doc §16 Phase 16 audit addendum): log sstatus.FS so
     * the FP-free discipline's ACTUAL backstop state is visible in the
     * serial stream; CI asserts the line. The kernel never writes FS.
     * The spike (spike-before-estimate) found the state is MODE-EXACT:
     * under the S-mode boot, OpenSBI leaves FS=3 (Dirty) — FP is
     * architecturally ENABLED, so an accidental F/D instruction would
     * EXECUTE silently and the load-bearing guard is the census, not a
     * trap; under the bare-metal M-mode boot FS stays at reset (Off)
     * and the dispatcher's scause=2 case is the loud backstop. The
     * message below reports the value and the honest state-dependent
     * interpretation. If a future change enables FP by design (Design
     * B), this line changes and the census + teeth gates must move. */
    uint64_t sstatus_val;
    __asm__ volatile("csrr %0, sstatus" : "=r"(sstatus_val));
    uint64_t fs = (sstatus_val >> 13) & 3;
    rv_boot_print("[M5-audit] sstatus.FS=");
    sbi_putchar((char)('0' + (int)fs));
    if (fs == 0)
        rv_boot_print(" (Off: FP/vector accesses trap scause=2)");
    else
        rv_boot_print(" (not Off: FP/vector would EXECUTE -- the FP-free discipline rests on the rv64_fp_census gate, not a trap)");
    rv_boot_print(" -- RV64 kernel FP use: the sanctioned fp_save_all/fp_load_all plumbing only (Design B part 2, rv64_fp_census gate)\n");

    /* OpenSBI (fw_dynamic) delivers the payload to exactly one hart -- the
     * boot hart -- whose id is NOT necessarily 0 (default: the last hart,
     * see boot_riscv.S). The former `hart_id == 0` gate never fired under
     * `-smp > 1`, so the trap path and the SIMI smoke never ran; the
     * delivered hart IS the boot hart, so run the boot sequence directly. */
    uint64_t trap_stack_top =
        (uint64_t)(uintptr_t)&g_hart0_trap_stack[sizeof(g_hart0_trap_stack) - 16];
    riscv_trap_init(&g_hart0_data, trap_stack_top);

    /* Phase 9i: the first device-driven kernel I/O. The trap path above
     * only routes interrupts that arrive; nothing made them arrive. This
     * programs the PLIC so the 16550 UART's RX line can reach the mode
     * this build runs in (source-10 priority, the hart-0 context enable +
     * threshold, the mode's external-interrupt bit, and the UART's own
     * IER bit 0 so a byte actually asserts the line), then raises the
     * GLOBAL interrupt enable. From here on, an incoming character
     * interrupts the hart, and handle_riscv_supervisor_interrupt
     * (arch/riscv/sbi.c) drains the 16550 directly and echoes it.
     * S-mode build: the PLIC S-mode context (2N+1) + sie.SEIE +
     * sstatus.SIE (bit 1). M-mode twin (bare metal, no firmware): the
     * M-mode context (2N+0) + mie.MEIE + mstatus.MIE (bit 3) -- the
     * interrupt is taken in M-mode at mtvec with mcause = bit63 + 11. */
#if defined(RISCV_MMODE)
    init_riscv_plic(0);

    /* Phase 9k M-mode twin: the same 1-second periodic timer over the
     * bare-metal path. sbi_arm_timer programs the CLINT's hart-0
     * mtimecmp MMIO directly (physical 0x02004000 — no firmware
     * exists, so there is no SBI call to make), and mie.MTIE (bit 7 =
     * 0x80 — which, like STIE, does NOT fit the CSRxI 5-bit immediate
     * field, so the register form is required) makes MTIP deliverable
     * to M-mode. Same order discipline as the S-mode arm below: arm
     * first (comparator in the future), THEN enable, so whatever
     * mtimecmp held at reset cannot produce an early tick. The machine
     * timer is taken at mtvec with mcause = bit63 + 7 and re-armed in
     * handle_riscv_supervisor_interrupt, exactly like the S-mode STIP
     * path. */
    sbi_arm_timer(SBI_TIMER_TICK_PERIOD);
    uint64_t mtie_bit = 0x80;
    __asm__ volatile("csrs mie, %0" : : "r"(mtie_bit) : "memory");
    __asm__ volatile("csrs mstatus, 8");
    rv_boot_print("[PLIC] UART RX interrupt wired (source 10 -> hart 0 M-mode, MEIE + MIE on).\n");
#if defined(KERNEL_UART_ECHO)
    rv_boot_print("[TIMER] CLINT mtimecmp armed (MTIP, 100ms period -- Phase 9n contention probe).\n");
#else
    rv_boot_print("[TIMER] CLINT mtimecmp armed (MTIP, 1s period).\n");
#endif
#else
    init_riscv_plic(0);
    __asm__ volatile("csrs sstatus, 2");
    rv_boot_print("[PLIC] UART RX interrupt wired (source 10 -> hart 0 S-mode, SEIE + SIE on).\n");

    /* Phase 9k: the kernel's first periodic interrupt-driven behavior —
     * arm a 1-second supervisor timer (STIP). From here on,
     * handle_riscv_supervisor_interrupt (arch/riscv/sbi.c) takes a tick
     * every second, re-arms the next one, and prints [TICK N] on the
     * echo builds — a committed, observable periodic heartbeat (the
     * echo client waits for [TICK 2], which also proves the re-arm: a
     * one-shot timer would never deliver a second tick). Order matters:
     * sbi_arm_timer first (stimecmp -> now+1s, far in the future), THEN
     * sie.STIE. Arming before enabling guarantees no early tick:
     * whatever stimecmp held at boot, the comparator is already in the
     * future when the STIP becomes deliverable. sie.STIE is bit 5 =
     * 0x20, which does NOT fit the CSRxI immediate field (5 bits,
     * 0-31) — `csrs sie, 0x20` is an assembler error, so the register
     * form is required (sstatus/mstatus's SIE/MIE bits above are 1/3
     * and fit fine as immediates). */
    sbi_arm_timer(SBI_TIMER_TICK_PERIOD);
    uint64_t stie_bit = 0x20;
    __asm__ volatile("csrs sie, %0" : : "r"(stie_bit) : "memory");
#if defined(KERNEL_UART_ECHO)
    rv_boot_print("[TIMER] stimecmp armed (STIP, 100ms period -- Phase 9n contention probe).\n");
#else
    rv_boot_print("[TIMER] stimecmp armed (STIP, 1s period).\n");
#endif
#endif

#if defined(KERNEL_UART_ECHO)
    /* The echo build (sls_riscv_kernel_echo.elf, -DKERNEL_UART_ECHO):
     * instead of the SIMI smoke (which would power the machine off),
     * spin with interrupts enabled. Every received character interrupts
     * the hart, is echoed by the SEIP handler, and a complete line is
     * routed to the headless shell (which reports it). The runner
     * (tools/simi/tests/run_riscv_tests.sh) feeds a line through a
     * serial socket and asserts the echo -- the first device-driven
     * kernel I/O verified end to end. */
    rv_boot_print("[UART] ECHO READY: device-driven RX interrupt echo.\n");
    while (1) { asm volatile("wfi"); }
#else
    rv64_float_smoke_test();     /* Design B part 2 -- before the boot smoke, which powers off */
    rv_fp_round_robin_demo();    /* Design B part 3 -- the two-task FP round-robin */
    rv64_boot_smoke_test();
#endif

    while(1) { asm volatile("wfi"); }
}
