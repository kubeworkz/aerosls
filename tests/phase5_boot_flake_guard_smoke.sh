#!/usr/bin/env bash
# tests/phase5_boot_flake_guard_smoke.sh — proves the Phase 5 flake guard
# still fails a boot that did not finish, and stops failing one that did.
#
# ─── Why this exists ───────────────────────────────────────────────────────
# tests/phase5_boot_flake_guard.sh fails a build on boot logs. Its previous
# criterion — a literal 'newline + "$ "' — failed ten recorded boots that had
# finished (their prompt bytes arrived interleaved: 'System ready\n$$ '), on
# top of the boots it is meant to catch. That is the inverse of the usual
# guard risk: not a tooth that never fires, but one that fires on healthy
# input, which is worse because it trains everyone to re-run a red CI.
#
# So the teeth come in two layers and prove both directions:
#
#   1. MARKER level — tests/phase5_boot_markers.py is driven over one log per
#      shape measured in the corpus: the intact end state, the glued prompt,
#      the one-byte-damaged ready line, the spliced ready line, a log
#      truncated after the caps table, a log cut mid-phase, a mid-line '$ '
#      that must NOT read as a prompt, and a log that does not exist.
#
#   2. GUARD level — the guard itself runs end to end against a STUB qemu
#      (the guard takes ISO and QEMU from the environment for exactly this)
#      that writes a chosen log. That covers the verdict, the exit code and
#      the failure report — including that a report names the missing marker
#      and the host pressure line — with no ISO, no QEMU and no boot.
#
# Source-only on purpose: it needs only bash and python3, so
# run_source_smokes.sh picks it up and CI's verify job runs it on every push.
#
# Exit: 0 when every tooth fired, 1 otherwise, 2 on a missing prerequisite.
set -u
cd "$(dirname "$0")/.."   # repo root

MARKERS=tests/phase5_boot_markers.py
GUARD=tests/phase5_boot_flake_guard.sh

[ -f "$MARKERS" ] || { echo "ABORT: $MARKERS missing" >&2; exit 2; }
[ -f "$GUARD" ] || { echo "ABORT: $GUARD missing" >&2; exit 2; }
command -v python3 >/dev/null 2>&1 || { echo "ABORT: python3 not installed" >&2; exit 2; }

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

checks=0
fails=0
ok()   { checks=$((checks + 1)); echo "  ok:   $1"; }
bad()  { checks=$((checks + 1)); fails=$((fails + 1)); echo "  FAIL: $1" >&2; }

# ── fixtures, one per measured shape ───────────────────────────────────────
printf '[irqtest] ALL PHASES PASS\n[CAP] objects active=31/1024  arena free=16384/16384 frames\nSystem ready\n\n$ ' > "$TMP/clean.log"
printf '[irqtest] ALL PHASES PASS\n[CAP] objects active=31/1024  arena free=16384/16384 frames\nSystem ready\n$$ ' > "$TMP/torn.log"
printf '[irqtest] ALL PHASES PASS\nSystem ready\n$ $ ' > "$TMP/torn2.log"
printf '[irqtest] ALL PHASES PASS\n[CAP] objects active=31/1024  arena free=16384/16384 frames\n\nystem ready\n\n$ ' > "$TMP/damaged-ready.log"
printf '[irqtest] ALL PHASES PASS\n[CAP] objects active=31/1024  arena free=16384/16384 frames\nSys\ntem ready\n$ ' > "$TMP/spliced-ready.log"
printf '[irqtest] ALL PHASES PASS\n[CAP] objects active=31/1024  arena free=16384/16384 frames\nSystem ready\n    [11] type=6 obj=1016 perms=0x03\n' > "$TMP/truncated.log"
printf '[CAP] woken PID 102 (channel 17)\n[nettest] client: socket(TCP)\n' > "$TMP/mid-phase.log"
printf '[irqtest] ALL PHASES PASS\nSystem ready\n\nPATH=/bin HOME=/root\nvalue is $ x\n' > "$TMP/midline-dollar.log"

# ── layer 1: marker classifier ─────────────────────────────────────────────
marker_tooth() {   # marker_tooth <name> <fixture> <expected_rc> [must_appear]
    local name="$1" fixture="$2" want="$3" must="${4:-}"
    local out rc
    out="$(python3 "$MARKERS" --check "$fixture" 2>&1)"
    rc=$?
    if [ "$rc" -ne "$want" ]; then
        bad "$name: --check exit $rc, expected $want ($out)"
        return
    fi
    if [ -n "$must" ] && ! printf '%s' "$out" | grep -qF "$must"; then
        bad "$name: report does not carry '$must' ($out)"
        return
    fi
    ok "$name: exit $want, $out"
}

echo "marker level"
marker_tooth "a finished boot passes"                     "$TMP/clean.log"          0 "prompt=1"
marker_tooth "a GLUED prompt is a prompt (the false failure)" "$TMP/torn.log"       0 "torn=1"
marker_tooth "two prompts in a row pass (second partial)" "$TMP/torn2.log"          0 "prompt=1"
marker_tooth "a one-byte-damaged ready line still reads"  "$TMP/damaged-ready.log"  0 "ready=1*"
marker_tooth "a spliced ready line still reads"           "$TMP/spliced-ready.log"  0 "ready=1*"
marker_tooth "truncated before the prompt fails"          "$TMP/truncated.log"      1 "prompt=0"
marker_tooth "a boot cut mid-phase fails"                 "$TMP/mid-phase.log"      1 "phases=0"
marker_tooth "a mid-line '\$ ' is not a prompt"           "$TMP/midline-dollar.log" 1 "prompt=0"

