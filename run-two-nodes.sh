#!/usr/bin/env bash
# run-two-nodes.sh — boots TWO real AeroSLS instances, networked together,
# to test real cross-node data movement (Multi-Node Partition Scaling
# Roadmap Phase 7: net/dspp.c's migrate wire family, kernel/partition.c's
# partition_migrate()) instead of the simulated single-process host test
# (tests/cross_node_migration_host_test.c).
#
# ─── First real run, and what it found ─────────────────────────────────
# This script previously carried a note saying it had never actually been
# run. It has now, on a headless Ubuntu server, and it failed three ways.
# All three are fixed below and are worth naming, because two of them were
# never headless-specific -- they were wrong everywhere and the headless
# box is simply what made them visible:
#
#   1. `-display gtk` was hardcoded. On a machine with no X/Wayland
#      display that is "gtk initialization failed" and an immediate exit.
#      The display backend is now chosen at runtime.
#
#   2. Nothing checked whether QEMU survived. `$!` is set for a process
#      that has already died, so the script cheerfully printed "Both
#      instances launched" and two PIDs after node A had exited. Each
#      launch is now probed, and QEMU's own stderr is shown if it did not
#      come up.
#
#   3. THE REAL ONE: `-serial file:` is output-only, and this kernel's
#      shell is serial-ONLY -- read_line() in kernel/kernel_io.c polls
#      inb(SERIAL_COM1_BASE) and there is no PS/2 keyboard driver
#      anywhere in the tree. So the old instructions ("type `cluster init
#      1` in each window") could never have worked on any host, with or
#      without a display: the GTK window renders VGA output and has
#      nowhere to send a keystroke. The console is now a listening socket
#      you attach a terminal to, which is interactive AND still writes the
#      same log file via the chardev's own logfile= option.
#
# ─── Why -netdev socket instead of a bridge/tap ────────────────────────
# The Makefile's own x86-run target uses `-netdev user` (QEMU's built-in
# NAT), which deliberately ISOLATES each VM from every other VM -- that's
# exactly why two instances booted via `make x86-run` twice would never see
# each other's Ethernet frames at all, regardless of cluster_init(). A real
# bridge or tap device would also work and is more "real," but needs root/
# admin privileges and host-specific setup (a Linux bridge, or Windows/Mac
# equivalents) this script can't assume. QEMU's `-netdev socket` mode opens
# a plain TCP socket directly between the two QEMU processes' own e1000
# NICs -- no host privileges, no bridge config, works identically on
# Linux/macOS/WSL2. One side listens, the other connects; whichever raw
# Ethernet frames one instance's e1000 transmits arrive at the other's,
# exactly like a real point-to-point cable between two machines' NICs would
# deliver them -- realistic enough for what this test actually needs to
# prove.
#
# ─── Why these nodes have no HTTP, and so no aeroslsctl ────────────────
# Each node gets exactly ONE NIC, spent on the DSPP link above. That is
# forced, not chosen: net/e1000.c keeps a single global tx/rx ring pair and
# one e1000_pci_slot, so the driver binds one NIC and a second -device
# e1000 would sit dead on the PCI bus. Giving a node a host-facing NIC with
# hostfwd would therefore cost it the very link this script exists to
# exercise. Use the serial consoles below; `aeroslsctl` needs a single node
# under `make x86-run` (host 3001 -> guest 3000).
#
# ─── Why two separate disk images ──────────────────────────────────────
# The default `sls_storage.img` (used by `make x86-run`) is left completely
# untouched by this script -- two real machines are naturally on separate
# physical hardware regardless, so each node here gets its own fresh 10GB
# image (auto-created on first run, same as the Makefile's own x86-run
# target does for the single-instance case).
#
# ─── A note on the console ports ───────────────────────────────────────
# The serial consoles bind to 127.0.0.1 ONLY, never 0.0.0.0. Anything that
# can reach one gets an unauthenticated shell on that kernel, so on a
# shared or internet-facing box treat the port as equivalent to a root
# login and tunnel over SSH rather than opening the firewall.
#
# Usage:
#   chmod +x run-two-nodes.sh
#   ./run-two-nodes.sh
#
#   Then, in two OTHER terminals:
#       telnet 127.0.0.1 12341     # node A   (Ctrl-] then "quit" to detach)
#       telnet 127.0.0.1 12342     # node B
#
#   Once each AeroSLS shell prompt appears:
#       node A:  cluster init 1
#       node B:  cluster init 2
#       (either) cluster status     -- confirms node_id/role/roster
#
#   Then, on whichever node currently owns a partition you want to move
#   (create one first with "partition create <name>" if needed):
#       partition migrate <partition_id> 2
#
#   # and watch node B's console for "[STREAM]"/"[CONSENSUS]" log lines
#   # confirming the migrated stream's slot was allocated and its pages
#   # verified on arrival (kernel/stream.c's stream_migrate_recv_begin()/
#   # _recv_page(), see the Roadmap doc's Phase 7 addendum for what to
#   # expect on the wire).
#
# Environment overrides:
#   AEROSLS_DISPLAY=gtk|sdl|none   force a display backend
#   AEROSLS_CON_A / AEROSLS_CON_B  console ports (default 12341 / 12342)
#
# Stop both instances with Ctrl-C in this terminal.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$REPO_ROOT"

