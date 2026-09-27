#!/usr/bin/env bash
# tests/runner_host_keep_awake.sh -- hold a Windows power request for the life
# of a self-hosted CI job, so the host cannot enter connected standby mid-job,
# and record (for the next job) any suspend gap that happened anyway.
#
# ─── Why this exists ───────────────────────────────────────────────────────
# On 2026-09-26, run 36270544785 (main, merge commit bc4cb5b) went red with
# kernel-guards cancelled 22 minutes into run_checks.sh, mid
# cap_boot_check.sh, and "The operation was canceled." as the only symptom.
# The cause was nowhere in the job. The Windows Kernel-Power log shows the
# host entering connected standby at 21:46:44Z -- 33 seconds after
# cap_boot_check started -- and leaving it at 22:07:20Z. The WSL VM froze
# with the host, the job stopped renewing, GitHub's run service expired the
# job at 21:56:25Z, and on resume the runner's renewjob got HTTP 404 ("job is
# no longer valid"), so it cancelled its own job (Job result: Abandoned).
# The runner never failed, the guards never failed, and after the resume the
# clock is correct again (WSL resyncs it), so nothing in the job's own output
# says "suspend" -- the evidence lives only in the Windows event log.
#
# A 60-70 minute job (kernel-guards, verify) is exposed to this the whole time
# it runs, and the red it produces is indistinguishable at a glance from a
# real CI failure. This script is the fix, at the job boundary:
#
#   * ARM -- spawn a Windows PowerShell (through WSL interop) that holds
#     SetThreadExecutionState(ES_CONTINUOUS | ES_SYSTEM_REQUIRED) for
#     AEROSLS_KEEP_AWAKE_SECONDS, so connected standby cannot be entered
#     while the job runs. The request is scoped to that process and needs no
#     elevation and changes no settings (it is what media players use to keep
#     a machine awake with the screen off; powercfg /change standby-timeout
#     would need admin, would change the machine permanently, and is
#     deliberately NOT what this does). The runner kills orphans at job end,
#     and the process releases on its own timer if it outlives that -- the
#     worst case is the host staying awake a little longer, the safe side.
#   * PREFLIGHT -- at job start, read the Windows Kernel-Power standby events
#     (506 enter / 507 exit connected standby, 42 sleep / 107 resume) and warn
#     when the host resumed from standby within AEROSLS_KEEP_AWAKE_RESUME_WINDOW
#     seconds: a long job starting then is the one that gets expired if it
#     happens again.
#   * EVIDENCE -- a detached sampler takes the wall clock every
#     AEROSLS_KEEP_AWAKE_SAMPLE_SECONDS; a gap past the sample interval plus
#     AEROSLS_KEEP_AWAKE_SLACK_SECONDS is the suspend signature (nothing else
#     stops a running 15 s sample for over a minute), and it is appended to
#     AEROSLS_KEEP_AWAKE_EVIDENCE with the job's name. The NEXT job's
#     preflight surfaces it -- which matters because the suspended job is
#     usually cancelled by the service and never writes another line.
#
# This step never fails the job: it is a best-effort mitigation, and a host
# without Windows interop (a plain Linux box, the build host) has no
# connected standby to keep the runner out of -- it says so and moves on. A
# mitigation that failed closed would red every job on every non-WSL host,
# a worse bug than the one it prevents.
#
# Modes:
#   start        (the ci.yml step) preflight + arm + sampler, then return
#   check        preflight only (smoke and by hand); exit 0 either way
#   jump E O     evaluate the gap rule on an expected/observed sample pair
#   payload      print the base64 -EncodedCommand string the arm path sends
#
# Test seams (the smoke drives the real parser and rule with these):
#   --events-file F    read standby events from F (the same "<iso-utc> <id>"
#                      lines the live query prints) instead of the host log
#   --evidence-file F  read/append the sampler evidence at F
#   --now-epoch N      treat N as the current time (deterministic windows)
#   --no-bridge        act as if powershell.exe is absent (foreign host)
#
# Env: AEROSLS_KEEP_AWAKE_SECONDS (7200), _SAMPLE_SECONDS (15),
#      _SLACK_SECONDS (60), _RESUME_WINDOW (600), _EVIDENCE_WINDOW (21600),
#      _LOG (/tmp/aerosls-keep-awake.log), _EVIDENCE (/tmp/aerosls-host-standby.log)
#
# Exit: 0 always (a skipped mitigation is not a failure), 2 on a usage error.
set -u
cd "$(dirname "$0")/.." || exit 1

