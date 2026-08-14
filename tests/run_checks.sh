#!/usr/bin/env bash
#
# tests/run_checks.sh -- runs every tests/*_check.sh guard.
#
# ─── Why this exists ───────────────────────────────────────────────────────
# tests/run_all.sh compiles and runs every tests/*_host_test.c. It does not
# touch the tests/*_check.sh guards, because they are shell scripts and its
# loop globs *_host_test.c. CI runs run_all.sh and tools/syntax_check_all.sh.
#
# So five guards were run by nothing:
#
#   stack_frame_budget_check.sh   frames must fit the stack that exists
#   kernel_image_end_check.sh     .bootstrap_stack must stay below _kernel_image_end
#   no_tls_relocations_check.sh   no TLS relocations in a kernel with no TLS
#   makefile_sources_check.sh     X86_C_SRC must match what is on disk
#   commands_doc_check.sh         COMMANDS.md must match the shell's real commands
#
# Every one of them was written AFTER the bug it catches had already happened
# and cost a diagnostic round. kernel_image_end_check.sh exists because
# *(.bootstrap_stack) nested inside .bss did not match, the section became an
# orphan placed above PROVIDE(_kernel_image_end), and the frame allocator
# handed out live kernel stack -- surfacing as rip=0xcdcdcdcdcdcdcdcd.
#
# stack_frame_budget_check.sh's own header says a check that gets suppressed
# protects nothing. A check that nobody runs is the stronger form of that: it
# passes review, it reads well, and it has never once looked at the code.
#
# ─── Exit conventions, which the guards already share ──────────────────────
#   0  pass
#   1  fail -- the property it asserts is violated
#   2  abort -- a prerequisite is missing (no binary, no objects, no readelf)
#
# A guard that aborts is classified by a header marker, and the classes are
# enforced differently:
#   GUARD-KIND: runtime  -> owed skip (needs a live cluster, never a build)
#   GUARD-KIND: build    -> registered skip on source-only runners (needs the
#                           linked kernel/objects; kernel-guards + deploy run
#                           it for real)
#   no marker            -> FAILURE. "Could not check" must not look like
#                           "it is fine" -- that is the silent-rot failure
#                           mode this runner exists to end.
#
# That third case is the reason this runner exists rather than a line of CI
# YAML per script. Three of the five need a linked kernel or its objects, and
# CI has no x86_64-elf-gcc (see .github/workflows/ci.yml, which says so). In
# CI those three legitimately have nothing to inspect; on a build host after
# `make`, all five do.
#
# ─── Skips are printed with their reason, and counted ──────────────────────
# A skip is not a pass. If these scrolled by silently, CI would go green while
# three guards did nothing, which is the exact failure this file was written
# to end -- one level further up. So each skip prints why, and the summary
# counts them separately.
#
# Use --require-all (or REQUIRE_ALL=1) where the prerequisites MUST exist --
# a build host, a release gate, deploy.sh -- to turn every skip into a
# failure. There, "I could not check" and "it is fine" must not look alike.
#
# Usage:
#   tests/run_checks.sh                # skips are reported, not fatal
#   tests/run_checks.sh --require-all  # a skip is a failure
#
set -u
cd "$(dirname "$0")/.."   # repo root, so each guard's own relative paths resolve

REQUIRE_ALL="${REQUIRE_ALL:-0}"
for arg in "$@"; do
    case "$arg" in
        --require-all) REQUIRE_ALL=1 ;;
        -h|--help)
            sed -n '2,/^set -u/p' "$0" | sed 's/^# \{0,1\}//'
            exit 0 ;;
        *) echo "error: unknown option '$arg'" >&2; exit 1 ;;
    esac
done

pass=0
fail=0
skip=0
skipped_names=""
runtime_skipped=""

echo "run_checks"
echo "=========="
if [ "$REQUIRE_ALL" = "1" ]; then
    echo "(--require-all: a missing prerequisite counts as a failure)"
fi
echo

