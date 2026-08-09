CC = cc
CFLAGS = -Wall -Wextra -std=c11 -O2 -g

all: simi-asm simi-run simi-dis simi-jit-test simi-riscv-verify simi-arm-verify a64_dump a64-f0-test stress-atomics bench-net bench-corpus

simi-asm: simi_asm.c simi_obj.c simi_isa.h simi_obj.h
	$(CC) $(CFLAGS) -o $@ simi_asm.c simi_obj.c

simi-run: simi_interp.c simi_obj.c simi_isa.h simi_obj.h
	$(CC) $(CFLAGS) -o $@ simi_interp.c simi_obj.c

simi-dis: simi_disasm.c simi_obj.c simi_isa.h simi_obj.h
	$(CC) $(CFLAGS) -o $@ simi_disasm.c simi_obj.c

# Phase 3: portable x86-64 translator core + host JIT-execution test harness.
simi-jit-test: simi_jit_test.c simi_x86.c simi_x86.h
	$(CC) $(CFLAGS) -o $@ simi_jit_test.c simi_x86.c

# Phase 15 (plan Part II §8): the real-hardware concurrency stress harness —
# translates tests/stress_atomics.simi (entries main + consumer) and runs it
# under pthreads, where the A1 `lock cmpxchg`/`lock xadd` codegen contends on
# real CPU cores. Closes the gap the single-threaded four-way parity cannot:
# mutual exclusion and no lost updates under contention. In `all` so the build
# always refreshes it (the M2.27 stale-binary lesson).
stress-atomics: simi-asm stress_atomics.c simi_x86.c simi_x86.h tests/stress_atomics.simi
	./simi-asm tests/stress_atomics.simi tests/stress_atomics.tmo
	$(CC) $(CFLAGS) -pthread -o $@ stress_atomics.c simi_x86.c

# Phase 5: portable RV64 translator core + a small purpose-built RV64
# decoder/executor standing in for real hardware/QEMU (neither available
# in this project's dev sandbox — see simi_riscv_verify.c's top comment).
simi-riscv-verify: simi_riscv_verify.c simi_riscv.c simi_riscv.h rv64_exec.c rv64_exec.h
	$(CC) $(CFLAGS) -o $@ simi_riscv_verify.c simi_riscv.c rv64_exec.c

# M0 (Step 7 / AeroSLS-SIMI-ARM-Backend-Plan-v0.1.md): portable AArch64
# (A64) translator core + a small purpose-built A64 decoder/executor
# standing in for real hardware/QEMU (neither available in this sandbox —
# see simi_arm_verify.c's top comment and a64_exec.h).
simi-arm-verify: simi_arm_verify.c simi_arm.c simi_arm.h a64_exec.c a64_exec.h
	$(CC) $(CFLAGS) -o $@ simi_arm_verify.c simi_arm.c a64_exec.c

# M0 transcription-error net: a from-scratch Python A64 encoder re-derives
# the bit layout independently and checks every static word simi_arm.c
# emitted (see a64_enc_check.py's docstring). Catches the error class
# where encoder AND decoder agree on a wrong constant.
a64_dump: a64_dump.c simi_arm.c simi_arm.h
	$(CC) $(CFLAGS) -o $@ a64_dump.c simi_arm.c

# M2.71: translate-time micro-benchmark for the fold fixpoint's safety
# net — translates chain36 (the only net-firing input: the M2.60
# relaxation 2-cycle) N times at the shipped trip count (16, the tunable
# g_ar_net_trip) vs the M2.69-era 512, asserting the committed speedup
# floor (see bench_net.c's top comment). In `all` so the committed
# measurement always refreshes with the build (the M2.27 stale-binary
# lesson); ~0.5 s at the default 100 iters.
bench-net: simi-asm bench_net.c simi_arm.c simi_arm.h tests/jmpr_chain36.simi
	./simi-asm tests/jmpr_chain36.simi tests/jmpr_chain36.tmo
	$(CC) $(CFLAGS) -o $@ bench_net.c simi_arm.c
	./bench-net tests/jmpr_chain36.tmo 100

