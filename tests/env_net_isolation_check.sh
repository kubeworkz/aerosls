#!/usr/bin/env bash
# tests/env_net_isolation_check.sh — POSIX-Environments v0.2 §6's guard, P2's
# second increment: the outbound path admits, attributed and quota-checked
# at the connect.
#
# ─── Why this exists ──────────────────────────────────────────────────────
# P2 lifts the tenant profile's ceiling (§6): the manifest gains a `network`
# Chan cap whose peer is the KERNEL-OWNED name "kernel.net.socket", never
# `drv.network.0` — that sidecar sits in the system partition, and a channel
# to it would cross the LPAR Phase 11 IPC boundary (E2's scoped registry
# refuses it at creation — the system-peer tooth). Increment 1 made that
# channel speak NET_* and refuse every verb BY NAME. Increment 2 — this
# guard's increment — replaces the blanket refusal with the OUTBOUND PATH:
# socket/connect/send/recv/shutdown/close run against the KERNEL's own stack
# (net/tcp.c), never drv.network.0. The load-bearing part is the ADMIT PATH
# at the connect: before a connection exists the caller's partition is
# attributed (tcp_conn_attribute_partition) and checked against that
# partition's quota (net/tcp_quota.h). An outbound connection with no
# partition counts against nobody — a flood looks like absence — which is
# the "denial looks like absence" failure the P2_TOOTH=unattributed tooth
# exists to catch. bind/listen/accept remain refused by name (increment 3's
# listener), so a tenant is never left wondering whether a verb is absent or
# broken.
#
# ─── Modes ────────────────────────────────────────────────────────────────
#   (no arguments)      the SOURCE clauses over this script's own repository root
#   --live              the source clauses, then the boot arm: a tenant console's
#                       `nc` reaches the service and is refused by name
#                       (needs the built ISO + QEMU; opt-in, never the default)
#   positional ROOT     the source clauses over another tree (the smoke's own
#                       hermetic seam; every real caller passes nothing)
#
# ─── The source clauses ───────────────────────────────────────────────────
#   N1. the tenant manifest: `network`, rights 0x7, peer EXACTLY
#       kernel.net.socket (never drv.network.0 — the system-peer tooth pinned
#       at the source), n_caps 4, the rotated pin test, and BOTH callers (E3
#       boot spawn, control-plane ENV_CREATE) grant the tenant manifest;
#   N2. the SYSTEM profile is untouched: its own network -> drv.network.0
#       stays where it was (only the tenant profile was to change);
#   N3. the mint arm: cap.c's kernel.net.socket case is there, and so is the
#       E2 scoped-registry branch a drv.network.0 peer falls to — the
#       boundary, not the API;
#   N4. the registration carries the caller's identity — partition, pid and
#       name — which is what the refusal later names;
#   N5. console_service skips the socket channel's kernel slot (NET_* frames
#       are not console text and must not reach the serial transcript);
#   N6. the tick runs in the non-IRQ service poll, with env_console_tick;
#   N7. the Makefile compiles the service;
#   N8. the wire refuses by name — the verbs not yet built (bind/listen/
#       accept) get NET_FLAG_ERROR + NET_ERR_CAP (9) with the rendered text,
#       and NET_INFO answers max_sockets the REAL NSS_MAX_SOCKS (the
#       increment-2 change from "0 = nothing admits");
#   N9. the host test links the REAL service and pins the exact rendering
#       (one rendering, three callers: host test, serial line, live clause);
#   N10. the ADMIT path: socket/connect/send/recv/shutdown/close reach the
#       kernel stack (net/tcp.c), and the connect is attributed to the
#       caller's partition (tcp_conn_attribute_partition) and quota-checked
#       (tcp_partition_get_conn_quota/usage) BEFORE a connection exists —
#       the property the whole increment rests on;
#   N11. the unattributed tooth's shape: the admission counters
#       (net_socket_admits / net_socket_quota_refusals) are present so a
#       quota refusal and an admit are each countable, and the quota module
#       (net/tcp_quota.c) is compiled in;
#   N12. the listener verbs (P2 increment 3): bind claims the port on the
#       channel's own socket, tcp_listen draws the listener slot, the
#       accept scans for a ready conn and attributes it — all against the
#       kernel's own stack, anchored to code lines rather than comments;
#   N13. each partition's listener is reachable only from inside it: the
#       listener is bound on its own channel's socket and the accept charges
#       the accepted conn to the CALLER's partition (e->partition);
#   N14. the descriptor's line: it carries the messenger channels and never
#       a connection id / socket / listener handle (P2_TOOTH=restore-sockets
#       reddens this);
#   N15. the quota surface the live arm drives (P2 increment 4): syscalls
#       277/278 and their dispatch, POST /api/partition/connquota and its
#       GET readback with the exact body keys the arm posts, and the
#       operator shell pair — "no new API", pinned end to end;
#   N16. the admit path's quota gate — deny before attributing, and 0 stays
#       unlimited (§11 Q4's contract). P2_TOOTH=no-quota reddens exactly
#       this while N12-N14 stay green: the vacuity control and the reason
#       the phase exists;
#   N17/N17b. the flood evidence's wiring: ci.yml invokes this guard's
#       --live arm, ordered AFTER the ISO build (P1b's S13/S13b shape —
#       deleting or reordering the step reddens the guard by name).
#
# ─── The live arm (--live) ────────────────────────────────────────────────
#   L1. the created tenant environment's channel registers under the
#       kernel-owned name (kernel TX — "[NET-SOCKET] aerosls.posix.1
#       (partition P, pid Q) wired: kernel ends …" — pid agreeing with the
#       control plane's posix_pid: kernel's own account and the API's);
#   L2. its console's `nc <host> <port>` reaches the service: NET_SOCKET is
#       now ADMITTED (increment 2 — the outbound path), so the connect to a
#       TEST-NET-1 address (192.0.2.1, which must never be reachable) is
#       refused at the quota/admit gate, NOT silently admitted and left
#       charging nobody — one serial line carries the caller's identity and
#       the honest refusal.
#   L3. no environment ever reports NETBOOT FAILED — every handshake got the
#       honest NET_INFO answer (a service that never replies makes each
#       sidecar's bounded retry loop print that line).
#   L4. two partitions, each listener reachable only from inside it: partition
#       B opens a TCP listener (bind+listen) through its OWN console, then
#       partition A runs `nc <guest> <port>` at it — a connect that would have
#       to cross the boundary. The service keys every socket to the CALLER's
#       partition (N13), so A's reach must be answered BY NAME and never
#       admitted into B. This is §6's isolation property, live — the data-path
#       half of the boundary E2's scoped registry refuses at creation.
#   L5. the bounds measured (increment 4): a small conn quota set on partition
#       A through POST /api/partition/connquota, a second environment in A
#       whose loopback client+listener push A to its quota, and the EXCESS
#       accept refused BY NAME (closed, charged to nobody) — while partition
#       B, quota 0 (unlimited), serves a client of its own throughout: the
#       B-keeps-serving clause that is the whole point of the phase, plus the
#       0=unlimited half read back off B.
#
# Teeth: tests/env_net_isolation_check_smoke.sh proves every clause above
# reddens when the property it carries is removed (P2_TOOTH=system-peer is
# the tooth §6 names for this increment), and that this guard is GREEN on
# the real tree — the vacuity control without which a guard that failed
# unconditionally would pass every tooth.
#
# GUARD-KIND: host (plain bash over the tree; --live adds QEMU + the ISO and
# is opt-in — the smoke proves every source tooth on every push with no build).
#
# Exit: 0 if every clause held, 1 if one failed, 2 a precondition missing.
set -u
cd "$(dirname "$0")/.."   # repo root: default ROOT, and where --live boots from

ROOT=""
LIVE=0
while [ $# -gt 0 ]; do
    case "$1" in
        --live) LIVE=1 ;;
        -*)     echo "ABORT: unknown argument '$1' (usage: $0 [ROOT] [--live])" >&2; exit 2 ;;
        *)      if [ -n "$ROOT" ]; then
                    echo "ABORT: more than one ROOT ('$ROOT' and '$1')" >&2; exit 2
                fi
                ROOT="$1" ;;
    esac
    shift
