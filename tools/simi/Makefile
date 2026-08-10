# NOTE (cleanup M2.83): the build outputs in this directory (simi-asm,
# simi-run, simi-dis, simi-jit-test, simi-riscv-verify, simi-arm-verify,
# a64_dump, a64-f0-test, stress-atomics, bench-*, cross-isa, cross-size,
# tests/*.tmo) are NOT tracked — a fresh checkout has none of them. Run
# `make` (or `make clean && make` if you want a guaranteed-fresh build)
# before `make test` or any bench. See plan doc §10.177.
CC = cc
CFLAGS = -Wall -Wextra -std=c11 -O2 -g

all: simi-asm simi-run simi-dis simi-jit-test simi-riscv-verify simi-arm-verify a64_dump a64-f0-test stress-atomics bench-net bench-corpus bench-exec bench-exec-rv64 bench-exec-interp cross-isa cross-size rv-syscall-exit-test rv-syscall-exit-test-m rv-syscall-exit-test-u rv-trap-test

simi-asm: simi_asm.c simi_obj.c simi_isa.h simi_obj.h
	$(CC) $(CFLAGS) -o $@ simi_asm.c simi_obj.c

simi-run: simi_interp.c simi_obj.c simi_isa.h simi_obj.h simi_interp.h bench_baselines_interp.h
	$(CC) $(CFLAGS) -o $@ simi_interp.c simi_obj.c

simi-dis: simi_disasm.c simi_obj.c simi_isa.h simi_obj.h
	$(CC) $(CFLAGS) -o $@ simi_disasm.c simi_obj.c

# Phase 3: portable x86-64 translator core + host JIT-execution test harness.
simi-jit-test: simi_jit_test.c simi_x86.c simi_x86.h bench_baselines_x86.h
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
simi-riscv-verify: simi_riscv_verify.c simi_riscv.c simi_riscv.h rv64_exec.c rv64_exec.h bench_baselines_rv64.h
	$(CC) $(CFLAGS) -o $@ simi_riscv_verify.c simi_riscv.c rv64_exec.c

# Phase 9g host-side tripwire: riscv_syscall_dispatch()'s SBI_SRST failure
# fallback (arch/riscv/trap_riscv.c). The REAL kernel file is compiled for
# the host with -DSIMI_HOST_TEST (RISC-V-only CSR/wfi pieces become host
# stubs, see trap_riscv.c's top comment) and linked against a FAILING
# sbi_system_reset() stub; tests/run_riscv_tests.sh asserts the fallback
# messages and that dispatch never returned. In `all` so the build always
# refreshes it (the M2.27 stale-binary lesson).
rv-syscall-exit-test: rv_syscall_exit_test.c ../../arch/riscv/trap_riscv.c ../../arch/riscv/trap_riscv.h ../../arch/riscv/sbi.h
	$(CC) $(CFLAGS) -DSIMI_HOST_TEST -o $@ rv_syscall_exit_test.c ../../arch/riscv/trap_riscv.c

# The M-mode twin: the same test source compiled with -DRISCV_MMODE, so
# dispatch takes the bare-metal branch ("no firmware to power off"); the
# runner additionally asserts sbi_system_reset() was NEVER called (the
# failing stub's marker must be absent from stderr). Pins both exit
# branches of RV_SYS_EXIT.
rv-syscall-exit-test-m: rv_syscall_exit_test.c ../../arch/riscv/trap_riscv.c ../../arch/riscv/trap_riscv.h ../../arch/riscv/sbi.h
	$(CC) $(CFLAGS) -DSIMI_HOST_TEST -DRISCV_MMODE -o $@ rv_syscall_exit_test.c ../../arch/riscv/trap_riscv.c

# The unimplemented-syscall twin: the same source with RV_TEST_A7
# overridden to a bogus number, so dispatch takes the "unimplemented
# syscall number N -- halting hart." branch (the reset stub is never
# reached). Pins the last untested branch of riscv_syscall_dispatch.
rv-syscall-exit-test-u: rv_syscall_exit_test.c ../../arch/riscv/trap_riscv.c ../../arch/riscv/trap_riscv.h ../../arch/riscv/sbi.h
	$(CC) $(CFLAGS) -DSIMI_HOST_TEST -DRV_TEST_A7=999 -o $@ rv_syscall_exit_test.c ../../arch/riscv/trap_riscv.c

