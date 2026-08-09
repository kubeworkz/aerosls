#!/usr/bin/env bash
#
# softmmu_ceiling_measurement.sh — Gate 0 of the QEMU-SLS repositioning plan.
#
# ─── What this measures, and why it needs no AeroSLS code ──────────────────
# QEMU already ships both halves of the experiment QEMU-SLS Phase 1 proposes:
#
#     tcg/tcg-internal.h:37
#         #ifdef CONFIG_USER_ONLY
#         #define tcg_use_softmmu false      <- qemu-<arch>        (host MMU)
#         #else
#         #define tcg_use_softmmu true       <- qemu-system-<arch> (software TLB)
#         #endif
#
# Same TCG core, same host, same guest binary, one difference: whether every
# guest memory access goes through a software TLB lookup or straight to the host
# MMU. That ratio is the CEILING on what Phase 1 can deliver, and it is available
# today, from upstream QEMU, with nothing of ours involved.
#
# ─── Why aarch64 and not x86-64 ────────────────────────────────────────────
# The guest ISA must differ from the host's, or QEMU-system uses KVM and the
# comparison silently becomes "KVM vs qemu-user", which measures nothing about
# TCG. On an x86-64 host, an aarch64 guest guarantees TCG on both sides. The
# script REFUSES to run same-ISA for exactly this reason -- see the guard below,
# which exists because that mistake produces a plausible-looking number.
#
# ─── What the number does NOT mean ─────────────────────────────────────────
# User mode avoids more than the TLB lookup: no guest page tables, no MMIO, no
# privilege transitions, no guest-visible faults. So this OVERSTATES what a
# full-system shadow-paging implementation can reach. That bias is in the safe
# direction for a ceiling, and it must be quoted alongside the number.
#
# ─── Acceptance gate (decided BEFORE the number is seen) ───────────────────
#   >= 3.0x   premise holds -- proceed with the plan
#   1.5-3.0x  real but not headline -- proceed, position as capability not speed
#   <  1.5x   the technique does not pay for its complexity -- stop, keep the
#             shadow paging for a future hypervisor, retire the TCG integration
#
# See docs/AeroSLS-QEMU-SLS-Cross-ISA-Repositioning-v0.1.md §5.
#
# ─── Usage ─────────────────────────────────────────────────────────────────
#   tests/softmmu_ceiling_measurement.sh --kernel <aarch64-Image>
#
# The aarch64 kernel Image is the one external input. On Debian/Ubuntu:
#   apt-get install qemu-user-static qemu-system-arm gcc-aarch64-linux-gnu cpio
#   apt-get download linux-image-generic-arm64   # then extract vmlinuz from it
#
set -u

GUEST_ARCH="${GUEST_ARCH:-aarch64}"
KERNEL=""
ITERS="${ITERS:-200000000}"     # inner loop count; tuned so user mode takes ~5-20s
WORKDIR="${WORKDIR:-/tmp/sls-ceiling-$$}"

# Working-set size lives HERE and is injected into the C source with -D, rather
# than being a #define that the report then repeats from a shell default. Two
# copies of a constant is how a report comes to describe a run that did not
# happen -- the label says 64 MiB because the label always said 64 MiB.
WS_MB="${WS_MB:-64}"

die()  { echo; echo "ABORT: $*" >&2; exit 1; }
step() { echo; echo "── $*"; }

while [ $# -gt 0 ]; do
    case "$1" in
        --kernel) KERNEL="${2:-}"; shift 2 ;;
        --iters)  ITERS="${2:-}";  shift 2 ;;
        *) die "unknown argument '$1'" ;;
    esac
done

# ─── 0. the same-ISA guard ─────────────────────────────────────────────────
# This is the gate that makes the rest of the script meaningful. If the guest
# and host share an ISA, qemu-system uses KVM, the "softmmu ON" arm measures
# hardware virtualization, and the ratio produced is not about softmmu at all --
# it is about KVM, and it will look spectacular for the wrong reason.
HOST_ARCH="$(uname -m)"
case "$HOST_ARCH:$GUEST_ARCH" in
    x86_64:x86_64|aarch64:aarch64|aarch64:arm64|riscv64:riscv64)
        die "guest arch '$GUEST_ARCH' matches host arch '$HOST_ARCH'.
       qemu-system would use hardware virtualization, so the 'softmmu ON' arm
       would measure KVM rather than TCG and the ratio would be meaningless.
       Set GUEST_ARCH to an architecture the host does not implement." ;;
esac
echo "host $HOST_ARCH, guest $GUEST_ARCH -- cross-ISA, TCG on both arms."

