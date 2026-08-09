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
# ─── Two NICs per node, and why their PCI slots are pinned ─────────────
# nic0 is management (slirp NAT + hostfwd, so the node has a URL); nic1 is
# the cluster segment. The kernel is told which is which explicitly, via
# `nic0=mgmt nic1=cluster` on its command line -- but "nic0" there means
# "the first e1000 the PCI scan finds", i.e. the lowest slot. QEMU does not
# promise that -device order equals slot order, so both cards carry an
# explicit addr=. Without it a bus reordering would silently put DSPP on
# the NAT and HTTP on the cluster wire -- both would "work" in the sense of
# not erroring, and nothing would reach anything.
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
CON_BASE="${AEROSLS_CON_BASE:-12340}"   # node i's console is CON_BASE + i
HTTP_BASE="${AEROSLS_HTTP_BASE:-3000}"  # node i's REST API is HTTP_BASE + i
MAC_PREFIX="52:54:00:AE:51"             # cluster NIC:    ...:51:0<i>
MGMT_MAC_PREFIX="52:54:00:AE:52"        # management NIC: ...:52:0<i>

NODES=2
NODES_GIVEN=0
FORCE=0
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
  --nodes auto  as many as this host can hold, with the reasoning printed
  --force       launch an explicit --nodes even if it exceeds capacity
  --ram SIZE    RAM per node, QEMU syntax (default $RAM)
  --smp N       vCPUs per node (default $SMP)
  --dry-run     print the plan and each node's QEMU argv; build and launch nothing
  --stop        stop a cluster started earlier, then exit
  -h, --help    this

environment: AEROSLS_MCAST, AEROSLS_MCAST_PORT, AEROSLS_CON_BASE,
             AEROSLS_RAM, AEROSLS_SMP, AEROSLS_DISPLAY, AEROSLS_SETTLE

capacity overrides (also how the sizing is tested):
             AEROSLS_HOST_CORES, AEROSLS_HOST_MEM_MB, AEROSLS_HOST_DISK_MB,
             AEROSLS_HOST_RESERVE_MB (default 2048, left for the host)
EOF
}

while [ $# -gt 0 ]; do
    case "$1" in
        --nodes)   NODES="${2:-}"; NODES_GIVEN=1; shift 2 ;;
        --force)   FORCE=1; shift ;;
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

# A hard prerequisite, checked before anything is computed: a capacity
# report is not useful to someone who cannot launch anything.
command -v "$QEMU" >/dev/null 2>&1 || { echo "error: $QEMU not found." >&2; exit 1; }

# ─── Host capacity ─────────────────────────────────────────────────────
# Every figure below is DETECTED, and every one can be overridden -- both
# because that is how the arithmetic gets tested without a big machine, and
# because an operator on a shared box has better information than df does.
RESERVE_MB="${AEROSLS_HOST_RESERVE_MB:-2048}"
# Overridable so the DETECTION can be tested, not just the arithmetic around
# it. nproc and df are already stubbable through PATH; this file is not, and
# leaving it hardcoded meant the one line that has to say MemAvailable rather
# than MemTotal was never exercised.
MEMINFO="${AEROSLS_MEMINFO:-/proc/meminfo}"

detect_cores() {
    if [ -n "${AEROSLS_HOST_CORES:-}" ]; then echo "$AEROSLS_HOST_CORES"; return; fi
    nproc 2>/dev/null || getconf _NPROCESSORS_ONLN 2>/dev/null || echo ""
}
detect_mem_mb() {
    if [ -n "${AEROSLS_HOST_MEM_MB:-}" ]; then echo "$AEROSLS_HOST_MEM_MB"; return; fi
    # MemAvailable, not MemTotal: the host is already using some of it, and
    # sizing against the total is how you end up swapping.
    local kb
    kb="$(sed -n 's/^MemAvailable:[[:space:]]*\([0-9]\+\) kB/\1/p' "$MEMINFO" 2>/dev/null | head -1)"
    [ -n "$kb" ] && echo $(( kb / 1024 )) || echo ""
}
detect_disk_mb() {
    if [ -n "${AEROSLS_HOST_DISK_MB:-}" ]; then echo "$AEROSLS_HOST_DISK_MB"; return; fi
    local kb; kb="$(df -Pk . 2>/dev/null | awk 'NR==2 {print $4}')"
    [ -n "$kb" ] && echo $(( kb / 1024 )) || echo ""
}

