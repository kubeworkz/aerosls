#!/usr/bin/env bash
# tests/env_shell_surface_check.sh — proves the shell half of POSIX-Environments
# E6 on the real target: on the E1 UNIFIED boot (grub menu entry 3 — the Ring-0
# control plane and the Ring-3 sidecar world alive in ONE boot), the kernel
# shell's `env create` / `env list` / `env attach` / `env destroy` reach an
# environment IN a partition, and the environment is the one they address.
#
# ─── Why this exists ───────────────────────────────────────────────────────
# E6 is the phase that makes an environment *reachable* (roadmap G7: "an
# environment you cannot reach is not an environment"). Its first half — a
# per-environment console (`kernel/env_console.c`) plus the HTTP attach route —
# shipped with its own guard (tests/env_console_attach_check.sh). §9 named the
# rest explicitly and left it unbuilt: "`env list`, `env attach`, `env destroy`
# in the shell and in `aeroslsctl`, with the `docs/COMMANDS.md` entries
# `commands_doc_check.sh` enforces". This is that half's guard.
#
# It is not decoration over the HTTP guard, because the shell surface is a
# DIFFERENT path into the same state. `commands_doc_check.sh` proves the verbs
# are *documented*; nothing until this file proved they *work*, and a shell verb
# that prints a plausible line without reaching the kernel is exactly the
# failure a documentation check cannot see.
#
# ─── What it asserts, and why each one needs a boot ────────────────────────
#   1. GET /api/health answers a live body (the control plane is serving).
#   2. POST /api/partitions defines the partition the environment will live in.
#   3. POST /api/partition/{id}/env creates the environment in it (the E4 round
#      trip), returning the manager's `env_id`.
#   4. `env list <partition>` (through POST /api/shell/exec, the SAME
#      sls_shell_execute() dispatch the serial console drives) reports
#      `live` >= 1 and a row whose `env_id` EQUALS the one the HTTP create
#      returned, whose `index` is the requested one, and whose `posix_pid` is
#      the environment's POSIX sidecar. The id tie is the assertion: the shell
#      reads the kernel's console registry (env_console_entry), the API reports
#      what env_service_create's reply bound, and only a real binding makes
#      those the same number.
#   5. `env attach <partition> <env_id>` returns the environment's OWN boot
#      announcement — `[env-id] index=<i> pid=<posix_pid>` (BIB v3, E6's in-band
#      identity) — with the pid that step 4 listed. That is the claim list/attach
#      cannot make about itself from inside one table: the environment says who
#      it is, on the console the shell attached to.
#   6. A line sent through `env attach <partition> <env_id> <line>` RUNS in that
#      environment and its output comes back through the same surface: the
#      environment's shell sets a variable to a marker generated in this guard
#      and only this guard knows, and `echo $VAR | cat` must produce it on a
#      later drain. A marker the guard typed into a request could not prove
#      execution; a marker the environment composed from its own state can.
#   7. `env destroy <partition> <env_id>` answers `ok`, the kernel logs the
#      environment ending, and the console LEAVES the listing (`live` 0, no row
#      with that env_id) within a bounded wait — a destroy that answers ok while
#      the console lingers is the leak env_console.h's own contract forbids.
#
# Prerequisite: sls_operating_system.iso (make x86-iso; the unified entry needs
# sidecars.cpio present — commit it, so CI ships it) and qemu-system-x86_64.
#
# GUARD-KIND: runtime (needs the built ISO + QEMU).
#
# Teeth: tests/env_shell_surface_check_smoke.sh — it points this guard at inputs
# that must turn it red and then requires it to pass on the real boot. Three are
# TOOTH arms that withhold an action so the guard's own assertion has to fail:
#   E6S_TOOTH=no-env      skip the HTTP create; the `env list` assertion MUST fail
#   E6S_TOOTH=no-input    skip the two `env attach … <line>` sends; the marker
#                         assertion MUST fail (a guard that never runs anything
#                         and still claims forwarding is exactly what this catches)
#   E6S_TOOTH=no-destroy  skip the destroy; the post-destroy assertion MUST fail
# The rest are inputs that lack the property: the kernel-only boot (no env
# manager and no control plane to drive) and a missing ISO (rc 2).
#
# Env knobs (used by the smoke; the defaults are what every other caller gets):
#   E6S_ISO          path to the ISO to boot      (default sls_operating_system.iso)
#   E6S_BOOT_ENTRY   1-based grub MENU POSITION   (default 3 = the unified entry)
#   E6S_WINDOW_S     seconds to wait for boot markers (default 120)
#   E6S_ATTACH_WAIT_S seconds to poll for the environment's output (default 60)
#   E6S_DESTROY_WAIT_S seconds for a destroyed console to leave the listing (default 45)
#   E6S_TOOTH        smokes only: no-env | no-input | no-destroy (see above)
#
# Exit: 0 if every assertion held, 1 if one failed (or QEMU died first),
# 2 on a missing prerequisite.
set -u