done
if [ "$LIVE" -eq 1 ] && [ -n "$ROOT" ]; then
    echo "ABORT: --live inspects the real repository; the ROOT seam is source-only" >&2
    exit 2
fi
[ -n "$ROOT" ] || ROOT="$(pwd)"

ok()   { echo "ok:   $*"; }
bad()  { echo "FAIL: $*"; fail=1; }
note() { echo "note: $*"; }
fail=0

MF="$ROOT/user/init/src/posix_manifest.rs"
INITE="$ROOT/user/init/src/entry.rs"
ENVM="$ROOT/user/init/src/env_manager.rs"
CAP="$ROOT/kernel/cap.c"
SVC="$ROOT/kernel/net_socket_service.c"
SVCH="$ROOT/kernel/net_socket_service.h"
CONS="$ROOT/kernel/console_service.c"
MMK="$ROOT/kernel/microkernel.c"
TQ="$ROOT/net/tcp_quota.c"
TQH="$ROOT/net/tcp_quota.h"
SYSD="$ROOT/kernel/syscall_dispatch.c"
HTTP="$ROOT/net/http.c"
USH="$ROOT/user/shell.c"
CIW="$ROOT/.github/workflows/ci.yml"
MKF="$ROOT/Makefile"
TST="$ROOT/tests/net_socket_service_host_test.c"
PROTO="$ROOT/user/proto/src/env_proto.rs"

# Precondition: every file the clauses read. Missing is a refusal to evaluate
# (exit 2), not a pass and not a finding — the same rule the restore guard's
# smoke pins with its arm A.
for f in "$MF" "$INITE" "$ENVM" "$CAP" "$SVC" "$SVCH" "$CONS" "$MMK" "$TQ" "$TQH" "$SYSD" "$HTTP" "$USH" "$CIW" "$MKF" "$TST" "$PROTO"; do
    [ -f "$f" ] || { echo "ABORT: ${f#"$ROOT"/} missing — the guard cannot be evaluated" >&2; exit 2; }
done

# has <file> <fixed string> — 0 when present. Fixed-string on purpose: every
# anchor below is source text, not a pattern.
has() { grep -qF -- "$2" "$1" 2>/dev/null; }
# hasstr <text> <fixed string> — the same, over an extracted body.
hasstr() { printf '%s\n' "$1" | grep -qF -- "$2" 2>/dev/null; }
# fn_body <file> <literal first line> — from the line that starts with the
# pattern to the first '^}' at column 0 (the shape every fn in these files has).
fn_body() {
    awk -v pat="$2" 'index($0, pat) == 1 { grab = 1 }
                     grab { print }
                     grab && /^}/ { exit }' "$1"
}
# order_ok <file> <first-fixed-string> <second-fixed-string> — 0 when both
# are present and the first occurs on an earlier line. The S13b shape P1b's
# guard established: a boot guard ordered ahead of its ISO build aborts
# before booting, reddening for the wrong reason while proving nothing — so
# the ORDER half gets its own name and its own tooth.
order_ok() {
    local a b
    a=$(grep -nF -- "$2" "$1" 2>/dev/null | head -1 | cut -d: -f1)
    b=$(grep -nF -- "$3" "$1" 2>/dev/null | head -1 | cut -d: -f1)
    [ -n "$a" ] && [ -n "$b" ] && [ "$a" -lt "$b" ]
}
# clause <mark> <id> <label> — exactly one ok line when nothing under that
# clause failed; the FAIL lines above it are already prefixed with the id.
clause() { [ "$fail" -eq "$1" ] && ok "$2. $3"; }

TENANT="$(fn_body "$MF" 'pub fn build_posix_manifest_tenant(')"
SYSTEM="$(fn_body "$MF" 'pub fn build_posix_manifest(')"

# ── N1. the tenant manifest's network cap, and the rotated pin ────────────
c1=$fail
hasstr "$TENANT" 'peer: Some("kernel.net.socket"),' ||
    bad "N1. tenant manifest: the network cap's peer must be exactly kernel.net.socket"
printf '%s\n' "$TENANT" | grep -A2 -F 'name: "network"' | grep -qF 'rights: 0x7' ||
    bad "N1. tenant manifest: the network cap's rights must be 0x7 (R|W|send, beside console and ramdisk)"
hasstr "$TENANT" 'n_caps: 4,' ||
    bad "N1. tenant manifest: n_caps must count the fourth (network) cap"
if hasstr "$TENANT" 'peer: Some("drv.network.0"),'; then
    bad "N1. tenant manifest: the network cap must never peer at drv.network.0 — the system partition's sidecar (the system-peer tooth)"
fi
has "$MF" 'fn tenant_profile_is_budget_console_ramdisk_and_kernel_socket(' ||
    bad "N1. the pin test must be renamed to count caps and name the kernel socket"
has "$MF" 'assert_eq!(peer, "kernel.net.socket",' ||
    bad "N1. the pin test must assert the peer is exactly kernel.net.socket (the positive half of the rotated pin)"
ALINE="$(grep -F 'for absent in [' "$MF" | head -1)"
if [ -z "$ALINE" ]; then
    bad "N1. the pin test's absent-list is gone — the hardware-absence half of the pin cannot be evaluated"
else
    case "$ALINE" in
        *'"network"'*) bad "N1. the pin test must not list network among the ABSENT caps — it is present now (that list is the old pin)" ;;
    esac
fi
has "$INITE" 'build_posix_manifest_tenant(' ||
    bad "N1. the E3 boot spawn no longer grants the tenant manifest"
has "$ENVM" 'build_posix_manifest_tenant(' ||
    bad "N1. the control plane's ENV_CREATE no longer grants the tenant manifest"
clause "$c1" N1 "tenant manifest: network, rights 0x7, peer kernel.net.socket, n_caps 4, pinned, both callers grant it"

# ── N2. the SYSTEM profile is untouched ───────────────────────────────────
c2=$fail
hasstr "$SYSTEM" 'name: "network",' ||
    bad "N2. system profile: its own network cap is gone — only the TENANT profile was to change"
if ! printf '%s\n' "$SYSTEM" | grep -A4 -F 'name: "network"' | grep -qF 'peer: Some("drv.network.0"),'; then
    bad "N2. system profile: network must keep peering at drv.network.0 (the system sidecar) — deliberately untouched by P2"
fi
clause "$c2" N2 "system profile untouched: network -> drv.network.0 stays where it was"

# ── N3. the mint arm, and the boundary branch a system peer falls to ──────
c3=$fail
has "$CAP" 'sidecar_prefix(sc->peer_name, "kernel.net.socket")' ||
    bad "N3. cap.c: the kernel.net.socket mint arm (beside kernel.env.control/console) is gone"
has "$CAP" 'sidecar_registry_resolve(sc->peer_name,' ||
    bad "N3. cap.c: the E2 scoped-registry branch — what a drv.network.0 peer falls to, refused for crossing partitions — is gone"
clause "$c3" N3 "mint arm present; a system peer still falls to the scoped registry (the boundary)"

# ── N4. the registration carries the caller's identity ────────────────────
c4=$fail
has "$CAP" 'net_socket_service_register(k_rd, k_wr,' ||
    bad "N4. cap.c: the mint arm must register the channel with the service"
ARM="$(grep -A8 -F 'sidecar_prefix(sc->peer_name, "kernel.net.socket")' "$CAP" 2>/dev/null)"
hasstr "$ARM" 'pd->partition_id,' ||
    bad "N4. registration must carry the caller's partition"
hasstr "$ARM" 'pd->pid, pd->name' ||
    bad "N4. registration must carry the sidecar's pid and name — the refusal names the caller with them"
clause "$c4" N4 "registration carries partition, pid and name under the kernel-owned name"

# ── N5. console_service skips the socket channel's kernel slot ────────────
c5=$fail
has "$CONS" '#include "net_socket_service.h"' ||
    bad "N5. console_service.c: the service's header is not included"
