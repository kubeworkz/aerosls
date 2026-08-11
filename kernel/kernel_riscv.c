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

/* ─── Design B part 3 (ISA doc §16 Phase 16 audit addendum) + the
 * N-task ready-queue follow-up + the preemption follow-up: the context
 * switch finally gets real callers. perform_riscv_context_switch
 * (arch/riscv/context_riscv.S) has been dead code since Phase 9c and
 * GPR-only (the audit, Finding 2: "the context switch is GPR-only AND
 * dead code"). This section gives it its first callers and teaches the
 * switch to save and restore the fp_save owner rows: rv_task_switch_fp
 * saves the LIVE owner's state, loads the incoming task's row, folds
 * the registry, disarms FP, then performs the coroutine switch.
 *
 * TWO switch mechanisms now share the scheduler, and the preemption
 * follow-up is what makes the tick a true preemptive scheduler instead
 * of the poll-and-yield protocol:
 *
 * 1. COOPERATIVE (rv_task_switch_fp -> perform_riscv_context_switch):
 *    used ONLY for the boot-context handoffs — entering the first task
 *    from the driver and returning to the boot context when the queue
 *    empties. g_rv_switching guards the whole sequence so a tick can
 *    never preempt mid-switch (the coroutine's in-flight stack/FP
 *    state is not in a preemptable shape).
 *
 * 2. PREEMPTIVE (rv_scheduler_tick, called from the timer interrupt in
 *    arch/riscv/sbi.c): the tick handler itself rotates the running
 *    task, so a compute-bound task cannot hog the hart. The interrupted
 *    task's FULL context (all 31 GPRs + sepc) is already sitting in
 *    RvPerHartData.trap_frame (the trap entry saved it); the scheduler
 *    copies it into the task's ctx[32] image, copies the next task's
 *    image into trap_frame, swaps the FP owner registry, and disarms
 *    FS — the trap epilogue then srets into the next task. The trap
 *    frame IS the switch buffer, zero new assembly.
 *
 * The ready queue is N-task: a task array + round-robin cursor
 * (rv_schedule_next skips `done` tasks), a generic runner
 * (rv_fp_task_common) driven by a per-task spec table, and a handoff
 * back to the boot context when no task remains runnable. The FP owner
 * registry grows with it: fp_save rows are now RV_FP_OWNERS (trap_riscv.h)
 * and the &1 two-owner masks are gone — owners are full 0..N-1 ids. */
#define RV_TASK_COUNT 3

static struct RvTask g_rv_main_ctx;    /* the boot context (suspended while tasks run) */
static struct RvTask g_rv_tasks[RV_TASK_COUNT];

/* The N-task scheduler state: g_rv_cursor is the index of the task that
 * last ran (the next pick starts just after it); g_rv_current is the
 * running task, or NULL while the boot context runs (set by
 * rv_task_switch_fp and rv_scheduler_tick before each switch so a
 * resumed task can identify itself -- the only task identity a
 * coroutine entry needs). */
static uint64_t g_rv_cursor;
static struct RvTask* g_rv_current;

/* The preemption guard: 1 while a COOPERATIVE switch is in flight
 * (rv_task_switch_fp's FP save/load + coroutine handoff), so the tick
 * handler's rv_scheduler_tick no-ops rather than preempt a task whose
 * context is half-way between its stack and its trap-frame image. Set
 * by the switching side, cleared by whichever context the switch
 * resumes into (the task entry's first line, or the driver right after
 * the switch returns). Written/read by different contexts, so
 * volatile — single hart, but the readers must never get a stale
 * hoisted load. */
volatile uint64_t g_rv_switching;

/* Lay a task's initial state: the COOPERATIVE entry path needs a
 * fabricated stack frame (the switch's restore pops 14 registers — ra,
 * s0-s11, tp — off the incoming sp and `ret`s, so the frame has ra =
 * the task entry and zeros elsewhere); the PREEMPTIVE entry path needs
 * the fabricated ctx[32] trap-frame image (sepc = entry, sp = stack
 * top, gp/tp = the kernel's live values — the tick loads it into
 * trap_frame and the trap epilogue srets into it). Both are laid here
 * so the first entry into ANY task works in either mechanism; every
 * later entry overwrites whichever image the tick/switch used. */