# ─── 1. preflight ──────────────────────────────────────────────────────────
step "1. preflight"
CC="${GUEST_ARCH}-linux-gnu-gcc"
QEMU_USER="qemu-${GUEST_ARCH}"
QEMU_SYS="qemu-system-${GUEST_ARCH}"

command -v "$CC"         >/dev/null || die "$CC not found (apt-get install gcc-${GUEST_ARCH}-linux-gnu)"
command -v "$QEMU_USER"  >/dev/null || command -v "${QEMU_USER}-static" >/dev/null \
    || die "$QEMU_USER not found (apt-get install qemu-user-static)"
command -v "$QEMU_SYS"   >/dev/null || die "$QEMU_SYS not found (apt-get install qemu-system-arm)"
command -v cpio          >/dev/null || die "cpio not found (needed to build the initramfs)"
command -v bc            >/dev/null || die "bc not found (needed for the ratio arithmetic)"
command -v "$QEMU_USER"  >/dev/null || QEMU_USER="${QEMU_USER}-static"

[ -n "$KERNEL" ] || die "--kernel <Image> is required.
       qemu-system needs a guest kernel to run the benchmark inside a full
       system. Any ${GUEST_ARCH} Linux Image will do; it is not part of the
       measurement, only the vehicle for it."
[ -f "$KERNEL" ] || die "kernel image '$KERNEL' does not exist"

echo "   $CC          $($CC --version | head -1)"
echo "   $QEMU_USER   $($QEMU_USER --version 2>/dev/null | head -1)"
echo "   $QEMU_SYS    $($QEMU_SYS --version | head -1)"
mkdir -p "$WORKDIR" || die "cannot create $WORKDIR"

# ─── 2. the benchmark ──────────────────────────────────────────────────────
# Pointer-chase over a working set far larger than any plausible software TLB,
# so nearly every access is a TLB miss in the softmmu arm. That is deliberate:
# the quantity under test is the cost of address translation, and a benchmark
# that hits in the TLB measures the fast path we are not trying to replace.
#
# The chase is dependent (each load's result is the next load's address), so the
# host cannot hide the latency behind out-of-order execution and the measurement
# stays on the memory path rather than on ILP.
step "2. build the guest benchmark"
cat > "$WORKDIR/bench.c" <<'EOF'
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

#ifndef WS_MB
#error "WS_MB must be supplied with -D so the report and the run cannot disagree"
#endif
#define STRIDE   4096              /* one guest page per hop: maximum TLB pressure */

int main(int argc, char **argv) {
    unsigned long iters = (argc > 1) ? strtoul(argv[1], 0, 10) : 100000000UL;
    size_t n = ((size_t)WS_MB << 20) / STRIDE;
    void **buf = aligned_alloc(4096, (size_t)WS_MB << 20);
    if (!buf) { fprintf(stderr, "alloc failed\n"); return 1; }

    /* Build a cyclic permutation so the chase cannot be prefetched. */
    size_t *idx = malloc(n * sizeof *idx);
    for (size_t i = 0; i < n; i++) idx[i] = i;
    for (size_t i = n - 1; i > 0; i--) {         /* deterministic shuffle */
        size_t j = (i * 2654435761u) % (i + 1);
        size_t t = idx[i]; idx[i] = idx[j]; idx[j] = t;
    }
    for (size_t i = 0; i < n; i++)
        buf[idx[i] * (STRIDE / sizeof(void *))] =
            &buf[idx[(i + 1) % n] * (STRIDE / sizeof(void *))];

    void **p = &buf[0];
    for (unsigned long i = 0; i < iters; i++) p = (void **)*p;

    /* Consume p so the loop cannot be optimised away. */
    fprintf(stderr, "done %lu iters, p=%p\n", iters, (void *)p);
    return 0;
}
EOF
"$CC" -O2 -static -DWS_MB="$WS_MB" -o "$WORKDIR/bench" "$WORKDIR/bench.c" \
    || die "guest benchmark failed to compile"
echo "   built $WORKDIR/bench ($(stat -c%s "$WORKDIR/bench") bytes, static, ${WS_MB} MiB working set)"

# ─── 3. arm A: user mode -- tcg_use_softmmu FALSE ──────────────────────────
step "3. arm A -- qemu-user (softmmu OFF, host MMU)"
USER_START=$(date +%s.%N)
"$QEMU_USER" "$WORKDIR/bench" "$ITERS" 2>"$WORKDIR/user.err" \
    || die "user-mode run failed -- see $WORKDIR/user.err"
