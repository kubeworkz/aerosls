#!/usr/bin/env bash
# run-cluster.sh — boot an N-node AeroSLS cluster on one host.
#
#   ./run-cluster.sh --nodes 4
#   ./run-cluster.sh --nodes 3 --ram 1G --dry-run
#   ./run-cluster.sh --stop
#
# Supersedes run-two-nodes.sh, which was capped at exactly two by its
# point-to-point netdev. See docs/AeroSLS-N-Node-Launcher-Plan-v0.1.md.
#
# ─── Each node needs its own boot media, and why ───────────────────────
# A node takes its cluster identity from the kernel command line
# (`node=<n>`, kernel/boot_params.c). One ISO can only say one thing, so
# this builds one per node -- a second or so each.
#
# That is not a workaround for laziness, it is the only route available.
# QEMU's `-append` requires `-kernel`, whose x86 loader implements
# Multiboot v1 only, and arch/x86/boot.asm declares a Multiboot2 header
# (0xe85250d6) with no v1 header beside it. Deriving the id from the MAC
# instead would need no per-node media and does not work either:
# partition_init() stamps PARTITION_SYSTEM's owner from
# cluster_local_node_id() early in boot, while net_my_mac does not exist
# until e1000_init() far below it.
#
# ─── One shared L2 segment ─────────────────────────────────────────────
# Every node joins the same `-netdev socket,mcast=` group. No host
# privileges, no bridge, and unlike the old listen/connect pair there is
# no launch ordering: nothing waits for a listener to bind.
#
# QEMU forces IP_MULTICAST_LOOP on for that mode -- deliberately, so
# several instances on one host hear each other -- so every node also
# hears ITSELF. net_rx_dispatch() drops frames bearing our own source MAC
# before any protocol handler sees them; without it each node would
# process its own gratuitous ARP for 10.0.2.15, an address they all share.
#
# ─── Driving a node ────────────────────────────────────────────────────
# Attach to a node's console and you get a shell prompt. That took work:
# kernel.c enters http_server_run() and never returns when a NIC is
# present, so sls_shell_loop() is unreachable on a networked boot -- the
# HTTP loop now polls the serial port between sweeps instead
# (kernel/console.c). There is still no aeroslsctl here: each node has one
# NIC, spent on the segment above, and net/e1000.c binds a single NIC, so
# no host port forward is possible.
#
# ─── Security ──────────────────────────────────────────────────────────
# Consoles bind 127.0.0.1 only. Treat each as a root login should a prompt
# ever appear on it; tunnel over SSH rather than opening the firewall.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$REPO_ROOT"

QEMU="qemu-system-x86_64"
X86_BIN="my_sls_kernel.bin"            # must match the Makefile's own $(X86_BIN)
CLUSTER_DIR="cluster"
PID_FILE="$CLUSTER_DIR/cluster.pids"

MCAST_GROUP="${AEROSLS_MCAST:-239.192.152.40}"
MCAST_PORT="${AEROSLS_MCAST_PORT:-12340}"
CON_BASE="${AEROSLS_CON_BASE:-12340}"  # node i's console is CON_BASE + i
MAC_PREFIX="52:54:00:AE:51"

NODES=2
RAM="${AEROSLS_RAM:-1G}"
SMP="${AEROSLS_SMP:-1}"
DRY_RUN=0
DO_STOP=0

# ─── The cap is read from the source, not hardcoded ────────────────────
# cluster_init() refuses an id above CLUSTER_NODE_MAX and the node stays
# STANDALONE, so a launcher that allowed more would produce nodes that
# boot and silently never join. Reading the header keeps the two in step.
NODE_MAX="$(sed -n 's/^#define CLUSTER_NODE_MAX[[:space:]]\+\([0-9]\+\).*/\1/p' \
            net/consensus.h 2>/dev/null | head -1)"
if [ -z "$NODE_MAX" ]; then
    echo "warning: could not read CLUSTER_NODE_MAX from net/consensus.h;" >&2
    echo "         assuming 8. If the header moved, this cap may be wrong." >&2
    NODE_MAX=8
