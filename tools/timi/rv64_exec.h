/*
 * rv64_exec.h — Phase 5 verification tool, NOT part of the translator.
 *
 * This sandbox has no riscv64 cross-compiler and no qemu-riscv64/
 * qemu-system-riscv64 (checked, neither installable — no root either).
 * Phase 3's x86 verification method (mmap the translated bytes PROT_EXEC
 * and call them as a real function on the host CPU) is unavailable here
 * because the host CPU is itself x86-64 — there is no RV64 execution unit
 * to hand the bytes to.
 *
 * So: this is a small, purpose-built RV64 instruction decoder+executor,
 * covering exactly the encodings timi_riscv.c emits (not a general RISC-V
 * simulator — no CSRs, no FP, no atomics, no compressed instructions, no
 * traps). It exists for the same reason timi_interp.c (Phase 1) exists:
 * an independently-written reference that actually executes the encoded
 * bytes, so a bit-packing mistake in the encoder shows up as a wrong
 * answer instead of passing silent review. See
 * AeroSLS-TIMI-ISA-v0.1.md §12 for why this is weaker evidence than
 * Phase 3's real-hardware execution (a bug in both the encoder AND this
 * decoder that happens to agree wouldn't be caught) but still materially
 * stronger than static review alone (which is exactly what missed the
 * x86 ModRM bug in Phase 3 — see §10).
 */
#ifndef RV64_EXEC_H
#define RV64_EXEC_H

#include <stdint.h>

enum {
    RV_EXEC_OK = 0,          /* pc reached the sentinel return address */
    RV_EXEC_STEP_LIMIT,      /* exceeded the step budget — likely an infinite loop or decode bug */
    RV_EXEC_BAD_INSTR,       /* opcode/funct3/funct7 combination this executor doesn't implement */
    RV_EXEC_MEM_FAULT,       /* load/store or fetch outside the guest memory buffer */
};

/* Guest sentinel: the caller sets x1 (ra) to this before starting
 * execution at the trampoline's offset. When a `jalr` sets pc to this
 * value, the trampoline's own final return has fired — that is the
 * "the top-level call has returned" signal a real hardware `call`
 * instruction's return address would provide implicitly. */
#define RV_EXEC_SENTINEL_RA 0xFFFFFFFFFFFFFFF0ull

/* v0.3 (Phase 6): host-callback sentinels — this executor's answer to
 * "how do you call a real function from simulated RV64 code when there's
 * no real RV64 CPU underneath it." timi_riscv.c's RESOLVE/OBJSIZE/OBJTYPE
 * codegen materializes an ordinary-looking absolute address into a
 * register and does a real `jalr ra,0(reg)` — from the encoder's point of
 * view that address is just an opaque uint64_t the caller supplied (see
 * timi_riscv.h), exactly like x86's movabs+call. On real hardware (or in
 * the kernel) that address would be real RV64 machine code. Here, this
 * executor's JALR handling special-cases addresses in this reserved range:
 * instead of faulting trying to fetch instructions at a fake address, it
 * looks up a registered host C function by index, calls it with x10 (a0,
 * the real RV64 ABI arg0/return register) as both argument and result
 * register, and then behaves exactly as if that function had executed and
 * returned via `ret` — i.e. control resumes at the JALR's own link
 * address, same observable effect, zero guest instructions actually
 * fetched at the sentinel. The host JIT test harness picks the sentinel
 * addresses (via rv64_exec_hostfn_addr) and passes them into
 * timi_riscv_translate() as if they were real function addresses; the
 * kernel, with a real RV64 core underneath it, would pass real ones
 * instead and never touch this table at all. */
#define RV_EXEC_HOSTFN_BASE 0xFFFFFFFE00000000ull
#define RV_EXEC_HOSTFN_MASK 0xFFFFFFFF00000000ull
#define RV_EXEC_MAX_HOSTFNS 8
/* Slots are spaced 16 apart, not 1 — real RV64 `jalr` always clears the
 * target address's least-significant bit ("... and then setting the
 * least-significant bit of the result to zero", per spec) before jumping.
 * A naive BASE+idx encoding silently corrupts every odd-numbered slot:
 * idx=1's sentinel has bit0 set, jalr clears it, and the executor ends up
 * invoking slot 0 instead — caught by rv64_exec_run() actually calling
 * the wrong host function (RESOLVE's mock instead of OBJSIZE's, handed
 * OBJSIZE's argument) rather than by review. See AeroSLS-TIMI-ISA-v0.1.md
 * §13. Any stride >= 2 fixes it; 16 leaves headroom and keeps the low
 * nibble visually zero in address dumps. */
#define RV_EXEC_HOSTFN_STRIDE 16u

typedef uint64_t (*RvHostFn)(uint64_t arg);

/* Address to hand to timi_riscv_translate() as rt_resolve_fn/rt_objsize_fn/
 * rt_objtype_fn for host-callback slot `idx` (0..RV_EXEC_MAX_HOSTFNS-1). */
static inline uint64_t rv64_exec_hostfn_addr(int idx) {
    return RV_EXEC_HOSTFN_BASE + (uint64_t)(uint32_t)idx * RV_EXEC_HOSTFN_STRIDE;
}

/* Registers (or clears, with fn==0) the host function invoked when a JALR
 * targets rv64_exec_hostfn_addr(idx). Must be called before rv64_exec_run()
 * for any slot the translated code might actually call. */
void rv64_exec_set_hostfn(int idx, RvHostFn fn);

struct RvCpu {
    uint64_t x[32];     /* x0 is architecturally hardwired to zero; this executor enforces that on every write */
    uint64_t pc;
    uint8_t* mem;
    uint32_t mem_size;
};

/* Runs until pc == RV_EXEC_SENTINEL_RA, a step-count budget is exceeded,
 * or a fault occurs. Returns one of the RV_EXEC_* codes above. */
int rv64_exec_run(struct RvCpu* cpu, uint64_t max_steps);

const char* rv64_exec_strerror(int code);

#endif /* RV64_EXEC_H */