cd "$(dirname "$0")/.."   # repo root

ISO="${E6S_ISO:-sls_operating_system.iso}"
LOG=/tmp/aerosls_env_shell_boot.log
ENTRY="${E6S_BOOT_ENTRY:-3}"     # 1 = Phase-5 initrd boot, 2 = kernel-only,
                                 # 3 = unified (the 1-based menu position)
WINDOW_S="${E6S_WINDOW_S:-120}"
ATTACH_WAIT_S="${E6S_ATTACH_WAIT_S:-60}"
DESTROY_WAIT_S="${E6S_DESTROY_WAIT_S:-45}"
TOOTH="${E6S_TOOTH:-}"

# The API's own token/role model: DB_ADMIN, which an environment create/destroy
# and a console write all require (tools/aeroslsctl's default token — the same
# one every other HTTP-driven boot check uses).
TOKEN=deadbeef01234567cafebabe76543210

[ -f "$ISO" ] || { echo "ABORT: $ISO missing — build it with: make x86-iso (with sidecars.cpio present)" >&2; exit 2; }
command -v qemu-system-x86_64 >/dev/null 2>&1 || { echo "ABORT: qemu-system-x86_64 not installed" >&2; exit 2; }
command -v python3 >/dev/null 2>&1 || { echo "ABORT: python3 not installed (JSON parsing)" >&2; exit 2; }

SER=/tmp/aerosls_env_shell_ser
rm -f "$SER.in" "$SER.out" "$LOG"
mkfifo "$SER.in" "$SER.out" 2>/dev/null || true
cat "$SER.out" > "$LOG" &
CATPID=$!

# Host port: the first free loopback one, never a fixed number (the deploy gate
# runs sibling checks while live cluster nodes hold 3000+i; tests/free_port.sh
# is the repo's shared allocator).
PORT=$(bash tests/free_port.sh) || { echo "ABORT: no free loopback port for the QEMU hostfwd (see tests/free_port.sh)" >&2; exit 2; }
BASE="http://127.0.0.1:$PORT"
W="$(mktemp -d)"
qemu_err() { [ -s "$W/qemu.err" ] && sed 's/^/      qemu: /' "$W/qemu.err" >&2; }

# No disk is attached, deliberately — the same choice tests/env_create_boot_check.sh
# makes: this check asserts nothing about storage, and a private fresh image
# would add the kernel's NVMe bring-up and TLS-CA persistence path to a boot
# whose subject is the environment surface.
ACCEL="${QEMU_ACCEL:-}"
if [ -z "$ACCEL" ]; then
    if [ -e /dev/kvm ] && [ -r /dev/kvm ]; then
        ACCEL="-accel kvm"
    else
        ACCEL="-accel tcg,thread=multi"
    fi
fi

qemu-system-x86_64 -cdrom "$ISO" \
    -netdev user,id=net0,hostfwd=tcp:127.0.0.1:$PORT-:3000 \
    -device e1000,netdev=net0,mac=52:54:00:12:34:04 \
    -display none -m 4G -smp 4 -boot d -no-reboot \
    $ACCEL \
    -serial pipe:"$SER" 2>"$W/qemu.err" &
QPID=$!

cleanup() { kill ${QPID:-} 2>/dev/null; kill ${CATPID:-} 2>/dev/null; rm -rf ${W:-}; }
trap cleanup EXIT
trap 'cleanup; exit 1' TERM INT

# Select the unified entry (grub's serial menu; see the helper's header).
bash tests/grub_select_kernel_only.sh "$SER.in" "$LOG" "$QPID" "$ENTRY" || {
    echo "FAILED: could not select grub entry $ENTRY (QEMU or grub failed)" >&2
    qemu_err
    exit 1
}

