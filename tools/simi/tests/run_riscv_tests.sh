#!/usr/bin/env bash
# Phase 5 verification: assembles every tests/*.simi (Phase 1 host toolchain,
# unchanged), translates the resulting .tmo to RV64 machine code
# (simi_riscv.c), and executes it via simi-riscv-verify — a small
# purpose-built RV64 decoder+executor (rv64_exec.c), not real hardware or
# QEMU (neither available in this environment; see simi_riscv_verify.c's
# top comment and AeroSLS-SIMI-ISA-v0.1.md §12). Checks the result against
# each program's "Expected result:" comment, exactly like run_native_tests.sh
# does for the x86 target — same .tmo files, same expected values, proving
# the SAME unmodified SIMI object retargets correctly to a second ISA.
#
# mem_ops.simi is skipped for the same reason run_native_tests.sh skips it:
# address-0 is a Phase 1 interpreter-only convenience, not a legitimate
# pointer under either native target's r7 scratch-pointer convention.
#
# float_ops.simi RUNS here starting at F4 (Gap Remediation SIMI Phase 10
# completion): simi_riscv.c now emits the scalar F/D GP-bounce codegen
# (fadd/fsub/fmul/fdiv, feq/flt/fle, fmv) and rv64_exec.c decodes and
# executes it against its new f file, so the 17-check fixture executes on
# the RV64 engine and returns 15 — completing the four-way float parity
# (interp / x86 / RV64 / ARM all 15). The A0-era skip block was removed.
# The kernel-side sstatus.FS lazy-save (saving f0-f31 across context
# switches) remains Phase 9's RISC-V kernel wiring; the tools-side
# translator and executor are complete.
set -u
cd "$(dirname "$0")"
ASM=../simi-asm
VERIFY=../simi-riscv-verify

pass=0
fail=0
skip=0

for src in *.simi; do
    name="${src%.simi}"
    if [ "$name" = "mem_ops" ]; then
        echo "SKIP  $name (address-0 pointer is a Phase 1 interpreter-only convenience; see mem_ops_native)"
        skip=$((skip+1))
        continue
    fi
    expected=$(grep -oE 'Expected result: -?[0-9]+' "$src" | grep -oE -- '-?[0-9]+$')
    if [ -z "$expected" ]; then
        echo "SKIP  $name (no 'Expected result:' comment)"
        skip=$((skip+1))
        continue
    fi

    if ! "$ASM" "$src" "$name.tmo" 2>"$name.asm.log"; then
        echo "FAIL  $name (assembler error)"
        cat "$name.asm.log"
        fail=$((fail+1))
        continue
    fi

    # M2.76: --steps asserts the fixture's committed executed-instruction
    # count (bench_baselines_rv64.h — the same deterministic, machine-
    # independent tripwire M2.75 added to the ARM leg).
    if "$VERIFY" "$name.tmo" main "$expected" --steps; then
        pass=$((pass+1))
    else
        fail=$((fail+1))
    fi
done

# Phase 9g host-side tripwire: the SBI_SRST failure fallback in
# riscv_syscall_dispatch() (arch/riscv/trap_riscv.c). The REAL kernel file
# is compiled for the host with -DSIMI_HOST_TEST and a FAILING
# sbi_system_reset() stub (tools/simi/rv_syscall_exit_test.c); this asserts
# the fallback messages fired, the stub was reached, and dispatch never
# returned (the process terminated from inside rv_halt's host branch
# instead of falling through to main's "returned" marker).
if ../rv-syscall-exit-test >rv_exit.out 2>rv_exit.err; then
    if grep -q "\[SYSCALL\] SYS_SLS_EXIT, code=42" rv_exit.out \
       && grep -q "powering off via OpenSBI SBI_SRST" rv_exit.out \
       && grep -q "SBI_SRST unsupported or failed -- halting hart instead" rv_exit.out \
       && grep -q "sbi_system_reset() stub" rv_exit.err \
       && ! grep -q "dispatch returned" rv_exit.err; then
        echo "PASS  rv-syscall-exit (SBI_SRST failure fallback messages + halt)"
        pass=$((pass+1))
    else
        echo "FAIL  rv-syscall-exit (fallback messages / stub / halt not as expected)"
        cat rv_exit.out rv_exit.err
        fail=$((fail+1))
    fi