fi

usage() {
    cat <<EOF
usage: run-cluster.sh [options]

  --nodes N     number of nodes, 1..$NODE_MAX (default $NODES)
  --ram SIZE    RAM per node, QEMU syntax (default $RAM)
  --smp N       vCPUs per node (default $SMP)
  --dry-run     print the plan and each node's QEMU argv; build and launch nothing
  --stop        stop a cluster started earlier, then exit
  -h, --help    this

environment: AEROSLS_MCAST, AEROSLS_MCAST_PORT, AEROSLS_CON_BASE,
             AEROSLS_RAM, AEROSLS_SMP, AEROSLS_DISPLAY, AEROSLS_SETTLE
EOF
}

while [ $# -gt 0 ]; do
    case "$1" in
        --nodes)   NODES="${2:-}"; shift 2 ;;
        --ram)     RAM="${2:-}";   shift 2 ;;
        --smp)     SMP="${2:-}";   shift 2 ;;
        --dry-run) DRY_RUN=1; shift ;;
        --stop)    DO_STOP=1; shift ;;
        -h|--help) usage; exit 0 ;;
        *) echo "error: unknown option '$1'" >&2; usage >&2; exit 1 ;;
    esac
done

# ─── --stop ────────────────────────────────────────────────────────────
# Reads the pid file rather than pattern-matching `ps`, so it cannot
# mistake somebody else's qemu for one of ours.
if [ "$DO_STOP" -eq 1 ]; then
    if [ ! -f "$PID_FILE" ]; then
        echo "no cluster to stop (no $PID_FILE)."
        exit 0
    fi
    stopped=0; gone=0
    while read -r nid pid; do
        [ -z "${pid:-}" ] && continue
        if kill "$pid" 2>/dev/null; then
            echo "  stopped node $nid (pid $pid)"; stopped=$((stopped+1))
        else
            echo "  node $nid (pid $pid) was not running"; gone=$((gone+1))
        fi
    done < "$PID_FILE"
    rm -f "$PID_FILE"
    echo "==> stopped $stopped, already gone $gone."
    exit 0
fi

case "$NODES" in
    ''|*[!0-9]*) echo "error: --nodes must be a whole number, got '$NODES'" >&2; exit 1 ;;
esac
if [ "$NODES" -lt 1 ] || [ "$NODES" -gt "$NODE_MAX" ]; then
    echo "error: --nodes $NODES is out of range 1..$NODE_MAX." >&2
    echo "       The cap is CLUSTER_NODE_MAX in net/consensus.h: cluster_init()" >&2
    echo "       refuses a higher id, so those nodes would boot and never join." >&2
    exit 1
fi

command -v "$QEMU" >/dev/null 2>&1 || { echo "error: $QEMU not found." >&2; exit 1; }

if [ -f "$PID_FILE" ]; then
    live=0
    while read -r _ pid; do
        [ -n "${pid:-}" ] && kill -0 "$pid" 2>/dev/null && live=$((live+1))
    done < "$PID_FILE"
    if [ "$live" -gt 0 ]; then
        echo "error: $live node(s) from a previous run are still up." >&2
        echo "       Run './run-cluster.sh --stop' first." >&2
        exit 1
    fi
    rm -f "$PID_FILE"
fi

# ─── Display backend ───────────────────────────────────────────────────
# A headless server has neither DISPLAY nor WAYLAND_DISPLAY, and its QEMU
# may not have gtk compiled in -- check both, because a build can advertise
# gtk via `-display help` and still fail to open one at runtime.
if [ -n "${AEROSLS_DISPLAY:-}" ]; then
    DISPLAY_BACKEND="$AEROSLS_DISPLAY"
elif [ -z "${DISPLAY:-}${WAYLAND_DISPLAY:-}" ]; then
    DISPLAY_BACKEND="none"
elif "$QEMU" -display help 2>/dev/null | grep -qw gtk; then
    DISPLAY_BACKEND="gtk"