# The trap-routing twin (rv-trap-test): drives riscv_trap_dispatch_common()
# -- now always-compiled so this build can reach it -- with INJECTED
# cause/tval (x86 has no scause/mcause CSRs), four modes via argv:
# syscall (scause=3 + a7=RV_SYS_EXIT), ecall (scause=9), unhandled
# (scause=2), interrupt (bit 63 set, the one branch that returns). See
# the SIMI_TEST_TRAP main in rv_syscall_exit_test.c.
rv-trap-test: rv_syscall_exit_test.c ../../arch/riscv/trap_riscv.c ../../arch/riscv/trap_riscv.h ../../arch/riscv/sbi.h
	$(CC) $(CFLAGS) -DSIMI_HOST_TEST -DSIMI_TEST_TRAP -o $@ rv_syscall_exit_test.c ../../arch/riscv/trap_riscv.c

# Phase 9h (ISA doc §16): the bare-metal trap-entry fixture. The REAL
# entry assembly (../../arch/riscv/trap_riscv.S) can't be assembled or
# executed on the x86 host, so this links it with a tiny freestanding
# M-mode test driver (tests/rv_trap_entry/) and tests/run_riscv_tests.sh
# boots the ELF under qemu-system-riscv64 -bios none. Pins the entry's
# save/restore round trip -- including the restore+return half no kernel
# boot exercises (every real trap halts or powers off). Two builds of the
# SAME fixture sources: the default is the M-mode bare-metal payload
# (-bios none, mtvec/riscv_trap_entry_m, raw 16550 console, linked at
# 0x80000000); rv-trap-entry-test-s compiles the driver + C with
# -DFIXTURE_SMODE (stvec/riscv_trap_entry under OpenSBI, SBI_DBCN
# console, linked at 0x80200000 -- the S-mode kernel's link address).
# trap_riscv.S itself is mode-agnostic (it emits BOTH entries), so the
# asm object is shared. NOT in `all`: requires the RISC-V cross
# toolchain; the runner builds it on demand and skips when the
# toolchain/QEMU are absent (so the runner's build keeps it fresh -- the
# M2.27 stale-binary lesson).
RV_CC ?= riscv64-unknown-elf-gcc
RV_LD ?= riscv64-unknown-elf-ld
rv_trap_entry.o: tests/rv_trap_entry/entry_test.c
	$(RV_CC) -ffreestanding -O2 -Wall -Wextra -mcmodel=medany -march=rv64gcv -mabi=lp64d -mno-relax -msmall-data-limit=0 -ffunction-sections -fdata-sections -c tests/rv_trap_entry/entry_test.c -o $@
rv_trap_entry_driver.o: tests/rv_trap_entry/entry_test.S
	$(RV_CC) -ffreestanding -O2 -Wall -Wextra -mcmodel=medany -march=rv64gcv -mabi=lp64d -mno-relax -ffunction-sections -fdata-sections -c tests/rv_trap_entry/entry_test.S -o $@
rv_trap_entry_asm.o: ../../arch/riscv/trap_riscv.S
	$(RV_CC) -ffreestanding -O2 -Wall -Wextra -mcmodel=medany -march=rv64gcv -mabi=lp64d -mno-relax -ffunction-sections -fdata-sections -c ../../arch/riscv/trap_riscv.S -o $@
rv-trap-entry-test: rv_trap_entry.o rv_trap_entry_driver.o rv_trap_entry_asm.o
	$(RV_LD) -T tests/rv_trap_entry/linker_fixture.ld -nostdlib --gc-sections rv_trap_entry_driver.o rv_trap_entry.o rv_trap_entry_asm.o -o rv_trap_entry.elf
