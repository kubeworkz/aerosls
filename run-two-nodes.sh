#!/usr/bin/env bash
# run-two-nodes.sh — boots TWO real AeroSLS instances, networked together,
# to test real cross-node data movement (Multi-Node Partition Scaling
# Roadmap Phase 7: net/dspp.c's migrate wire family, kernel/partition.c's
# partition_migrate()) instead of the simulated single-process host test
# (tests/cross_node_migration_host_test.c).
#
# This is unverified in the sense that it has never actually been run --
# it was written and reviewed against the Makefile's own working x86-run
# target (the proven, presumably human-run single-instance recipe this
# script's own qemu invocations are deliberately kept as close to as
# possible), but this dev sandbox has no qemu-system-x86_64/cross-compiler/
# nasm installed and cannot run it end-to-end itself. Named honestly rather
# than presented as tested, matching this project's own "verification
# ceiling" disclosure convention throughout its docs.
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
# ─── Why two separate disk images ──────────────────────────────────────
# The default `sls_storage.img` (used by `make x86-run`) is left completely
# untouched by this script -- two real machines are naturally on separate
# physical hardware regardless, so each node here gets its own fresh 10GB
# image (auto-created on first run, same as the Makefile's own x86-run
# target does for the single-instance case).
#
# Usage:
#   chmod +x run-two-nodes.sh
#   ./run-two-nodes.sh
#   # two QEMU/GTK windows open -- node A's, then node B's, a few seconds
#   # apart (node A's socket listener needs to be up before node B tries
#   # to connect to it).
#   #
#   # In EACH window's console, once the AeroSLS shell prompt appears:
#   #   node A:  cluster init 1
#   #   node B:  cluster init 2
#   #   (either window)  cluster status     -- confirms node_id/role/roster
#   #
#   # Then, on whichever node currently owns a partition you want to move
#   # (create one first with "partition create <name>" if needed):
#   #   partition migrate <partition_id> 2
#   # and watch node B's console for "[STREAM]"/"[CONSENSUS]" log lines
#   # confirming the migrated stream's slot was allocated and its pages
#   # verified on arrival (kernel/stream.c's stream_migrate_recv_begin()/
#   # _recv_page(), see the Roadmap doc's Phase 7 addendum for what to
#   # expect on the wire).
#
# Stop both instances with Ctrl-C in this terminal (it waits on both
# background qemu processes and forwards signals) or by closing both
# windows.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$REPO_ROOT"

X86_ISO="sls_operating_system.iso"     # must match the Makefile's own $(X86_ISO) -- if that
                                        # variable's value ever changes there, update it here too.
IMG_A="sls_storage_nodeA.img"
IMG_B="sls_storage_nodeB.img"
LOG_A="sls_kernel_debug_nodeA.log"
LOG_B="sls_kernel_debug_nodeB.log"
SOCKET_PORT=12340                      # arbitrary, unprivileged, unlikely to collide -- change
                                        # if something else on your machine already uses it.
MAC_A="52:54:00:12:34:0a"
MAC_B="52:54:00:12:34:0b"

echo "==> Building $X86_ISO (make x86-iso)..."
make x86-iso

for img in "$IMG_A" "$IMG_B"; do
    if [ ! -f "$img" ]; then
        echo "==> Creating $img (10G, raw)..."
        qemu-img create -f raw "$img" 10G
    fi
done

echo "==> Launching node A (listening on 127.0.0.1:$SOCKET_PORT, mac=$MAC_A)..."
qemu-system-x86_64 -cdrom "$X86_ISO" \
    -drive id=disk,file="$IMG_A",if=none,format=raw \
    -device nvme,drive=disk,serial=slsdevA \
    -netdev socket,id=net0,listen=:"$SOCKET_PORT" \
    -device e1000,netdev=net0,mac="$MAC_A" \
    -vga std -display gtk \
    -m 4G -smp 4 -boot d -serial file:"$LOG_A" &
NODE_A_PID=$!

# Give node A's socket listener a moment to actually bind before node B
# tries to connect -- QEMU's socket netdev connect side does not retry.
sleep 2

echo "==> Launching node B (connecting to 127.0.0.1:$SOCKET_PORT, mac=$MAC_B)..."
qemu-system-x86_64 -cdrom "$X86_ISO" \
    -drive id=disk,file="$IMG_B",if=none,format=raw \
    -device nvme,drive=disk,serial=slsdevB \
    -netdev socket,id=net0,connect=127.0.0.1:"$SOCKET_PORT" \
    -device e1000,netdev=net0,mac="$MAC_B" \
    -vga std -display gtk \
    -m 4G -smp 4 -boot d -serial file:"$LOG_B" &
NODE_B_PID=$!

echo ""
echo "==> Both instances launched (node A pid=$NODE_A_PID, node B pid=$NODE_B_PID)."
echo "==> Serial/debug logs: $LOG_A / $LOG_B"
echo "==> Once each window's AeroSLS shell prompt appears:"
echo "        node A window:  cluster init 1"
echo "        node B window:  cluster init 2"
echo "    then, on node A:    partition create test-tenant"
echo "                        partition migrate <id-just-printed> 2"
echo "==> Ctrl-C here stops both instances."

trap 'echo; echo "==> Stopping both nodes..."; kill "$NODE_A_PID" "$NODE_B_PID" 2>/dev/null || true' INT TERM
wait "$NODE_A_PID" "$NODE_B_PID"