hb_now() { grep -ac "\[INIT\] heartbeat" "$LOG" 2>/dev/null || true; }

# ── Phase 1: wait until the boot is far enough along to assert on ───────────
# The two contradiction branches are the guard's honesty checks (same shape as
# tests/env_create_boot_check.sh): a boot that is not the unified boot is this
# guard pointed at the wrong thing, and they name that as soon as the kernel
# makes it visible instead of waiting out the window for the same verdict.
ready=0
qemu_alive=1
for i in $(seq 1 $((WINDOW_S * 2))); do
    if grep -aq "\[BOOT\] command line:" "$LOG" 2>/dev/null &&
       ! grep -aq "unified=1" "$LOG" 2>/dev/null; then
        echo "FAILED: grub menu entry $ENTRY booted without unified=1 on the kernel command line (this is not the unified boot)" >&2
        grep -a "\[BOOT\] command line:" "$LOG" | head -1 | sed 's/^/      /' >&2
        exit 1
    fi
    if grep -aq "Listening on port 3000" "$LOG" 2>/dev/null &&
       ! grep -aq "\[E1\] control plane planted" "$LOG" 2>/dev/null; then
        echo "FAILED: grub menu entry $ENTRY reached its HTTP listener without planting the Ring-0 control plane — the environment manager cannot be driven from this boot" >&2
        exit 1
    fi
    if [ "$(hb_now)" -ge 5 ] && grep -aq "Listening on port 3000" "$LOG" 2>/dev/null; then
        ready=1
        break
    fi
    if ! kill -0 "$QPID" 2>/dev/null; then
        qemu_alive=0
        break
    fi
    sleep 0.5
done

fail=0
report_and_exit() {
    echo "FAIL  the E6 shell surface (env create/list/attach/destroy over the unified boot)" >&2
    echo "      E1/INIT/ENV/ENV-CONSOLE/SIDECAR/PARTITION lines seen:" >&2
    grep -a "\[E1\]\|\[INIT\]\|\[ENV\]\|\[SIDECAR\]\|\[PARTITION\]" "$LOG" 2>/dev/null | tail -30 | sed 's/^/      /' >&2
    echo "      last 20 serial lines:" >&2
    tail -20 "$LOG" 2>/dev/null | sed 's/^/      /' >&2
    exit 1
}

if [ "$ready" -ne 1 ]; then
    if [ "$qemu_alive" -eq 0 ]; then
        echo "FAILED: QEMU exited before the unified boot reached its markers — the boot crashed" >&2
        qemu_err
    else
        echo "FAILED: markers missing within ${WINDOW_S}s (heartbeats: $(hb_now), yields: $(grep -ac '\[E1\] yield' "$LOG" 2>/dev/null))" >&2
    fi
    report_and_exit
fi
echo "ok:   the unified boot is up (control plane serving, init making progress)"

# ── HTTP helpers ───────────────────────────────────────────────────────────
# Every request retries: one refused connection is a host-side hiccup (the
# guest's accept path is Ring-0 work on a CPU shared with Ring 3), while a
# control plane that is genuinely not serving fails every attempt.
api() {   # api <out-file> <curl args...>
    local out="$1"; shift
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

# ── The shell passthrough ──────────────────────────────────────────────────
# POST /api/shell/exec runs the FULL sls_shell_execute() dispatch (user/shell.c)
# — the same one the serial console drives — and returns its captured output.
# That is why this guard is a real test of the shell verbs and not just of the
# HTTP layer: a command that the dispatch did not recognise comes back with
# `recognized` false and no output, which the assertions below would catch as a
# missing line rather than passing quietly.
shell_all="$W/shell_all.txt"
: > "$shell_all"
SHELL_OUT=""
SHELL_RECOG=""
sh_exec() {   # sh_exec <command>
    python3 - "$1" > "$W/shell_body.json" <<'PY'
import json, sys
print(json.dumps({"command": sys.argv[1]}))
PY
    api "$W/shell.json" -X POST -d "$(cat "$W/shell_body.json")" \
        "$BASE/api/shell/exec" || true
    SHELL_RECOG="$(jval "$W/shell.json" recognized)"
    SHELL_OUT="$(python3 - "$W/shell.json" <<'PY' 2>/dev/null
import json, sys
try:
    d = json.load(open(sys.argv[1]))
except Exception:
    sys.exit(0)
sys.stdout.write(d.get("output") or "")
PY
)"
    SHELL_OUT="$(printf '%s' "$SHELL_OUT" | tr -d '\r')"
    printf '=== %s\n%s\n' "$1" "$SHELL_OUT" >> "$shell_all"
}

