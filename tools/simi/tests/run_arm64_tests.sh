#!/usr/bin/env bash
# M3 verification (docs/AeroSLS-SIMI-ARM-Backend-Plan-v0.1.md §6): the
# REAL AArch64 execution proof. simi-arm-verify (run_arm_tests.sh) runs
# the translated A64 words through a64_exec.c, a purpose-built decoder —
# but a decoder that AGREES with a wrong encoder passes the four-way
# parity invisibly, and A64 architectural rules a64_exec is documented
# laxer about (SP 16-byte alignment, 4-byte branch-target alignment) are
# never checked at all. This runner instead cross-compiles
# simi_arm_jit.c (aarch64-linux-gnu-gcc -static — the SAME simi_arm.c
# translator) and EXECUTES every tests/*.simi on a real A64
# implementation, qemu-aarch64 user-mode: the emitted words are mmap'd
# executable and branched into for real, with the result checked against
# each program's "Expected result:" comment — exactly like
# simi_jit_test.c does for x86-64 on the host CPU.
#
# M3 is the "optional, environment-dependent" milestone: with no aarch64
# toolchain or qemu-aarch64 this runner skips cleanly (the same pattern
# as the riscv checks when qemu-system-riscv64 is absent). The
# arm64-guards CI job installs the toolchain, so there the run must be
# all-pass.
#
# Same skip set as run_arm_tests.sh: mem_ops.simi (address-0 is a Phase 1
# interpreter-only convenience) and any fixture with no "Expected
# result:" comment (jmpr_oob self-skips — its whole point is that
# execution must NOT produce one).
set -u
cd "$(dirname "$0")"
ASM=../simi-asm
JIT=../simi-arm-jit

# ─── Native aarch64 hosts must NOT skip ────────────────────────────────────
# The checks below were written for cross-compiling from x86: they require
# aarch64-linux-gnu-gcc and qemu-aarch64. On a machine that IS aarch64 both
# are absent for good reason -- the cross-compiler is redundant when gcc
# targets A64 natively, and qemu-aarch64 emulates A64 on x86, which is
# pointless here. So this runner used to print SKIP and exit 0 on real ARM
# silicon: green, having tested nothing, on the one host where "the emitted
# words are branched into for real" stops being a simulation.
#
# That is the same failure the SIMI corpus had -- a check that examined
# nothing reporting success -- and it would have been at its most expensive
# here, because a skip on an Ampere box looks identical to a pass in CI.
#
# Native: compile with plain gcc, run the binary directly, and treat a missing
# compiler as a FAILURE rather than an unsupported environment. An aarch64
# machine with no C compiler is a broken machine, not a skip.
HOST_ARCH="$(uname -m)"
if [ "$HOST_ARCH" = "aarch64" ] || [ "$HOST_ARCH" = "arm64" ]; then
    NATIVE=1
    A64_RUN=""                       # no emulator: this host executes A64
    if ! command -v gcc >/dev/null 2>&1; then
        echo "FAIL  arm64-jit (native $HOST_ARCH host with no gcc -- cannot build the JIT)"
        exit 1
    fi
    echo "arm64-jit: NATIVE $HOST_ARCH -- emitted A64 executes on this CPU, not under emulation"
else
    NATIVE=0
    A64_RUN="qemu-aarch64"
    if ! command -v aarch64-linux-gnu-gcc >/dev/null 2>&1 || ! command -v qemu-aarch64 >/dev/null 2>&1; then
        echo "SKIP  arm64-jit (no aarch64-linux-gnu-gcc / qemu-aarch64 — M3 is environment-dependent; see plan doc §6)"
        exit 0
    fi
fi

# simi-asm is a gitignored host build artifact, absent on a fresh
# checkout; every fixture must be assembled with it before execution,
# so build it alongside the JIT or the whole corpus dies with
# "assembler error" the way it did in CI (the arm64-guards job built
# only simi-arm-jit). Same on-demand build discipline as run_all.sh's
# corpus setup and the Makefile's M2.27 stale-binary lesson.
if ! make -C .. simi-asm simi-arm-jit >/dev/null 2>&1; then
    # On a native host this is not an environment problem, so it is fatal.
    if [ "$NATIVE" = "1" ]; then
        echo "FAIL  arm64-jit (build failed on a native $HOST_ARCH host)"
        make -C .. simi-arm-jit 2>&1 | tail -5
        exit 1
    fi
    echo "SKIP  arm64-jit (simi-arm-jit cross build failed — check the aarch64 toolchain)"
    exit 0
fi

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

    # $A64_RUN is "qemu-aarch64" when cross-testing and empty when native, so
    # the emitted words run under emulation or on the real CPU with no other
    # difference in the path.
    if $A64_RUN "$JIT" "$name.tmo" main "$expected"; then
        pass=$((pass+1))
    else
        fail=$((fail+1))
    fi
done

echo ""
echo "$pass passed, $fail failed, $skip skipped"
[ "$fail" -eq 0 ]
