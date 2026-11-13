#!/usr/bin/env bash
# tests/e7_gate_all_smoke.sh — stub-driven check of the one-command E7 driver.
#
# It proves the green path copies a complete evidence bundle and excludes the
# sparse disk image, and that a failed arm makes the driver exit non-zero with
# that arm's status in the manifest. Source-only: no build, no QEMU.
#
# Exit: 0 pass, 1 fail.
set -u
cd "$(dirname "$0")/.." || exit 1

TMP="$(mktemp -d)" || { echo "FAIL: cannot create a temp directory"; exit 1; }
ev=".e7_gate_smoke_$$"
trap 'rm -rf "$TMP" "$ev"' EXIT

STUB="$TMP/stub.sh"
cat > "$STUB" <<'STUB_EOF'
set -u
mode="${2:-}"
[ -n "$mode" ] || { echo "stub: source clauses"; exit 0; }
art="${E7G_ARTIFACTS:?}"
case "$mode" in
    --live)
        printf 'boot one\n' > "$art/boot1.log"
        printf 'boot two\n' > "$art/boot2.log"
        printf 'payloads=1\n' > "$art/facts"
        : > "$art/nvme.img" || exit 2
        ;;
    --teeth)
        mkdir -p "$art/teeth/control"
        printf 'control\n' > "$art/teeth/control/boot2.log"
        ;;
    --selfkill)
        printf 'selfkill\n' > "$art/boot1.log"
        ;;
esac
[ "${E7_GATE_STUB_FAIL:-}" = "$mode" ] && exit 7
echo "stub: $mode ok"
STUB_EOF

run() {   # run <evidence-dir> <output> [extra env assignments...]
    local ev="$1" out="$2"
    shift 2
    env E7_GATE_CHECK="$STUB" E7_GATE_EVIDENCE="$ev" "$@" \
        bash tests/e7_gate_all.sh > "$out" 2>&1
}

fails=0
ok()  { echo "ok:   $1"; }
bad() { echo "FAIL: $1"; fails=$((fails + 1)); }

if run "$ev" "$TMP/green.out"; then
    ok "the driver returns 0 when every arm is green"
else
    bad "the driver returned non-zero on a green run"
    sed 's/^/      /' "$TMP/green.out"
fi
for f in source.log manifest.txt live/harness.log live/boot1.log live/boot2.log live/facts \
         teeth/harness.log teeth/teeth/control/boot2.log \
         selfkill/harness.log selfkill/boot1.log; do
    [ -s "$ev/$f" ] && ok "evidence contains $f" || bad "evidence is missing $f"
done
[ ! -e "$ev/live/nvme.img" ] && ok "the sparse disk image is excluded" || bad "the sparse disk image was copied"
for m in source live teeth selfkill; do
    grep -q "^$m.rc=0$" "$ev/manifest.txt" && ok "manifest records $m.rc=0" || bad "manifest does not record $m.rc=0"
done
grep -q 'e7_gate_all: PASS' "$TMP/green.out" && ok "the driver prints its PASS verdict" || bad "no PASS verdict"

if run "$TMP/red" "$TMP/red.out" E7_GATE_STUB_FAIL=--teeth; then
    bad "the driver returned 0 when the teeth arm failed"
else
    ok "the driver returns non-zero when an arm fails"
fi
grep -q '^teeth.rc=7$' "$TMP/red/manifest.txt" && ok "the manifest records the failed arm" || bad "the manifest lost the failed arm"
grep -q 'e7_gate_all: FAIL' "$TMP/red.out" && ok "the driver prints its FAIL verdict" || bad "no FAIL verdict"

if [ "$fails" -ne 0 ]; then
    echo "e7_gate_all_smoke: $fails check(s) failed"
    exit 1
fi
echo "e7_gate_all_smoke: OK — green evidence and a failed arm are both reported correctly"