# ── Phase 2: the control plane answers with a live body ────────────────────
api "$W/health.json" "$BASE/api/health" || true
if [ "$(jval "$W/health.json" status)" = "ok" ]; then
    echo "ok:   /api/health answers with a live kernel body"
else
    echo "FAILED: /api/health did not answer a live body (see $LOG)" >&2
    fail=1
fi

# ── Phase 3: a partition, then an environment in it ────────────────────────
pname="e6shell"
api "$W/pcreate.json" -X POST -d "{\"name\":\"$pname\"}" "$BASE/api/partitions" || true
pid="$(jval "$W/pcreate.json" partition_id)"
if [ "$(jval "$W/pcreate.json" ok)" = "true" ] && [ -n "$pid" ] && [ "$pid" != "0" ] && [ "$pid" != "4294967295" ]; then
    echo "ok:   POST /api/partitions defined '$pname' as partition $pid"
else
    echo "FAILED: POST /api/partitions did not define a partition (ok='$(jval "$W/pcreate.json" ok)' id='$pid')" >&2
    jval "$W/pcreate.json" error >&2 || true
    fail=1
fi

E1=""
if [ "$TOOTH" = "no-env" ]; then
    # The tooth's input: the environment is never created, so every assertion
    # that needs one MUST fail. A guard whose `env list` arm was vacuous (say it
    # accepted an empty listing) would pass here, and the smoke would catch it.
    echo "note: E6S_TOOTH=no-env — not creating an environment; the list/attach assertions below MUST fail"
else
    api "$W/env.json" -X POST -d '{"index":1}' "$BASE/api/partition/$pid/env" || true
    E1="$(jval "$W/env.json" env_id)"
    if [ "$(jval "$W/env.json" ok)" = "true" ] && [ -n "$E1" ] && [ "$E1" != "0" ] \
       && [ "$(jval "$W/env.json" partition)" = "$pid" ]; then
        echo "ok:   POST /api/partition/$pid/env created env $E1 in partition $pid"
    else
        echo "FAILED: the environment manager did not create an environment in partition $pid (ok='$(jval "$W/env.json" ok)' env_id='$E1')" >&2
        jval "$W/env.json" error >&2 || true
        grep -a "\[ENV\]\|\[CS\]\|\[SIDECAR\]" "$LOG" | tail -10 | sed 's/^/      /' >&2
        fail=1
    fi
fi

# ── Phase 4: `env list` — the shell reads the kernel registry ──────────────
# The row's env_id is tied to the HTTP create's env_id, which is the assertion
# that the shell is reporting the environment that actually exists rather than
# printing a shape. `index` is the identity the create asked for, and
# `posix_pid` is carried forward because phase 5 ties the environment's own
# announcement to it.
PX_PID=""
LIST_ROW=""
list_env() {   # list_env — sets LIST_LIVE, LIST_ROW
    sh_exec "env list $pid"
    LIST_LIVE="$(printf '%s' "$SHELL_OUT" | grep -o 'live=[0-9]*' | head -1 | cut -d= -f2)"
    LIST_ROW="$(printf '%s' "$SHELL_OUT" | grep -F "[ENV] env partition=$pid env_id=$E1 " | head -1)"
}
list_env
if [ -n "$E1" ]; then
    if [ -n "$LIST_ROW" ]; then
        echo "ok:   \`env list $pid\` reports the environment: ${LIST_ROW#\[ENV\] env }"
        PX_PID="$(printf '%s' "$LIST_ROW" | grep -o 'posix_pid=[0-9]*' | cut -d= -f2)"
        if [ "$(printf '%s' "$LIST_ROW" | grep -o 'index=[0-9]*' | cut -d= -f2)" = "1" ]; then
            echo "ok:   ...its index is the one the create asked for (1)"
        else
            echo "FAILED: \`env list\` reported the wrong index (row: $LIST_ROW)" >&2
            fail=1
        fi
        case "$LIST_LIVE" in
            ''|0) echo "FAILED: \`env list $pid\` reported live=$LIST_LIVE while an environment exists" >&2; fail=1 ;;
            *)    echo "ok:   ...and live=$LIST_LIVE" ;;
        esac
        # The relationship the HTTP create reported and the shell reports are
        # the same number, which is what a real env_id binding looks like: the
        # registry knows the id env_service_create's reply carried.
        if [ -n "$PX_PID" ] && [ "$PX_PID" != "0" ]; then
            echo "ok:   ...carrying posix_pid=$PX_PID (the environment's POSIX sidecar)"
        else
            echo "FAILED: \`env list\` reported no usable posix_pid (row: $LIST_ROW)" >&2
            fail=1
        fi
    else
        echo "FAILED: \`env list $pid\` does not report env_id=$E1 (live=$LIST_LIVE). Output was:" >&2
        printf '%s\n' "$SHELL_OUT" | sed 's/^/      /' >&2
        fail=1
    fi
