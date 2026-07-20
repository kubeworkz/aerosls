CC = cc
CFLAGS = -Wall -Wextra -std=c11 -O2 -g

all: simi-asm simi-run simi-dis simi-jit-test simi-riscv-verify

simi-asm: simi_asm.c simi_obj.c simi_isa.h simi_obj.h
	$(CC) $(CFLAGS) -o $@ simi_asm.c simi_obj.c

simi-run: simi_interp.c simi_obj.c simi_isa.h simi_obj.h
	$(CC) $(CFLAGS) -o $@ simi_interp.c simi_obj.c

simi-dis: simi_disasm.c simi_obj.c simi_isa.h simi_obj.h
	$(CC) $(CFLAGS) -o $@ simi_disasm.c simi_obj.c

# Phase 3: portable x86-64 translator core + host JIT-execution test harness.
simi-jit-test: simi_jit_test.c simi_x86.c simi_x86.h
	$(CC) $(CFLAGS) -o $@ simi_jit_test.c simi_x86.c

# Phase 5: portable RV64 translator core + a small purpose-built RV64
# decoder/executor standing in for real hardware/QEMU (neither available
# in this project's dev sandbox — see simi_riscv_verify.c's top comment).
simi-riscv-verify: simi_riscv_verify.c simi_riscv.c simi_riscv.h rv64_exec.c rv64_exec.h
	$(CC) $(CFLAGS) -o $@ simi_riscv_verify.c simi_riscv.c rv64_exec.c

test: all
	./tests/run_tests.sh

test-native: all
	./tests/run_native_tests.sh

test-riscv: all
	./tests/run_riscv_tests.sh

clean:
	rm -f simi-asm simi-run simi-dis simi-jit-test simi-riscv-verify tests/*.tmo

.PHONY: all test test-native test-riscv clean