static void rv_task_init(struct RvTask* t, void (*fn)(void), uint64_t owner,
                         const char* name) {
    uint64_t* sp = (uint64_t*)(t->stack + sizeof(t->stack));
    sp -= 14;
    sp[0] = (uint64_t)(uintptr_t)fn;   /* ra: the task entry */
    for (int i = 1; i < 14; i++) sp[i] = 0;
    t->rsp = (uint64_t)(uintptr_t)sp;
    t->fp_owner = owner;   /* full owner id, 0..RV_FP_OWNERS-1 */
    t->name = name;
    t->slices = 0;
    t->done = 0;
    t->preemptions = 0;
    t->work_done = 0;
    t->lcg_acc = 0;
    register uint64_t gp_v __asm__("gp");
    register uint64_t tp_v __asm__("tp");
    for (int i = 0; i < 32; i++) t->ctx[i] = 0;
    t->ctx[TF_RA] = 0;
    t->ctx[TF_SP] = (uint64_t)(uintptr_t)(t->stack + sizeof(t->stack));
    t->ctx[TF_GP] = gp_v;
    t->ctx[TF_TP] = tp_v;
    t->ctx[TF_SEPC] = (uint64_t)(uintptr_t)fn;
    /* The rest of ctx stays zero: the task entry's prologue sets up its
     * own frame and clobbers caller-saved registers anyway. */
}

/* The FP-aware cooperative task switch — the coroutine path's only
 * caller, used ONLY for the boot-context handoffs (driver -> first
 * task, last task -> driver). Saves the live owner's FP state
 * (whoever's is in the registers) into its fp_save row, loads the
 * incoming task's row, folds the owner registry, disarms FP (so the
 * incoming task's FIRST FP instruction lazy-saves — proving the eager
 * load and the trap path agree), then performs the GPR coroutine
 * switch. g_rv_switching is raised for the whole sequence and lowered
 * by whichever context the switch resumes into — the task entry's
 * first line, or right here when the switch returns into the driver —
 * so a tick can never preempt a task whose context is mid-handoff.
 * When the switch later returns into this task, the switch that left it
 * here already loaded THIS task's state — no FP work happens on
 * resume. All FP instructions here are the sanctioned
 * fp_save_all/fp_load_all plumbing (rv64_fp_census gate). */
static void rv_task_switch_fp(struct RvTask* cur, struct RvTask* next) {
    struct RvPerHartData* phd = &g_hart0_data;
    g_rv_switching = 1;
    uint64_t sst;
    __asm__ volatile("csrr %0, sstatus" : "=r"(sst));
    /* FS=Dirty first: an fsd with FS=Off would itself trap. */
    __asm__ volatile("csrw sstatus, %0" : : "r"(sst | (3ULL << 13)) : "memory");
    uint64_t live = phd->fp_current;   /* full owner id */
    fp_save_all(&phd->fp_save[live][0]);
    uint64_t no = next->fp_owner;
    fp_load_all(&phd->fp_save[no][0]);
    phd->fp_owner = no;
    phd->fp_current = no;
    g_rv_current = (next == &g_rv_main_ctx) ? NULL : next;  /* the resumed
                                      * task identifies itself via this */
    /* FS=Off: the incoming task's first FP instruction lazy-saves. */
    __asm__ volatile("csrc sstatus, %0" : : "r"(3ULL << 13) : "memory");
    perform_riscv_context_switch(cur, next);
    g_rv_switching = 0;
    /* Resumed later (boot-context side): the tasks never return from a
     * cooperative switch — they are resumed by the tick's ctx reload,
     * which clears the guard at task entry instead. */
}