has "$CONS" 'net_socket_service_kernel_slot((uint16_t)s)' ||
    bad "N5. console_service.c: the socket channel's kernel slot must be skipped — NET_* frames are not console text and must not reach the serial transcript"
clause "$c5" N5 "console_service skips the socket channel's kernel slot"

# ── N6. the tick runs in the non-IRQ service poll, with env_console_tick ──
c6=$fail
has "$MMK" '#include "net_socket_service.h"' ||
    bad "N6. microkernel.c: the service's header is not included"
NT="$(grep -nF 'net_socket_service_tick();' "$MMK" | head -1 | cut -d: -f1)"
ET="$(grep -nF 'env_console_tick();' "$MMK" | head -1 | cut -d: -f1)"
if [ -z "$NT" ]; then
    bad "N6. microkernel.c: the service is never ticked — registered channels would never be drained"
elif [ -n "$ET" ] && [ "$NT" -le "$ET" ]; then
    bad "N6. microkernel.c: the socket tick must run in the service poll with (after) env_console_tick, not somewhere else"
fi
clause "$c6" N6 "net_socket_service_tick() runs in the non-IRQ service poll beside env_console_tick()"

# ── N7. the Makefile compiles the service ─────────────────────────────────
c7=$fail
has "$MKF" 'kernel/net_socket_service.c' ||
    bad "N7. Makefile: kernel/net_socket_service.c is not compiled into the kernel"
clause "$c7" N7 "the service is compiled into the kernel"

# ── N8. the wire refuses by name, and NET_INFO answers honestly ───────────
c8=$fail
has "$SVC" 'refused by kernel.net.socket' ||
    bad "N8. net_socket_service.c: the refusal does not name the service"
has "$SVC" 'nss_wire_u16(reply + 12, NSS_ERR_FLAG);' ||
    bad "N8. net_socket_service.c: the refusal reply must carry NET_FLAG_ERROR"
has "$SVC" 'nss_wire_u16(reply + 16, status);' ||
    bad "N8. net_socket_service.c: the refusal reply's status must be written (NET_ERR_CAP via the status arg)"
has "$SVC" '#define NSS_STATUS_CAP 9u' ||
    bad "N8. net_socket_service.c: NET_ERR_CAP must stay 9 (the status the client parses)"
has "$SVCH" '#define NSS_MAX_SOCKS 8u' ||
    bad "N8. net_socket_service.h: NSS_MAX_SOCKS must be a real cap (P2 increment 2 — sockets now admit, not 0)"
has "$SVC" 'nss_wire_u32(reply + 16, NSS_MAX_SOCKS);' ||
    bad "N8. net_socket_service.c: NET_INFO must answer max_sockets the real NSS_MAX_SOCKS"
has "$SVC" 'net_socket_verb_name(' ||
    bad "N8. net_socket_service.c: the verb-name table (the refusal's 'by name') is gone"
clause "$c8" N8 "the wire refuses the not-yet-built verbs by name; NET_INFO answers the real max_sockets"

# ── N9. the host test links the real service and pins the rendering ───────
c9=$fail
has "$TST" 'kernel/net_socket_service.c' ||
    bad "N9. the host test must link the REAL service (kernel/net_socket_service.c), not a reimplementation"
has "$TST" 'refused by kernel.net.socket — not admitted in P2 increment 3' ||
    bad "N9. the host test must pin the exact refusal rendering the tick logs and the live clause greps"
clause "$c9" N9 "the host test links the real service and pins the one refusal rendering"

# ── N10. the ADMIT path: the client verbs reach the kernel stack, attributed
# and quota-checked at the connect ────────────────────────────────────────
# This is the increment's heart. Every element the connect's admit path
# depends on is pinned at the source: the verbs dispatch to net/tcp.c, the
# caller's partition is attributed the moment a connection exists, and the
# quota is read from the module that owns it BEFORE that moment. A service
# that connected without attributing, or skipped the quota read, would let a
# tenant's flood count against nobody — the exact "denial looks like
# absence" failure this guard exists to catch.
c10=$fail
for v in tcp_connect tcp_send tcp_recv tcp_close; do
    has "$SVC" "$v(" ||
        bad "N10. net_socket_service.c: the outbound path no longer reaches the kernel stack ($v)"
done
has "$SVC" 'tcp_conn_attribute_partition(' ||
    bad "N10. net_socket_service.c: the connect does not attribute the caller's partition (tcp_conn_attribute_partition)"
has "$SVC" 'tcp_partition_get_conn_quota(' ||
    bad "N10. net_socket_service.c: the connect does not read the partition's quota"
has "$SVC" 'tcp_partition_get_conn_usage(' ||
    bad "N10. net_socket_service.c: the connect does not read the partition's usage"
has "$SVC" 'tcp_conn_release(' ||
    bad "N10. net_socket_service.c: a closed socket does not release its attribution (tcp_conn_release)"
clause "$c10" N10 "the outbound verbs reach the kernel stack; the connect is attributed and quota-checked before the connection exists"

# ── N11. the admission counters and the quota module are present ─────────
# The unattributed tooth's teeth need somewhere to bite: an ADMIT and a
# quota REFUSAL must each be independently countable, and the quota module
# (net/tcp_quota.c, the real attribution+quota logic) must be compiled into
# the kernel — not reimplemented, not stubbed out.
c11=$fail
has "$SVC" 'nss_admits_total' ||
    bad "N11. net_socket_service.c: the admission counter (nss_admits_total) is gone — an admit would be uncountable"
has "$SVC" 'nss_quota_refusals_total' ||
    bad "N11. net_socket_service.c: the quota-refusal counter is gone — a quota deny would be uncountable"
has "$MKF" 'net/tcp_quota.c' ||
    bad "N11. Makefile: net/tcp_quota.c (the attribution + quota module) is not compiled into the kernel"
has "$TQ" 'tcp_conn_attribute_partition' ||
    bad "N11. net/tcp_quota.c: the attribution entry point is gone"
has "$TST" 'net/tcp_quota.c' ||
    bad "N11. the host test must link the REAL net/tcp_quota.c (the attribution + quota logic under test)"
clause "$c11" N11 "the admission counters are present and the quota module is compiled in (and linked by the host test)"

# ── N12. the LISTENER verbs (P2 increment 3): bind/listen/accept ─────────
# The other half of §6's "client and listener". bind/listen/accept run
# against the KERNEL's own stack (net/tcp.c) — the bind claims the port on
# the channel's own socket, then tcp_listen and tcp_accept reach the stack —
# never a stub and never drv.network.0. A listener is the inbound face of the
# same quota discipline the connect (N10) enforces outbound: an accepted
# connection is admitted and charged the moment it is handed out, so a
# tenant's inbound flood cannot count against nobody either.
# The listener itself is deliberately UNattributed until accept — attribution
# binds the instant a real inbound conn exists (same admit-at-the-moment rule
# as the connect path). What keeps a listener partition-scoped is that it is
# opened on this channel's socket (sk), which is per-(partition,pid).
c12=$fail
# Anchored to CODE, not comments: `int lid = tcp_listen(` is the listen
# call-site, and the accept's conn scan (`c->state == TCP_ESTABLISHED &&`) is
# the non-blocking pickup loop that replaces tcp_accept()'s spin. A bare
# `tcp_listen(`/`tcp_accept(` grep would be satisfied by the prose above it.
has "$SVC" 'int lid = tcp_listen(' ||
    bad "N12. net_socket_service.c: the listen no longer draws a listener slot from the stack (tcp_listen)"
has "$SVC" 'c->state == TCP_ESTABLISHED &&' ||
    bad "N12. net_socket_service.c: the accept no longer scans for a ready conn (the non-blocking pickup loop)"
has "$SVC" 'tcp_conn_attribute_partition(found' ||
    bad "N12. net_socket_service.c: the accept does not attribute the accepted conn to the caller's partition (the inbound half of the unattributed tooth)"
has "$SVC" 'sk->lport = port;' ||
    bad "N12. net_socket_service.c: the bind no longer claims the port on the channel's own socket (sk->lport)"