# "1G", "512M", "2048" (bare = MiB) -> MiB. Rejects anything else rather
# than guessing, because a misread size silently changes the node count.
size_to_mb() {
    local n
    case "$1" in
        *[Gg]) n="${1%[Gg]}"
               # Validate BEFORE the arithmetic: "1.5G" would otherwise reach
               # $(( 1.5 * 1024 )), which is a bash error, not a size.
               case "$n" in ''|*[!0-9]*) echo ""; return ;; esac
               echo $(( n * 1024 )) ;;
        *[Mm]) n="${1%[Mm]}"
               case "$n" in ''|*[!0-9]*) echo ""; return ;; esac
               echo "$n" ;;
        ''|*[!0-9]*) echo "" ;;
        *) echo "$1" ;;
    esac
}

HOST_CORES="$(detect_cores)"
HOST_MEM_MB="$(detect_mem_mb)"
HOST_DISK_MB="$(detect_disk_mb)"
RAM_MB="$(size_to_mb "$RAM")"

if [ -z "$RAM_MB" ] || [ "$RAM_MB" -lt 1 ] 2>/dev/null; then
    echo "error: --ram '$RAM' is not a size I understand (try 1G, 512M, 2048)." >&2
    exit 1
fi

# The kernel image ends around 119.5 MiB (_kernel_image_end), so below a
# couple of hundred the frame pool has almost nothing above it. Refuse
# rather than let a node boot into an allocator with no free frames.
RAM_FLOOR_MB=256
if [ "$RAM_MB" -lt "$RAM_FLOOR_MB" ]; then
    echo "error: --ram ${RAM_MB}M is below the ${RAM_FLOOR_MB}M floor." >&2
    echo "       The kernel image alone ends near 120 MiB (_kernel_image_end)," >&2
    echo "       so the frame pool would have almost nothing above it." >&2
    exit 1
fi

# ─── The bounds ────────────────────────────────────────────────────────
# RAM, disk and the roster cap are HARD. CPU is ADVISORY, and that
# distinction is the whole point of the model.
#
# An idle node costs almost no CPU: with -smp 1 there is no application
# processor, and the AP's loop was the one that never idled -- it spun in
# kernel_sleep_ticks() rather than halting (kernel/smp.h). The BSP's HTTP
# loop yields through net_event_hlt_wait(). So a mostly-idle cluster is
# bounded by memory, not cores. A node under real load still wants a core,
# which is what the advisory number is for.
N_RAM=""; N_DISK=""; N_CPU_BUSY=""
[ -n "$HOST_MEM_MB" ]  && N_RAM="$(( (HOST_MEM_MB - RESERVE_MB) / RAM_MB ))"
[ -n "$N_RAM" ] && [ "$N_RAM" -lt 0 ] && N_RAM=0
# Disk is budgeted on the 10 G VIRTUAL size, not observed growth. The images
# are sparse and start near zero, so this is pessimistic on purpose: running
# a host out of disk underneath a live cluster is worse than launching one
# node fewer.
DISK_PER_NODE_MB=10240
[ -n "$HOST_DISK_MB" ] && N_DISK="$(( (HOST_DISK_MB - 2048) / DISK_PER_NODE_MB ))"
[ -n "$N_DISK" ] && [ "$N_DISK" -lt 0 ] && N_DISK=0
[ -n "$HOST_CORES" ] && N_CPU_BUSY="$(( HOST_CORES - 1 ))"
[ -n "$N_CPU_BUSY" ] && [ "$N_CPU_BUSY" -lt 1 ] && N_CPU_BUSY=1

CAPACITY="$NODE_MAX"; BINDING="the roster cap (CLUSTER_NODE_MAX)"
if [ -n "$N_RAM" ]  && [ "$N_RAM"  -lt "$CAPACITY" ]; then CAPACITY="$N_RAM";  BINDING="memory"; fi
if [ -n "$N_DISK" ] && [ "$N_DISK" -lt "$CAPACITY" ]; then CAPACITY="$N_DISK"; BINDING="free disk"; fi

fmt() { [ -n "$1" ] && echo "$1" || echo "unknown"; }
echo "==> Host capacity"
echo "      cores              $(fmt "$HOST_CORES")"
echo "      memory available   $(fmt "$HOST_MEM_MB") MiB   (reserving $RESERVE_MB for the host)"
echo "      free disk          $(fmt "$HOST_DISK_MB") MiB"
if [ -w /dev/kvm ] 2>/dev/null; then
    echo "      KVM                yes"
else
    echo "      KVM                NO -- QEMU will emulate, roughly an order of"
    echo "                         magnitude slower. Node count is not the limit here."
fi
echo "==> Fits at ${RAM_MB} MiB / ${SMP} vCPU per node"
echo "      by memory          $(fmt "$N_RAM")"
echo "      by free disk       $(fmt "$N_DISK")   (10 GiB virtual each; sparse, so pessimistic)"
echo "      roster cap         $NODE_MAX"
echo "      => capacity $CAPACITY, bound by $BINDING"
if [ -n "$N_CPU_BUSY" ]; then
    echo "      CPU (advisory)     $N_CPU_BUSY busy nodes at once; idle nodes halt and cost"
    echo "                         almost nothing, so this is not a hard limit"
