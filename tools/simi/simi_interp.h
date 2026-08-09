/* simi_interp.h — the Phase 1 reference interpreter's run entry point,
 * extracted in M2.80 so bench_exec_interp.c can drive the interpreter
 * in-process (the same purpose-built-executor pattern as a64_exec.h /
 * rv64_exec.h — but this one IS the reference, not a stand-in for
 * hardware). simi-run's own main() is guarded by SIMI_INTERP_NO_MAIN
 * (see simi_interp.c) so the bench links simi_interp.c without a main
 * conflict. */
#ifndef SIMI_INTERP_H
#define SIMI_INTERP_H

#include "simi_obj.h"

/* Runs `obj` from `entry_pc` until a top-level RET, exactly as simi-run
 * does for a named entry. On success writes the result value (frame 0's
 * r0 at the top-level RET — the value simi-run prints) to *result_out
 * and the executed SIMI-instruction count to *steps_out, and returns 0.
 * Traps still exit(2) via die() inside simi_interp.c (as simi-run does).
 * Each call starts from a fresh frame stack and zeroed memory, so
 * repeated calls are independent and deterministic. */
int simi_interp_run(SimiObject obj, uint32_t entry_pc, long *steps_out, long long *result_out);

#endif /* SIMI_INTERP_H */