/* The ready-queue pick: advance the cursor and return the next task
 * that has not finished; NULL when every task is done (the yield then
 * hands control back to the boot context). */
static struct RvTask* rv_schedule_next(void) {
    for (uint64_t i = 1; i <= RV_TASK_COUNT; i++) {
        struct RvTask* t = &g_rv_tasks[(g_rv_cursor + i) % RV_TASK_COUNT];
        if (!t->done) { g_rv_cursor = (g_rv_cursor + i) % RV_TASK_COUNT; return t; }
    }
    return NULL;
}

/* Yield: run the next ready task, or go back to the boot context when
 * none is left. Used ONLY for the queue-empty handoff now — the per-
 * slice rotation is the tick handler's job (rv_scheduler_tick). */
static void rv_task_yield(struct RvTask* cur) {
    struct RvTask* next = rv_schedule_next();
    rv_task_switch_fp(cur, next ? next : &g_rv_main_ctx);
}

/* The preemptive scheduler hook — called from the timer interrupt
 * (arch/riscv/sbi.c) on every tick, replacing the old poll-and-yield
 * protocol: the tick handler itself rotates the running task, so a
 * compute-bound task cannot hog the hart. The interrupted task's full
 * context (all 31 GPRs + sepc) is already sitting in
 * RvPerHartData.trap_frame — the trap entry saved it, and the epilogue
 * will restore whatever is there when the handler returns. Preemption
 * is therefore just: save the running task's trap-frame image into its
 * ctx[32], save its FP state into its owner row, load the next runnable
 * task's ctx image into trap_frame, load its FP state, fold the
 * registry, and disarm FS — the trap epilogue then srets into the next
 * task. The trap frame IS the switch buffer: zero new assembly. A no-op
 * while the boot context runs (g_rv_current == NULL) or a cooperative
 * switch is in flight (g_rv_switching) — the timer already re-armed
 * before this call, so no tick is lost. Only runnable-task preemptions
 * count (preemptions++ happens only when next exists AND the preempted
 * task is still runnable — a done task's rotation-out is just the
 * scheduler clearing the stage for the remaining tasks). */
void rv_scheduler_tick(void) {
    struct RvPerHartData* phd = &g_hart0_data;
    if (g_rv_switching || g_rv_current == NULL) return;
    struct RvTask* cur = g_rv_current;
    struct RvTask* next = rv_schedule_next();   /* skips done tasks */
    if (next == NULL) return;   /* all done: the last-finishing task's
                                  * cooperative handoff to the boot
                                  * context is already in flight or
                                  * imminent — never preempt it */
    /* 1. The interrupted task's context: trap_frame[TF_RA..TF_SEPC] is
     * slots 0..31 (TF_RA is 0), mirroring ctx[i] 1:1. */
    for (int i = 0; i < 32; i++) cur->ctx[i] = phd->trap_frame[i];
    if (!cur->done) cur->preemptions++;
    /* 2. Its FP state into its owner row (FS=Dirty first: an fsd with
     * FS=Off would itself trap). fp_current == cur->fp_owner: the last
     * rotation loaded THIS task's row and folded the registry, and the
     * per-slice lazy-save only ever folds owner->owner (a 0->0 no-op). */
    uint64_t sst;
    __asm__ volatile("csrr %0, sstatus" : "=r"(sst));
    __asm__ volatile("csrw sstatus, %0" : : "r"(sst | (3ULL << 13)) : "memory");
    fp_save_all(&phd->fp_save[cur->fp_owner][0]);
    /* 3. The next task's context + FP state into place. */
    for (int i = 0; i < 32; i++) phd->trap_frame[i] = next->ctx[i];
    fp_load_all(&phd->fp_save[next->fp_owner][0]);
    phd->fp_owner = next->fp_owner;
    phd->fp_current = next->fp_owner;
    g_rv_current = next;
    /* 4. Disarm FS: the incoming task's first FP instruction lazy-saves
     * — one trap per slice, the count the demo and CI pin. The eager
     * load + disarm makes that trap a 0->0 no-op round-trip through its
     * own row, exactly like the cooperative path. */
    __asm__ volatile("csrc sstatus, %0" : : "r"(3ULL << 13) : "memory");
    rv_boot_print("[TASK] preempt ");
    rv_boot_print(cur->name);
    rv_boot_print(" -> ");
    rv_boot_print(next->name);
    rv_boot_print("\n");
}

