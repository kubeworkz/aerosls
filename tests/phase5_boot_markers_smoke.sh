#!/usr/bin/env bash
# tests/phase5_boot_markers_smoke.sh — proves the Phase 5 boot smoke still
# fails a boot whose driver-SDK demos did not pass, and stops failing one
# that did but whose log arrived damaged.
#
# ─── Why this exists ───────────────────────────────────────────────────────
# tests/phase5_boot_smoke.sh fails a build when the demo PASS lines are not in
# the serial log. Its previous judge was a literal `grep -F "irqtest] PASS"`
# and `grep -F "devtest] PASS"`, and the serial log is measurably not
# byte-faithful: a dropped, spliced or substituted byte is the common shape
# (see tests/phase5_boot_markers.py for the counts). A healthy demo line that
# arrived one byte short therefore read as "the demo never ran" — the same
# false-negative class the flake guard was fixed for, in a second script.
#
# So the teeth come in two layers and prove both directions:
#
#   1. MARKER level — tests/phase5_boot_markers.py is driven with
#      `--preset demos` over one log per shape that matters: a healthy demo
#      log, the glued-prompt shape, a demo marker with one byte dropped, one
#      with a byte spliced in, a boot that stalled after the vector-36 unmask
#      (the shape the real CI failure had), a boot cut before irqtest ever
#      reported, a devtest SKIP (no e1000 — must NOT count as PASS), a log
#      that does not exist, and a bad preset (usage error). It also checks
#      that the guard preset still classifies the demo markers, so one
#      classifier serves both callers.
#
#   2. SMOKE level — the smoke itself runs end to end against a STUB qemu
#      (it takes ISO, QEMU, LOG and WINDOW_S from the environment for exactly
#      this) that writes a chosen log or dies a chosen way. That covers the
#      verdict, the exit code and the failure report: it must name the
#      missing marker and the stage, must say the guest stalled rather than
#      crashed, must retry a QEMU that died with a host diagnostic, and must
#      NOT retry a QEMU that died with an empty stderr (a guest reset).
#
# Source-only on purpose: it needs only bash and python3, so
# run_source_smokes.sh picks it up and CI's verify job runs it on every push.
#
# Exit: 0 when every tooth fired, 1 otherwise, 2 on a missing prerequisite.
set -u
cd "$(dirname "$0")/.."   # repo root

MARKERS=tests/phase5_boot_markers.py
SMOKE=tests/phase5_boot_smoke.sh

[ -f "$MARKERS" ] || { echo "ABORT: $MARKERS missing" >&2; exit 2; }
[ -f "$SMOKE" ] || { echo "ABORT: $SMOKE missing" >&2; exit 2; }
command -v python3 >/dev/null 2>&1 || { echo "ABORT: python3 not installed" >&2; exit 2; }

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

checks=0
fails=0
ok()   { checks=$((checks + 1)); echo "  ok:   $1"; }
bad()  { checks=$((checks + 1)); fails=$((fails + 1)); echo "  FAIL: $1" >&2; }

# ── fixtures, one per shape ────────────────────────────────────────────────
# A healthy boot: both demo PASS lines, the caps line, ready, clean prompt.
printf '[nettest] ALL DONE\n[IRQ] unmask vector 32: pin 0 rte 0x00008020/00000000\n[irqtest] PASS: 50/50 timer edges delivered\n[irqtest] ALL PHASES PASS: timer 50/50, serial loopback 10/10, stuck-driver 3/3, driver-death respawn OK\n[devtest] e1000 MAC: 52:54:00:12:34:01\n[devtest] PASS: CAP_TYPE_DEV + SYS_DEV_MMAP verified on target\n[CAP] objects active=31/1024  arena free=16384/16384 frames\nSystem ready\n\n$ ' > "$TMP/demos.log"
# The same boot whose prompt bytes arrived glued (the shell's own two writes).
printf '[irqtest] PASS: 50/50 timer edges delivered\n[devtest] PASS: SYS_DEV_MMAP succeeded\nSystem ready\n$$ ' > "$TMP/glued.log"
# The real CI shape, in the order a real boot writes it: irqtest's early
# phases PASS, then the chain runs a SECOND time (nettest -> irqtest) and the
# log stops at that second bind — vectors 32 and 36 unmasked, devtest never
# reached. That is why the old report read 'irqtest] PASS: 1 devtest] PASS: 0'
# while its own tail showed only nettest and the unmask lines.
printf '[irqtest] PASS: 50/50 timer edges delivered\n[irqtest] PASS: serial IRQ4 loopback: 10/10 edges delivered\n[irqtest] PASS: stuck-driver: 3/3 windows masked clean, 3/3 recoveries\n[nettest] PASS: shutdown+close\n[nettest] ALL DONE\n[PROC] PID 104 yielded to PID 103\n[IRQ] unmask vector 32: pin 0 rte 0x00008020/00000000\n[IRQ] unmask vector 36: pin 4 rte 0x00008024/00000000\n' > "$TMP/stall.log"
# Cut before irqtest ever reported.
printf '[CAP] woken PID 102 (channel 17)\n[nettest] client: socket(TCP)\n' > "$TMP/midphase.log"
# One byte dropped from the devtest marker.
printf '[irqtest] PASS: 50/50 timer edges delivered\n[devtest] PAS: CAP_TYPE_DEV + SYS_DEV_MMAP verified on target\nSystem ready\n\n$ ' > "$TMP/damaged.log"
# One byte spliced into the irqtest marker (the other writer's newline).
printf '[irqtest]\nPASS: 50/50 timer edges delivered\n[devtest] PASS: SYS_DEV_MMAP succeeded\nSystem ready\n\n$ ' > "$TMP/spliced.log"
# No e1000 in this config: devtest SKIPs. Must NOT read as a PASS.
printf '[irqtest] PASS: 50/50 timer edges delivered\n[devtest] SKIP: CAP_TYPE_DEV not exercised on this target\nSystem ready\n\n$ ' > "$TMP/skip.log"

