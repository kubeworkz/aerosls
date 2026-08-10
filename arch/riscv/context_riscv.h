/*
 * context_riscv.h — the RISC-V coroutine task context (Design B part 3,
 * ISA doc §16 Phase 16 audit addendum): the missing definition behind
 * arch/riscv/context_riscv.S's perform_riscv_context_switch, which the
 * audit found GPR-only and with ZERO callers. The N-task FP ready
 * queue (kernel/kernel_riscv.c) is its first real caller, and the
 * FP-aware wrapper (rv_task_switch_fp) is what finally makes the switch
 * save and restore the RvPerHartData fp_save owner rows. Part 3's
 * follow-up generalized the two-task round-robin into an N-task queue:
 * a task array + round-robin cursor, `done` marking a finished task so
 * the scheduler skips it, and a yield-to-boot-context handoff when no
 * task remains runnable.
 *
 * LAYOUT CONTRACT: rsp must sit at offset 8. context_riscv.S stores the
 * outgoing task's sp at 8(a0) and loads the incoming one from 8(a1) —
 * the offsets are hardcoded in assembly (asm cannot #include this
 * struct), so pad0 exists purely to hold that contract.
 */
#ifndef CONTEXT_RISCV_H
#define CONTEXT_RISCV_H

#include <stdint.h>

/* The coroutine task descriptor. The stack is per-task and dedicated;
 * the switch never allocates — it saves the 14 callee-saved registers
 * (ra, s0-s11, tp) onto the outgoing task's own stack and stores the
 * resulting sp here. */
struct RvTask {
    uint64_t pad0;         /* offset 0 — keeps rsp at the asm's offset 8 */
    uint64_t rsp;          /* offset 8: saved sp of the suspended task */
    uint64_t fp_owner;     /* this task's FP owner id (0..RV_FP_OWNERS-1) into
                              the shared RvPerHartData fp_save registry (part 2) */
    const char* name;      /* printable task name */
    uint64_t slices;       /* round-robin accounting: slices this task ran */
    uint64_t done;         /* N-task ready queue (part 3 follow-up): 1 = this
                              task finished its slices — the scheduler skips it */
    uint8_t  stack[4096];  /* dedicated kernel stack */
};

/* The coroutine switch (arch/riscv/context_riscv.S): saves the current
 * task's callee-saved registers onto its stack, stores the sp at
 * 8(cur), loads the incoming task's sp from 8(next), pops its registers
 * and returns into its execution path. The FP-aware wrapper
 * (kernel_riscv.c's rv_task_switch_fp) is the ONLY intended caller —
 * call it directly only when no FP state must round-trip. */
void perform_riscv_context_switch(struct RvTask* cur, struct RvTask* next);

/* The time-slice boundary marker: written by the timer interrupt
 * (arch/riscv/sbi.c's handle_riscv_supervisor_interrupt) on every tick,
 * polled+cleared by the running task at its slice boundary (Design B
 * part 3). volatile: written from interrupt context, read from task
 * context, no locking (single hart, the task polls while interrupts are
 * enabled — a tick between poll and clear just sets it again, which the
 * next task's poll consumes harmlessly). */
extern volatile uint64_t g_rv_slice_expired;

#endif /* CONTEXT_RISCV_H */