else
    # On the no-env tooth there is no id to look for; the property is that the
    # listing does NOT invent one, so `live` must be 0 and no row may appear.
    if [ "$LIST_LIVE" = "0" ] && [ -z "$LIST_ROW" ]; then
        echo "FAILED: \`env list\` reported no environment — but this arm withheld only the create, so this is the tooth's input, not a pass" >&2
        fail=1
    else
        echo "ok:   (no-env tooth) the listing is empty without a create — see the tooth note above"
    fi
fi

# ── Phase 5: `env attach` — the environment's own account of itself ────────
# The in-band identity E6 added for exactly this: the sidecar's own boot line,
# emitted on its own console, naming the index the kernel parsed from its name
# and the pid the kernel gave it. Step 4 listed that pid from the kernel's
# table; this step reads the environment's own claim through the shell's attach
# and requires them to agree. A shell that drained the wrong console, or a
# registry that filed this environment's console under another's address, moves
# one side and not the other.
ATTACH_OUT=""
attach_drain() {   # attach_drain — one `env attach` read, appended to ATTACH_OUT
    sh_exec "env attach $pid $E1"
    ATTACH_OUT="$ATTACH_OUT$SHELL_OUT"
}
if [ -n "$E1" ]; then
    announce=""
    for i in $(seq 1 $((ATTACH_WAIT_S * 2))); do
        attach_drain
        announce="$(printf '%s' "$ATTACH_OUT" | grep -o '\[env-id\] index=1[^$]*' | head -1)"
        [ -n "$announce" ] && break
        sleep 0.5
    done
    if [ -n "$announce" ] && [ -n "$PX_PID" ] \
       && [ "$(printf '%s' "$announce" | tr -d '\r')" = "[env-id] index=1 pid=$PX_PID" ]; then
        echo "ok:   \`env attach $pid $E1\` carries the environment's own announcement ('$announce') and it names the pid \`env list\` reported"
    else
        echo "FAILED: \`env attach $pid $E1\` did not carry this environment's own announcement (expected '[env-id] index=1 pid=$PX_PID'; got '${announce:-nothing}' after ${ATTACH_WAIT_S}s) — the console the shell attached to does not belong to this environment" >&2
        fail=1
    fi
fi

