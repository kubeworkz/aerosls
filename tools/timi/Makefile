CC = cc
CFLAGS = -Wall -Wextra -std=c11 -O2 -g

all: timi-asm timi-run timi-dis timi-jit-test timi-riscv-verify

timi-asm: timi_asm.c timi_obj.c timi_isa.h timi_obj.h
	$(CC) $(CFLAGS) -o $@ timi_asm.c timi_obj.c

timi-run: timi_interp.c timi_obj.c timi_isa.h timi_obj.h
	$(CC) $(CFLAGS) -o $@ timi_interp.c timi_obj.c

timi-dis: timi_disasm.c timi_obj.c timi_isa.h timi_obj.h
	$(CC) $(CFLAGS) -o $@ timi_disasm.c timi_obj.c

# Phase 3: portable x86-64 translator core + host JIT-execution test harness.
timi-jit-test: timi_jit_test.c timi_x86.c timi_x86.h
	$(CC) $(CFLAGS) -o $@ timi_jit_test.c timi_x86.c

# Phase 5: portable RV64 translator core + a small purpose-built RV64
# decoder/executor standing in for real hardware/QEMU (neither available
# in this project's dev sandbox — see timi_riscv_verify.c's top comment).
timi-riscv-verify: timi_riscv_verify.c timi_riscv.c timi_riscv.h rv64_exec.c rv64_exec.h
	$(CC) $(CFLAGS) -o $@ timi_riscv_verify.c timi_riscv.c rv64_exec.c

test: all
	./tests/run_tests.sh

test-native: all
	./tests/run_native_tests.sh

test-riscv: all
	./tests/run_riscv_tests.sh

clean:
	rm -f timi-asm timi-run timi-dis timi-jit-test timi-riscv-verify tests/*.tmo

.PHONY: all test test-native test-riscv clean