# M2.72: corpus-wide translate-cost bench — translates every fixture N
# times at the shipped trip count, recording per-instruction translate
# time and the fixpoint scan counts, as the regression guard for the
# fixpoint series beyond chain36 (bench-net pins the net-firing input;
# this pins the whole corpus). Assembles all fixtures on the spot;
# FAILS if the total scan count moves (deterministic) or the aggregate
# per-instruction time exceeds the committed ceiling (see bench_corpus.c
# and plan doc §10.154/10.155). In `all` with bench-net (~1 s total at
# the default 50 iters — the M2.27 stale-binary lesson).
bench-corpus: simi-asm bench_corpus.c simi_arm.c simi_arm.h
	$(CC) $(CFLAGS) -o $@ bench_corpus.c simi_arm.c
	# Assemble on the spot, but only when stale/missing — a fresh checkout
	# has no .tmo (gitignored), so the first build pays the full assembly;
	# incremental builds re-assemble only the fixtures whose .simi moved.
	for f in tests/*.simi; do tmo="$${f%.simi}.tmo"; \
	    if [ ! -f "$$tmo" ] || [ "$$f" -nt "$$tmo" ]; then \
	        ./simi-asm "$$f" "$$tmo" >/dev/null 2>&1 || exit 1; fi; done
	./bench-corpus tests/*.tmo 50

# F0 (Phase 10 float): hand-assembled float_ops.simi-semantics gate for
# the a64_exec.c float decode+execute work (see a64_f0_test.c's top
# comment). simi_arm.c can't emit float yet (F1) — this proves the
# decoder before the encoder exists. In `all` so the build always
# refreshes it (the M2.27 stale-binary lesson).
a64-f0-test: a64_f0_test.c a64_exec.c a64_exec.h
	$(CC) $(CFLAGS) -o $@ a64_f0_test.c a64_exec.c

a64-enc-check: simi-asm a64_dump
	./simi-asm tests/straight_line_bench.simi tests/straight_line_bench.tmo
	./simi-asm tests/loop_sum.simi tests/loop_sum.tmo
	./simi-asm tests/extra_ops.simi tests/extra_ops.tmo
	./simi-asm tests/float_ops.simi tests/float_ops.tmo
	./simi-asm tests/cas_simple.simi tests/cas_simple.tmo
	./simi-asm tests/atomic_add.simi tests/atomic_add.tmo
	./a64_dump tests/straight_line_bench.tmo > /tmp/slb.dump
	./a64_dump tests/loop_sum.tmo > /tmp/ls.dump
	./a64_dump tests/extra_ops.tmo > /tmp/eo.dump
	./a64_dump tests/float_ops.tmo > /tmp/fo.dump
	./a64_dump tests/cas_simple.tmo > /tmp/cs.dump
	./a64_dump tests/atomic_add.tmo > /tmp/aa.dump
	python3 a64_enc_check.py /tmp/slb.dump straight_line_bench
	python3 a64_enc_check.py /tmp/ls.dump loop_sum
	python3 a64_enc_check.py /tmp/eo.dump extra_ops
	python3 a64_enc_check.py /tmp/fo.dump float_ops
	python3 a64_enc_check.py /tmp/cs.dump cas_simple
	python3 a64_enc_check.py /tmp/aa.dump atomic_add

test: all
	./tests/run_tests.sh

test-native: all
	./tests/run_native_tests.sh

test-riscv: all
	./tests/run_riscv_tests.sh

test-arm: all
	./tests/run_arm_tests.sh

# M1 code-size gate: every ARM test must stay at or below its M0 emitted
# byte count (baselines recorded from git 1729f50, the committed M0
# translator). The allocator must never be a net regression on the parity
# corpus — see tests/size_gate_arm.sh's top comment.
size-gate-arm: simi-arm-verify simi-asm
	./tests/size_gate_arm.sh

clean:
	rm -f simi-asm simi-run simi-dis simi-jit-test simi-riscv-verify simi-arm-verify a64_dump bench-net bench-corpus tests/*.tmo

.PHONY: all test test-native test-riscv test-arm size-gate-arm stress-atomics bench-net bench-corpus clean