clause "$c12" N12 "bind claims the port, listen/accept reach the kernel stack, and the accept attributes the accepted conn"

# ── N13. each partition's listener is reachable only from inside it ──────
# The isolation property the whole increment rests on: the service keys
# every socket — listener and accepted — to the CALLER's partition
# (sk->partition at bind/listen time, e->partition at accept), so a
# listener belongs to exactly the partition whose sidecar opened it. A
# listener that carried no partition, or carried the wrong one, would be
# reachable across partitions — the boundary violation E2's scoped
# registry refuses at creation and this service must not reintroduce on
# the data path.
c13=$fail
has "$SVC" 'sk->lport' ||
    bad "N13. net_socket_service.c: the listener is no longer bound to this channel's own socket (sk->lport) — it would not be partition-scoped"
has "$SVC" 'tcp_conn_attribute_partition(found, e->partition)' ||
    bad "N13. net_socket_service.c: the accept no longer charges the accepted conn to the CALLER's own partition (e->partition)"
clause "$c13" N13 "each listener is bound on its own channel's socket and its accepted conns are charged to the caller's partition (no cross-partition reach)"

# ── N14. the descriptor's line: listener CONFIG, never a conn id ─────────
# P1a's checkpoint descriptor (the ENV_REGISTER reply, user/proto's
# RegisterReply) carries the environment's identity, its three regions and
# its four messenger endpoints — and NOT one socket or connection id. There
# is no port/listener field either: the listener is re-established by
# re-running the applet over the restored channels, not carried as a bound
# socket. Networking does not survive a checkpoint: a restore re-establishes
# the listener from scratch with every socket closed. A descriptor that named
# a live conn_id would be exactly the "half-open socket restored" failure §6
# says is not a thing to restore, and would drag TCP state into P1a's
# reviewers' lap.
c14=$fail
has "$PROTO" 'pub struct RegisterReply' ||
    bad "N14. user/proto/env_proto.rs: the descriptor's wire shape (RegisterReply) is gone"
DESC="$(fn_body "$PROTO" 'pub struct RegisterReply {')"
if hasstr "$DESC" 'conn_id'; then
    bad "N14. the descriptor must never carry a connection id — a restore re-establishes the listener from scratch with every socket closed (§6's descriptor angle)"
fi
if hasstr "$DESC" 'listener'; then
    bad "N14. the descriptor must not carry a live listener handle — it carries listener CONFIGURATION, not a bound socket"
fi
if hasstr "$DESC" 'socket'; then
    bad "N14. the descriptor must not carry a socket id — networking does not survive a checkpoint"
fi
hasstr "$DESC" 'chans' ||
    bad "N14. the descriptor must still carry its messenger endpoints (the config a restore DOES re-establish)"
# Honest scope: the descriptor carries the channels a restore re-wires, and
# the listener is re-established by re-running the applet over them — there is
# no port/listener field to carry. What the clause pins is the NEGATIVE: no
# conn id, socket id, or listener handle ever enters this record.
clause "$c14" N14 "the descriptor carries the messenger channels and never a connection id / socket / listener handle"

# ── N15. the quota surface the live arm drives (P2 increment 4) ──────────
# §6's plan sets the quota "through the surface that already exists — no new
# API": syscalls 277/278, POST /api/partition/connquota, and the operator
# shell pair. This clause pins every link that surface needs so the live arm
# (L5) can set a quota and read it back: the syscall numbers, their dispatch,
# both HTTP routes with the exact body keys the arm posts, and the shell
# commands an operator would use instead.
c15=$fail
has "$TQH" '#define SYS_SLS_PARTITION_CONN_QUOTA_SET  277' ||
    bad "N15. net/tcp_quota.h: SYS_SLS_PARTITION_CONN_QUOTA_SET (277) is gone"
has "$TQH" '#define SYS_SLS_PARTITION_CONN_QUOTA_LIST 278' ||
    bad "N15. net/tcp_quota.h: SYS_SLS_PARTITION_CONN_QUOTA_LIST (278) is gone"
has "$SYSD" 'SYS_SLS_PARTITION_CONN_QUOTA_SET' ||
    bad "N15. syscall_dispatch.c: the conn-quota syscall is no longer dispatched"
has "$SYSD" 'SYS_SLS_PARTITION_CONN_QUOTA_LIST' ||
    bad "N15. syscall_dispatch.c: the conn-quota LIST syscall is no longer dispatched"
has "$HTTP" '"/api/partition/connquota"' ||
    bad "N15. net/http.c: the POST /api/partition/connquota route is gone — the live arm has no way to set A's quota"
has "$HTTP" '"/api/partition/connquotas"' ||
    bad "N15. net/http.c: the GET /api/partition/connquotas route is gone — the live arm has no way to read a quota back"
has "$HTTP" 'json_int(body, "partition_id")' ||
    bad "N15. net/http.c: the connquota POST no longer reads partition_id — the arm's body would be silently ignored"
has "$HTTP" 'json_int(body, "quota")' ||
    bad "N15. net/http.c: the connquota POST no longer reads quota — the arm's body would be silently ignored"
has "$USH" 'partition connquota set ' ||
    bad "N15. user/shell.c: the operator's 'partition connquota set' command is gone"
has "$USH" 'partition connquotas' ||
    bad "N15. user/shell.c: the operator's 'partition connquotas' readback is gone"
clause "$c15" N15 "the quota surface exists end to end: syscalls 277/278, dispatch, both HTTP routes with the arm's body keys, shell commands"

# ── N16. the admit path's quota gate — the starvation clause ─────────────
# The whole point of the phase, at the source: a partition whose clients hold
# many simultaneous connections starves neighbours without ever tripping a
# time-window rate limit (tcp_quota.h's documented hazard). The gate in
# net/tcp_quota.c is what closes it — deny BEFORE attributing, so an
# over-quota connect charges nobody — and its `quota != 0` half is §11 Q4's
# contract that 0 stays unlimited, the BSS-zero-safe default an operator who
# never opted in gets. This is the clause P2's `no-quota` tooth reddens.
c16=$fail
has "$TQ" 'if (quota != 0 && partition_conn_count[partition_id] >= quota) {' ||
    bad "N16. net/tcp_quota.c: the admit path's quota gate is gone — a flood would count against no ceiling, and a starved neighbour is exactly the failure this phase closes"
has "$TQ" 'Over quota: deny before attributing' ||
    bad "N16. net/tcp_quota.c: the gate's deny-before-side-effect posture is no longer documented where it is enforced"
clause "$c16" N16 "the admit path denies over-quota before attributing, and 0 stays unlimited (the no-quota tooth's clause)"

# ── N17/N17b. the flood evidence's wiring: CI runs the live arm ──────────
# P1b's S13 shape, point for point. A source clause cannot flood anything —
# but it can insist that the job that CAN does. §10's P2 gate IS the live
# arm; without this clause the flood evidence runs only when someone runs it
# by hand. Two halves, each with its own name: the invocation (present at
# all) and its order after the ISO build.
c17=$fail
if ! has "$CIW" "env_net_isolation_check.sh --live"; then
    bad "N17. ci.yml never invokes this guard's live arm — the flood evidence would run only by hand, and §10's P2 gate would not be exercised on any push"
else
    ok "N17. ci.yml invokes this guard's live arm (the quota-flood evidence is wired into CI)"
    if order_ok "$CIW" "make X86_CC=gcc X86_LD=ld x86-iso" "env_net_isolation_check.sh --live"; then
        ok "N17b. the live arm runs AFTER the ISO build — the boot guard finds its image instead of aborting before it"
    else
        bad "N17b. ci.yml's live arm does not run AFTER the ISO build — a boot guard ahead of its image aborts before booting and proves nothing about the flood"
    fi
fi

