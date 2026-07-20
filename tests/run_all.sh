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
set -u
cd "$(dirname "$0")/.."   # repo root, so each file's own -I paths (kernel, drivers, net, ...) resolve

BIN_DIR="$(mktemp -d)"
trap 'rm -rf "$BIN_DIR"' EXIT

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
[ "$fail" -eq 0 ]