else
    rc=$?
    echo "FAIL  rv-syscall-exit (exit rc=$rc — dispatch crashed or did not halt)"
    cat rv_exit.out rv_exit.err
    fail=$((fail+1))
fi
rm -f rv_exit.out rv_exit.err

# The M-mode twin (rv-syscall-exit-test-m, compiled -DRISCV_MMODE): the
# bare-metal branch never calls the firmware, so the failing stub's marker
# must be ABSENT from stderr while the "no firmware to power off" message
# fires and dispatch still terminates inside rv_halt.
if ../rv-syscall-exit-test-m >rv_exitm.out 2>rv_exitm.err; then
    if grep -q "\[SYSCALL\] SYS_SLS_EXIT, code=42" rv_exitm.out \
       && grep -q "direct M-mode boot: no firmware to power off -- halting hart" rv_exitm.out \
       && ! grep -q "sbi_system_reset() stub" rv_exitm.err \
       && ! grep -q "dispatch returned" rv_exitm.err; then
        echo "PASS  rv-syscall-exit-m (M-mode no-firmware branch + halt, reset never attempted)"
        pass=$((pass+1))
    else
        echo "FAIL  rv-syscall-exit-m (M-mode branch messages / stub-absence / halt not as expected)"
        cat rv_exitm.out rv_exitm.err
        fail=$((fail+1))
    fi
else
    rc=$?
    echo "FAIL  rv-syscall-exit-m (exit rc=$rc — dispatch crashed or did not halt)"
    cat rv_exitm.out rv_exitm.err
    fail=$((fail+1))
fi
rm -f rv_exitm.out rv_exitm.err

# The unimplemented-syscall twin (rv-syscall-exit-test-u, compiled
# -DRV_TEST_A7=999): a7 is not RV_SYS_EXIT, so dispatch reports the
# unimplemented number and halts — the reset stub is never reached, so
# its marker must be ABSENT from stderr.
if ../rv-syscall-exit-test-u >rv_exitu.out 2>rv_exitu.err; then
    if grep -q "\[SYSCALL\] unimplemented syscall number 999" rv_exitu.out \
       && grep -q "halting hart" rv_exitu.out \
       && ! grep -q "sbi_system_reset() stub" rv_exitu.err \
       && ! grep -q "dispatch returned" rv_exitu.err; then
        echo "PASS  rv-syscall-exit-u (unimplemented-syscall branch + halt, reset never attempted)"
        pass=$((pass+1))
    else
        echo "FAIL  rv-syscall-exit-u (unimplemented-syscall message / stub-absence / halt not as expected)"
        cat rv_exitu.out rv_exitu.err
        fail=$((fail+1))
    fi
else
    rc=$?
    echo "FAIL  rv-syscall-exit-u (exit rc=$rc — dispatch crashed or did not halt)"
    cat rv_exitu.out rv_exitu.err
    fail=$((fail+1))
fi
rm -f rv_exitu.out rv_exitu.err