# ── layer 1: marker classifier, demos preset ───────────────────────────────
marker_tooth() {   # marker_tooth <name> <fixture> <expected_rc> [must_appear]
    local name="$1" fixture="$2" want="$3" must="${4:-}"
    local out rc
    out="$(python3 "$MARKERS" --check --preset demos "$fixture" 2>&1)"
    rc=$?
    if [ "$rc" -ne "$want" ]; then
        bad "$name: --check --preset demos exit $rc, expected $want ($out)"
        return
    fi
    if [ -n "$must" ] && ! printf '%s' "$out" | grep -qF "$must"; then
        bad "$name: report does not carry '$must' ($out)"
        return
    fi
    ok "$name: exit $want, $out"
}

echo "marker level (demos preset)"
marker_tooth "a healthy demo boot passes"                  "$TMP/demos.log"    0 "dev=1"
marker_tooth "a glued prompt does not fail the demos"      "$TMP/glued.log"    0 "dev=1"
marker_tooth "a one-byte-dropped demo marker still passes" "$TMP/damaged.log"  0 "dev=1*"
marker_tooth "a one-byte-spliced demo marker still passes" "$TMP/spliced.log"  0 "irq=1*"
marker_tooth "a stall after the vector-36 unmask fails"    "$TMP/stall.log"    1 "dev=0"
marker_tooth "the stall report names the stage it died on" "$TMP/stall.log"    1 "[IRQ] unmask vector 36: pin 4"
marker_tooth "a boot cut before irqtest fails"             "$TMP/midphase.log" 1 "irq=0"
marker_tooth "a devtest SKIP is not a PASS"                "$TMP/skip.log"     1 "dev=0"
marker_tooth "a healthy demo boot also reads phases=1"      "$TMP/demos.log"   0 "phases=1"

out="$(python3 "$MARKERS" --check --preset demos "$TMP/does-not-exist.log" 2>&1)"; rc=$?
if [ "$rc" -eq 1 ]; then ok "an absent log reads as not finished (exit 1)"; else bad "absent log exit $rc, expected 1"; fi

out="$(python3 "$MARKERS" --check --preset nope "$TMP/demos.log" 2>&1)"; rc=$?
if [ "$rc" -eq 2 ]; then ok "an unknown preset is a usage error (exit 2)"; else bad "bad preset exit $rc, expected 2"; fi

# One classifier serves both callers: the guard preset classifies the demo
# markers too (it just does not require them).
out="$(python3 "$MARKERS" --print "$TMP/demos.log" 2>&1)"
if printf '%s' "$out" | grep -qF "need=phases,prompt" && printf '%s' "$out" | grep -qF "irq=1" && printf '%s' "$out" | grep -qF "dev=1"; then
    ok "the guard preset still judges its own markers and reports the demos"
else
    bad "guard preset report lost a token ($out)"
fi

# ── layer 2: the smoke end to end, against a stub qemu ─────────────────────
# The stub answers the two things the smoke needs from qemu: it writes the
# boot log to the path given by -serial file:PATH, and it can die the two ways
# a real QEMU does (a host-resource diagnostic, or a guest reset with an empty
# stderr). Everything else about the smoke's verdict path is exercised for
# real.
cat > "$TMP/stub-qemu" <<'STUB'
#!/usr/bin/env bash
# stub qemu for tests/phase5_boot_markers_smoke.sh
#   STUB_LOG            log to write (empty = write nothing)
#   STUB_FAIL_ATTEMPT   space-separated attempt numbers that exit with a
#                       host-resource diagnostic instead of booting
#   STUB_RESET_ATTEMPT  space-separated attempt numbers that exit with an
#                       EMPTY stderr (what a guest triple fault looks like)
#   STUB_COUNTER        file remembering how many times it has been started
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
case " ${STUB_RESET_ATTEMPT:-} " in
    *" $N "*) exit 1 ;;