HOLD="${AEROSLS_KEEP_AWAKE_SECONDS:-7200}"
SAMPLE="${AEROSLS_KEEP_AWAKE_SAMPLE_SECONDS:-15}"
SLACK="${AEROSLS_KEEP_AWAKE_SLACK_SECONDS:-60}"
RESUME_WINDOW="${AEROSLS_KEEP_AWAKE_RESUME_WINDOW:-600}"
EVIDENCE_WINDOW="${AEROSLS_KEEP_AWAKE_EVIDENCE_WINDOW:-21600}"
LOG="${AEROSLS_KEEP_AWAKE_LOG:-/tmp/aerosls-keep-awake.log}"
EVIDENCE="${AEROSLS_KEEP_AWAKE_EVIDENCE:-/tmp/aerosls-host-standby.log}"

MODE="${1:-start}"
[ $# -gt 0 ] && shift
EVENTS_FILE=""
NOW_EPOCH=""
NO_BRIDGE=0
POSITIONAL=()
while [ $# -gt 0 ]; do
    case "$1" in
        --events-file)   EVENTS_FILE="${2:?--events-file needs a path}"; shift 2 ;;
        --evidence-file) EVIDENCE="${2:?--evidence-file needs a path}"; shift 2 ;;
        --now-epoch)     NOW_EPOCH="${2:?--now-epoch needs seconds}"; shift 2 ;;
        --no-bridge)     NO_BRIDGE=1; shift ;;
        --*) echo "usage: $0 {start|check|jump E O|payload} [--events-file F]" >&2
             echo "       [--evidence-file F] [--now-epoch N] [--no-bridge]" >&2
             exit 2 ;;
        # Leading-dash POSITIONALS reach here too (jump can take a negative
        # observed delta: a backwards clock step makes now-prev negative).
        *)  POSITIONAL+=("$1"); shift ;;
    esac
done

now() { if [ -n "$NOW_EPOCH" ]; then printf '%s\n' "$NOW_EPOCH"; else date +%s; fi; }
# Resolve the Windows bridge once: the name if WSL interop put the Windows
# directories on PATH (the default, appendWindowsPath=true), else the
# absolute path -- a runner with appendWindowsPath=false still gets the
# mitigation instead of a silent skip. none = a host with no Windows here.
resolve_bridge() {
    if [ "$NO_BRIDGE" = 1 ]; then echo none; return; fi
    if command -v powershell.exe >/dev/null 2>&1; then echo powershell.exe; return; fi
    if [ -x /mnt/c/Windows/System32/WindowsPowerShell/v1.0/powershell.exe ]; then
        echo /mnt/c/Windows/System32/WindowsPowerShell/v1.0/powershell.exe
        return
    fi
    echo none
}
BRIDGE="$(resolve_bridge)"
iso_to_epoch() { date -u -d "$1" +%s 2>/dev/null || true; }

# ─── the gap rule (also the sampler's classifier) ──────────────────────────
# A sample that arrives later than its cadence is either slop (a normal +-1 s),
# a suspended guest (the whole VM froze; the next sample lands when it resumes),
# or a clock step backwards (NTP correction, a VM time sync). Only the first is
# normal, and the rule is stated once, here, so jump/payload/sampler agree.
gap_verdict() {
    local expected="$1" observed="$2" gap verdict
    gap=$(( observed - expected ))
    if [ "$gap" -gt "$SLACK" ]; then
        verdict=suspend
    elif [ "$gap" -lt $(( 0 - SLACK )) ]; then
        verdict=step-back
    else
        verdict=normal
    fi
    printf 'gap=%ss verdict=%s\n' "$gap" "$verdict"
}

warn() {   # warn ANNOTATE(1/0) MESSAGE...  -- annotation in the CI step, text always
    local annotate="$1"
    shift
    if [ "$annotate" = 1 ]; then
        printf '::warning title=Runner host standby::%s\n' "$*"
    fi
    printf 'WARN  %s\n' "$*"
}

# ─── PowerShell payloads, as -EncodedCommand strings ───────────────────────
# -EncodedCommand (UTF-16LE base64) is used instead of -Command because the
# quoting that a `[DllImport("kernel32.dll")]` attribute needs survives zero
# layers of cmd/bash/powershell escaping that way -- the first live attempt at
# this exact call died on a mangled backslash-quote. WSL interop runs
# powershell.exe directly; a `-File /tmp/...` path is NOT translated for
# Windows binaries, which is the other reason to carry the payload inline.
encode_ps() {
    command -v iconv >/dev/null 2>&1 || return 1
    command -v base64 >/dev/null 2>&1 || return 1
    printf '%s' "$1" | iconv -f UTF-8 -t UTF-16LE 2>/dev/null | base64 -w 0 2>/dev/null
}