/* The FP work of the tasks — the ONLY FP instructions outside the
 * fp_save_all/fp_load_all plumbing, and the rv64_fp_census allow-list
 * sanctions exactly rv_fp_task_common (an injected fadd.d in any other
 * kernel function still fails the gate). One generic runner drives all
 * N tasks off a per-task spec (step, expected bit patterns, name,
 * per-slice work budget): each slice does one fadd.d accumulating the
 * task's step into f10, reads it back with fmv.x.d, asserts the exact
 * bit pattern (the expected constants are precomputed double(s) bit
 * patterns — no FP conversion needed in C), then runs its per-slice
 * integer work budget (a deterministic LCG, the FAIRNESS probe: all
 * three tasks carry the same 10x-heavy budget, yet the tick's
 * round-robin still grants every task the same slice count), and waits
 * for the next tick-granted boundary in its spin. Because every switch disarms FP,
 * each slice's first fadd.d traps and lazy-saves — one trap per slice.
 * The step constant rides in a GPR and is materialized INSIDE the asm
 * via fmv.d.x into scratch ft0: the compiler therefore sees no FP
 * value live in this function, so it cannot hoist a loop-invariant
 * double into callee-saved fs0 (which would put a compiler-generated
 * fld in the epilogue AFTER the handback switch disarms FS -- the
 * spurious 11th lazy-save that broke the per-slice trap count). The
 * only FP instructions in this function are the asm blocks themselves. */
#define RV_TASK_SLICES 5

struct RvTaskSpec {
    uint64_t step;            /* f10 += step per slice, as double bits */
    const uint64_t* exp;      /* RV_TASK_SLICES expected f10 bit patterns */
    const char* name;         /* "fp-a" / "fp-b" / "fp-c" for the [TASK] log */
    uint32_t work;            /* per-slice LCG iterations — the fairness
                                 probe: all tasks run the 10x-heavy budget */
};

/* Task A: f10 += 1.0 -> 1.0..5.0. Task B: f10 += 0.5 -> 0.5..2.5.
 * Task C: f10 += 0.25 -> 0.25..1.25 — the CONTRAST owner: after every
 * switch back, each task's f10 must be ITS OWN value, never another
 * task's — that is the round-trip proof with N owners in the registry. */
static const uint64_t g_rv_exp_a[RV_TASK_SLICES] = {
    0x3FF0000000000000ULL, /* 1.0 */
    0x4000000000000000ULL, /* 2.0 */
    0x4008000000000000ULL, /* 3.0 */
    0x4010000000000000ULL, /* 4.0 */
    0x4014000000000000ULL, /* 5.0 */
};
static const uint64_t g_rv_exp_b[RV_TASK_SLICES] = {
    0x3FE0000000000000ULL, /* 0.5 */
    0x3FF0000000000000ULL, /* 1.0 */
    0x3FF8000000000000ULL, /* 1.5 */
    0x4000000000000000ULL, /* 2.0 */
    0x4004000000000000ULL, /* 2.5 */
};
static const uint64_t g_rv_exp_c[RV_TASK_SLICES] = {
    0x3FD0000000000000ULL, /* 0.25 */
    0x3FE0000000000000ULL, /* 0.5 */
    0x3FE8000000000000ULL, /* 0.75 */
    0x3FF0000000000000ULL, /* 1.0 */
    0x3FF4000000000000ULL, /* 1.25 */
};
/* The work budgets are the fairness probe, ALL-HEAVY extreme: every
 * task carries the 200k-iteration per-slice budget (10x the 20k light
 * baseline of the original variant) — yet the preemptive round-robin
 * must still grant every task exactly RV_TASK_SLICES
 * slices/preemptions, because the rotation is tick-cadence-driven, not
 * work-driven. The end-of-demo lcg_acc values (A=B=C=0xf2dc5340 after
 * 5*200k=1M iterations each, starting from 0) are precomputed on the
 * host — the acc is a deterministic function of the budget, so the
 * work provably ran its iterations (the Phase 9l discipline). */
