#!/usr/bin/env bash
# tests/e3_multi_env_boot_check_smoke.sh — the TEETH for
# tests/e3_multi_env_boot_check.sh. A guard that cannot fail proves nothing, so
# every judgement this check makes is driven here with a canned boot log.
#
# ─── How a boot check gets teeth without a boot ─────────────────────────────
# The guard takes E3_QEMU, so the stand-in below receives the same argv QEMU
# would (including -serial file:LOG), writes the canned log to that path, holds
# the "machine" alive for a few seconds so the guard's poll observes a live
# QEMU, and exits. No ISO, no KVM, no QEMU install needed for the arms below —
# which is the point: these teeth run on every source-only runner, and only the
# last arm (the real negative control) wants an image.
#
# ─── The arms, and the ONE reason each may fail ─────────────────────────────
#   three-live     -> PASS  the property, as the real boot reports it
#   two-live       -> FAIL  a tenant never reached a live rootfs
#   shared-storage -> FAIL  both tenants handed the same region (not isolated)
#   marker-lost    -> PASS  the hardening tooth: the sidecar console dropped
#                           init's marker, and the boot is still correct. This
#                           is the byte-loss case that made the guard fail on a
#                           healthy machine before the gate was narrowed to the
#                           drop-proof evidence.
#   marker-only    -> FAIL  init finished spawning and no POSIX reported a live
#                           rootfs: the gate must NOT be satisfiable by init's
#                           own account of itself.
#   no-iso         -> ABORT rc=2  a missing image is a prerequisite failure, not
#                           a pass: the guard fails closed, which is why arms
#                           1–6 hand it a placeholder ISO.
#   shipped-iso    -> FAIL  the real negative control (only when the shipped ISO
#                           exists): that image never spawns tenants, so the
#                           guard must go red rather than quietly skip.
#
# Exit: 0 if every arm behaves as named, 1 otherwise, 2 on a missing prerequisite
# (bash only; arms 1–6 need no image at all — see $DUMMY_ISO — and the last arm
# is skipped with a note when the shipped ISO is absent).
set -u

cd "$(dirname "$0")/.."   # repo root

GUARD=tests/e3_multi_env_boot_check.sh
WORK=$(mktemp -d /tmp/e3_teeth.XXXXXX)
trap 'rm -rf "$WORK"' EXIT

pass=0
fail=0

# The guard's prerequisite check is real — it aborts (rc=2) when its ISO is
# absent — and arms 1–6 exercise the GATE, not the boot, so they must satisfy
# that check without a built image. Hence a placeholder: the guard stats it and
# then hands the launch to $E3_QEMU, which these arms replace with the stub.
# (Verified the hard way: with $E3_ISO unset these arms only passed on a host
# that happened to have sls_operating_system_e3.iso, and every one of them
# aborted rc=2 on a clean source-only runner — exactly where CI's `verify` job
# runs this file, since run_source_smokes.sh globs tests/*_smoke.sh.)
# ABSOLUTE, because the guard cds to the repo root before stat'ing it.
DUMMY_ISO="$WORK/placeholder-e3.iso"
: > "$DUMMY_ISO"

# ─── The stand-in "QEMU": writes the canned log, stays alive, exits ─────────
cat > "$WORK/stubqemu" <<'STUB'
#!/usr/bin/env bash
# argv matches the guard's qemu invocation; take the -serial file: target and
# the canned log from $E3_STUB_LOG.
log=""
prev=""
for a in "$@"; do
    case "$prev" in -serial) case "$a" in file:*) log="${a#file:}" ;; esac ;; esac
    prev="$a"
done
[ -n "$log" ] || { echo "stubqemu: no -serial file: argument" >&2; exit 9; }
# A few seconds so the guard polls a LIVE qemu (it treats an exited one as a
# different failure mode, which is not what these arms are about).
sleep 6
cp "$E3_STUB_LOG" "$log"
sleep 6
exit 0
STUB
chmod +x "$WORK/stubqemu"

