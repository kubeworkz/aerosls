#!/usr/bin/env bash
#
# node_gdb_attach.sh — relaunch one cluster node with QEMU's gdbstub enabled,
# so a silent kernel halt can be read off the program counter instead of guessed
# at from the log.
#
# ─── Why this exists ───────────────────────────────────────────────────────
# `qemu bench` halts a node with NO console output. That is a hard shape to
# diagnose, because every deliberate halt path in this kernel prints before it
# halts -- the 130 helper stubs each announce their own name, exit() halts
# loudly, handle_page_fault() emits [FAULT]. Something reached a `hlt` without
# passing through any of them, so the log cannot say what.
#
# Three hypotheses were advanced from reading the source during the session that
# found this bug -- wedged at boot, triple fault, quadratic allocation -- and all
# three were wrong. Each died to a two-command measurement. The program counter
# is not a hypothesis.
#
# ─── Why the argv comes from /proc ─────────────────────────────────────────
# Same reason reap_slot_e2e.sh reads it: `run-cluster.sh --dry-run` refuses to
# run while a cluster is up, which is exactly when this is needed. And the
# command line of the process being replaced is strictly better than re-deriving
# it -- it is what is ACTUALLY running, including whatever RAM/SMP autosizing
# chose on this host, so the relaunched node cannot silently differ from the one
# it replaces. A node that differs from the one that hung is not the node that
# hung.
#
# ─── Usage ─────────────────────────────────────────────────────────────────
#   tests/node_gdb_attach.sh [node] [gdbport]        # defaults: node 2, port 1234
#
# Then, in another shell:
#   tools/aeroslsctl --host localhost:300<node> shell "qemu bench 8"   # will wedge
#   gdb my_sls_kernel.bin -ex 'target remote :1234'
#
set -u

NODE="${1:-2}"
GDB_PORT="${2:-1234}"
CLUSTER_DIR="${CLUSTER_DIR:-cluster}"
KERNEL_ELF="${KERNEL_ELF:-my_sls_kernel.bin}"

die()  { echo; echo "ABORT: $*" >&2; exit 1; }
step() { echo; echo "── $*"; }

# ─── 1. locate the node and capture its identity BEFORE killing it ─────────
step "1. capture node $NODE's command line"
PID="$(awk -v n="$NODE" '$1==n{print $2}' "$CLUSTER_DIR/cluster.pids" 2>/dev/null)"
[ -n "$PID" ] || die "no pid for node $NODE in $CLUSTER_DIR/cluster.pids"
kill -0 "$PID" 2>/dev/null || die "node $NODE (pid $PID) is not running"

mapfile -t -d '' ARGV < "/proc/$PID/cmdline" || die "cannot read /proc/$PID/cmdline"
[ "${#ARGV[@]}" -gt 3 ] || die "node $NODE's argv came back with only ${#ARGV[@]} element(s)"
case "${ARGV[0]}" in
    *qemu*) ;;
    *) die "pid $PID does not look like QEMU (argv[0]='${ARGV[0]}') --
       refusing to relaunch something unidentified" ;;
esac
echo "   node $NODE = pid $PID, ${#ARGV[@]} argv elements, ${ARGV[0]##*/}"

# The gdbstub must not already be present, or QEMU refuses to start and the
# failure looks like a crash rather than a duplicate flag.
for a in "${ARGV[@]}"; do
    case "$a" in
        -gdb|-s) die "node $NODE is already running with a gdbstub -- attach to it
       instead of relaunching: gdb $KERNEL_ELF -ex 'target remote :$GDB_PORT'" ;;
    esac
done

# ─── 2. the kernel ELF must match the running image ────────────────────────
# Symbols from a different build resolve to plausible-looking WRONG function
# names, which is worse than no symbols: it produces a confident answer that
# sends the next hour in the wrong direction.
step "2. check the kernel ELF"
[ -f "$KERNEL_ELF" ] || die "$KERNEL_ELF not found -- run from the repo root"
file "$KERNEL_ELF" | grep -q ELF \
    || die "$KERNEL_ELF is not an ELF file. If the build was changed to emit a
       flat binary, gdb has no symbols to work with and this script is useless."