rv_trap_entry_s.o: tests/rv_trap_entry/entry_test.c
	$(RV_CC) -ffreestanding -O2 -Wall -Wextra -mcmodel=medany -march=rv64gcv -mabi=lp64d -mno-relax -msmall-data-limit=0 -ffunction-sections -fdata-sections -DFIXTURE_SMODE -c tests/rv_trap_entry/entry_test.c -o $@
rv_trap_entry_driver_s.o: tests/rv_trap_entry/entry_test.S
	$(RV_CC) -ffreestanding -O2 -Wall -Wextra -mcmodel=medany -march=rv64gcv -mabi=lp64d -mno-relax -ffunction-sections -fdata-sections -DFIXTURE_SMODE -c tests/rv_trap_entry/entry_test.S -o $@
rv-trap-entry-test-s: rv_trap_entry_s.o rv_trap_entry_driver_s.o rv_trap_entry_asm.o
	$(RV_LD) -T tests/rv_trap_entry/linker_fixture_s.ld -nostdlib --gc-sections rv_trap_entry_driver_s.o rv_trap_entry_s.o rv_trap_entry_asm.o -o rv_trap_entry_s.elf
# The interrupt twin: S-mode build triggered by a supervisor software
# interrupt (bit-63 scause) instead of ebreak, proving the sepc/sret
# restore path when the dispatcher does NOT advance the exception PC.
rv_trap_entry_si.o: tests/rv_trap_entry/entry_test.c
	$(RV_CC) -ffreestanding -O2 -Wall -Wextra -mcmodel=medany -march=rv64gcv -mabi=lp64d -mno-relax -msmall-data-limit=0 -ffunction-sections -fdata-sections -DFIXTURE_SMODE -DFIXTURE_SMODE_INTR -c tests/rv_trap_entry/entry_test.c -o $@
rv_trap_entry_driver_si.o: tests/rv_trap_entry/entry_test.S
	$(RV_CC) -ffreestanding -O2 -Wall -Wextra -mcmodel=medany -march=rv64gcv -mabi=lp64d -mno-relax -ffunction-sections -fdata-sections -DFIXTURE_SMODE -DFIXTURE_SMODE_INTR -c tests/rv_trap_entry/entry_test.S -o $@
rv-trap-entry-test-si: rv_trap_entry_si.o rv_trap_entry_driver_si.o rv_trap_entry_asm.o
	$(RV_LD) -T tests/rv_trap_entry/linker_fixture_s.ld -nostdlib --gc-sections rv_trap_entry_driver_si.o rv_trap_entry_si.o rv_trap_entry_asm.o -o rv_trap_entry_si.elf
# The timer twin: S-mode build triggered by a supervisor TIMER interrupt
# (STIP, armed via SBI_SET_TIMER through the ACLINT/OpenSBI) instead of
# ebreak — the device-driven interrupt path (level-pending, not
# software-solicited). Same un-advanced-sepc restore assertion as the SI
# twin; the stub acknowledges by moving mtimecmp to the far future.
rv_trap_entry_t.o: tests/rv_trap_entry/entry_test.c
	$(RV_CC) -ffreestanding -O2 -Wall -Wextra -mcmodel=medany -march=rv64gcv -mabi=lp64d -mno-relax -msmall-data-limit=0 -ffunction-sections -fdata-sections -DFIXTURE_SMODE -DFIXTURE_SMODE_TIMER -c tests/rv_trap_entry/entry_test.c -o $@
rv_trap_entry_driver_t.o: tests/rv_trap_entry/entry_test.S
	$(RV_CC) -ffreestanding -O2 -Wall -Wextra -mcmodel=medany -march=rv64gcv -mabi=lp64d -mno-relax -ffunction-sections -fdata-sections -DFIXTURE_SMODE -DFIXTURE_SMODE_TIMER -c tests/rv_trap_entry/entry_test.S -o $@
rv-trap-entry-test-t: rv_trap_entry_t.o rv_trap_entry_driver_t.o rv_trap_entry_asm.o
	$(RV_LD) -T tests/rv_trap_entry/linker_fixture_s.ld -nostdlib --gc-sections rv_trap_entry_driver_t.o rv_trap_entry_t.o rv_trap_entry_asm.o -o rv_trap_entry_t.elf