out="$(python3 "$MARKERS" --check "$TMP/does-not-exist.log" 2>&1)"; rc=$?
if [ "$rc" -eq 1 ]; then ok "an absent log reads as not finished (exit 1)"; else bad "absent log exit $rc, expected 1"; fi

# ── layer 2: the guard end to end, against a stub qemu ─────────────────────
# The stub answers the one thing the guard needs from qemu: it writes the
# boot log to the path given by -serial file:PATH. Everything else about the
# guard's verdict path is then exercised for real.
cat > "$TMP/stub-qemu" <<'STUB'
#!/usr/bin/env bash
# stub qemu for tests/phase5_boot_flake_guard_smoke.sh
#   STUB_LOG         log to write (empty = write nothing)
#   STUB_FAIL_ATTEMPT  space-separated attempt numbers that exit with a
#                   host-resource diagnostic instead of booting
#   STUB_COUNTER     file remembering how many times it has been started
for a in "$@"; do
    case "$a" in
        file:*) OUT="${a#file:}" ;;
    esac
done
N=1
if [ -n "${STUB_COUNTER:-}" ]; then
    [ -f "$STUB_COUNTER" ] && N=$(( $(cat "$STUB_COUNTER") + 1 ))
    echo "$N" > "$STUB_COUNTER"
fi
case " ${STUB_FAIL_ATTEMPT:-} " in
    *" $N "*)
        echo "qemu-system-x86_64: cannot set up guest memory 'pc.ram': Cannot allocate memory" >&2
        exit 1
        ;;
esac
if [ -n "${STUB_LOG:-}" ] && [ -n "${OUT:-}" ]; then cat "$STUB_LOG" > "$OUT"; fi
sleep 30
STUB
chmod +x "$TMP/stub-qemu"
printf 'dummy ISO for the stub qemu\n' > "$TMP/stub.iso"

guard_run() {   # guard_run <fixture-or-empty> [attempts-that-fail] -> GOUT / GRC
    rm -f "$TMP/counter"
    GOUT="$(STUB_LOG="$1" STUB_FAIL_ATTEMPT="${2:-}" STUB_COUNTER="$TMP/counter" \
            ISO="$TMP/stub.iso" QEMU="$TMP/stub-qemu" \
            BOOTS=1 WINDOW_S=6 bash "$GUARD" 2>&1)"
    GRC=$?
}

echo "guard level"
guard_run "$TMP/clean.log"
if [ "$GRC" -eq 0 ] && printf '%s' "$GOUT" | grep -qF "OK   boot 1/1"; then
    ok "guard passes a finished boot (exit 0)"
else
    bad "guard rc=$GRC on a finished boot, expected 0"; printf '%s\n' "$GOUT" | sed 's/^/        /' >&2
fi

guard_run "$TMP/torn.log"
if [ "$GRC" -eq 0 ]; then
    ok "guard passes the glued-prompt boot it used to fail (exit 0)"
else
    bad "guard rc=$GRC on the glued-prompt boot, expected 0 — the false failure is back"
    printf '%s\n' "$GOUT" | sed 's/^/        /' >&2
fi
if printf '%s' "$GOUT" | grep -qF "byte-interleaved"; then
    ok "guard still reports the glued prompt (counted, not failed)"
else
    bad "guard did not report the glued prompt"; printf '%s\n' "$GOUT" | sed 's/^/        /' >&2
fi

guard_run "$TMP/truncated.log"
if [ "$GRC" -eq 1 ] && printf '%s' "$GOUT" | grep -qF "prompt=0" && printf '%s' "$GOUT" | grep -qF "host after:"; then
    ok "guard fails a boot truncated before the prompt, naming the marker + host state"
else
    bad "guard rc=$GRC on a truncated boot, expected 1 with prompt=0 and host state"
    printf '%s\n' "$GOUT" | sed 's/^/        /' >&2
fi

guard_run "$TMP/mid-phase.log"
if [ "$GRC" -eq 1 ] && printf '%s' "$GOUT" | grep -qF "phases=0"; then
    ok "guard fails a boot cut mid-phase, naming phases=0"
else
    bad "guard rc=$GRC on a mid-phase boot, expected 1 with phases=0"
fi

guard_run ""
if [ "$GRC" -eq 1 ]; then
    ok "guard fails a boot that wrote no log at all (exit 1)"
else
    bad "guard rc=$GRC with no log written, expected 1"
fi

# A host-side QEMU death (it never ran the tree and says why) is retried
# once instead of counted; a guest reset leaves no diagnostic and is not.
guard_run "$TMP/clean.log" "1"
if [ "$GRC" -eq 0 ] && printf '%s' "$GOUT" | grep -qF "RETRY boot 1/1" \
   && printf '%s' "$GOUT" | grep -qF "after a retry"; then
    ok "guard retries a QEMU that died with a host diagnostic, then passes"
else
    bad "guard rc=$GRC after an infra QEMU death, expected 0 with a RETRY line"
    printf '%s\n' "$GOUT" | sed 's/^/        /' >&2
fi

guard_run "$TMP/clean.log" "1 2"
if [ "$GRC" -eq 1 ] && printf '%s' "$GOUT" | grep -qF "RETRY boot 1/1"; then
    ok "guard still fails when the host kills the boot twice (retry bounded)"
else
    bad "guard rc=$GRC when both attempts died with a host diagnostic, expected 1"
fi

echo
if [ "$fails" -eq 0 ]; then
    echo "OK: flake guard has teeth ($checks checks): it fails boots that did not finish, and passes the glued-prompt boot it used to fail"
    exit 0
fi
echo "FAIL  $fails/$checks assertions failed" >&2
exit 1