# ── Phase 6: a line sent through the shell RUNS in that environment ────────
# The marker is composed by the environment from its own state, so it cannot be
# satisfied by echoing back what the guard typed: the guard sets the variable
# through one `env attach … <line>` call and requires the value out of a later
# drain. `env_console_write` refuses a line while the peer still holds the
# previous one (the kernel's flow control), so the send retries — the same
# discipline tests/env_console_attach_check.sh's console_send uses on the
# equivalent HTTP route.
MARKER="e6shell-$(date +%s)-$$"
attach_send() {   # attach_send <line>
    local line="$1" t
    if [ "$TOOTH" = "no-input" ]; then
        echo "note: E6S_TOOTH=no-input — withholding '$line'"
        return 0
    fi
    for t in $(seq 1 20); do
        sh_exec "env attach $pid $E1 $line"
        case "$SHELL_OUT" in
            *"no such environment console"*)
                echo "FAILED: \`env attach\` could not reach the environment console for '$line'" >&2
                fail=1; return 1 ;;
            *"input refused"*|*"queued 0 byte"*)
                sleep 1; continue ;;
            *"queued "*"byte"*)
                return 0 ;;
        esac
        sleep 1
    done
    echo "FAILED: the environment never accepted a line through \`env attach\` ('$line' stayed refused for 20s)" >&2
    fail=1
    return 1
}
if [ -n "$E1" ]; then
    attach_send "setenv E6S $MARKER"
    attach_send 'echo $E6S | cat'
    got=0
    for i in $(seq 1 $((ATTACH_WAIT_S * 2))); do
        attach_drain
        case "$ATTACH_OUT" in *"$MARKER"*) got=1; break ;; esac
        sleep 0.5
    done
    if [ "$got" = 1 ]; then
        echo "ok:   a line sent through \`env attach\` ran in the environment and its output came back ('$MARKER')"
    else
        echo "FAILED: a line sent through \`env attach $pid $E1\` never produced its output ('$MARKER' absent after ${ATTACH_WAIT_S}s) — the shell's attach surface forwards lines but nothing runs" >&2
        printf '%s\n' "$ATTACH_OUT" | tail -20 | sed 's/^/      /' >&2
        fail=1
    fi
fi

# ── Phase 7: `env destroy` — and the console really leaves ─────────────────
if [ -n "$E1" ]; then
    if [ "$TOOTH" = "no-destroy" ]; then
        # The tooth's input: the destroy is withheld, so the assertions below
        # MUST fail. They are not tautologies about the guard's own bookkeeping
        # — they read the listing back through the same surface an operator
        # would, so a destroy that answered ok without ending anything fails
        # them too (that is the leak this phase exists to catch).
        echo "note: E6S_TOOTH=no-destroy — not destroying env $E1; the post-destroy assertions below MUST fail"
    else
        sh_exec "env destroy $pid $E1"
        # The `[ENV] destroy …` line specifically, not the first line of the
        # capture: a destroy wakes the sidecars, so the kernel's own log lines
        # (`[CAP] woken …` and friends) land in the same captured output.
        destroy_line="$(printf '%s\n' "$SHELL_OUT" | grep -F "[ENV] destroy" | head -1)"
        case "$SHELL_OUT" in
            *"-> ok"*) echo "ok:   \`env destroy $pid $E1\` ended the environment ($destroy_line)" ;;
            *) echo "FAILED: \`env destroy $pid $E1\` did not answer ok (output: $SHELL_OUT)" >&2; fail=1 ;;
        esac
        # No kernel-side "… ended" assertion here on purpose: env_console.c
        # prints that line only when the console still held bytes nobody had
        # read. A clean destroy is therefore SILENT by design, and grepping for
        # the line would report a missing marker where the absence is correct.
        # The listing check below is the property itself.
    fi
    gone=0
    for i in $(seq 1 $((DESTROY_WAIT_S * 2))); do
        list_env
        if [ "$LIST_LIVE" = "0" ] && [ -z "$LIST_ROW" ]; then gone=1; break; fi
        sleep 0.5
    done
    if [ "$gone" = 1 ]; then
        echo "ok:   env $E1 left the listing (\`env list $pid\` reports live=0, no row)"
    else
        echo "FAILED: env $E1 is still listed after ${DESTROY_WAIT_S}s (live=$LIST_LIVE, row: ${LIST_ROW:-none}) — the destroy answered ok but the environment's console lingers" >&2
        fail=1
    fi
else
    echo "note: (no-env tooth) no environment to destroy — see the tooth note above"
fi

# ── Stop the guest (bounded: SIGTERM, 10 s grace, SIGKILL) ─────────────────
kill "$QPID" 2>/dev/null || true
for _i in $(seq 1 10); do
    kill -0 "$QPID" 2>/dev/null || break
    case "$(ps -o stat= -p "$QPID" 2>/dev/null)" in Z*|'') break ;; esac
    sleep 1
done
kill -9 "$QPID" 2>/dev/null || true
wait "$QPID" 2>/dev/null || true
kill "$CATPID" 2>/dev/null || true

[ "$fail" -eq 0 ] || report_and_exit

echo "OK: the shell's environment surface reached a real environment — created over HTTP, listed and attached by id, ran a line, and destroyed with its console retired (E6 shell half)"
exit 0