fi

if [ "$NODES" = "auto" ]; then
    if [ -z "$HOST_MEM_MB" ] && [ -z "$HOST_DISK_MB" ]; then
        echo "error: --nodes auto needs host figures, and none could be detected." >&2
        echo "       /proc/meminfo and df both came back empty (non-Linux host?)." >&2
        echo "       Give an explicit --nodes, or set AEROSLS_HOST_MEM_MB." >&2
        exit 1
    fi
    if [ "$CAPACITY" -lt 1 ]; then
        # Reporting this as "out of range 1..8" would blame the roster cap
        # for what is actually a small host, and send the reader to the
        # wrong file.
        echo >&2
        echo "error: this host has room for 0 nodes at ${RAM_MB} MiB each ($BINDING)." >&2
        if [ -n "$HOST_MEM_MB" ]; then
            largest=$(( HOST_MEM_MB - RESERVE_MB ))
            [ "$largest" -lt 0 ] && largest=0
            echo "       ${HOST_MEM_MB} MiB available, less ${RESERVE_MB} reserved for the host," >&2
            echo "       leaves ${largest} MiB -- not one node's worth." >&2
            if [ "$largest" -ge "$RAM_FLOOR_MB" ]; then
                echo "       A single node would fit at --ram ${largest}M." >&2
            else
                echo "       Even the ${RAM_FLOOR_MB}M floor does not fit; free memory," >&2
                echo "       or lower AEROSLS_HOST_RESERVE_MB if $RESERVE_MB is too cautious." >&2
            fi
        fi
        exit 1
    fi
    NODES="$CAPACITY"
    echo "==> --nodes auto -> $NODES"
fi

case "$NODES" in
    ''|*[!0-9]*) echo "error: --nodes must be a whole number or 'auto', got '$NODES'" >&2; exit 1 ;;
esac

# The ABSOLUTE limit first. CLUSTER_NODE_MAX is a protocol constant, not a
# property of this machine -- 9 nodes is impossible everywhere, and calling
# that "capacity" would send someone looking for a bigger host. --force does
# not apply: the roster genuinely has no ninth slot.
if [ "$NODES" -lt 1 ] || [ "$NODES" -gt "$NODE_MAX" ]; then
    echo "error: --nodes $NODES is out of range 1..$NODE_MAX." >&2
    echo "       The cap is CLUSTER_NODE_MAX in net/consensus.h: cluster_init()" >&2
    echo "       refuses a higher id, so those nodes would boot and never join." >&2
    echo "       This is a protocol limit, not this host's -- no machine holds more." >&2
    exit 1
fi

# THEN the host limit. Refused, not quietly clamped: asking for 8 and
# silently getting 3 is the outcome that wastes an afternoon.
if [ "$NODES_GIVEN" -eq 1 ] && [ "$NODES" -gt "$CAPACITY" ] && [ "$FORCE" -eq 0 ]; then
    echo "error: --nodes $NODES exceeds this host's capacity of $CAPACITY ($BINDING)." >&2
    echo "       Lower --nodes or --ram, or pass --force to try anyway." >&2
    exit 1
fi

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

node_mac()      { printf '%s:%02x' "$MAC_PREFIX" "$1"; }
node_mgmt_mac() { printf '%s:%02x' "$MGMT_MAC_PREFIX" "$1"; }
node_con()      { echo $(( CON_BASE + $1 )); }
node_http()     { echo $(( HTTP_BASE + $1 )); }
node_iso()  { echo "$CLUSTER_DIR/node$1.iso"; }
node_img()  { echo "$CLUSTER_DIR/node$1.img"; }
node_log()  { echo "$CLUSTER_DIR/node$1.log"; }

