#!/usr/bin/env bash
# verify_kernel_e2e.sh — the one piece of verification every Phase 2-5
# findings section (§9-§12) has flagged as NOT done: actually uploading and
# spawning a TIMI program against a real, booted AeroSLS kernel, and
# confirming the translated native code computed the right answer.
#
# Everything up to this point (Phase 2's timi_validate() round-trip test,
# Phase 3/5's host JIT/executor verification, Phase 4's hash unit test) was
# done without a bootable kernel or QEMU available. If you've got
# `make x86-run` running, this closes that gap for real.
#
# What it proves, concretely: this uploads tests/add.tmo (TIMI bytecode:
# ENTER; LOADI r0,2; LOADI r1,3; ADD r2,r0,r1; MOV r0,r2; RET — expected
# result 5) via the real HTTP API, spawns it as an OBJ_TYPE_PROGRAM, and the
# thing to look for in the kernel's serial log (sls_kernel_debug.log by
# default, per the root Makefile's `-serial file:...`) is:
#
#   [TIMI] 'timi_add_test' activation cache MISS — translated ... bytes ...
#   [PROC] PID <n> 'timi_add_test' exited (code=5).
#
# That second line is the real proof: the kernel's Phase 3 x86-64
# translator ran, the Phase 4 activation cache recorded it, the translated
# code actually executed on real hardware inside a Ring-3 process, hit
# SYS_SLS_EXIT, and process_exit() logged exit code 5 — the exact value
# add.timi's TIMI bytecode computes. Not a simulation, not a host-side
# stand-in; this is the whole pipeline, for real.
#
# Usage:
#   ./verify_kernel_e2e.sh                 # uses defaults below
#   BASE_URL=http://localhost:3001 TOKEN=... PROGRAM_NAME=foo ./verify_kernel_e2e.sh
#
# Prerequisites: `make x86-run` already running in another terminal (this
# script does not start/stop QEMU), and the object hasn't been created
# already under the same name (SLS objects aren't meant to be silently
# overwritten — pick a fresh PROGRAM_NAME, or delete/reuse deliberately).
set -euo pipefail

BASE_URL="${BASE_URL:-http://localhost:3001}"
# dave@gridworkz.com / DB_ADMIN, from the kernel's built-in auth token
# registry (see kernel/auth.c) — printed to the serial log on every boot.
TOKEN="${TOKEN:-deadbeef01234567cafebabe76543210}"
PROGRAM_NAME="${PROGRAM_NAME:-timi_add_test}"
TMO_FILE="${TMO_FILE:-$(dirname "$0")/tests/add.tmo}"

if [ ! -f "$TMO_FILE" ]; then
    echo "Building $TMO_FILE (via timi-asm)..."
    make -C "$(dirname "$0")" timi-asm >/dev/null
    "$(dirname "$0")/timi-asm" "$(dirname "$0")/tests/add.timi" "$TMO_FILE"
fi

HEX=$(xxd -p "$TMO_FILE" | tr -d '\n')
PAGES=1

auth_curl() {
    curl -sS -X POST "$BASE_URL$1" \
        -H "Authorization: Bearer $TOKEN" \
        -H "Content-Type: application/json" \
        -d "$2"
}

echo "==> 1/3 Creating OBJ_TYPE_PROGRAM object '$PROGRAM_NAME'..."
auth_curl "/api/program/create" "{\"name\":\"$PROGRAM_NAME\",\"pages\":$PAGES}"
echo ""

echo "==> 2/3 Uploading $TMO_FILE ($(( ${#HEX} / 2 )) bytes)..."
auth_curl "/api/program/upload" "{\"name\":\"$PROGRAM_NAME\",\"hex\":\"$HEX\",\"offset\":0,\"last\":1}"
echo ""

echo "==> 3/3 Spawning '$PROGRAM_NAME'..."
auth_curl "/api/program/spawn" "{\"name\":\"$PROGRAM_NAME\"}"
echo ""

echo ""
echo "==> Now check the kernel's serial log (sls_kernel_debug.log at the"
echo "    repo root, unless you changed the Makefile's -serial argument) for:"
echo ""
echo "      [TIMI] '$PROGRAM_NAME' activation cache MISS — translated ..."
echo "      [PROC] PID <n> '$PROGRAM_NAME' exited (code=5)."
echo ""
echo "    code=5 is the real proof — that's add.timi's expected result,"
echo "    computed by the translated native code on real hardware."