QEMU="qemu-system-x86_64"
X86_ISO="sls_operating_system.iso"     # must match the Makefile's own $(X86_ISO) -- if that
                                        # variable's value ever changes there, update it here too.
IMG_A="sls_storage_nodeA.img"
IMG_B="sls_storage_nodeB.img"
LOG_A="sls_kernel_debug_nodeA.log"
LOG_B="sls_kernel_debug_nodeB.log"
SOCKET_PORT=12340                      # node A <-> node B DSPP link. Arbitrary, unprivileged;
                                        # change if something else already uses it.
CON_PORT_A="${AEROSLS_CON_A:-12341}"   # node A serial console
CON_PORT_B="${AEROSLS_CON_B:-12342}"   # node B serial console
MAC_A="52:54:00:12:34:0a"
MAC_B="52:54:00:12:34:0b"

ERR_A="$(mktemp)"
ERR_B="$(mktemp)"
NODE_A_PID=""
NODE_B_PID=""

cleanup() {
    [ -n "$NODE_A_PID" ] && kill "$NODE_A_PID" 2>/dev/null || true
    [ -n "$NODE_B_PID" ] && kill "$NODE_B_PID" 2>/dev/null || true
    rm -f "$ERR_A" "$ERR_B"
}
trap 'echo; echo "==> Stopping both nodes..."; cleanup; exit 130' INT TERM
# Deliberately NOT an EXIT trap. Bash runs EXIT traps when a command
# substitution subshell finishes, so `PID=$(launch ...)` would delete these
# temp files the moment the launch helper returned -- and the error report
# that needs them runs afterwards. Found by the stub-QEMU harness in
# tests/run_two_nodes_harness.sh; cleanup is explicit instead.

command -v "$QEMU" >/dev/null 2>&1 || {
    echo "error: $QEMU not found on PATH." >&2; exit 1; }

# ─── Pick a display backend ────────────────────────────────────────────
# A headless server has neither DISPLAY nor WAYLAND_DISPLAY, and its QEMU
# build may not even have gtk compiled in -- so check both the environment
# and what this QEMU actually supports, rather than assuming either.
if [ -n "${AEROSLS_DISPLAY:-}" ]; then
    DISPLAY_BACKEND="$AEROSLS_DISPLAY"
elif [ -z "${DISPLAY:-}${WAYLAND_DISPLAY:-}" ]; then
    DISPLAY_BACKEND="none"
elif "$QEMU" -display help 2>/dev/null | grep -qw gtk; then
    DISPLAY_BACKEND="gtk"
else
    DISPLAY_BACKEND="none"
fi
echo "==> Display backend: $DISPLAY_BACKEND"

for port in "$CON_PORT_A" "$CON_PORT_B" "$SOCKET_PORT"; do
    if command -v ss >/dev/null 2>&1 && ss -ltn 2>/dev/null | grep -q ":$port "; then
        echo "error: port $port is already in use. Set AEROSLS_CON_A/AEROSLS_CON_B," >&2
        echo "       or stop whatever is holding it (perhaps an earlier run)." >&2
        exit 1
    fi
done

echo "==> Building $X86_ISO (make x86-iso)..."
make x86-iso

for img in "$IMG_A" "$IMG_B"; do
    if [ ! -f "$img" ]; then
        echo "==> Creating $img (10G, raw)..."
        qemu-img create -f raw "$img" 10G
    fi
done

