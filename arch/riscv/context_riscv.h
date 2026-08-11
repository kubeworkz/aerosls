/*
 * context_riscv.h — the RISC-V task context (Design B part 3 + the
 * preemption follow-up, ISA doc §16 Phase 16 audit addendum): the
 * definition behind arch/riscv/context_riscv.S's
 * perform_riscv_context_switch, which the audit found GPR-only and with
 * ZERO callers. Two switch mechanisms now share this struct:
 *
 * 1. COOPERATIVE (rv_task_switch_fp, the switch's only intended
 *    caller): used for the boot-context handoffs — entering the first
 *    task and returning to the boot context when the queue empties. It
 *    saves the live owner's FP state into its fp_save row, loads the
 *    incoming task's, folds the registry, and performs the coroutine
 *    switch. The 14-slot stack frame (ra, s0-s11, tp) and rsp field are
 *    its format.
 *
 * 2. PREEMPTIVE (rv_scheduler_tick, called from the timer interrupt in
 *    arch/riscv/sbi.c): the tick handler itself rotates the running
 *    task, so a compute-bound task cannot hog the hart. The interrupted
 *    task's FULL context (all 31 GPRs + sepc) is already sitting in
 *    RvPerHartData.trap_frame (the trap entry saved it); the scheduler
 *    copies it into this task's ctx[32] image, copies the next task's
 *    ctx[32] image into trap_frame, and the trap epilogue srets into
 *    the next task — the trap frame IS the switch buffer, no new
 *    assembly. ctx layout mirrors trap_frame[TF_RA..TF_SEPC] (0..31).
 *    FP state rides in the owner registry rows, not in ctx (the FP
 *    region of the trap frame stays reserved).
 *
 * LAYOUT CONTRACT: rsp must sit at offset 8. context_riscv.S stores the
 * outgoing task's sp at 8(a0) and loads the incoming one from 8(a1) —
 * the offsets are hardcoded in assembly (asm cannot #include this
 * struct), so pad0 exists purely to hold that contract.
 */
#ifndef CONTEXT_RISCV_H
#define CONTEXT_RISCV_H

#include <stdint.h>

/* The task descriptor. The stack is per-task and dedicated; both switch
 * mechanisms never allocate on it. preemptions/done are read by the
 * task (spin gates) and written by the tick-handler scheduler, so they
 * are volatile — single hart, but the task-side spin loops must never
 * have the compiler hoist the load out of the loop. */
struct RvTask {
    uint64_t pad0;         /* offset 0 — keeps rsp at the asm's offset 8 */
    uint64_t rsp;          /* offset 8: saved sp of the cooperatively
                              suspended task */
    uint64_t fp_owner;     /* this task's FP owner id (0..RV_FP_OWNERS-1) into
                              the shared RvPerHartData fp_save registry (part 2) */
    const char* name;      /* printable task name */
    volatile uint64_t slices;      /* slices this task ran */
    volatile uint64_t done;        /* 1 = finished — the scheduler skips it */
    volatile uint64_t preemptions; /* how many times the tick-handler
                                      scheduler has preempted this task
                                      (only runnable-task preemptions count) */
    volatile uint64_t work_done;   /* total integer-work iterations this
                                      task executed across all slices
                                      (the fairness probe: two tasks'
                                      per-slice budgets are 10x the
                                      third's, yet the scheduler still
                                      grants equal slices — written by
                                      the task, read by the demo driver) */
    uint64_t lcg_acc;      /* the accumulated LCG work state — a
                              deterministic function of work_done, so
                              the work provably ran its iterations (the
                              Phase 9l discipline: an observable result
                              that depends on the budget) */
    uint64_t ctx[32];      /* preemptive-resume image: trap_frame[TF_RA..
                              TF_SEPC], the full 31 GPRs + sepc. Fabricated
                              at rv_task_init (sepc=entry, sp=stack top,
                              gp/tp = kernel's); rewritten by the scheduler
                              from trap_frame at every preemption. */
    uint8_t  stack[4096];  /* dedicated kernel stack */
};

/* The coroutine switch (arch/riscv/context_riscv.S): saves the current
 * task's callee-saved registers onto its stack, stores the sp at
 * 8(cur), loads the incoming task's sp from 8(next), pops its registers
 * and returns into its execution path. The FP-aware wrapper
 * (kernel_riscv.c's rv_task_switch_fp) is the ONLY intended caller —
 * call it directly only when no FP state must round-trip. */
void perform_riscv_context_switch(struct RvTask* cur, struct RvTask* next);

/* The preemptive scheduler hook (kernel/kernel_riscv.c), called from
 * the timer interrupt (arch/riscv/sbi.c) on every tick. Rotates the
 * running task by swapping its trap-frame image for the next task's:
 * saves g_rv_current's ctx from RvPerHartData.trap_frame, loads the
 * next runnable task's ctx into it, swaps the FP owner registry, and
 * disarms FS — the trap epilogue then srets into the next task. A
 * no-op when the boot context is running (g_rv_current == NULL) or a
 * cooperative switch is in flight (g_rv_switching). */
void rv_scheduler_tick(void);

#endif /* CONTEXT_RISCV_H */