esac
if [ -n "${STUB_LOG:-}" ] && [ -n "${OUT:-}" ]; then cat "$STUB_LOG" > "$OUT"; fi
# Stay alive past the window without leaving a long-lived child behind.
for _ in $(seq 1 30); do sleep 1; done
STUB
chmod +x "$TMP/stub-qemu"
printf 'dummy ISO for the stub qemu\n' > "$TMP/stub.iso"

smoke_run() {   # smoke_run <fixture-or-empty> [fail-attempts] [reset-attempts]
    rm -f "$TMP/counter"
    SOUT="$(STUB_LOG="$1" STUB_FAIL_ATTEMPT="${2:-}" STUB_RESET_ATTEMPT="${3:-}" \
            STUB_COUNTER="$TMP/counter" \
            ISO="$TMP/stub.iso" QEMU="$TMP/stub-qemu" \
            LOG="$TMP/smoke.log" WINDOW_S=6 bash "$SMOKE" 2>&1)"
    SRC=$?
}

echo "smoke level"
smoke_run "$TMP/demos.log"
if [ "$SRC" -eq 0 ] && printf '%s' "$SOUT" | grep -qF "OK: '[irqtest] PASS' and '[devtest] PASS'"; then
    ok "smoke passes a boot whose demos both passed (exit 0)"
else
    bad "smoke rc=$SRC on a healthy demo boot, expected 0"; printf '%s\n' "$SOUT" | sed 's/^/        /' >&2
fi

smoke_run "$TMP/stall.log"
if [ "$SRC" -eq 1 ] && printf '%s' "$SOUT" | grep -qF "dev=0" \
   && printf '%s' "$SOUT" | grep -qF "the guest was alive when the window ran out" \
   && printf '%s' "$SOUT" | grep -qF "host after:"; then
    ok "smoke fails a stalled demo boot, naming the missing marker + stall + host state"
else
    bad "smoke rc=$SRC on a stalled boot, expected 1 with dev=0 and the stall noted"
    printf '%s\n' "$SOUT" | sed 's/^/        /' >&2
fi

smoke_run "$TMP/glued.log"
if [ "$SRC" -eq 0 ]; then
    ok "smoke passes the glued-prompt boot it used to mis-read (exit 0)"
else
    bad "smoke rc=$SRC on the glued-prompt boot, expected 0"
    printf '%s\n' "$SOUT" | sed 's/^/        /' >&2
fi

smoke_run "$TMP/skip.log"
if [ "$SRC" -eq 1 ] && printf '%s' "$SOUT" | grep -qF "dev=0"; then
    ok "smoke fails a boot with no DEV cap (devtest SKIP is not a PASS)"
else
    bad "smoke rc=$SRC on a devtest SKIP boot, expected 1 with dev=0"
fi

smoke_run ""
if [ "$SRC" -eq 1 ]; then
    ok "smoke fails a boot that wrote no log at all (exit 1)"
else
    bad "smoke rc=$SRC with no log written, expected 1"
fi

# A host-side QEMU death (it never ran the tree and says why) is retried once
# instead of counted; a guest reset leaves no diagnostic and is not.
smoke_run "$TMP/demos.log" "1"
if [ "$SRC" -eq 0 ] && printf '%s' "$SOUT" | grep -qF "RETRY:" \
   && printf '%s' "$SOUT" | grep -qF "after a retry"; then
    ok "smoke retries a QEMU that died with a host diagnostic, then passes"
else
    bad "smoke rc=$SRC after an infra QEMU death, expected 0 with a RETRY line"
    printf '%s\n' "$SOUT" | sed 's/^/        /' >&2
fi

smoke_run "$TMP/demos.log" "1 2"
if [ "$SRC" -eq 1 ] && printf '%s' "$SOUT" | grep -qF "RETRY:"; then
    ok "smoke still fails when the host kills the boot twice (retry bounded)"
else
    bad "smoke rc=$SRC when both attempts died with a host diagnostic, expected 1"
fi

smoke_run "" "" "1"
if [ "$SRC" -eq 1 ] && printf '%s' "$SOUT" | grep -qF "qemu stderr empty" \
   && ! printf '%s' "$SOUT" | grep -qF "RETRY:"; then
    ok "smoke fails a guest reset (empty stderr) and does NOT retry it"
else
    bad "smoke rc=$SRC on a reset, expected 1 with 'qemu stderr empty' and no RETRY"
    printf '%s\n' "$SOUT" | sed 's/^/        /' >&2
fi

echo
if [ "$fails" -eq 0 ]; then
    echo "OK: Phase 5 boot smoke has teeth ($checks checks): it fails a boot whose demos did not pass, and passes a healthy boot whose log arrived damaged"
    exit 0
fi
echo "FAIL  $fails/$checks assertions failed" >&2
exit 1