# Prints the full QEMU argv for node $1, one argument per line, so both the
# launcher and --dry-run render exactly the same command. Two code paths
# that build the argv separately would be two things to keep in step.
node_argv() {
    local i="$1"
    local -a a=()

    a+=("$QEMU")
    a+=(-cdrom "$(node_iso "$i")")
    a+=(-drive "id=disk,file=$(node_img "$i"),if=none,format=raw")
    a+=(-device "nvme,drive=disk,serial=slsdev$i")

    # nic0 -- MANAGEMENT. Its own isolated slirp NAT, so the guest address
    # never meets a peer, plus a host port forward that gives this node a
    # URL. slirp also runs a DHCP server, which is why net/dhcp.c finally
    # gets a lease here instead of timing out to the compiled-in default.
    a+=(-netdev "user,id=mgmt0,hostfwd=tcp:127.0.0.1:$(node_http "$i")-:3000")
    a+=(-device "e1000,netdev=mgmt0,mac=$(node_mgmt_mac "$i"),addr=0x4")

    # nic1 -- CLUSTER. The shared multicast L2 segment carrying DSPP.
    a+=(-netdev "socket,id=net0,mcast=$MCAST_GROUP:$MCAST_PORT,localaddr=127.0.0.1")
    a+=(-device "e1000,netdev=net0,mac=$(node_mac "$i"),addr=0x5")

    a+=(-vga std -display "$DISPLAY_BACKEND" -monitor none)
    a+=(-chardev "socket,id=con0,host=127.0.0.1,port=$(node_con "$i"),server=on,wait=off,telnet=on,logfile=$(node_log "$i")")
    a+=(-serial chardev:con0)
    a+=(-m "$RAM" -smp "$SMP" -boot d)

    printf '%s\n' "${a[@]}"
}

echo "==> Cluster plan"
echo "      nodes            $NODES  (cap $NODE_MAX, CLUSTER_NODE_MAX)"
echo "      per node         $RAM RAM, $SMP vCPU"
echo "      segment          $MCAST_GROUP:$MCAST_PORT (shared, all nodes)"
echo "      display          $DISPLAY_BACKEND"
echo "      consoles         $(node_con 1)..$(node_con "$NODES") on 127.0.0.1"
echo "      REST API         http://localhost:$(node_http 1)..$(node_http "$NODES")"

# Only CONSOLE ports are checked. The segment port is bound by every node
# on purpose -- that shared bind IS the segment -- so treating it as a
# clash would refuse a launch working exactly as designed.
for i in $(seq 1 "$NODES"); do
    for spec in "console:$(node_con "$i"):AEROSLS_CON_BASE" \
                "REST API:$(node_http "$i"):AEROSLS_HTTP_BASE"; do
        what="${spec%%:*}"; rest="${spec#*:}"; p="${rest%%:*}"; var="${rest#*:}"
        if command -v ss >/dev/null 2>&1 && ss -ltn 2>/dev/null | grep -q ":$p "; then
            echo "error: $what port $p (node $i) is already in use." >&2
            echo "       Set $var, or stop whatever holds it." >&2
            echo "       Note: 'make x86-run' forwards host 3001, which collides" >&2
            echo "       with node 1's REST port by default." >&2
            exit 1
        fi
    done
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
# ─── Say which kernel this is ──────────────────────────────────────────
# This script rebuilds, so it decides what the cluster runs. It used to say
# only "building the kernel" -- and after `make x86-iso SLS_SOFTMMU=on`, a
# plain run here silently rebuilt the OFF kernel and the A/B measured the
# wrong side. (Before the Makefile grew configuration stamps it was worse in
# the other direction: this make was a no-op that left whatever was on disk,
# so the cluster ran a kernel nobody had asked for either way.)
#
# SLS_SOFTMMU is honoured from the environment because the Makefile declares
# it with ?=, so `SLS_SOFTMMU=on ./run-cluster.sh --nodes 4` builds the ON
# side without this script needing an option of its own. What it does need is
# to print the answer, so the configuration is never inferred from memory.
SLS_SOFTMMU="${SLS_SOFTMMU:-off}"
case "$SLS_SOFTMMU" in
    on)  echo "==> Building the kernel once (make $X86_BIN)  [softmmu=ON -- QEMU software TLB, the A/B comparison side]" ;;
    off) echo "==> Building the kernel once (make $X86_BIN)  [softmmu=off -- shadow PT, the shipping default]" ;;
    *)   echo "error: SLS_SOFTMMU must be 'on' or 'off', got '$SLS_SOFTMMU'" >&2; exit 1 ;;
esac
export SLS_SOFTMMU
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
    multiboot2 /boot/$X86_BIN node=$i nic0=mgmt nic1=cluster
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

# The address book slsos-sim needs, emitted rather than left to be typed --
# a hand-written one with a wrong port silently shows another node's data.
NODES_ENV=""
for i in $(seq 1 "$NODES"); do
    [ -n "$NODES_ENV" ] && NODES_ENV="$NODES_ENV,"
    NODES_ENV="$NODES_ENV$i=http://localhost:$(node_http "$i")"
done

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

    Point the dashboard at the cluster by exporting this before
    'npm run dev' in slsos-sim -- the /node/<id> proxy is already there,
    and an id NOT in this list is refused rather than served by node 1:

        export AEROSLS_NODES="$NODES_ENV"

==> Ctrl-C stops the cluster, or run './run-cluster.sh --stop' elsewhere.
EOF

wait