# ── the live arm ──────────────────────────────────────────────────────────
if [ "$LIVE" -eq 1 ]; then
    if [ "$fail" -ne 0 ]; then
        note "source clauses already red — not booting (fix the source first)"
    else
        # ── boot: the same QEMU + grub drive the E6 attach guard uses ──────
        ISO="${P2_ISO:-sls_operating_system.iso}"
        LOG=/tmp/aerosls_p2_net_boot.log
        ENTRY="${P2_BOOT_ENTRY:-3}"
        WINDOW_S="${P2_WINDOW_S:-180}"
        WAIT_S="${P2_WAIT_S:-60}"
        ATTACH_WAIT_S="${P2_ATTACH_WAIT_S:-45}"
        SMP="${P2_SMP:-4}"
        # The API's own token/role model: DB_ADMIN, what partition create and
        # an environment's create and console write all require (the same
        # token the other HTTP-driven boot checks use).
        TOKEN=deadbeef01234567cafebabe76543210

        [ -f "$ISO" ] || { echo "ABORT: $ISO missing — build it with: make x86-iso (with sidecars.cpio present)" >&2; exit 2; }
        command -v qemu-system-x86_64 >/dev/null 2>&1 || { echo "ABORT: qemu-system-x86_64 not installed" >&2; exit 2; }
        command -v python3 >/dev/null 2>&1 || { echo "ABORT: python3 not installed (JSON parsing)" >&2; exit 2; }

        SER=/tmp/aerosls_p2_net_ser
        rm -f "$SER.in" "$SER.out" "$LOG"
        mkfifo "$SER.in" "$SER.out" 2>/dev/null || true
        cat "$SER.out" > "$LOG" &
        CATPID=$!

        # Host port: the first free loopback one, never a fixed number.
        PORT=$(bash tests/free_port.sh) || { echo "ABORT: no free loopback port for the QEMU hostfwd (see tests/free_port.sh)" >&2; exit 2; }
        BASE="http://127.0.0.1:$PORT"
        W="$(mktemp -d)"
        qemu_err() { [ -s "$W/qemu.err" ] && sed 's/^/      qemu: /' "$W/qemu.err" >&2; }

        # No disk is attached — this arm asserts nothing about storage (the
        # same choice every console/boot guard makes).
        ACCEL="${QEMU_ACCEL:-}"
        if [ -z "$ACCEL" ]; then
            if [ -e /dev/kvm ] && [ -r /dev/kvm ]; then ACCEL="-accel kvm"; else ACCEL="-accel tcg,thread=multi"; fi
        fi

        qemu-system-x86_64 -cdrom "$ISO" \
            -netdev user,id=net0,hostfwd=tcp:127.0.0.1:$PORT-:3000 \
            -device e1000,netdev=net0,mac=52:54:00:12:34:07 \
            -display none -m 4G -smp "$SMP" -boot d -no-reboot \
            $ACCEL \
            -serial pipe:"$SER" 2>"$W/qemu.err" &
        QPID=$!

        cleanup() { kill ${QPID:-} 2>/dev/null; kill ${CATPID:-} 2>/dev/null; rm -rf ${W:-}; }
        trap cleanup EXIT
        trap 'cleanup; exit 1' TERM INT

        bash tests/grub_select_kernel_only.sh "$SER.in" "$LOG" "$QPID" "$ENTRY" || {
            echo "FAILED: could not select grub entry $ENTRY (QEMU or grub failed)" >&2
            qemu_err
            exit 1
        }

        hb_now() { grep -ac "\[INIT\] heartbeat" "$LOG" 2>/dev/null || true; }
        ready=0
        qemu_alive=1
        for i in $(seq 1 $((WINDOW_S * 2))); do
            if [ "$(hb_now)" -ge 5 ] && grep -aq "Listening on port 3000" "$LOG" 2>/dev/null; then
                ready=1
                break
            fi
            if ! kill -0 "$QPID" 2>/dev/null; then qemu_alive=0; break; fi
            sleep 0.5
        done
        if [ "$ready" -ne 1 ]; then
            if [ "$qemu_alive" -eq 0 ]; then
                echo "FAILED: QEMU exited before the boot reached its markers — the boot crashed" >&2
                qemu_err
            else
                echo "FAILED: markers missing within ${WINDOW_S}s (heartbeats: $(hb_now))" >&2
            fi
            tail -20 "$LOG" 2>/dev/null | sed 's/^/      /' >&2
            exit 1
        fi
        echo "ok:   L0. the unified boot is up (control plane serving, init making progress)"
        # ── HTTP helpers (the same shapes the E6 attach guard uses) ──────
        api() {   # api <out-file> <curl args...> — out file CLEARED first, so
            # a request that never landed reads as empty, never as the PREVIOUS
            # request's stale success.
            local out="$1"; shift
            : > "$out"
            local t
            for t in 1 2 3 4 5; do
                if curl -sf --max-time 30 -H "Authorization: Bearer $TOKEN" \
                        -H "Content-Type: application/json" -o "$out" "$@" 2>/dev/null; then
                    return 0
                fi
                sleep 2
            done
            return 1
        }
        jval() {   # jval <json-file> <key> — the value, or empty when absent/not JSON
            python3 - "$1" "$2" <<'PY' 2>/dev/null
import json, sys
try:
    d = json.load(open(sys.argv[1]))
except Exception:
    print(""); sys.exit(0)
v = d.get(sys.argv[2]) if isinstance(d, dict) else None
if v is None:            print("")
elif v is True:          print("true")
elif v is False:         print("false")
else:                    print(v)
PY
        }
        env_field() {   # env_field <envs.json> <env_id> <field>
            python3 - "$1" "$2" "$3" <<'PY' 2>/dev/null
import json, sys
try:
    d = json.load(open(sys.argv[1]))
except Exception:
    sys.exit(0)
for e in d.get("envs", []):
    if str(e.get("env_id")) == sys.argv[2]:
        v = e.get(sys.argv[3])
        sys.stdout.write("" if v is None else str(v))
        break
PY
        }
        console_out() {   # stdout: the `output` field of $W/cons.json
            python3 - "$W/cons.json" <<'PY' 2>/dev/null
import json, sys
try:
    d = json.load(open(sys.argv[1]))
except Exception:
    sys.exit(0)
v = d.get("output") if isinstance(d, dict) else None
if isinstance(v, str):
    sys.stdout.write(v)
PY
        }
        envs_snapshot() { api "$W/envs.json" "$BASE/api/partition/$pid/env" || true; }

        # ── the tenant: one partition, one environment (tenant profile) ────
        api "$W/health.json" "$BASE/api/health" || true
        if [ "$(jval "$W/health.json" status)" != "ok" ]; then
            echo "FAILED: /api/health did not answer a live body (see $LOG)" >&2
            exit 1
        fi
        api "$W/pcreate.json" -X POST -d '{"name":"p2net"}' "$BASE/api/partitions" || true
        pid="$(jval "$W/pcreate.json" partition_id)"
        if [ "$(jval "$W/pcreate.json" ok)" != "true" ] || [ -z "$pid" ] || [ "$pid" = "0" ] || [ "$pid" = "4294967295" ]; then
            echo "FAILED: POST /api/partitions did not define a partition (error='$(jval "$W/pcreate.json" error)')" >&2
            exit 1
        fi
        api "$W/env.json" -X POST -d '{"index":1}' "$BASE/api/partition/$pid/env" || true
        E1="$(jval "$W/env.json" env_id)"
        if [ "$(jval "$W/env.json" ok)" != "true" ] || [ -z "$E1" ] || [ "$E1" = "0" ]; then
            echo "FAILED: could not create environment 1 in partition $pid (env_id='$E1' error='$(jval "$W/env.json" error)')" >&2
            exit 1
        fi
        echo "ok:   L0. environment placed in partition $pid (env_id $E1, index 1)"

        # Bound: the create round-trips through the manager's channel, so a
        # console that is index-less moments after the create is mid-flight.
        IDX=""; SPID=""
        for i in $(seq 1 120); do
            envs_snapshot
            IDX="$(env_field "$W/envs.json" "$E1" index)"
            SPID="$(env_field "$W/envs.json" "$E1" posix_pid)"
            if [ -n "$IDX" ] && [ -n "$SPID" ]; then break; fi
            sleep 0.5
        done
        if [ "$IDX" != "1" ] || [ -z "$SPID" ]; then
            echo "FAILED: the console never bound to index 1 (index='$IDX' posix_pid='$SPID')" >&2
            exit 1
        fi
        SIDE="aerosls.posix.$IDX"
        echo "ok:   L0. console bound: index $IDX, posix pid $SPID ($SIDE)"

        # ── console plumbing: ONE stream, drained through the attach surface ─
        S1=""
        drain_once() {
            api "$W/cons.json" "$BASE/api/partition/$pid/env/$E1/console" || true
            if [ "$(jval "$W/cons.json" ok)" = "true" ]; then
                S1="$S1$(console_out)"
                return 0
            fi
            return 1
        }
        wait_for() {   # wait_for <marker> <seconds> — 0 once the stream shows it
            local marker="$1" secs="$2" i
            for i in $(seq 1 $((secs * 2))); do
                drain_once || true
                case "$S1" in *"$marker"*) return 0 ;; esac
                sleep 0.5
            done
            return 1
        }
        console_send() {   # console_send <line> — waits for the peer to take it
            local line="$1" t ok queued
            python3 - "$line" > "$W/send_body.json" <<'PY'
import json, sys
print(json.dumps({"input": sys.argv[1]}))
PY
            for t in $(seq 1 20); do
                api "$W/send.json" -X POST -d "$(cat "$W/send_body.json")" \
                    "$BASE/api/partition/$pid/env/$E1/console" || true
                ok="$(jval "$W/send.json" ok)"
                queued="$(jval "$W/send.json" queued)"
                if [ "$ok" = "true" ] && [ -n "$queued" ] && [ "$queued" != "0" ]; then return 0; fi
                if [ "$ok" = "false" ]; then
                    echo "FAILED: writing to environment console $E1 was refused ('$(jval "$W/send.json" error)')" >&2
                    return 1
                fi
                sleep 1   # ok=true, queued=0: the peer has not drained yet
            done
            echo "FAILED: environment console $E1 never accepted a line (20s queued=0)" >&2
            return 1
        }
        wait_log() {   # wait_log <fixed string> <seconds> — 0 once LOG carries it
            local pat="$1" secs="$2" i
            for i in $(seq 1 $((secs * 4))); do
                grep -aqF -- "$pat" "$LOG" 2>/dev/null && return 0
                kill -0 "$QPID" 2>/dev/null || return 1
                sleep 0.25
            done
            return 1
        }
        # ── L1: the channel registered under the kernel-owned name ────────
        # Registration happens inside cap_create_sidecar at the create above;
        # kernel TX, so it cannot be lost to a console race. Its pid must be
        # the control plane's posix_pid: the kernel's own account of the
        # wiring and the API's must name the same sidecar.
        WIRE_PAT="[NET-SOCKET] $SIDE (partition $pid, pid $SPID) wired: kernel ends"
        if wait_log "$WIRE_PAT" "$WAIT_S"; then
            echo "ok:   L1. the network cap registered under the kernel-owned name ($SIDE, partition $pid, pid $SPID)"
        else
            echo "FAILED: L1. no '[NET-SOCKET] $SIDE (partition $pid, pid $SPID) wired:' in $LOG — the tenant's network cap never reached the service (last serial lines):" >&2
            grep -a "NET-SOCKET\|SIDECAR" "$LOG" 2>/dev/null | tail -10 | sed 's/^/      /' >&2
            exit 1
        fi

        # ── L2: the console's nc reaches the service; the connect is honest ──
        # Increment 2 ADMITS NET_SOCKET (the outbound path), so `nc 192.0.2.1
        # 80` proceeds to the connect. TEST-NET-1 (192.0.2.1) is reserved and
        # must never be reachable, so the connect is refused at the stack /
        # quota gate — the honest refusal, never a silent admit left charging
        # nobody. The marker after it proves the shell ran both commands.
        console_send "nc 192.0.2.1 80" || exit 1
        console_send "echo P2-NC-DONE" || exit 1
        if ! wait_for "P2-NC-DONE" "$ATTACH_WAIT_S"; then
            echo "FAILED: the tenant console never ran the commands (no P2-NC-DONE after ${ATTACH_WAIT_S}s)" >&2
            exit 1
        fi
        echo "ok:   L0. the tenant console ran nc (marker P2-NC-DONE seen)"

        # The service logged SOME line naming this caller — proving nc's
        # frame reached it. What that line says is checked next: it must be
        # the honest admission/refusal, never silence.
        REF_PAT="[NET-SOCKET] $SIDE (partition $pid, pid $SPID): "
        if ! wait_log "$REF_PAT" "$WAIT_S"; then
            echo "FAILED: L2. no admission/refusal line for $SIDE (partition $pid, pid $SPID) in $LOG after ${WAIT_S}s — nc never reached the service" >&2
            grep -a "NET-SOCKET" "$LOG" 2>/dev/null | tail -10 | sed 's/^/      /' >&2
            exit 1
        fi
        # A refusal (the expected outcome for an unreachable TEST-NET-1
        # address) must still name the service — increment 1's by-name
        # contract, kept for every verb this increment does not admit or
        # cannot reach. A connect that SUCCEEDED would instead be counted
        # admitted (nss_admits_total) and attributed; either way the service
        # answered, which is the property L2 pins.
        CALLLINE="$(grep -aF "$REF_PAT" "$LOG" | head -1)"
        case "$CALLLINE" in
            *"refused by kernel.net.socket"*)
                echo "ok:   L2. nc's call was answered by name ($CALLLINE)"
                ;;
            *)
                echo "FAILED: L2. the service's line is not an admission or a by-name refusal: $CALLLINE" >&2
                exit 1
                ;;
        esac

        # ── L3: every handshake got the honest NET_INFO answer ─────────────
        # A service that never replies makes each sidecar's bounded retry loop
        # print this line before giving up; the honest answer must keep it
        # absent for every environment in the boot, E3's included.
        if grep -aqF "[POSIX] NETBOOT FAILED" "$LOG"; then
            echo "FAILED: L3. an environment's NET_INFO handshake went unanswered — the service must answer it honestly:" >&2
            grep -aF "[POSIX] NETBOOT FAILED" "$LOG" | tail -5 | sed 's/^/      /' >&2
            fail=1
        else
            echo "ok:   L3. no environment reports NETBOOT FAILED — every handshake was answered"
        fi

        # ── L4: two partitions, each listener reachable only from inside it ──
        # §6's isolation property, live. Partition A holds the environment E1
        # above. We now create partition B and, inside it, an environment that
        # opens a TCP listener on port 8081 (bind+listen) through its OWN
        # console. Then, from A's console, we run `nc <guest> 8081`: a connect
        # that would have to cross the partition boundary. The service keys
        # every socket to the CALLER's partition (N13), so A's outbound conn
        # can never be admitted to B's listener — it must be answered BY NAME,
        # the refusal, never a silent cross-partition admit. A listener that A
        # could reach would be exactly the boundary E2's scoped registry
        # refuses at creation, and this clause is the data-path half.
        #
        # Guest address: the QEMU user-net default 10.0.2.15 (the guest's own
        # static IP, include/config.h KERNEL_STATIC_IP); the hostfwd above only
        # forwards host->guest:3000, so 10.0.2.15:8081 is the guest's own
        # listener, reached from inside the guest — never from the host, which
        # is the point: the reach must originate in partition A's console.
        #
        # NOTE: this clause is source-verified in the guard's N13 and its teeth;
        # it is LIVE here so a boot sees the whole property hold end-to-end.
        GUEST_IP="${P2_GUEST_IP:-10.0.2.15}"
        LISTEN_PORT="${P2_LISTEN_PORT:-8081}"
        api "$W/pcreate_b.json" -X POST -d '{"name":"p2net-b"}' "$BASE/api/partitions" || true
        pidb="$(jval "$W/pcreate_b.json" partition_id)"
        if [ "$(jval "$W/pcreate_b.json" ok)" != "true" ] || [ -z "$pidb" ] || [ "$pidb" = "0" ] || [ "$pidb" = "4294967295" ]; then
            echo "FAILED: could not create partition B (error='$(jval "$W/pcreate_b.json" error)')" >&2
            exit 1
        fi
        api "$W/envb.json" -X POST -d '{"index":1}' "$BASE/api/partition/$pidb/env" || true
        EB="$(jval "$W/envb.json" env_id)"
        if [ "$(jval "$W/envb.json" ok)" != "true" ] || [ -z "$EB" ] || [ "$EB" = "0" ]; then
            echo "FAILED: could not create B's environment (env_id='$EB' error='$(jval "$W/envb.json" error)')" >&2
            exit 1
        fi
        BIDX=""; BSPID=""
        for i in $(seq 1 120); do
            envsb="$W/envsb.json"; api "$envsb" "$BASE/api/partition/$pidb/env" || true
            BIDX="$(python3 - "$envsb" "$EB" index <<'PY' 2>/dev/null
