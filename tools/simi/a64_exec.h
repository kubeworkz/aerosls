/*
 * a64_exec.h — M0 verification tool for the AArch64 (A64) backend, NOT
 * part of the translator itself.
 *
 * This sandbox has no AArch64 cross-compiler and no qemu-aarch64/
 * qemu-system-aarch64 (checked — no clang/llvm-mc, no root to install
 * anything; the same constraint Phase 5 documented for riscv64). Phase
 * 3's x86 verification method (mmap the translated bytes PROT_EXEC and
 * call them as a real function on the host CPU) is unavailable here
 * because the host CPU is itself x86-64 — there is no A64 execution
 * unit to hand the bytes to.
 *
 * So, exactly like rv64_exec.c: this is a small, purpose-built A64
 * instruction decoder+executor covering precisely the encodings
 * simi_arm.c emits (not a general AArch64 simulator — no exception
 * model, no MTE/PAuth/SVE/NEON, no AArch32, no traps beyond UDF being
 * reported as AR_EXEC_BAD_INSTR). It exists for the same reason the
 * other two executors exist: an independently-written reference that
 * actually executes the encoded bytes, so a bit-packing mistake in the
 * encoder shows up as a wrong answer instead of passing silent review.
 * See AeroSLS-SIMI-ISA-v0.1.md §12 for why this is weaker evidence than
 * Phase 3's real-hardware execution (a bug in both the encoder AND this
 * decoder that happens to agree wouldn't be caught) but still materially
 * stronger than static review alone.
 *
 * The M0 subset (per docs/AeroSLS-SIMI-ARM-Backend-Plan-v0.1.md §3):
 *   movz/movk, add/sub/subs (immediate and shifted-register, LSL only
 *   where shifted), and/eor/orr (shifted, incl. ORN for mvn), lslv/lsrv/
 *   asrv, mul/sdiv/udiv/msub, csel family (cset), ldr/str x + the typed
 *   byte/half/word/sign-extending forms, b/bl, cbz/cbnz, br/blr/ret,
 *   subs-based cmp + NZCV flags (the one piece of implicit machine state
 *   M0 must model — see simi_arm.c's emit_cmp() design note).
 */
#ifndef A64_EXEC_H
#define A64_EXEC_H

#include <stdint.h>

enum {
    AR_EXEC_OK = 0,          /* pc reached the sentinel link register */
    AR_EXEC_STEP_LIMIT,      /* exceeded the step budget — likely an infinite loop or decode bug */
    AR_EXEC_BAD_INSTR,       /* encoding this executor doesn't implement */
    AR_EXEC_MEM_FAULT,       /* load/store or fetch outside the guest memory buffer */
};

/* Guest sentinel: the caller sets x30 (LR) to this before starting
 * execution at the trampoline's offset. When a `br x30` (or `ret`)
 * transfers to this value, the trampoline's own final return has fired —
 * the "the top-level call has returned" signal a real hardware `bl`
 * return address would provide implicitly. Mirrors RV_EXEC_SENTINEL_RA
 * (rv64_exec.h). */
#define AR_EXEC_SENTINEL_LR 0xFFFFFFFFFFFFFFF0ull

/* v0.3 (Phase 6): host-callback sentinels — this executor's answer to
 * "how do you call a real function from simulated A64 code when there's
 * no real A64 CPU underneath it." simi_arm.c's RESOLVE/OBJSIZE/OBJTYPE
 * codegen materializes an ordinary-looking absolute address into a
 * register and does a real `blr xN` — from the encoder's point of view
 * that address is just an opaque uint64_t the caller supplied (see
 * simi_arm.h), exactly like x86's movabs+call and RV64's auipc+ld+jalr.
 * Here, this executor's BLR handling special-cases addresses in this
 * reserved range: instead of faulting trying to fetch instructions at a
 * fake address, it looks up a registered host C function by index, calls
 * it with x0 (the real A64 ABI arg0/return register) as both argument
 * and result register, and then behaves exactly as if that function had
 * executed and returned via `ret` — control resumes at the BLR's own
 * link address (pc+4), same observable effect, zero guest instructions
 * actually fetched at the sentinel.
 *
 * STRIDE is 1, deliberately NOT the 16 rv64_exec.h uses — RV64's stride
 * existed because a real RV64 `jalr` always clears the target's
 * least-significant bit, silently corrupting odd-indexed sentinels; A64
 * `br`/`blr` does no such bit-clearing (there is no compressed
 * instruction to hide behind), so the bug class that forced stride 16
 * cannot occur here. See AeroSLS-SIMI-ARM-Backend-Plan-v0.1.md §4. */
#define AR_EXEC_HOSTFN_BASE 0xFFFFFFFE00000000ull
#define AR_EXEC_HOSTFN_MASK 0xFFFFFFFF00000000ull
#define AR_EXEC_MAX_HOSTFNS 8
#define AR_EXEC_HOSTFN_STRIDE 1u

typedef uint64_t (*ArHostFn)(uint64_t arg);

/* Address to hand to simi_arm_translate() as rt_resolve_fn/rt_objsize_fn/
 * rt_objtype_fn for host-callback slot `idx` (0..AR_EXEC_MAX_HOSTFNS-1). */
static inline uint64_t a64_exec_hostfn_addr(int idx) {
    return AR_EXEC_HOSTFN_BASE + (uint64_t)(uint32_t)idx * AR_EXEC_HOSTFN_STRIDE;
}

/* Registers (or clears, with fn==0) the host function invoked when a BLR
 * targets a64_exec_hostfn_addr(idx). Must be called before a64_exec_run()
 * for any slot the translated code might actually call. */
void a64_exec_set_hostfn(int idx, ArHostFn fn);

struct A64Cpu {
    uint64_t x[32];     /* x0..x30 plus x31 = SP (A64: only add/sub-immediate
                         * and load/store treat 31 as SP; everywhere else 31
                         * is XZR — this executor applies that contextually) */
    uint64_t pc;
    /* NZCV condition flags — the one piece of implicit machine state this
     * subset must carry: simi_arm.c's CMP synthesis uses `cmp` (subs xzr)
     * + `cset` for all 10 SIMI relations, so the decoder must model the
     * flag writes of SUBS and the flag reads of the CSEL family. */
    uint8_t n, z, c, v;
    uint8_t* mem;
    uint32_t mem_size;
};

/* Runs until pc == AR_EXEC_SENTINEL_LR, a step-count budget is exceeded,
 * or a fault occurs. Returns one of the AR_EXEC_* codes above. */
int a64_exec_run(struct A64Cpu* cpu, uint64_t max_steps);

const char* a64_exec_strerror(int code);

#endif /* A64_EXEC_H */