# The external-interrupt twin: S-mode build triggered by a supervisor
# EXTERNAL interrupt (SEIP) — the 16550 UART's THRE line through the
# PLIC/OpenSBI, the device-driven path a real peripheral takes. Same
# un-advanced-sepc restore assertion as the SI/TIMER twins; the stub
# acknowledges by clearing the UART IER bit and completing the PLIC
# claim.
rv_trap_entry_seip.o: tests/rv_trap_entry/entry_test.c
	$(RV_CC) -ffreestanding -O2 -Wall -Wextra -mcmodel=medany -march=rv64gcv -mabi=lp64d -mno-relax -msmall-data-limit=0 -ffunction-sections -fdata-sections -DFIXTURE_SMODE -DFIXTURE_SMODE_SEIP -c tests/rv_trap_entry/entry_test.c -o $@
rv_trap_entry_driver_seip.o: tests/rv_trap_entry/entry_test.S
	$(RV_CC) -ffreestanding -O2 -Wall -Wextra -mcmodel=medany -march=rv64gcv -mabi=lp64d -mno-relax -ffunction-sections -fdata-sections -DFIXTURE_SMODE -DFIXTURE_SMODE_SEIP -c tests/rv_trap_entry/entry_test.S -o $@
rv-trap-entry-test-seip: rv_trap_entry_seip.o rv_trap_entry_driver_seip.o rv_trap_entry_asm.o
	$(RV_LD) -T tests/rv_trap_entry/linker_fixture_s.ld -nostdlib --gc-sections rv_trap_entry_driver_seip.o rv_trap_entry_seip.o rv_trap_entry_asm.o -o rv_trap_entry_seip.elf

# M0 (Step 7 / AeroSLS-SIMI-ARM-Backend-Plan-v0.1.md): portable AArch64
# (A64) translator core + a small purpose-built A64 decoder/executor
# standing in for real hardware/QEMU (neither available in this sandbox —
# see simi_arm_verify.c's top comment and a64_exec.h).
simi-arm-verify: simi_arm_verify.c simi_arm.c simi_arm.h a64_exec.c a64_exec.h bench_baselines.h
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