import json, sys
for e in (json.load(open(sys.argv[1])).get("envs", []) if True else []):
    if str(e.get("env_id")) == sys.argv[2]: print(e.get("index") or ""); break
PY
)"
            BSPID="$(python3 - "$envsb" "$EB" posix_pid <<'PY' 2>/dev/null
import json, sys
for e in (json.load(open(sys.argv[1])).get("envs", []) if True else []):
    if str(e.get("env_id")) == sys.argv[2]: print(e.get("posix_pid") or ""); break
PY
)"
            if [ -n "$BIDX" ] && [ -n "$BSPID" ]; then break; fi
            sleep 0.5
        done
        if [ -z "$BIDX" ] || [ -z "$BSPID" ]; then
            echo "FAILED: B's console never bound (index='$BIDX' posix_pid='$BSPID')" >&2
            exit 1
        fi
        SIDE_B="aerosls.posix.$BIDX"
        echo "ok:   L0. partition B placed (pid $pidb, env $EB, console $SIDE_B)"

        # ── B opens its listener through its OWN console ─────────────────────
        b_console_send() {   # same shape as console_send, against partition B
            local line="$1" t ok queued
            python3 - "$line" > "$W/sendb_body.json" <<'PY'
import json, sys
print(json.dumps({"input": sys.argv[1]}))
PY
            for t in $(seq 1 20); do
                api "$W/sendb.json" -X POST -d "$(cat "$W/sendb_body.json")" \
                    "$BASE/api/partition/$pidb/env/$EB/console" || true
                ok="$(jval "$W/sendb.json" ok)"; queued="$(jval "$W/sendb.json" queued)"
                if [ "$ok" = "true" ] && [ -n "$queued" ] && [ "$queued" != "0" ]; then return 0; fi
                if [ "$ok" = "false" ]; then echo "FAILED: writing to B's console was refused ('$(jval "$W/sendb.json" error)')" >&2; return 1; fi
                sleep 1
            done
            echo "FAILED: B's console never accepted a line (20s queued=0)" >&2; return 1
        }
        b_console_send "nc -l $LISTEN_PORT" || exit 1
        b_console_send "echo P2-B-LISTEN-ARMED" || exit 1
        # B's listener registration reaches the service under B's identity.
        B_WIRE_PAT="[NET-SOCKET] $SIDE_B (partition $pidb, pid $BSPID) wired: kernel ends"
        if ! wait_log "$B_WIRE_PAT" "$WAIT_S"; then
            echo "FAILED: L4. B's network cap never registered under its own name (no '$B_WIRE_PAT')" >&2
            grep -a "NET-SOCKET" "$LOG" 2>/dev/null | tail -10 | sed 's/^/      /' >&2
            exit 1
        fi
        echo "ok:   L4. partition B's listener armed on :$LISTEN_PORT ($SIDE_B, partition $pidb)"

        # ── A reaches for B's listener: the cross-partition refusal ─────────
        # A's `nc <GUEST_IP> <port>` resolves to B's listener from inside the
        # guest. Because the service charges every socket to the CALLER's
        # partition and B's listener is keyed to B, A's connect must be
        # answered BY NAME and never admitted into B. The marker proves A's
        # shell ran the command; the service line for A's identity is what
        # carries the verdict.
        console_send "nc $GUEST_IP $LISTEN_PORT" || exit 1
        console_send "echo P2-A-CROSS-DONE" || exit 1
        if ! wait_for "P2-A-CROSS-DONE" "$ATTACH_WAIT_S"; then
            echo "FAILED: L4. A's console never ran the cross-partition nc (no P2-A-CROSS-DONE)" >&2
            exit 1
        fi
        A_REF_PAT="[NET-SOCKET] $SIDE (partition $pid, pid $SPID): "
        if ! wait_log "$A_REF_PAT" "$WAIT_S"; then
            echo "FAILED: L4. no admission/refusal line for A ($SIDE, partition $pid) after its cross-partition nc" >&2
            grep -a "NET-SOCKET" "$LOG" 2>/dev/null | tail -10 | sed 's/^/      /' >&2
            exit 1
        fi
        # The service must have ANSWERED A — either an honest by-name refusal
        # (the expected outcome: A cannot reach into B) or an admission that
        # is still charged to A's own partition. What must NEVER happen is
        # silence, and what must never be logged is a line showing A's conn
        # admitted under B's identity. The by-name refusal is the property
        # §6 names, so that is the asserted outcome.
        A_LAST="$(grep -aF "$A_REF_PAT" "$LOG" | tail -1)"
        case "$A_LAST" in
            *"refused by kernel.net.socket"*)
                echo "ok:   L4. A's cross-partition reach was answered BY NAME (never admitted into B): $A_LAST"
                ;;
            *)
                echo "FAILED: L4. A's cross-partition reach was not a by-name refusal — it may have crossed into B: $A_LAST" >&2
                exit 1
                ;;
        esac

        # ── L5: the bounds measured — quota on A, the flood refused, B serves ──
        # §6's verification plan in full (P2 increment 4). The surface is the
        # one that already exists (N15): POST /api/partition/connquota sets a
        # partition's max concurrent connections; 0 = unlimited, the module's
        # BSS-zero-safe default. Topology: partition A gains a SECOND
        # environment (E2) whose console arms a loopback listener on :8082;
        # A's first environment (E1) then connects to it. Both endpoints are
        # inside A, so both conns are A's to charge: the outbound connect is
        # admitted (usage 0→1 against the quota of 1) and the ACCEPT is the
        # excess — refused BY NAME, the pending conn closed, charged to
        # nobody (deny before side-effect). Meanwhile partition B — quota 0,
        # read back — serves a client of its own: B's usage goes live and
        # stays there while A's flood is refused. "B keeps serving
        # throughout" is the whole point of the phase.
        # HONEST SCOPE: the loopback leg is guest→guest (10.0.2.15:8082 via
        # QEMU user-net). If user-net does not hairpin the guest's own
        # address, the flood never reaches the accept gate and this arm FAILS
        # BY NAME below rather than passing vacuously.

        # ── a second environment in A (the flood's far end) and one in B ───
        api "$W/env2.json" -X POST -d '{"index":2}' "$BASE/api/partition/$pid/env" || true
        E2="$(jval "$W/env2.json" env_id)"
        if [ "$(jval "$W/env2.json" ok)" != "true" ] || [ -z "$E2" ] || [ "$E2" = "0" ]; then
            echo "FAILED: could not create A's second environment (env_id='$E2' error='$(jval "$W/env2.json" error)')" >&2
            exit 1
        fi
        E2IDX=""; E2SPID=""
        for i in $(seq 1 120); do
            envs_snapshot
            E2IDX="$(env_field "$W/envs.json" "$E2" index)"
            E2SPID="$(env_field "$W/envs.json" "$E2" posix_pid)"
            if [ -n "$E2IDX" ] && [ -n "$E2SPID" ]; then break; fi
            sleep 0.5
        done
        if [ -z "$E2IDX" ] || [ -z "$E2SPID" ]; then
            echo "FAILED: A's second console never bound (index='$E2IDX' posix_pid='$E2SPID')" >&2
            exit 1
        fi
        SIDE2="aerosls.posix.$E2IDX"
        echo "ok:   L0. A's second environment placed (env $E2, console $SIDE2, partition $pid)"

        api "$W/envb2.json" -X POST -d '{"index":2}' "$BASE/api/partition/$pidb/env" || true
        EB2="$(jval "$W/envb2.json" env_id)"
        if [ "$(jval "$W/envb2.json" ok)" != "true" ] || [ -z "$EB2" ] || [ "$EB2" = "0" ]; then
            echo "FAILED: could not create B's second environment (env_id='$EB2' error='$(jval "$W/envb2.json" error)')" >&2
            exit 1
        fi
        EB2IDX=""; EB2SPID=""
        for i in $(seq 1 120); do
            api "$W/envsb2.json" "$BASE/api/partition/$pidb/env" || true
            EB2IDX="$(env_field "$W/envsb2.json" "$EB2" index)"
            EB2SPID="$(env_field "$W/envsb2.json" "$EB2" posix_pid)"
            if [ -n "$EB2IDX" ] && [ -n "$EB2SPID" ]; then break; fi
            sleep 0.5
        done
        if [ -z "$EB2IDX" ] || [ -z "$EB2SPID" ]; then
            echo "FAILED: B's second console never bound (index='$EB2IDX' posix_pid='$EB2SPID')" >&2
            exit 1
        fi
        SIDE_B2="aerosls.posix.$EB2IDX"
        echo "ok:   L0. B's second environment placed (env $EB2, console $SIDE_B2, partition $pidb)"

        # cons_send <partition> <env_id> <line> — the console-write shape
        # console_send/b_console_send duplicate, parameterised over ANY
        # environment (L5 drives four of them).
        cons_send() {
            local p="$1" e="$2" line="$3" t ok queued
            python3 - "$line" > "$W/sendg_body.json" <<'PY'
import json, sys
print(json.dumps({"input": sys.argv[1]}))
PY
            for t in $(seq 1 20); do
                api "$W/sendg.json" -X POST -d "$(cat "$W/sendg_body.json")" \
                    "$BASE/api/partition/$p/env/$e/console" || true
                ok="$(jval "$W/sendg.json" ok)"
                queued="$(jval "$W/sendg.json" queued)"
                if [ "$ok" = "true" ] && [ -n "$queued" ] && [ "$queued" != "0" ]; then return 0; fi
                if [ "$ok" = "false" ]; then
                    echo "FAILED: writing to environment $e console was refused ('$(jval "$W/sendg.json" error)')" >&2
                    return 1
                fi
                sleep 1
            done
            echo "FAILED: environment $e console never accepted a line (20s queued=0)" >&2
            return 1
        }
        # cq_field <partition-id> <quota|usage> — the readback the arm asserts on.
        cq_field() {
            python3 - "$W/cq.json" "$1" "$2" <<'PY' 2>/dev/null
import json, sys
try:
    d = json.load(open(sys.argv[1]))
except Exception:
    print(""); sys.exit(0)
for c in d.get("connquotas", []):
    if str(c.get("partition_id")) == sys.argv[2]:
        k = "conn_quota" if sys.argv[3] == "quota" else "conn_usage"
        v = c.get(k)
        print("" if v is None else v); break
PY
        }

        # A's second console arms the loopback listener (blocks in accept —
        # its console is spent from here, which is fine: every later A-side
        # action is driven from E1 or the API).
        cons_send "$pid" "$E2" "nc -l 8082" || exit 1
        sleep 4   # the applet's socket/bind/listen round is silent on serial; settle before the client dials

        # ── set A's quota to 1 and read BOTH partitions back ──────────────
        python3 - "$pid" > "$W/cqset_body.json" <<'PY'