else
    DISPLAY_BACKEND="none"
fi

node_mac()  { printf '%s:%02x' "$MAC_PREFIX" "$1"; }
node_con()  { echo $(( CON_BASE + $1 )); }
node_iso()  { echo "$CLUSTER_DIR/node$1.iso"; }
node_img()  { echo "$CLUSTER_DIR/node$1.img"; }
node_log()  { echo "$CLUSTER_DIR/node$1.log"; }

# Prints the full QEMU argv for node $1, one argument per line, so both the
# launcher and --dry-run render exactly the same command. Two code paths
# that build the argv separately would be two things to keep in step.
node_argv() {
    local i="$1"
    printf '%s\n' \
        "$QEMU" \
        -cdrom "$(node_iso "$i")" \
        -drive "id=disk,file=$(node_img "$i"),if=none,format=raw" \
        -device "nvme,drive=disk,serial=slsdev$i" \
        -netdev "socket,id=net0,mcast=$MCAST_GROUP:$MCAST_PORT,localaddr=127.0.0.1" \
        -device "e1000,netdev=net0,mac=$(node_mac "$i")" \
        -vga std -display "$DISPLAY_BACKEND" -monitor none \
        -chardev "socket,id=con0,host=127.0.0.1,port=$(node_con "$i"),server=on,wait=off,telnet=on,logfile=$(node_log "$i")" \
        -serial chardev:con0 \
        -m "$RAM" -smp "$SMP" -boot d
}

echo "==> Cluster plan"
echo "      nodes            $NODES  (cap $NODE_MAX, CLUSTER_NODE_MAX)"
echo "      per node         $RAM RAM, $SMP vCPU"
echo "      segment          $MCAST_GROUP:$MCAST_PORT (shared, all nodes)"
echo "      display          $DISPLAY_BACKEND"
echo "      consoles         $(node_con 1)..$(node_con "$NODES") on 127.0.0.1"

# Only CONSOLE ports are checked. The segment port is bound by every node
# on purpose -- that shared bind IS the segment -- so treating it as a
# clash would refuse a launch working exactly as designed.
for i in $(seq 1 "$NODES"); do
    p="$(node_con "$i")"
    if command -v ss >/dev/null 2>&1 && ss -ltn 2>/dev/null | grep -q ":$p "; then
        echo "error: console port $p (node $i) is already in use." >&2
        echo "       Set AEROSLS_CON_BASE, or stop whatever holds it." >&2
        exit 1
    fi
done

if [ "$DRY_RUN" -eq 1 ]; then
    echo
    echo "==> --dry-run: nothing will be built or launched."
    for i in $(seq 1 "$NODES"); do
        echo
        echo "  node $i  (id from '$(node_iso "$i")' carrying node=$i)"
        node_argv "$i" | sed 's/^/      /'
    done
    exit 0
fi

mkdir -p "$CLUSTER_DIR"

echo
echo "==> Building the kernel once (make $X86_BIN)..."
make "$X86_BIN"

# ─── Per-node ISO ──────────────────────────────────────────────────────
# Same recipe as the Makefile's x86-iso target, with a generated grub.cfg
# carrying this node's identity.
build_node_iso() {
    local i="$1" iso; iso="$(node_iso "$i")"
    local staging; staging="$(mktemp -d)"
    mkdir -p "$staging/boot/grub"
    cp "$X86_BIN" "$staging/boot/"
    cat > "$staging/boot/grub/grub.cfg" <<EOF
set timeout=0
set default=0
menuentry "AeroSLS — cluster node $i" {
    insmod multiboot2
    multiboot2 /boot/$X86_BIN node=$i
    boot
}
EOF
    if ! grub-mkrescue --modules="normal multiboot2 iso9660 gfxterm font" \
                       -o "$iso" "$staging" >/dev/null 2>"$CLUSTER_DIR/mkrescue$i.err"; then
        echo "error: building $iso failed:" >&2
        sed 's/^/  /' "$CLUSTER_DIR/mkrescue$i.err" >&2
        rm -rf "$staging"; return 1
    fi
    rm -rf "$staging" "$CLUSTER_DIR/mkrescue$i.err"
}

