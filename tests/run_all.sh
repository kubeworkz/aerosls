#!/usr/bin/env bash
# tests/run_all.sh -- Operational Phase A
# (docs/AeroSLS-Operational-MVP-Roadmap-v0.1.md).
#
# Compiles and runs every tests/*_host_test.c against the REAL, unmodified
# kernel/net/user .c files each one links against -- not a reimplementation
# of any of them (every one of these files already says so in its own
# header comment). Each file documents its own exact build command in a
# "Build and run:" comment; this script extracts and runs THAT command
# rather than hardcoding a second copy here that could silently drift out
# of sync as tests are added or their dependencies change -- whoever edits
# a test's own build command remains the one source of truth.
#
# Previously these 24 files each had to be compiled and run by hand, one at
# a time, by whoever remembered to. This is what turns that into one
# command with one pass/fail verdict, suitable for a CI gate
# (.github/workflows/ci.yml calls this).
#
# ─── This file does NOT run the *_check.sh guards ──────────────────────────
# The loop below globs tests/*_host_test.c. tests/*_check.sh are shell
# scripts and are not matched, which is why five of them -- including
# stack_frame_budget_check.sh and kernel_image_end_check.sh -- ran under
# nothing at all for as long as they existed, while reading as though the
# suite covered them.
#
# They now have their own runner: tests/run_checks.sh, also called by CI.
# This cross-reference exists so that discovering one leads to the other;
# their invisibility was most of why they went unrun.
set -u
cd "$(dirname "$0")/.."   # repo root, so each file's own -I paths (kernel, drivers, net, ...) resolve

BIN_DIR="$(mktemp -d)"
trap 'rm -rf "$BIN_DIR"' EXIT

# ─── Build the SIMI corpus before anything runs it ──────────────────────────
# simi_interp_host_test.c executes .tmo programs from tools/simi/tests. Those
# are build artifacts: not tracked, not gitignored, and until now produced by
# nothing. They existed only because somebody had run tools/simi's Makefile at
# some point on that machine.
#
# On a fresh clone every one of them was therefore missing -- and the test
# SKIPS a corpus program it cannot read, so the suite reported PASS having
# executed no SIMI program at all. Twenty checks quietly not happening, in the
# harness this project relies on to know whether anything works. That is the
# rule the guards apply everywhere else -- a check that examined nothing must
# not pass -- being broken by the runner that enforces it.
#
# Assembling here fixes the cause. simi_interp_host_test.c now FAILS rather
# than skips when a program is missing, which fixes the symptom; the two go
# together, since making absence loud without making the files exist would
# simply turn every fresh clone red.
#
# Failure to build the assembler is reported and not fatal: it means no host
# toolchain, which is a different problem from a broken kernel, and the other
# 89 tests still have something to say.
if [ -d tools/simi ]; then
    if make -C tools/simi simi-asm >/dev/null 2>&1; then
        built=0
        for src in tools/simi/tests/*.simi; do
            [ -f "$src" ] || continue
            out="${src%.simi}.tmo"
            if [ ! -f "$out" ] || [ "$src" -nt "$out" ]; then
                tools/simi/simi-asm "$src" "$out" >/dev/null 2>&1 && built=$((built + 1))
            fi
        done
        [ "$built" -gt 0 ] && echo "corpus: assembled $built SIMI program(s)"
    else
        echo "corpus: WARNING -- could not build tools/simi/simi-asm."
        echo "        Any SIMI program not already assembled will be reported"
        echo "        as a failure below rather than skipped, which is correct:"
        echo "        a corpus that cannot be read has not been tested."
    fi
fi

pass=0
fail=0
skip=0

# Pulls the gcc invocation out of a test file's own "Build and run:" header
# comment -- starts at the line containing "gcc ", strips the leading
# " * " comment-block prefix from each line, and keeps consuming
# continuation lines (those ending in a trailing backslash) until the
# command's last line.
extract_build_cmd() {
    awk '
        /\* *gcc / { grab=1 }
        grab {
            line = $0
            # ─── Strip CR before anything else ────────────────────────────
            # This repository is edited on Windows and three test files are
            # CRLF. A backslash continuation then ends "\" CR, and bash does
            # not treat that as a continuation -- so every line of the build
            # command ran as a SEPARATE command. The result was
            #     ld: cannot find : No such file or directory
            #     -o: command not found
            #     tests/foo.c: line 2: LICENSE: command not found
            # i.e. the C source being executed as a shell script, reported by
            # run_all.sh as "compile error". Two tests were red for a long time
            # over a line ending, and the message pointed at the compiler,
            # which was never involved.
            gsub(/\r/, "", line)
            sub(/^[[:space:]]*\*[[:space:]]?/, "", line)
            print line
            if (line !~ /\\[[:space:]]*$/) { exit }
        }
    ' "$1"
}

for src in tests/*_host_test.c; do
    name="$(basename "${src%.c}")"
    cmd="$(extract_build_cmd "$src")"
    if [ -z "$cmd" ]; then
        echo "SKIP  $name (no 'gcc' build command found in its header comment)"
        skip=$((skip + 1))
        continue
    fi

    outbin="$BIN_DIR/$name"
    # Redirect -o at our own scratch dir regardless of whatever /tmp path
    # the file's own comment used, so this script always knows exactly
    # where the binary it just built landed.
    cmd="$(echo "$cmd" | sed -E "s#-o[[:space:]]+[^[:space:]\\\\]+#-o $outbin#")"

    if ! build_log="$(eval "$cmd" 2>&1)"; then
        echo "FAIL  $name (compile error)"
        echo "$build_log" | sed 's/^/      /'
        fail=$((fail + 1))
        continue
    fi

    if ! run_log="$("$outbin" 2>&1)"; then
        echo "FAIL  $name (test binary exited non-zero)"
        echo "$run_log" | sed 's/^/      /'
        fail=$((fail + 1))
        continue
    fi

    checks=$(echo "$run_log" | grep -c '^ok:')
    echo "PASS  $name ($checks checks)"
    pass=$((pass + 1))
done

echo ""
echo "$pass passed, $fail failed, $skip skipped"
echo ""
echo "Note: this covers tests/*_host_test.c only. The tests/*_check.sh guards"
echo "      (stack frame budget, kernel image end, TLS relocations, Makefile"
echo "      sources, COMMANDS.md) have their own runner: tests/run_checks.sh"
[ "$fail" -eq 0 ]