static const struct RvTaskSpec g_rv_spec[RV_TASK_COUNT] = {
    { 0x3FF0000000000000ULL, g_rv_exp_a, "fp-a", 200000u },  /* += 1.0   -> 1.0..5.0  */
    { 0x3FE0000000000000ULL, g_rv_exp_b, "fp-b", 200000u },  /* += 0.5   -> 0.5..2.5  */
    { 0x3FD0000000000000ULL, g_rv_exp_c, "fp-c", 200000u },  /* += 0.25  -> 0.25..1.25 */
};

static void rv_fp_task_common(void) {
    struct RvTask* me = g_rv_current;   /* set by the switch/tick that entered us */
    g_rv_switching = 0;   /* the cooperative entry (driver -> task 0) is
                           * complete: the tick may now preempt us. For
                           * tasks entered preemptively (via the tick
                           * loading their fabricated ctx) this is a
                           * no-op — the flag was already clear. */
    uint64_t idx = (uint64_t)(me - g_rv_tasks);
    const struct RvTaskSpec* sp = &g_rv_spec[idx];
    for (int s = 0; s < RV_TASK_SLICES; s++) {
        /* Wait for the tick to grant slice s+1. Every rotation into us
         * resumes at this spin — the tick saved our sepc mid-spin — and
         * each rotation also incremented me->preemptions, so the spin
         * exits exactly when s+1 boundaries have passed. A BUSY spin,
         * deliberately NOT wfi: a preemption taken at the wfi would
         * re-execute it on resume and burn the grant that just woke us,
         * drifting the per-task preemptions accounting (the
         * spurious-wake lesson of part 3's 11th lazy-save, in spin
         * form). The task spends ~100ms in this spin per slice vs.
         * microseconds on the work below, so the tick lands here —
         * exactly one preemption per slice. */
        while (me->preemptions < (uint64_t)(s + 1)) { }
        uint64_t got;
        __asm__ volatile(
            "fmv.d.x ft0, %1\n\t"
            "fadd.d f10, f10, ft0\n\t"
            "fmv.x.d %0, f10\n\t"
            : "=r"(got)
            : "r"(sp->step)
            : "ft0", "f10", "memory");
        me->slices++;
        rv_boot_print("[TASK] ");
        rv_boot_print(sp->name);
        rv_boot_print(" slice ");
        rv_boot_print_udec64((uint64_t)(s + 1));
        rv_boot_print(": ");
        rv_boot_print_hex64(got);
        rv_boot_print(got == sp->exp[s] ? " PASS\n" : " FAIL\n");
        /* The per-slice integer work — the fairness probe's heavy
         * lift: a deterministic LCG (Numerical Recipes constants)
         * stepped exactly sp->work times, accumulating across slices
         * so the final lcg_acc is a function of the TOTAL budget. The
         * acc is observable (stored back to the task), so the loop can
         * never be dead-code-eliminated. Integer only — no FP, so the
         * census allow-list and the per-slice lazy-save discipline are
         * untouched. 200k iterations is a small fraction of the 100ms
         * slice even on the slowest CI host, so the tick still lands
         * in the spin and the preemption accounting stays exact. */
        uint32_t acc = (uint32_t)me->lcg_acc;
        for (uint32_t w = 0; w < sp->work; w++)
            acc = acc * 1664525u + 1013904223u;
        me->lcg_acc = acc;
        me->work_done += sp->work;
    }
    /* All slices done: mark finished. The handoff discipline matters
     * here: a task that was PREEMPTIVELY suspended holds its resume
     * state in its ctx image, so the coroutine path must never resume
     * it — therefore a done task never cooperatively hands off to
     * another task; it either hands back to the boot context (only
     * valid when it is the LAST runnable task — switching OUT of a
     * running task is always sound, the coroutine saves our current
     * state and pops the driver's) or spins until the tick rotates it
     * out of the way so the next task can resume. */
    me->done = 1;
    int last = 1;
    for (int i = 0; i < RV_TASK_COUNT; i++) {
        if (&g_rv_tasks[i] != me && !g_rv_tasks[i].done) { last = 0; break; }
    }
    if (last) {
        rv_task_yield(me);   /* -> boot context (queue empty) */
        /* Not reached: the driver resumed and never switches back. */
    }
    for (;;) { __asm__ volatile(""); }   /* wait to be preempted away */
}