# ES_CONTINUOUS | ES_SYSTEM_REQUIRED = 0x80000000 | 0x1 = 2147483649: "do not
# enter standby, and keep applying this until I say otherwise". The release
# value 2147483648 (ES_CONTINUOUS alone) clears it.
ps_payload_arm() {
    cat <<EOF
\$sig = '[DllImport("kernel32.dll")] public static extern uint SetThreadExecutionState(uint esFlags);'
\$t = Add-Type -MemberDefinition \$sig -Name Awake -Namespace AeroslsHost -PassThru
\$prev = \$t::SetThreadExecutionState(2147483649)
if (\$prev -eq 0) { "arm_FAILED pid=\$PID"; exit 1 }
"armed prev=\$prev pid=\$PID hold=$1s"
Start-Sleep -Seconds $1
[void]\$t::SetThreadExecutionState(2147483648)
"released pid=\$PID"
EOF
}

ps_payload_events() {
    cat <<EOF
Get-WinEvent -FilterHashtable @{LogName='System'; ProviderName='Microsoft-Windows-Kernel-Power'; StartTime=(Get-Date).AddSeconds(-$1)} -MaxEvents 80 -ErrorAction SilentlyContinue | ForEach-Object { '{0} {1}' -f \$_.TimeCreated.ToUniversalTime().ToString('o'), \$_.Id }
EOF
}

collect_events() {   # prints "<iso-utc> <id>" lines; empty when unavailable
    if [ -n "$EVENTS_FILE" ]; then
        cat "$EVENTS_FILE" 2>/dev/null || true
        return 0
    fi
    [ "$BRIDGE" = none ] && return 0
    local enc
    enc="$(encode_ps "$(ps_payload_events "$RESUME_WINDOW")")" || return 0
    [ -n "$enc" ] || return 0
    timeout 25 "$BRIDGE" -NoProfile -EncodedCommand "$enc" 2>>"$LOG" \
        | tr -d '\r' || true
}

# ─── preflight: recent resume (OS truth) + suspend evidence (job history) ──
preflight() {   # preflight ANNOTATE(1/0)
    local annotate="$1" src=none events n=0 resumes=0 unparsed=0 warns=0
    local ts id epoch age tag ets rest egap ejob eepoch eage events_blob

    if [ -n "$EVENTS_FILE" ]; then
        src=file
    elif [ "$BRIDGE" != none ]; then
        src=live
    fi
    events_blob="$(collect_events)"
    local latest_resume="" latest_id="" latest_epoch=""
    while read -r ts id _; do
        [ -n "$ts" ] || continue
        n=$(( n + 1 ))
        epoch="$(iso_to_epoch "$ts")"
        if [ -z "$epoch" ]; then
            unparsed=$(( unparsed + 1 ))
            continue
        fi
        case "$id" in
            507|107)   # 507 exit connected standby, 107 resume from sleep
                resumes=$(( resumes + 1 ))
                latest_resume="$ts"
                latest_id="$id"
                latest_epoch="$epoch"
                ;;
        esac
    done <<EOF
$events_blob
EOF

    printf 'preflight: source=%s events=%s resumes=%s unparsed=%s evidence=%s\n' \
        "$src" "$n" "$resumes" "$unparsed" "$EVIDENCE"

    if [ -n "$latest_epoch" ]; then
        age=$(( $(now) - latest_epoch ))
        if [ "$age" -ge 0 ] && [ "$age" -le "$RESUME_WINDOW" ]; then
            warn "$annotate" "host resumed from standby ${age}s ago (event ${latest_id} ${latest_resume}, window ${RESUME_WINDOW}s) -- a long job started now risks service-side expiry if the host sleeps again."
            warns=$(( warns + 1 ))
        fi
    fi

    if [ -f "$EVIDENCE" ]; then
        while read -r tag ets rest; do
            case "$tag" in SUSPENDED|STEPBACK) ;; *) continue ;; esac
            egap="$(printf '%s\n' "$rest" | sed -n 's/.*gap=\(-\{0,1\}[0-9][0-9]*\)s.*/\1/p')"
            ejob="$(printf '%s\n' "$rest" | sed -n 's/.*job=\([^ ]*\).*/\1/p')"
            eepoch="$(iso_to_epoch "$ets")"
            [ -n "$eepoch" ] || continue
            eage=$(( $(now) - eepoch ))
            if [ "$eage" -ge 0 ] && [ "$eage" -le "$EVIDENCE_WINDOW" ]; then
                if [ "$tag" = SUSPENDED ]; then
                    warn "$annotate" "${EVIDENCE} records a suspend gap during job '${ejob:-unknown}': gap=${egap:-?}s at ${ets} (within the last ${EVIDENCE_WINDOW}s)."
                else
                    warn "$annotate" "${EVIDENCE} records a backwards clock step during job '${ejob:-unknown}': gap=${egap:-?}s at ${ets} (within the last ${EVIDENCE_WINDOW}s)."
                fi
                warns=$(( warns + 1 ))
            fi
        done < "$EVIDENCE"
    fi

    printf 'preflight: warnings=%s\n' "$warns"
    return 0
}

