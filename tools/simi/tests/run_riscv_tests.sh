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
#   unhandled  scause=2 (illegal instruction) -> [TRAP] unhandled branch.
#   interrupt  scause bit 63 set -> the interrupt stub, which RETURNS.
for mode in syscall unhandled interrupt; do
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

echo ""
echo "$pass passed, $fail failed, $skip skipped"
[ "$fail" -eq 0 ]