/* The demo driver: reset the FP registry (the float smoke left rows 0/1
 * holding its +inf/20.0 state), initialize the N-task ready queue, run
 * the round-robin at a fast 100ms tick cadence, then assert the FINAL
 * fp_save rows (one per task owner), the lazy-save delta (one per
 * task-slice = RV_TASK_COUNT*RV_TASK_SLICES = 15), and the preemption
 * accounting (one tick-handler rotation per slice boundary, also 15).
 * The FAIRNESS probe rides along, now at the ALL-HEAVY extreme: every
 * task runs the same 10x-heavy per-slice budget, so the demo asserts
 * each did exactly 1,000,000 LCG iterations (with the host-precomputed
 * lcg_acc end-states as proof) while STILL getting exactly
 * RV_TASK_SLICES preemptions like everyone else — the preemptive
 * round-robin is workload-oblivious. Task 0 is
 * entered COOPERATIVELY by the driver (rv_task_switch_fp); every
 * subsequent rotation is the tick handler's rv_scheduler_tick
 * preempting the running task — the demo's only cooperative switches
 * are the entry and the queue-empty handoff back to this driver. */
static void rv_fp_round_robin_demo(void) __attribute__((unused)); /* the echo build never calls it (wfi loop) */
static void rv_fp_round_robin_demo(void) {
    struct RvPerHartData* phd = &g_hart0_data;
    rv_boot_print("[TASK] three-task FP ready queue (preemptive, real register contexts, Design B part 3 + preemption follow-up)...\n");
    for (uint64_t o = 0; o < RV_FP_OWNERS; o++)
        for (int i = 0; i < 33; i++) phd->fp_save[o][i] = 0;
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
    g_rv_cursor = 0;                   /* task 0 is about to run: the first
                                         * yield picks (0+1)%N = task 1 */
    g_rv_current = NULL;               /* boot context runs the driver */
    for (uint64_t i = 0; i < RV_TASK_COUNT; i++)
        rv_task_init(&g_rv_tasks[i], rv_fp_task_common, i, g_rv_spec[i].name);
    uint64_t lazy_before = g_fp_lazy_count;
    sbi_set_tick_period(1000000UL);   /* 100ms slices for the demo */
    __asm__ volatile("csrc sstatus, %0" : : "r"(3ULL << 13) : "memory");
    rv_task_switch_fp(&g_rv_main_ctx, &g_rv_tasks[0]);
    /* Back here when the last task finishes and no ready task remains. */
    sbi_set_tick_period(0);
    uint64_t lazy_delta = g_fp_lazy_count - lazy_before;
    int rows_ok = (phd->fp_save[0][10] == 0x4014000000000000ULL) &&  /* A: 5.0 */
                  (phd->fp_save[1][10] == 0x4004000000000000ULL) &&  /* B: 2.5 */
                  (phd->fp_save[2][10] == 0x3FF4000000000000ULL);    /* C: 1.25 */
    int lazy_ok = (lazy_delta == RV_TASK_COUNT * RV_TASK_SLICES);
    /* The preemption accounting: each task must have been preempted
     * exactly RV_TASK_SLICES times (one rotation per slice boundary) —
     * the preemption-side twin of the lazy-save count. The tick never
     * fires while the boot context runs or a cooperative switch is in
     * flight, and it lands in the tasks' spin loops, so the sum is
     * exact: RV_TASK_COUNT * RV_TASK_SLICES = 15. */
    uint64_t preempt_sum = 0;
    int preempt_ok = 1;
    for (int i = 0; i < RV_TASK_COUNT; i++) {
        preempt_sum += g_rv_tasks[i].preemptions;
        if (g_rv_tasks[i].preemptions != RV_TASK_SLICES) preempt_ok = 0;
    }
    if (preempt_sum != RV_TASK_COUNT * RV_TASK_SLICES) preempt_ok = 0;
    /* The fairness probe asserts, at the all-heavy extreme: every task
     * ran the SAME heavy total (1,000,000 LCG iterations each — exactly
     * its 200k budget x RV_TASK_SLICES) AND still got exactly
     * RV_TASK_SLICES preemptions/slices like everyone else — the
     * preemptive round-robin is workload-oblivious, the property this
     * probe exists to pin. The lcg_acc end-states are the "the work
     * provably ran" teeth: precomputed on the host (A = B = C =
     * 0xf2dc5340 after 1M steps each, from 0). */
    uint64_t wa = g_rv_tasks[0].work_done;
    uint64_t wb = g_rv_tasks[1].work_done;
    uint64_t wc = g_rv_tasks[2].work_done;
    int work_ok = (wa == wb) && (wb == wc) &&
                  (wa == (uint64_t)g_rv_spec[0].work * RV_TASK_SLICES) &&
                  (wb == (uint64_t)g_rv_spec[1].work * RV_TASK_SLICES) &&
                  (wc == (uint64_t)g_rv_spec[2].work * RV_TASK_SLICES);
    int acc_ok = (g_rv_tasks[0].lcg_acc == 0xf2dc5340ULL) &&
                 (g_rv_tasks[1].lcg_acc == 0xf2dc5340ULL) &&
                 (g_rv_tasks[2].lcg_acc == 0xf2dc5340ULL);
    rv_boot_print("[TASK] fp_save rows: A=");
    rv_boot_print_hex64(phd->fp_save[0][10]);
    rv_boot_print(" B=");
    rv_boot_print_hex64(phd->fp_save[1][10]);
    rv_boot_print(" C=");
    rv_boot_print_hex64(phd->fp_save[2][10]);
    rv_boot_print(rows_ok ? " PASS;" : " FAIL;");
    rv_boot_print(" lazy-saves during demo: ");
    rv_boot_print_udec64(lazy_delta);
    rv_boot_print(lazy_ok ? " PASS;" : " FAIL;");
    rv_boot_print(" preemptions: ");
    rv_boot_print_udec64(preempt_sum);
    rv_boot_print(preempt_ok ? " PASS;" : " FAIL;");
    rv_boot_print(" work: ");
    rv_boot_print_udec64(wa);
    rv_boot_print("/");
    rv_boot_print_udec64(wb);
    rv_boot_print("/");
    rv_boot_print_udec64(wc);
    rv_boot_print(work_ok ? " (equal) PASS; acc: " : " (equal) FAIL; acc: ");
    rv_boot_print_hex64(g_rv_tasks[0].lcg_acc);
    rv_boot_print("/");
    rv_boot_print_hex64(g_rv_tasks[1].lcg_acc);
    rv_boot_print("/");
    rv_boot_print_hex64(g_rv_tasks[2].lcg_acc);
    rv_boot_print(acc_ok ? " PASS\n" : " FAIL\n");
    rv_boot_print(rows_ok && lazy_ok && preempt_ok && work_ok && acc_ok
                      ? "[TASK] ready queue: ALL PASS\n"
                      : "[TASK] ready queue: FAIL\n");
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
    rv_fp_round_robin_demo();    /* Design B part 3 -- the N-task FP ready queue */
    rv64_boot_smoke_test();
#endif

    while(1) { asm volatile("wfi"); }
}