# M2.72/M2.73: corpus-wide translate-cost bench — translates every
# fixture N times at the shipped trip count, as the regression guard for
# the fixpoint series beyond chain36 (bench-net pins the net-firing
# input; this pins the whole corpus). M2.73 committed PER-ROW baselines
# (exact scan counts + us/instr at a 3x wall-clock margin) in
# bench_corpus.c's BASELINES table — a fixture that changes convergence
# or grows its translate cost 3x+ FAILS its row (see bench_corpus.c's
# top comment and plan doc §10.156/10.157). N=500 for per-row timing
# stability. Assembles fixtures on the spot only when stale/missing (a
# fresh checkout pays the full assembly once). In `all` with bench-net
# (~2-3 s total — the M2.27 stale-binary lesson).
bench-corpus: simi-asm bench_corpus.c simi_arm.c simi_arm.h
	$(CC) $(CFLAGS) -o $@ bench_corpus.c simi_arm.c
	# Assemble on the spot, but only when stale/missing — a fresh checkout
	# has no .tmo (gitignored), so the first build pays the full assembly;
	# incremental builds re-assemble only the fixtures whose .simi moved.
	for f in tests/*.simi; do tmo="$${f%.simi}.tmo"; \
	    if [ ! -f "$$tmo" ] || [ "$$f" -nt "$$tmo" ]; then \
	        ./simi-asm "$$f" "$$tmo" >/dev/null 2>&1 || exit 1; fi; done
	./bench-corpus tests/*.tmo 500

# M2.74: corpus-wide EXECUTION-cost bench — the runtime-side counterpart
# of bench-corpus. Each fixture is translated once, then executed N times
# through a64_exec.c (the same purpose-built A64 decoder/executor the
# four-way parity uses) with the same mock host functions and the expected
# result parsed from the .simi comment. COMMITTED PER-ROW BASELINES in
# bench_baselines.h — the single source of truth shared with the
# simi-arm-verify --steps check (M2.75): exact per-fixture steps
# (a64_exec's per-instruction counter — deterministic, machine-
# independent) + ns/call at a 4x wall-clock margin. Re-measure
# deliberately with
# BENCH_EXEC_MEASURE=1 ./bench-exec tests/*.simi 500 (see bench_exec.c's
# top comment and plan doc §10.158/10.159). In `all` with bench-corpus
# (~1-2 s total).
bench-exec: simi-asm bench_exec.c simi_arm.c simi_arm.h a64_exec.c a64_exec.h bench_baselines.h
	$(CC) $(CFLAGS) -o $@ bench_exec.c simi_arm.c a64_exec.c
	# Same assemble-on-the-spot loop as bench-corpus (stale/missing only).
	for f in tests/*.simi; do tmo="$${f%.simi}.tmo"; \
	    if [ ! -f "$$tmo" ] || [ "$$f" -nt "$$tmo" ]; then \
	        ./simi-asm "$$f" "$$tmo" >/dev/null 2>&1 || exit 1; fi; done
	./bench-exec tests/*.simi 500

# M2.77: corpus-wide RV64 EXECUTION-cost bench — the symmetric counterpart
# of bench-exec, driving rv64_exec.c instead of a64_exec. COMMITTED
# PER-ROW BASELINES in bench_baselines_rv64.h (the single source of truth
# shared with the simi-riscv-verify --steps check): exact per-fixture
# steps + ns/call at a 4x wall-clock margin. Re-measure deliberately with
# RV64_BENCH_MEASURE=1 ./bench-exec-rv64 <tests/ fixtures> 500 (see
# bench_exec_rv64.c's top comment and plan doc §10.164/10.165). In `all`
# with the other benches (~1-2 s total).
bench-exec-rv64: simi-asm bench_exec_rv64.c simi_riscv.c simi_riscv.h rv64_exec.c rv64_exec.h bench_baselines_rv64.h
	$(CC) $(CFLAGS) -o $@ bench_exec_rv64.c simi_riscv.c rv64_exec.c
	# Same assemble-on-the-spot loop as bench-corpus (stale/missing only).
	for f in tests/*.simi; do tmo="$${f%.simi}.tmo"; \
	    if [ ! -f "$$tmo" ] || [ "$$f" -nt "$$tmo" ]; then \
	        ./simi-asm "$$f" "$$tmo" >/dev/null 2>&1 || exit 1; fi; done
	./bench-exec-rv64 tests/*.simi 500

# M2.80: corpus-wide INTERPRETER execution-cost bench — the last leg of
# the four-way measurement story. Each fixture is read once (the
# interpreter doesn't translate — simi_obj_read loads the .tmo directly),
# then executed N times through simi_interp_run() — the exact run loop
# simi-run uses, extracted from its main() in M2.80 and driven in-process
# (simi_interp.c is linked with -DSIMI_INTERP_NO_MAIN so the bench's own
# main links). COMMITTED PER-ROW BASELINES in bench_baselines_interp.h:
# exact per-fixture SIMI-instruction steps + ns/call at a 4x wall-clock
# margin — the ground-truth work the translated legs' counts compare
# against. Fixture set is run_tests.sh's — 97 rows (incl. mem_ops.simi;
# jmpr_oob/cap_forge_debug skipped — no Expected result). Re-measure
# deliberately with
# SIMI_BENCH_MEASURE=1 ./bench-exec-interp <tests/ fixtures> 500 (see
# bench_exec_interp.c's top comment and plan doc §10.170/10.171). In
# `all` with the other benches (~1-2 s total).
bench-exec-interp: simi-asm bench_exec_interp.c simi_interp.c simi_obj.c simi_interp.h simi_obj.h bench_baselines_interp.h
	$(CC) $(CFLAGS) -DSIMI_INTERP_NO_MAIN -o $@ bench_exec_interp.c simi_interp.c simi_obj.c
	# Same assemble-on-the-spot loop as bench-corpus (stale/missing only).
	for f in tests/*.simi; do tmo="$${f%.simi}.tmo"; \
	    if [ ! -f "$$tmo" ] || [ "$$f" -nt "$$tmo" ]; then \
	        ./simi-asm "$$f" "$$tmo" >/dev/null 2>&1 || exit 1; fi; done
	./bench-exec-interp tests/*.simi 500

# M2.78/M2.82: cross-ISA execution-work report — the derived view over
# the committed per-fixture step counts of all three executors (A64 /
# RV64 / the interpreter, each pinned exactly by its parity tripwire):
# per-fixture A64-vs-RV64 ratios sorted by density, the M2.82 per-fixture
# ISA/interp EXPANSION table (how many translated instructions one SIMI
# instruction costs per fixture), the aggregate ratio and expansion
# factors, and consistency gates (no orphan rows across the three tables
# — mem_ops.simi is the deliberate interpreter-only fixture; the
# aggregate ratio and both expansion factors within +/-25% committed
# bands around the measured 1.0674 / ~13.55 / ~12.70). Pure header
# computation — no execution, instant. In `all` so the three tables stay
# honest on every build (see cross_isa.c's top comment and plan doc
# §10.166/10.167 and §10.174/10.175).
cross-isa: cross_isa.c bench_baselines.h bench_baselines_rv64.h bench_baselines_interp.h
	$(CC) $(CFLAGS) -o $@ cross_isa.c
	./cross-isa

# M2.79: cross-ISA emitted-size report — the code-density dimension.
# A64 side: the CURRENT emitted bytes measured live by translating each
# fixture with the real simi_arm.c (the size gate's M1 mechanism — no
# execution). x86 side: the COMMITTED current bytes in
# bench_baselines_x86.h (the simi-jit-test --bytes table). Prints the
# per-fixture a64/x86 ratio table sorted by density, the totals and the
# aggregate ratio, and gates on no-orphans + the aggregate within a
# +/-25% committed band (see cross_size.c's top comment and plan doc
# §10.168/10.169). In `all` with the other checks (~1 s).
cross-size: simi-asm cross_size.c simi_arm.c simi_arm.h bench_baselines_x86.h
	$(CC) $(CFLAGS) -o $@ cross_size.c simi_arm.c
	# Same assemble-on-the-spot loop as bench-corpus (stale/missing only).
	for f in tests/*.simi; do tmo="$${f%.simi}.tmo"; \
	    if [ ! -f "$$tmo" ] || [ "$$f" -nt "$$tmo" ]; then \
	        ./simi-asm "$$f" "$$tmo" >/dev/null 2>&1 || exit 1; fi; done
	./cross-size tests/*.simi

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
	rm -f simi-asm simi-run simi-dis simi-jit-test simi-riscv-verify simi-arm-verify a64_dump bench-net bench-corpus bench-exec bench-exec-rv64 bench-exec-interp cross-isa cross-size rv-syscall-exit-test rv-syscall-exit-test-m rv-syscall-exit-test-u rv-trap-test rv_trap_entry.elf rv_trap_entry.o rv_trap_entry_driver.o rv_trap_entry_asm.o rv_trap_entry_s.elf rv_trap_entry_s.o rv_trap_entry_driver_s.o rv_trap_entry_si.elf rv_trap_entry_si.o rv_trap_entry_driver_si.o rv_trap_entry_t.elf rv_trap_entry_t.o rv_trap_entry_driver_t.o rv_trap_entry_seip.elf rv_trap_entry_seip.o rv_trap_entry_driver_seip.o tests/*.tmo

.PHONY: all test test-native test-riscv test-arm size-gate-arm stress-atomics bench-net bench-corpus bench-exec bench-exec-rv64 bench-exec-interp cross-isa cross-size rv-trap-entry-test rv-trap-entry-test-s rv-trap-entry-test-si rv-trap-entry-test-t rv-trap-entry-test-seip clean