import json, sys
print(json.dumps({"partition_id": int(sys.argv[1]), "quota": 1}))
PY
        api "$W/cqset.json" -X POST -d "$(cat "$W/cqset_body.json")" "$BASE/api/partition/connquota" || true
        if [ "$(jval "$W/cqset.json" ok)" != "true" ]; then
            echo "FAILED: L5. POST /api/partition/connquota did not set A's quota (error='$(jval "$W/cqset.json" error)')" >&2
            exit 1
        fi
        api "$W/cq.json" "$BASE/api/partition/connquotas" || true
        if [ "$(cq_field "$pid" quota)" != "1" ]; then
            echo "FAILED: L5. A's conn quota does not read back as 1 (got '$(cq_field "$pid" quota)')" >&2
            exit 1
        fi
        BQ="$(cq_field "$pidb" quota)"
        if [ -n "$BQ" ] && [ "$BQ" != "0" ]; then
            echo "FAILED: L5. B was expected to hold NO quota (0 = unlimited), read back '$BQ'" >&2
            exit 1
        fi
        echo "ok:   L5. quota set on A (=1) and read back; B holds none (0 = unlimited, unchanged)"

        # ── A's flood: the client connect is ADMITTED, the accept is the ──
        # excess and must be refused BY NAME. The refusal is logged under
        # E2's identity (the channel driving the accept) — partition A's own.
        # E2's earlier bind/listen lines also carry its identity, so the wait
        # requires the LATEST line for E2 to be the by-name refusal.
        cons_send "$pid" "$E1" "nc 10.0.2.15 8082" || exit 1
        A_FLOOD_PAT="[NET-SOCKET] $SIDE2 (partition $pid, pid $E2SPID): "
        FLOOD_OK=0
        for i in $(seq 1 $((WAIT_S * 4))); do
            if grep -aF "$A_FLOOD_PAT" "$LOG" 2>/dev/null | tail -1 | grep -qF "refused by kernel.net.socket"; then
                FLOOD_OK=1; break
            fi
            kill -0 "$QPID" 2>/dev/null || break
            sleep 0.25
        done
        if [ "$FLOOD_OK" -ne 1 ]; then
            echo "FAILED: L5. A's excess accept was never refused by name (last line for $SIDE2: '$(grep -aF "$A_FLOOD_PAT" "$LOG" 2>/dev/null | tail -1)') — the flood did not reach the accept gate (is the guest's own address hairpinned by user-net?)" >&2
            grep -a "NET-SOCKET" "$LOG" 2>/dev/null | tail -10 | sed 's/^/      /' >&2
            exit 1
        fi
        echo "ok:   L5. A's excess accept was refused BY NAME while A sat at its quota (charged to nobody)"

        # ── B keeps serving throughout: a B client connects and its accept ──
        # SUCCEEDS while A is quota-blocked. B holds no quota (0 = unlimited,
        # read back above), so the observable is B's conn usage going LIVE:
        # the outbound conn plus the listener's accepted conn, both charged to
        # B and held open by the two relaying endpoints.
        cons_send "$pidb" "$EB2" "nc 10.0.2.15 8081" || exit 1
        B_SERVED=0; BU=""
        for i in $(seq 1 $((ATTACH_WAIT_S * 2))); do
            api "$W/cq.json" "$BASE/api/partition/connquotas" || true
            BU="$(cq_field "$pidb" usage)"
            case "$BU" in *[1-9]*) B_SERVED=1; break ;; esac
            kill -0 "$QPID" 2>/dev/null || break
            sleep 0.5
        done
        if [ "$B_SERVED" -ne 1 ]; then
            echo "FAILED: L5. B never served during A's flood (B conn_usage stayed '$BU') — B-keeps-serving did not hold" >&2
            exit 1
        fi
        # And B's identity must carry NO refusal: the flood's whole point.
        if grep -a "NET-SOCKET" "$LOG" 2>/dev/null | grep -F "partition $pidb" | grep -qF "refused by kernel.net.socket"; then
            echo "FAILED: L5. B's channels were refused during A's flood — A's quota leaked into B's service:" >&2
            grep -a "NET-SOCKET" "$LOG" | grep -F "partition $pidb" | grep -F "refused" | tail -5 | sed 's/^/      /' >&2
            exit 1
        fi
        echo "ok:   L5. B kept serving throughout (conn_usage=$BU live while A's flood was refused; B never refused)"
        echo "ok:   L5. the bounds are measured — quota on A enforced BY NAME, B (0 = unlimited) untouched: §10's P2 gate, live"
        :
    fi
fi

exit "$fail"