echo "==> Building $NODES node ISOs (each carries its own node=<id>)..."
for i in $(seq 1 "$NODES"); do
    build_node_iso "$i" || exit 1
    [ -f "$(node_img "$i")" ] || qemu-img create -f raw "$(node_img "$i")" 10G >/dev/null
    echo "      node $i -> $(node_iso "$i")"
done

PIDS=()
IDS=()
cleanup_all() {
    for p in "${PIDS[@]:-}"; do [ -n "${p:-}" ] && kill "$p" 2>/dev/null || true; done
}
trap 'echo; echo "==> Stopping cluster..."; cleanup_all; rm -f "$PID_FILE"; exit 130' INT TERM

# How long to give a node to fail before calling it up. QEMU rejects bad
# arguments in well under a second; this is generous and bounded.
SETTLE_SECS="${AEROSLS_SETTLE:-2}"

echo
# Launch everything FIRST, then check once. The old two-node script waited
# after each launch because its point-to-point netdev required the listener
# to be bound before the other side connected. A multicast segment has no
# such ordering -- every node joins independently -- so serialising the
# waits would cost N x SETTLE_SECS to learn nothing extra.
for i in $(seq 1 "$NODES"); do
    mapfile -t argv < <(node_argv "$i")
    "${argv[@]}" </dev/null 2>"$CLUSTER_DIR/node$i.stderr" &
    PIDS+=("$!"); IDS+=("$i")
done

sleep "$SETTLE_SECS"

# $! is set for a process that has already died, so "launched" must be
# confirmed rather than assumed -- the failure this replaces printed PIDs
# for nodes that had already exited.
dead=()
for idx in "${!PIDS[@]}"; do
    kill -0 "${PIDS[$idx]}" 2>/dev/null || dead+=("$idx")
done

if [ "${#dead[@]}" -gt 0 ]; then
    # Report EVERY failure, not just the first. When several nodes die they
    # usually die of the same thing, and seeing one of five is enough to
    # start fixing the wrong node.
    echo "error: ${#dead[@]} of $NODES node(s) failed to start." >&2
    for idx in "${dead[@]}"; do
        echo "  node ${IDS[$idx]} failed to start. QEMU said:" >&2
        sed 's/^/    /' "$CLUSTER_DIR/node${IDS[$idx]}.stderr" >&2
    done
    alive=$(( NODES - ${#dead[@]} ))
    echo "==> tearing down the $alive node(s) that did come up." >&2
    cleanup_all; rm -f "$PID_FILE"
    exit 1
fi

: > "$PID_FILE"
for idx in "${!PIDS[@]}"; do
    i="${IDS[$idx]}"
    echo "$i ${PIDS[$idx]}" >> "$PID_FILE"
    echo "  node $i up (pid ${PIDS[$idx]}, console $(node_con "$i"))"
    rm -f "$CLUSTER_DIR/node$i.stderr"
done

if command -v telnet >/dev/null 2>&1; then ATTACH="telnet 127.0.0.1"; else ATTACH="nc 127.0.0.1"; fi

cat <<EOF

==> $NODES nodes up and confirmed running.
    Logs: $CLUSTER_DIR/node<i>.log     PIDs: $PID_FILE

    Each node already knows its identity -- no 'cluster init' needed.
    Attach a console to watch:
EOF
for i in $(seq 1 "$NODES"); do echo "        node $i:  $ATTACH $(node_con "$i")"; done
cat <<EOF

    Each console gives you a shell prompt. Look for
    "[BOOT] node identity <i> taken from the command line" to confirm the
    node came up as itself, then try "cluster status" -- on a formed
    cluster the roster should list every node on the segment.

==> Ctrl-C stops the cluster, or run './run-cluster.sh --stop' elsewhere.
EOF

wait