# ─── arm: one detached PowerShell holding the request ──────────────────────
arm() {   # 0 armed, 1 arm failed, 2 no arm line in time, 3/4 unusable host
    [ "$BRIDGE" != none ] || return 3
    local enc before line deadline
    enc="$(encode_ps "$(ps_payload_arm "$HOLD")")" || return 4
    [ -n "$enc" ] || return 4

    before=0
    [ -f "$LOG" ] && before="$(wc -l < "$LOG")"
    printf 'keep-awake: arming hold=%ss at %s\n' "$HOLD" "$(date -u +%Y-%m-%dT%H:%M:%SZ)" >> "$LOG"
    nohup "$BRIDGE" -NoProfile -EncodedCommand "$enc" >>"$LOG" 2>&1 </dev/null &
    deadline=$(( $(date +%s) + 20 ))
    while [ "$(date +%s)" -lt "$deadline" ]; do
        line="$(tail -n +"$((before + 1))" "$LOG" 2>/dev/null \
            | grep -m1 -E '^armed |^arm_FAILED' || true)"
        case "$line" in
            armed*)      printf 'keep-awake: %s\n' "$line"; return 0 ;;
            arm_FAILED*) printf 'keep-awake: %s\n' "$line"; return 1 ;;
        esac
        sleep 1
    done
    return 2
}

# ─── sampler: the suspend evidence, written for the next job to read ───────
sampler() {   # sampler JOB  (detached; writes only to $EVIDENCE)
    local job="$1" prev nowv delta verdict horizon
    prev="$(date +%s)"
    horizon=$(( prev + HOLD + SAMPLE ))
    while [ "$(date +%s)" -le "$horizon" ]; do
        sleep "$SAMPLE" || exit 0
        nowv="$(date +%s)" || exit 0
        delta=$(( nowv - prev ))
        prev="$nowv"
        [ "$delta" -eq "$SAMPLE" ] && continue
        verdict="$(gap_verdict "$SAMPLE" "$delta")"
        case "$verdict" in
            *verdict=normal)   continue ;;
            *verdict=suspend)  printf 'SUSPENDED %s gap=%ss job=%s\n' \
                                   "$(date -u +%Y-%m-%dT%H:%M:%SZ)" "$delta" "${job:-local}" >> "$EVIDENCE" ;;
            *)                 printf 'STEPBACK %s gap=%ss job=%s\n' \
                                   "$(date -u +%Y-%m-%dT%H:%M:%SZ)" "$delta" "${job:-local}" >> "$EVIDENCE" ;;
        esac
    done
}

case "$MODE" in
start)
    printf 'keep-awake: host=%s hold=%ss sample=%ss slack=%ss evidence=%s\n' \
        "$BRIDGE" "$HOLD" "$SAMPLE" "$SLACK" "$EVIDENCE"
    preflight 1
    if [ "$BRIDGE" = none ]; then
        echo "keep-awake: SKIP -- no Windows bridge (powershell.exe) on this host;"
        echo "keep-awake: a plain Linux runner has no connected standby to stay out of."
        exit 0
    fi
    arm
    case $? in
        0)
            # A background job survives the step's bash exiting (the runner
            # reaps the job's orphans at job end), and stdio is detached from
            # the step's pipe so the step does not wait for the sampler.
            ( sampler "${GITHUB_JOB:-local}" ) >>/dev/null 2>&1 </dev/null &
            echo "keep-awake: sampler armed (${SAMPLE}s cadence, slack ${SLACK}s)"
            ;;
        *)
            warn 1 "could not arm the Windows keep-awake on this host -- a host standby mid-job can still expire this job service-side."
            ;;
    esac
    exit 0
    ;;
check)
    preflight 0
    exit 0
    ;;
jump)
    [ "${#POSITIONAL[@]}" -ge 2 ] || { echo "usage: $0 jump EXPECTED OBSERVED" >&2; exit 2; }
    gap_verdict "${POSITIONAL[0]}" "${POSITIONAL[1]}"
    exit 0
    ;;
payload)
    encode_ps "$(ps_payload_arm "$HOLD")"
    echo
    exit 0
    ;;
*)
    echo "usage: $0 {start|check|jump E O|payload} [--events-file F] [--evidence-file F] [--now-epoch N] [--no-bridge]" >&2
    exit 2
    ;;
esac