shopt -s nullglob
guards=(tests/*_check.sh)
shopt -u nullglob

if [ "${#guards[@]}" -eq 0 ]; then
    # A runner that found nothing to run must not report success. This is the
    # same rule the guards apply to themselves -- see stack_frame_budget_check
    # aborting when it scans zero sources.
    echo "ABORT: no tests/*_check.sh found. Run from the repo root." >&2
    exit 2
fi

for g in "${guards[@]}"; do
    name="$(basename "$g")"
    out="$("bash" "$g" 2>&1)"
    rc=$?

    case "$rc" in
        0)
            echo "PASS  $name"
            pass=$((pass + 1))
            ;;
        2)
            # Prerequisite missing. Show the guard's own explanation -- it
            # states what it needed, and that is the actionable part.
            reason="$(echo "$out" | grep -m1 -E 'ABORT|missing' || echo 'prerequisite missing')"
            # A guard marked GUARD-KIND: runtime needs a LIVE SYSTEM, not a
            # build artefact. --require-all exists for hosts that can satisfy a
            # prerequisite by building; it cannot conjure a running cluster, and
            # deploy.sh runs these BEFORE restarting the kernel. Forcing such a
            # guard to fail blocks every deploy for a reason unrelated to the
            # build, and the only way out is --no-verify, which switches off the
            # guards that were doing real work.
            if grep -q '^# GUARD-KIND: runtime' "$g" 2>/dev/null; then
                echo "SKIP  $name (runtime guard -- needs a live cluster, not a build)"
                echo "      $reason"
                runtime_skipped="$runtime_skipped $name"
                skip=$((skip + 1))
            elif [ "$REQUIRE_ALL" = "1" ]; then
                echo "FAIL  $name (prerequisite missing, and --require-all is set)"
                echo "      $reason"
                fail=$((fail + 1))
            elif grep -q '^# GUARD-KIND: build' "$g" 2>/dev/null; then
                # A REGISTERED build-needing guard: it inspects the linked
                # kernel or its objects, which a source-only runner does not
                # have. The skip is legitimate ONLY because the guard is
                # classified -- kernel-guards and the deploy gate run it for
                # real. The else below fails any guard that cannot run
                # without a marker, so a guard that stops being source-only
                # cannot silently go dark.
                echo "SKIP  $name (build guard -- needs a linked kernel; enforced by kernel-guards)"
                echo "      $reason"
                skip=$((skip + 1))
                skipped_names="$skipped_names $name"
            else
                # FAIL-CLOSED. An unclassified guard that could not run is
                # rot, not a skip -- the exact silent failure this file was
                # written against, one level up. Mark it GUARD-KIND: build
                # (or runtime) if the prerequisite is legitimately absent
                # from this environment.
                echo "FAIL  $name (unclassified guard could not run -- prerequisite missing)"
                echo "      $reason"
                fail=$((fail + 1))
            fi
            ;;
        *)
            echo "FAIL  $name (exit $rc)"
            echo "$out" | sed 's/^/      /'
            fail=$((fail + 1))
            ;;
    esac
done

echo
echo "$pass passed, $fail failed, $skip skipped"

if [ "$skip" -gt 0 ]; then
    echo
    # Two kinds of skip, reported separately because they need different
    # actions. Merging them produced an empty list and advice about linked
    # kernels for a guard that wanted a running cluster.
    build_skipped=""
    for n in $skipped_names; do
        case " $runtime_skipped " in *" $n "*) ;; *) build_skipped="$build_skipped $n" ;; esac
    done

    if [ -n "$build_skipped" ]; then
        echo "Skipped guards inspected NOTHING:$build_skipped"
        echo "They need a linked kernel or its objects. Run 'make x86-iso' first,"
        echo "then re-run this. On a build host use --require-all so that a missing"
        echo "prerequisite fails instead of passing quietly."
    fi

    if [ -n "$runtime_skipped" ]; then
        [ -n "$build_skipped" ] && echo
        echo "RUNTIME guards NOT RUN, and still owed:$runtime_skipped"
        echo "These need a live cluster, which a build host does not have, so"
        echo "--require-all deliberately does not force them. A green build gate"
        echo "is therefore NOT evidence that they pass. Run them against a"
        echo "started cluster before trusting the property they assert:"
        echo "    ./run-cluster.sh --nodes 3"
        echo "    tests/entropy_boot_diversity_check.sh"
    fi
fi

[ "$fail" -eq 0 ]