# Phase 9g trap-routing twin (rv-trap-test, -DSIMI_TEST_TRAP): drives
# riscv_trap_dispatch_common() with INJECTED cause/tval (x86 has no
# scause/mcause CSRs). Three modes, one scenario per process:
#   syscall    scause=3 (ebreak) + a7=RV_SYS_EXIT -> routes into the
#              S-mode syscall exit path (SRST attempted, fallback halt).
#   ecall      scause=9 (environment call) + a7=RV_SYS_EXIT -> the same
#              exit path through the code==9 branch.
#   unhandled  scause=2 (illegal instruction) -> [TRAP] unhandled branch.
#   interrupt  scause bit 63 set -> the interrupt stub, which RETURNS.
for mode in syscall ecall unhandled interrupt; do
    if ../rv-trap-test "$mode" >rv_trap_$mode.out 2>rv_trap_$mode.err; then
        case "$mode" in
        syscall)
            if grep -q "\[SYSCALL\] SYS_SLS_EXIT, code=42" rv_trap_syscall.out \
               && grep -q "SBI_SRST unsupported or failed -- halting hart instead" rv_trap_syscall.out \
               && grep -q "sbi_system_reset() stub" rv_trap_syscall.err \
               && ! grep -q "\[TRAP\] unhandled" rv_trap_syscall.out \
               && ! grep -q "no branch halted" rv_trap_syscall.err; then
                echo "PASS  rv-trap-test syscall (scause=3 + a7=RV_SYS_EXIT -> syscall path)"
                pass=$((pass+1))
            else
                echo "FAIL  rv-trap-test syscall (routing / messages not as expected)"
                cat rv_trap_syscall.out rv_trap_syscall.err
                fail=$((fail+1))
            fi ;;
        ecall)
            if grep -q "\[SYSCALL\] SYS_SLS_EXIT, code=42" rv_trap_ecall.out \
               && grep -q "SBI_SRST unsupported or failed -- halting hart instead" rv_trap_ecall.out \
               && grep -q "sbi_system_reset() stub" rv_trap_ecall.err \
               && ! grep -q "\[TRAP\] unhandled" rv_trap_ecall.out \
               && ! grep -q "no branch halted" rv_trap_ecall.err; then
                echo "PASS  rv-trap-test ecall (scause=9 + a7=RV_SYS_EXIT -> code==9 syscall path)"
                pass=$((pass+1))
            else
                echo "FAIL  rv-trap-test ecall (routing / messages not as expected)"
                cat rv_trap_ecall.out rv_trap_ecall.err
                fail=$((fail+1))
            fi ;;
        unhandled)
            if grep -q "\[TRAP\] unhandled exception, scause=2, stval=4660, sepc=2149584896" rv_trap_unhandled.out \
               && grep -q "halting hart" rv_trap_unhandled.out \
               && ! grep -q "\[SYSCALL\]" rv_trap_unhandled.out \
               && ! grep -q "sbi_system_reset() stub" rv_trap_unhandled.err \
               && ! grep -q "no branch halted" rv_trap_unhandled.err; then
                echo "PASS  rv-trap-test unhandled (scause=2 -> [TRAP] unhandled branch)"
                pass=$((pass+1))
            else
                echo "FAIL  rv-trap-test unhandled (routing / messages not as expected)"
                cat rv_trap_unhandled.out rv_trap_unhandled.err
                fail=$((fail+1))
            fi ;;
        interrupt)
            if grep -q "interrupt stub fired" rv_trap_interrupt.err \
               && grep -q "interrupt path returned as expected" rv_trap_interrupt.err \
               && ! grep -q "\[SYSCALL\]" rv_trap_interrupt.out \
               && ! grep -q "\[TRAP\]" rv_trap_interrupt.out; then
                echo "PASS  rv-trap-test interrupt (bit 63 -> interrupt stub, returns)"
                pass=$((pass+1))
            else
                echo "FAIL  rv-trap-test interrupt (routing / messages not as expected)"
                cat rv_trap_interrupt.out rv_trap_interrupt.err
                fail=$((fail+1))
            fi ;;
        esac
    else
        rc=$?
        echo "FAIL  rv-trap-test $mode (exit rc=$rc — dispatch crashed or behaved wrongly)"
        cat rv_trap_$mode.out rv_trap_$mode.err
        fail=$((fail+1))
    fi
done
rm -f rv_trap_*.out rv_trap_*.err