# A canned boot log: $1 = how many live-rootfs lines, $2 = storage addresses
# ('same' shares one address), $3 = include init's done-marker (yes/no).
mklog() {
    local n="$1" mode="$2" marker="$3" out="$4"
    {
        echo "[SIDECAR] PID 104 'aerosls.posix.0': wired chan 'console' to kernel service 'kernel.debug.console' (slots 3/4, kernel end 14/15)"
        echo "[POSIX] boot: rw=06 rr=05 cw=03 nw=08"
        echo "[INIT] E3: spawning tenant POSIX environments..."
        if [ "$mode" = same ]; then
            echo "[INIT] E3 env 1: drv.ramdisk.1 storage @ 0x12310000, aerosls.posix.1 heap @ 0x12410000"
            echo "[INIT] E3 env 2: drv.ramdisk.2 storage @ 0x12310000, aerosls.posix.2 heap @ 0x12a10000"
        else
            echo "[INIT] E3 env 1: drv.ramdisk.1 storage @ 0x12310000, aerosls.posix.1 heap @ 0x12410000"
            echo "[INIT] E3 env 2: drv.ramdisk.2 storage @ 0x12910000, aerosls.posix.2 heap @ 0x12a10000"
        fi
        echo "[INIT] E3 env 1: drv.ramdisk.1 spawned"
        echo "[INIT] E3 env 1: aerosls.posix.1 spawned"
        echo "[INIT] E3 env 2: drv.ramdisk.2 spawned"
        echo "[INIT] E3 env 2: aerosls.posix.2 spawned"
        [ "$marker" = yes ] && echo "[INIT] E3: tenant environments spawned."
        # n = number of instances reporting a live rootfs (3 = system + both tenants)
        for i in $(seq 1 "$n"); do
            echo "[POSIX] loop exit0=0000FEED "
            echo "[POSIX] loop tasks=00000001 "
            if [ "$i" -eq 1 ]; then
                echo "[POSIX] rc read=00000080 "
            else
                echo "[POSIX] rc open err=00000002 "
            fi
            echo "[POSIX] aero state=00000000 "
        done
        echo "System ready"
        echo "\$ "
    } > "$out"
}

arm() {   # $1 = label, $2 = expected rc, $3 = expected stdout/stderr substring
    local label="$1" want="$2" needle="$3" got out
    out="$("$WORK/run" 2>&1)"
    got=$?
    if [ "$got" -ne "$want" ]; then
        echo "FAIL  $label: expected rc=$want, got rc=$got" >&2
        echo "$out" | sed 's/^/      /' >&2
        fail=$((fail + 1))
        return
    fi
    if ! printf '%s' "$out" | grep -qF "$needle"; then
        echo "FAIL  $label: rc=$want as expected, but the verdict never said '$needle'" >&2
        echo "$out" | sed 's/^/      /' >&2
        fail=$((fail + 1))
        return
    fi
    echo "ok:   $label (rc=$want, said: $needle)"
    pass=$((pass + 1))
}

# ─── Arm 1: the property, as the real boot reports it ──────────────────────
mklog 3 distinct yes "$WORK/three.log"
cat > "$WORK/run" <<EOF
#!/usr/bin/env bash
E3_ISO="$DUMMY_ISO" E3_QEMU="$WORK/stubqemu" E3_STUB_LOG="$WORK/three.log" bash $GUARD
EOF
chmod +x "$WORK/run"
arm "three live rootfs + init marker -> PASS" 0 "live-rootfs lines (>= 3: system POSIX + both tenants"

# ─── Arm 2: one tenant never reached a live rootfs ─────────────────────────
mklog 2 distinct yes "$WORK/two.log"
cat > "$WORK/run" <<EOF
#!/usr/bin/env bash
E3_ISO="$DUMMY_ISO" E3_QEMU="$WORK/stubqemu" E3_STUB_LOG="$WORK/two.log" bash $GUARD
EOF
chmod +x "$WORK/run"
# The count is named by the gate itself: with fewer than three lines the gate
# never opens, and the verdict carries the number it did see.
arm "only two live rootfs -> FAIL" 1 "live rootfs: 2/3"

# ─── Arm 3: the hardening tooth — the console lost init's marker ───────────
# Before the gate was narrowed to the drop-proof evidence, this boot FAILED.
mklog 3 distinct no "$WORK/nomarker.log"
cat > "$WORK/run" <<EOF
#!/usr/bin/env bash
E3_ISO="$DUMMY_ISO" E3_QEMU="$WORK/stubqemu" E3_STUB_LOG="$WORK/nomarker.log" bash $GUARD
EOF
chmod +x "$WORK/run"
arm "three live rootfs, init marker lost to the console -> PASS" 0 \
    "done-marker was not seen in this log"

# ─── Arm 4: the tenants were NOT isolated ──────────────────────────────────
mklog 3 same yes "$WORK/same.log"
cat > "$WORK/run" <<EOF
#!/usr/bin/env bash
E3_ISO="$DUMMY_ISO" E3_QEMU="$WORK/stubqemu" E3_STUB_LOG="$WORK/same.log" bash $GUARD
EOF
chmod +x "$WORK/run"
arm "both tenants handed one storage region -> FAIL" 1 "regions are NOT isolated"