USER_END=$(date +%s.%N)
USER_SEC=$(echo "$USER_END - $USER_START" | bc)
echo "   $USER_SEC s"

# ─── 4. arm B: full system -- tcg_use_softmmu TRUE ─────────────────────────
# The same static binary runs as PID 1 in a minimal initramfs. Same code, same
# host, same TCG; the guest kernel exists only to provide an address space.
#
# Boot time is included in the wall clock and must be subtracted -- so the guest
# prints its own timing markers to the console and those are what get compared.
# Timing the qemu-system process instead would fold kernel boot into the result
# and inflate the ratio in our favour, which is the direction that matters most
# to guard against.
step "4. arm B -- qemu-system (softmmu ON, software TLB)"
mkdir -p "$WORKDIR/initramfs"
cp "$WORKDIR/bench" "$WORKDIR/initramfs/bench"
cat > "$WORKDIR/initramfs/init" <<EOF
#!/bin/sh
echo "SLS-MARK-START \$(cat /proc/uptime)"
/bench $ITERS
echo "SLS-MARK-END \$(cat /proc/uptime)"
echo o > /proc/sysrq-trigger 2>/dev/null
poweroff -f 2>/dev/null
EOF
chmod +x "$WORKDIR/initramfs/init"
mkdir -p "$WORKDIR/initramfs/proc"
( cd "$WORKDIR/initramfs" && find . | cpio -o -H newc 2>/dev/null | gzip ) \
    > "$WORKDIR/initramfs.cpio.gz" || die "initramfs build failed"

"$QEMU_SYS" -M virt -cpu cortex-a57 -m 1G -smp 1 -nographic \
    -kernel "$KERNEL" -initrd "$WORKDIR/initramfs.cpio.gz" \
    -append "console=ttyAMA0 quiet rdinit=/init" \
    > "$WORKDIR/sys.log" 2>&1 &
SYS_PID=$!
( sleep 1800; kill -9 "$SYS_PID" 2>/dev/null ) &   # hard cap, never hang the run
WATCHDOG=$!
wait "$SYS_PID" 2>/dev/null
kill "$WATCHDOG" 2>/dev/null

T0=$(grep -o 'SLS-MARK-START [0-9.]*' "$WORKDIR/sys.log" | awk '{print $2}' | head -1)
T1=$(grep -o 'SLS-MARK-END [0-9.]*'   "$WORKDIR/sys.log" | awk '{print $2}' | head -1)
[ -n "$T0" ] && [ -n "$T1" ] || die "the guest did not report both timing markers.
       Console log is $WORKDIR/sys.log -- the usual cause is a kernel without
       initramfs support, or a console other than ttyAMA0. Without both markers
       there is no measurement, only a wall-clock number that includes boot."
SYS_SEC=$(echo "$T1 - $T0" | bc)
echo "   $SYS_SEC s (guest-reported, boot excluded)"

# ─── 5. the ratio and the gate ─────────────────────────────────────────────
step "5. result"
RATIO=$(echo "scale=2; $SYS_SEC / $USER_SEC" | bc)
cat <<EOF

   softmmu OFF (qemu-user)     ${USER_SEC} s
   softmmu ON  (qemu-system)   ${SYS_SEC} s
   ─────────────────────────────────────────
   ceiling ratio               ${RATIO}x

   Host:  $HOST_ARCH, $(nproc) cpu(s)
   Guest: $GUEST_ARCH, ${WS_MB} MiB working set, ${ITERS} dependent loads
   QEMU:  $($QEMU_SYS --version | head -1)

   This is an UPPER BOUND. User mode avoids guest page tables, MMIO, and
   privilege transitions in addition to the software TLB, so a full-system
   shadow-paging implementation will land at or below this figure. Quote the
   method with the number or do not quote the number.
EOF

VERDICT=$(echo "$RATIO >= 3.0" | bc)
MARGINAL=$(echo "$RATIO >= 1.5" | bc)
if [ "$VERDICT" = "1" ]; then
    echo "   GATE: PASS (>= 3.0x) -- premise holds where TCG is the only option."
    exit 0
elif [ "$MARGINAL" = "1" ]; then
    echo "   GATE: MARGINAL (1.5-3.0x) -- proceed, but position as capability,"
    echo "         not as a speed claim."
    exit 0
else
    echo "   GATE: FAIL (< 1.5x) -- the technique does not pay for its complexity."
    echo "         Keep the shadow paging for a future hypervisor; retire the TCG"
    echo "         integration. This verdict was written down before the number"
    echo "         was seen; honour it."
    exit 1
fi