readelf -S "$KERNEL_ELF" 2>/dev/null | grep -q '\.symtab' \
    || die "$KERNEL_ELF has no .symtab -- it has been stripped, so gdb cannot
       name functions. Rebuild without stripping."

ISO="$CLUSTER_DIR/node$NODE.iso"
if [ -f "$ISO" ] && [ "$KERNEL_ELF" -nt "$ISO" ]; then
    echo
    echo "   WARNING: $KERNEL_ELF is NEWER than $ISO."
    echo "   The node is running the kernel baked into the ISO, not this file."
    echo "   Symbols will be resolved against a build the node is not executing,"
    echo "   and every function name gdb reports may be wrong. Rebuild the ISO"
    echo "   or check out the matching source before trusting anything below."
    echo
    printf "   continue anyway? [y/N] "
    read -r ans
    case "$ans" in y|Y) ;; *) die "stopped -- rebuild first" ;; esac
fi
echo "   $KERNEL_ELF: ELF with .symtab, $(stat -c%s "$KERNEL_ELF") bytes"

# gdb's backtrace is unreliable for this build and saying so here is cheaper
# than discovering it mid-diagnosis.
if ! grep -q 'fno-omit-frame-pointer' Makefile 2>/dev/null; then
    echo
    echo "   NOTE: X86_CFLAGS has no -fno-omit-frame-pointer and no -g, so gdb's"
    echo "   'bt' will likely show one frame and then garbage. Trust 'info"
    echo "   registers rip' and map it by hand:"
    echo "       nm -n $KERNEL_ELF | awk '\$1 <= \"<rip>\"' | tail -3"
fi

# ─── 3. relaunch with the gdbstub ──────────────────────────────────────────
step "3. relaunch node $NODE with -gdb tcp:127.0.0.1:$GDB_PORT"
kill "$PID" 2>/dev/null
for _ in $(seq 1 50); do kill -0 "$PID" 2>/dev/null || break; sleep 0.1; done
kill -0 "$PID" 2>/dev/null && { kill -9 "$PID" 2>/dev/null; sleep 0.5; }

# Note: no -S. The node must boot and join the cluster normally; freezing the
# vCPU at reset would mean attaching to a node that never got far enough to
# reproduce anything.
"${ARGV[@]}" -gdb "tcp:127.0.0.1:$GDB_PORT" &
NEW_PID=$!
sleep 2
kill -0 "$NEW_PID" 2>/dev/null \
    || die "the relaunched node exited immediately. Its console log is
       $CLUSTER_DIR/node$NODE.log -- the usual cause is the previous QEMU still
       holding the telnet console port, so give it a few seconds and retry."

# Keep cluster.pids honest, or the next script to read it kills the wrong thing.
if [ -f "$CLUSTER_DIR/cluster.pids" ]; then
    awk -v n="$NODE" -v p="$NEW_PID" '$1==n{$2=p} {print}' \
        "$CLUSTER_DIR/cluster.pids" > "$CLUSTER_DIR/cluster.pids.tmp" \
        && mv "$CLUSTER_DIR/cluster.pids.tmp" "$CLUSTER_DIR/cluster.pids"
    echo "   cluster.pids updated: node $NODE -> $NEW_PID"
fi

cat <<EOF

   node $NODE is up as pid $NEW_PID with a gdbstub on 127.0.0.1:$GDB_PORT

   Reproduce, then attach:

     tools/aeroslsctl --host localhost:300$NODE shell "qemu bench 8"    # wedges
     gdb $KERNEL_ELF -ex 'target remote :$GDB_PORT'

   Once attached (Ctrl-C to interrupt the guest):

     (gdb) info registers rip rsp cr3
     (gdb) x/12i \$rip-24

   What the disassembly settles that the log cannot:
     'cli; hlt' in a loop  -> a deliberate panic path, reached without printing
     a bare 'hlt'          -> an idle loop that stopped receiving interrupts

   Those are different bugs with different fixes, and no amount of log evidence
   distinguishes them.
EOF