# ─── Arm 5: init's account of itself is not the property ───────────────────
# init says it spawned both tenants; no POSIX instance reports a live rootfs.
mklog 0 distinct yes "$WORK/markeronly.log"
cat > "$WORK/run" <<EOF
#!/usr/bin/env bash
E3_ISO="$DUMMY_ISO" E3_QEMU="$WORK/stubqemu" E3_STUB_LOG="$WORK/markeronly.log" bash $GUARD
EOF
chmod +x "$WORK/run"
arm "init reports both spawns, no live rootfs -> FAIL" 1 "live rootfs: 0/3"

# ─── Arm 6: init's console output as the kernel really emits it ────────────
# ONE coalesced line carrying an EARLIER 0x address (the e1000 BAR) before the
# E3 lines. The kernel's console buffer concatenates sidecar messages, and a
# looser parse that took "the first 0x on the line" read the BAR as the tenant
# storage base — reporting two genuinely distinct regions as shared. This arm
# is that shape.
{
    printf '%s' "[INIT] e1000 BAR0 @ 0xfeb80000 -> POSIX manifest DEV cap 'nic0.bar0'"
    printf '%s' "[INIT] E3: spawning tenant POSIX environments..."
    printf '%s' "[INIT] E3 env 1: drv.ramdisk.1 storage @ 0x12310000, aerosls.posix.1 heap @ 0x12410000"
    printf '%s' "[INIT] E3 env 1: drv.ramdisk.1 spawned[INIT] E3 env 1: aerosls.posix.1 spawned"
    printf '%s' "[INIT] E3 env 2: drv.ramdisk.2 storage @ 0x12910000, aerosls.posix.2 heap @ 0x12a10000"
    printf '%s\n' "[INIT] E3: tenant environments spawned."
    # The live-rootfs lines are one per line: each is its own SYS_SLS_SERIAL_WRITE
    # with a trailing newline, under the kernel TX lock — the console coalesces
    # sidecar CONSOLE messages, not these. The gate counts lines.
    echo "[POSIX] aero state=00000000 "
    echo "[POSIX] aero state=00000000 "
    echo "[POSIX] aero state=00000000 "
} > "$WORK/coalesced.log"
cat > "$WORK/run" <<EOF
#!/usr/bin/env bash
E3_ISO="$DUMMY_ISO" E3_QEMU="$WORK/stubqemu" E3_STUB_LOG="$WORK/coalesced.log" bash $GUARD
EOF
chmod +x "$WORK/run"
arm "coalesced init line with an earlier 0x before the E3 text -> PASS" 0 \
    "tenant storage regions are distinct"

# ─── Arm 7: a missing ISO is a prerequisite ABORT, not a fallback ──────────
# Pins the reason arms 1–6 pass $DUMMY_ISO: this guard fails CLOSED on its
# prerequisite (rc=2, naming what to build) rather than silently judging
# nothing. A guard that treated a missing image as a pass would be the exact
# rot this whole file exists to prevent, one level up.
out="$(E3_ISO="$WORK/no-such-image.iso" E3_QEMU="$WORK/stubqemu" \
       E3_STUB_LOG="$WORK/three.log" bash "$GUARD" 2>&1)"; got=$?
if [ "$got" -eq 2 ] && printf '%s' "$out" | grep -qF "missing"; then
    echo "ok:   missing E3 ISO -> ABORT rc=2, naming what to build"
    pass=$((pass + 1))
else
    echo "FAIL  missing E3 ISO: expected rc=2 naming the missing prerequisite, got rc=$got" >&2
    echo "$out" | sed 's/^/      /' >&2
    fail=$((fail + 1))
fi

# ─── Arm 8: the real negative control (wants the shipped ISO) ──────────────
# The guard's own header used to claim this was a SKIP. It is not: the shipped
# image spawns no tenants, so the guard must go red. This is the one arm that
# needs an image: it runs the guard with NO stub, i.e. a real QEMU boot of the
# real shipped ISO. Absent (source-only `verify`), it is a stated note rather
# than a silent pass — and kernel-guards DOES have the image, so the control
# runs for real there (run_guard_smokes.sh globs tests/*_smoke.sh).
if [ -f sls_operating_system.iso ]; then
    out="$(E3_ISO=sls_operating_system.iso bash "$GUARD" 2>&1)"; got=$?
    if [ "$got" -eq 1 ] && printf '%s' "$out" | grep -qF "3 live rootfs never seen"; then
        echo "ok:   shipped ISO (no tenant spawns) -> FAIL"
        pass=$((pass + 1))
    else
        echo "FAIL  shipped ISO: expected rc=1 naming the missing live rootfs, got rc=$got" >&2
        echo "$out" | sed 's/^/      /' >&2
        fail=$((fail + 1))
    fi
else
    echo "note: sls_operating_system.iso absent — skipping the real negative control"
fi

echo
echo "$pass passed, $fail failed"
[ "$fail" -eq 0 ] || exit 1
exit 0