# Phase 9h (ISA doc §16): the bare-metal trap-entry fixture. The REAL
# entry assembly (arch/riscv/trap_riscv.S) cannot run on the x86 host,
# so the fixture links it into a tiny freestanding M-mode payload
# (tests/rv_trap_entry/, built on demand by `make rv-trap-entry-test`)
# and boots it under qemu-system-riscv64 -bios none (single hart). It
# pins the entry's save/restore round trip -- including the restore+
# return half no kernel boot exercises (every real trap halts or powers
# off). The fixture halts deliberately, so QEMU is killed by the timeout
# (rc=124); the assertions are the serial messages. Skipped when the
# cross toolchain or system QEMU is absent.
if command -v qemu-system-riscv64 >/dev/null 2>&1 && command -v riscv64-unknown-elf-gcc >/dev/null 2>&1; then
    if make -C .. rv-trap-entry-test >/dev/null 2>&1; then
        timeout 30 qemu-system-riscv64 -M virt -m 128M -smp 1 -bios none -kernel ../rv_trap_entry.elf -nographic >rv_trap_entry.out 2>&1
        rc=$?
        if [ "$rc" -eq 124 ] \
           && grep -q "\[FIXTURE\] dispatcher: frame save verified" rv_trap_entry.out \
           && grep -q "\[FIXTURE\] PASS: entry save/restore round-trip verified" rv_trap_entry.out \
           && ! grep -q "\[FIXTURE\] FAIL" rv_trap_entry.out; then
            echo "PASS  rv-trap-entry (real entry save/restore round-trip under system QEMU)"
            pass=$((pass+1))
        else
            echo "FAIL  rv-trap-entry (rc=$rc — serial output not as expected)"
            cat rv_trap_entry.out
            fail=$((fail+1))
        fi
    else
        echo "SKIP  rv-trap-entry (fixture build failed — check the cross toolchain)"
        skip=$((skip+1))
    fi
    rm -f rv_trap_entry.out
else
    echo "SKIP  rv-trap-entry (no qemu-system-riscv64 / riscv64-unknown-elf-gcc)"
    skip=$((skip+1))
fi

# Phase 9h twin: the SAME fixture sources built with -DFIXTURE_SMODE and
# booted under OpenSBI (default -bios) at 0x80200000 -- the sepc/sret
# entry (riscv_trap_entry, stvec) and the SBI_DBCN console, i.e. the
# exact mode the S-mode kernel runs in, so the S/M entry delta is
# exercised for real instead of argued from CSR names. Same assertions,
# mode-specific markers.
if command -v qemu-system-riscv64 >/dev/null 2>&1 && command -v riscv64-unknown-elf-gcc >/dev/null 2>&1; then
    if make -C .. rv-trap-entry-test-s >/dev/null 2>&1; then
        timeout 30 qemu-system-riscv64 -M virt -m 128M -smp 1 -kernel ../rv_trap_entry_s.elf -nographic >rv_trap_entry_s.out 2>&1
        rc=$?
        if [ "$rc" -eq 124 ] \
           && grep -q "\[FIXTURE\] dispatcher (S-mode): frame save verified" rv_trap_entry_s.out \
           && grep -q "\[FIXTURE\] PASS: S-mode entry save/restore round-trip verified" rv_trap_entry_s.out \
           && ! grep -q "\[FIXTURE\] FAIL" rv_trap_entry_s.out; then
            echo "PASS  rv-trap-entry-s (S-mode entry save/restore round-trip under OpenSBI)"
            pass=$((pass+1))
        else
            echo "FAIL  rv-trap-entry-s (rc=$rc — serial output not as expected)"
            cat rv_trap_entry_s.out
            fail=$((fail+1))
        fi
    else
        echo "SKIP  rv-trap-entry-s (fixture build failed — check the cross toolchain)"
        skip=$((skip+1))
    fi
    rm -f rv_trap_entry_s.out
else
    echo "SKIP  rv-trap-entry-s (no qemu-system-riscv64 / riscv64-unknown-elf-gcc)"
    skip=$((skip+1))
fi

echo ""
echo "$pass passed, $fail failed, $skip skipped"
[ "$fail" -eq 0 ]