# ─── launch ────────────────────────────────────────────────────────────
# The console chardev does double duty: `telnet=on,server=on,wait=off`
# makes it an interactive terminal that does NOT block boot waiting for
# someone to attach, and `logfile=` keeps the same debug log the old
# `-serial file:` produced. Both, not either.
#
# Sets LAUNCHED_PID rather than echoing it. Echoing would mean calling this
# as `PID=$(launch_node ...)`, and a command substitution is a subshell --
# which changes trap behaviour and reparents the background job. Assigning
# a global keeps the launch in the main shell where `wait` can reach it.
LAUNCHED_PID=""
launch_node() {
    local img="$1" serial="$2" netdev="$3" mac="$4" \
          con_port="$5" log="$6" errfile="$7"
    "$QEMU" -cdrom "$X86_ISO" \
        -drive id=disk,file="$img",if=none,format=raw \
        -device nvme,drive=disk,serial="$serial" \
        -netdev "$netdev" \
        -device e1000,netdev=net0,mac="$mac" \
        -vga std -display "$DISPLAY_BACKEND" -monitor none \
        -chardev socket,id=con0,host=127.0.0.1,port="$con_port",server=on,wait=off,telnet=on,logfile="$log" \
        -serial chardev:con0 \
        -m 4G -smp 4 -boot d </dev/null 2>"$errfile" &
    LAUNCHED_PID=$!
}

# A launch that died leaves $! set exactly as a live one does, which is how
# the old script came to report success for a node that was already gone.
assert_alive() {
    local pid="$1" label="$2" errfile="$3"
    sleep 2
    if ! kill -0 "$pid" 2>/dev/null; then
        echo >&2
        echo "error: $label failed to start. QEMU said:" >&2
        echo "----------------------------------------------------------" >&2
        sed 's/^/  /' "$errfile" >&2
        echo "----------------------------------------------------------" >&2
        cleanup
        exit 1
    fi
}

echo "==> Launching node A (DSPP listen :$SOCKET_PORT, console :$CON_PORT_A, mac=$MAC_A)..."
launch_node "$IMG_A" slsdevA \
    "socket,id=net0,listen=:$SOCKET_PORT" "$MAC_A" "$CON_PORT_A" "$LOG_A" "$ERR_A"
NODE_A_PID="$LAUNCHED_PID"
# Node A's socket listener must be bound before node B tries to connect --
# QEMU's socket netdev connect side does not retry. The liveness probe's
# own sleep covers that wait.
assert_alive "$NODE_A_PID" "node A" "$ERR_A"

echo "==> Launching node B (DSPP connect :$SOCKET_PORT, console :$CON_PORT_B, mac=$MAC_B)..."
launch_node "$IMG_B" slsdevB \
    "socket,id=net0,connect=127.0.0.1:$SOCKET_PORT" "$MAC_B" "$CON_PORT_B" "$LOG_B" "$ERR_B"
NODE_B_PID="$LAUNCHED_PID"
assert_alive "$NODE_B_PID" "node B" "$ERR_B"

if command -v telnet >/dev/null 2>&1; then
    ATTACH_A="telnet 127.0.0.1 $CON_PORT_A"; ATTACH_B="telnet 127.0.0.1 $CON_PORT_B"
else
    ATTACH_A="nc 127.0.0.1 $CON_PORT_A";     ATTACH_B="nc 127.0.0.1 $CON_PORT_B"
fi

cat <<EOF

==> Both nodes are up and confirmed running (A pid=$NODE_A_PID, B pid=$NODE_B_PID).
==> Debug logs: $LOG_A / $LOG_B

    Attach a console from two other terminals:
        node A:  $ATTACH_A
        node B:  $ATTACH_B
$( [ "$DISPLAY_BACKEND" = none ] && printf '%s\n' \
"    (No display, so there are no QEMU windows -- and there is nothing to" \
"     miss: this kernel's shell is serial-only, so the console above is" \
"     the only way in on any host.)" )

    Once each AeroSLS shell prompt appears:
        node A:   cluster init 1
        node B:   cluster init 2
        (either)  cluster status

    Then on node A:
        partition create test-tenant
        partition migrate <id-just-printed> 2

==> Ctrl-C here stops both nodes.
EOF

# Do not let `set -e` turn a node's own exit status into a silent script
# abort -- if one dies, say which.
wait "$NODE_A_PID" || echo "==> node A exited (status $?)."
wait "$NODE_B_PID" || echo "==> node B exited (status $?)."
rm -f "$ERR_A" "$ERR_B"
