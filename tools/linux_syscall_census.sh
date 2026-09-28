#!/usr/bin/env bash
# tools/linux_syscall_census.sh — produce and check the E7 Linux syscall census.
# (docs/AeroSLS-Linux-ABI-Shim-Design-v0.1.md §5.5.)
#
# ─── Why this exists, and why it refuses to run ────────────────────────────
# The H2 roadmap's constraint for the Linux ABI shim is that its syscall surface
# is "chosen by what the target binary actually calls — measured with `strace`,
# not guessed". That makes the census the FIRST deliverable of E7, before any
# shim code. Which raises the failure this tool exists to prevent: a census run
# against whatever binary happened to be lying around — a hello-world, a toy, a
# "representative" program someone wrote to make the table look reasonable.
#
# A census of the wrong binary is worse than no census, because the table it
# produces looks like evidence and is not. So this tool refuses to measure
# anything until a candidate is DESIGNATED, and a designation is not a name —
# it is a name plus a PROVENANCE: the real, existing operation in this tree the
# binary is a rewrite of. A placeholder has no provenance. That is the whole
# mechanism: no provenance, no census.
#
# The candidate file is PARSED, not sourced. It is a configuration file, so it
# has nothing to execute — and a value containing spaces (`CANDIDATE_BUILD=cargo
# build --release ...`) has to be a value, not an assignment followed by a
# command called `build`. It was sourced first, and that is exactly what Bash
# did: every `--check` printed "build: command not found", and the value never
# reached the tool.
#
# ─── What it checks beyond "it ran" ────────────────────────────────────────
# The census is also the first place the §5.1 image contract is enforced on a
# REAL binary rather than a paragraph in a document: the traced executable must
# be statically linked, ET_EXEC (not PIE, not dynamic), with no PT_INTERP and no
# DT_NEEDED. A candidate that fails that is not an E7 candidate, whatever its
# provenance, and this tool says so instead of censusing it.
#
# And the run has to be the candidate's REAL invocation: a census that traces
# `<binary>` with no arguments traces a usage message, which is the placeholder
# failure wearing a different hat. `--run` therefore refuses an empty
# CANDIDATE_ARGS — and refuses it BEFORE it looks for strace, so that refusal is
# testable on a host that has no strace (this one). CANDIDATE_SETUP is the same
# idea for the workload's INPUT: if the run needs a file to exist, the file's
# creation is declared in the designation rather than fabricated by the tool.
#
# ─── How syscalls are counted ──────────────────────────────────────────────
# A full `strace -f -o` trace, not `strace -c`'s summary table: `-c`'s column
# layout shifts depending on whether an `errors` column is present, and a parser
# that silently misreads the count column would produce a plausible, wrong
# histogram — the exact failure mode this tool exists to prevent. The full trace
# is parsed deterministically (take the text before the first `(` on every line
# that looks like a syscall entry). Consequence, stated because it is real: a
# syscall that blocks and is reported as unfinished is counted at ENTRY, so
# counts are accurate for a single-threaded run that is not interrupted, which
# is what E7's v1 targets are.
#
# ─── Modes ─────────────────────────────────────────────────────────────────
#   --check     (default) verify the designation and the checked-in artifact are
#               consistent — that the artifact still belongs to the binary the
#               candidate names, by sha256. This is the mode a guard would run.
#   --run       build the candidate, enforce the image contract, run it under
#               `strace -f`, and write the artifact. Needs `strace` and the
#               candidate's toolchain, so it runs on a Linux host, not in CI's
#               Windows guard set.
#   --selftest  exercise the refusal paths. Every guarantee in this header is a
#               refusal, and a refusal that has never been observed to fire is
#               not a guarantee. Runs anywhere; needs no candidate, no strace.
#
# ─── Not a tests/*_check.sh guard yet, deliberately ────────────────────────
# CI runs `tests/run_checks.sh --require-all`, where an exit-2 guard WITHOUT a
# `# GUARD-KIND:` classification becomes a FAILURE. There is no honest
# classification for "no first user has been designated" — the missing input is
# a business decision, not a build artefact and not a live cluster — so wiring
# this in as a guard today would either redden CI for a reason nothing in CI can
# fix, or add a permanent silent SKIP. The guard and its smoke land with the
# artifact, as a short `tests/linux_abi_census_check.sh` that execs `--check`;
# until then this tool is runnable by hand and by its own selftest.
#
# Exit: 0 pass, 1 fail (the designation or the artifact is wrong), 2 prerequisite
# missing (no candidate designated, no strace, no toolchain).
set -u
cd "$(dirname "$0")/.."   # repo root: every path in this script is repo-relative

CONF="${LINUX_CENSUS_CONF:-tools/linux_abi_candidate.conf}"
ART="${LINUX_CENSUS_ARTIFACT:-tools/linux_syscall_census.txt}"
UNISTD="${LINUX_CENSUS_UNISTD:-/usr/include/x86_64-linux-gnu/asm/unistd_64.h}"

abort() { echo "ABORT: $*" >&2; exit 2; }
fail()  { echo "FAIL: $*" >&2; exit 1; }

MODE=check
for a in "$@"; do
    case "$a" in
        --run)      MODE=run ;;
        --check)    MODE=check ;;
        --selftest) MODE=selftest ;;
        -h|--help)  sed -n '2,60p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *)          abort "unknown argument '$a' (try --help)" ;;
    esac
done

# ─── The designation ───────────────────────────────────────────────────────
# Keys the file is allowed to set. A whitelist rather than `eval`, so a typo
# cannot become a variable assignment and the file cannot touch anything else.
CONF_KEYS="CANDIDATE_STATE CANDIDATE_NAME CANDIDATE_PROVENANCE CANDIDATE_BIN CANDIDATE_TARGET CANDIDATE_BUILD CANDIDATE_ARGS CANDIDATE_SETUP CANDIDATE_VDSO_SYSCALLS"

load_conf() {
    [ -f "$CONF" ] || abort "no candidate file at $CONF.
       The census cannot run without a designated candidate, and a designation
       names a real operation in this tree (CANDIDATE_PROVENANCE), not just a
       binary. See tools/linux_abi_candidate.conf."
    CANDIDATE_STATE="" CANDIDATE_NAME="" CANDIDATE_PROVENANCE="" CANDIDATE_BIN=""
    CANDIDATE_TARGET="" CANDIDATE_BUILD="" CANDIDATE_ARGS="" CANDIDATE_SETUP=""
    CANDIDATE_VDSO_SYSCALLS=""
    local line key val k
    while IFS= read -r line || [ -n "$line" ]; do
        line="${line#"${line%%[![:space:]]*}"}"          # trim leading space
        case "$line" in ''|'#'*) continue ;; esac
        case "$line" in *=*) ;; *) continue ;; esac        # not an assignment
        key="${line%%=*}"
        val="${line#*=}"
        while [ "${val% }" != "$val" ]; do val="${val% }"; done   # trim trailing space
        case "$val" in                                     # optional quotes
            \"*\") val="${val#\"}"; val="${val%\"}" ;;
            \'*\') val="${val#\'}"; val="${val%\'}" ;;
        esac
        for k in $CONF_KEYS; do
            [ "$key" = "$k" ] && { printf -v "$key" '%s' "$val"; break; }
        done
        # An unknown key is warned about rather than ignored: `CANDIDATE_PROVANENCE`
        # would otherwise leave the field empty and fail later with a message
        # about the wrong thing.
        case " $CONF_KEYS " in
            *" $key "*) ;;
            *) echo "WARN: ignoring unknown key '$key' in $CONF" >&2 ;;
        esac
    done < "$CONF"
    : "${CANDIDATE_STATE:=undesignated}"
}

# A usable candidate: designated, named, with provenance that EXISTS. The
# existence check is the anti-placeholder mechanism — it is the one field a
# toy cannot supply.
require_designated() {
    if [ "$CANDIDATE_STATE" != "designated" ]; then
        abort "no candidate designated (CANDIDATE_STATE=$CANDIDATE_STATE in $CONF).
       E7's first user is a decision, not a discovery: AeroSLS-Roadmap-2026H2-v0.1.md
       §5 states there is no named first user, and §5/§7 say the census runs on
       'the first user's actual work' via dogfooding. The recommended candidate
       and the reasoning behind it are in $CONF's own comments. Designate one,
       then re-run."
    fi
    [ -n "$CANDIDATE_NAME" ] || fail "CANDIDATE_NAME is empty in $CONF"
    [ -n "$CANDIDATE_PROVENANCE" ] || fail "CANDIDATE_PROVENANCE is empty in $CONF — a census with no provenance is the placeholder case this tool exists to refuse"
    [ -e "$CANDIDATE_PROVENANCE" ] || fail "CANDIDATE_PROVENANCE '$CANDIDATE_PROVENANCE' does not exist in the tree — the candidate is not a rewrite of a real operation"
    [ -n "$CANDIDATE_BIN" ] || fail "CANDIDATE_BIN is empty in $CONF"
}

# ─── The artifact ──────────────────────────────────────────────────────────
# Format: `# key: value` header lines, then `<number> <name> <count>`. The
# number is the point — the shim's table is keyed by it, and the ABI is numbers,
# not names.
check_artifact() {
    [ -f "$ART" ] || abort "candidate '$CANDIDATE_NAME' is designated but not yet censused:
       no artifact at $ART. Run '$0 --run' on a Linux host with strace."

    local rows bad n
    rows="$(grep -cE '^[0-9]+ [a-z0-9_]+ [0-9]+$' "$ART" || true)"
    bad="$(grep -vcE '^(#|[0-9]+ [a-z0-9_]+ [0-9]+$)' "$ART" || true)"
    [ "$bad" -eq 0 ] || fail "$ART has $bad line(s) that are neither a comment nor '<number> <name> <count>'"

    if [ "$rows" -eq 0 ]; then
        # An empty artifact passing is the vacuous-success case: a census that
        # measured nothing would hand the shim an empty table and look green.
        fail "$ART contains no syscall rows — an empty census is not a census"
    fi

    # Numbers and names must both be unique: a table with two rows for one
    # number is a table whose dispatch cannot be built from it.
    n="$(awk '/^[0-9]+ /{print $1}' "$ART" | sort -n | uniq -d | head -1)"
    [ -z "$n" ] || fail "$ART has duplicate syscall number $n"
    n="$(awk '/^[0-9]+ /{print $2}' "$ART" | sort | uniq -d | head -1)"
    [ -z "$n" ] || fail "$ART has duplicate syscall name '$n'"

    # Provenance and identity are recorded IN the artifact, so it cannot be
    # silently re-pointed at a different designation.
    local art_name art_sha art_target now_sha
    art_name="$(sed -n 's/^# candidate: //p' "$ART" | head -1)"
    art_sha="$(sed -n 's/^# sha256: //p' "$ART" | head -1)"
    art_target="$(sed -n 's/^# target: //p' "$ART" | head -1)"
    [ "$art_name" = "$CANDIDATE_NAME" ] || fail "$ART was produced for candidate '$art_name' but $CONF designates '$CANDIDATE_NAME'"
    [ "$art_target" = "$CANDIDATE_TARGET" ] || fail "$ART was produced for target '$art_target' but $CONF designates '$CANDIDATE_TARGET'"

    # Freshness first, so a stale artifact is reported as stale rather than as
    # whatever other complaint it also happens to have.
    local verified=0
    if [ -f "$CANDIDATE_BIN" ]; then
        now_sha="$(sha256sum "$CANDIDATE_BIN" | cut -d' ' -f1)"
        [ "$now_sha" = "$art_sha" ] || fail "$CANDIDATE_BIN has changed since the census (artifact sha256 $art_sha, binary $now_sha) — re-run '$0 --run'; a stale census is a guess with a timestamp on it"
        verified=1
    fi

    # The vDSO declaration. Required, and required to be a decision: an artifact
    # that simply omits it is the one failure mode that would silently shrink the
    # shim's syscall table, since a missing requirement looks exactly like a
    # requirement that was never needed.
    local vdso
    vdso="$(sed -n 's/^# vdso-backstopped: //p' "$ART" | head -1)"
    [ -n "$vdso" ] || fail "$ART does not record the vDSO decision (a '# vdso-backstopped: <number> <name>' line, or 'none'). This measurement runs on a Linux WITH a vDSO and the shim has none, so the line is the artifact's statement about syscalls it cannot observe."
    local n_vdso=0
    if [ "$vdso" != "none" ]; then
        local vnum vname expect
        while read -r vnum vname; do
            [ -n "$vname" ] || continue
            expect="$(sed -n "s/^#define __NR_${vname}[[:space:]]*\([0-9]*\).*/\1/p" "$UNISTD" | head -1)"
            [ "$expect" = "$vnum" ] || fail "$ART declares '$vnum $vname' vdso-backstopped, but $UNISTD numbers $vname $expect"
            grep -q "^$vnum " "$ART" && fail "$ART lists $vname ($vnum) BOTH as a traced syscall and as vdso-backstopped — it cannot be both, and one of the two is wrong"
            n_vdso=$((n_vdso + 1))
        done < <(sed -n 's/^# vdso-backstopped: //p' "$ART")
        [ "$n_vdso" -gt 0 ] || fail "$ART's vdso-backstopped line is unparseable"
    fi

    if [ "$verified" = 1 ]; then
        echo "census: ok — $rows syscalls + $n_vdso vdso-backstopped, candidate $CANDIDATE_NAME, sha256 re-verified against $CANDIDATE_BIN"
    else
        echo "census: ok — $rows syscalls + $n_vdso vdso-backstopped recorded for $CANDIDATE_NAME"
        echo "        (note: $CANDIDATE_BIN is not built here, so the sha256 was not re-verified)"
    fi
}

# ─── The image contract (§5.1), on a real binary ───────────────────────────
check_image_contract() {
    command -v readelf >/dev/null || abort "readelf not found (binutils)"
    local type interp needed
    type="$(readelf -h "$CANDIDATE_BIN" 2>/dev/null | sed -n 's/ *Type: *//p')"
    case "$type" in
        EXEC*) : ;;
        DYN*)  fail "$CANDIDATE_BIN is ET_DYN (PIE): §5.1 refuses static-PIE in v1 — it needs a load bias and self-relocation that the kernel's ELF64 loader does not do. Build non-PIE (the default for x86_64-unknown-linux-musl and for Go's exe buildmode)." ;;
        *)     fail "$CANDIDATE_BIN has ELF type '$type'; expected EXEC" ;;
    esac
    interp="$(readelf -l "$CANDIDATE_BIN" 2>/dev/null | grep -c 'INTERP' || true)"
    [ "$interp" -eq 0 ] || fail "$CANDIDATE_BIN has a PT_INTERP segment: it is dynamically linked, and §5.1 refuses PT_INTERP up front"
    needed="$(readelf -d "$CANDIDATE_BIN" 2>/dev/null | grep -c '(NEEDED)' || true)"
    [ "$needed" -eq 0 ] || fail "$CANDIDATE_BIN has $needed DT_NEEDED entries: not statically linked"
    echo "image contract: ok — $type, static, no PT_INTERP"
}

# ─── Produce ───────────────────────────────────────────────────────────────
run_census() {
    require_designated
    # The real invocation, or the trace measures a usage message. Checked before
    # strace deliberately, so the refusal is testable without one.
    [ -n "$CANDIDATE_ARGS" ] || fail "CANDIDATE_ARGS is empty in $CONF — a census run with no arguments traces the candidate's usage message, which is the placeholder failure wearing a different hat. Give the workload's real inputs."
    # vDSO-backed syscalls are INVISIBLE to this measurement, and the shim has no
    # vDSO, so they become real calls there. Declare the set, or the literal
    # 'none': an unset key cannot be told apart from an oversight.
    [ -n "$CANDIDATE_VDSO_SYSCALLS" ] || fail "CANDIDATE_VDSO_SYSCALLS is unset in $CONF — this run traces on a Linux that HAS a vDSO, so musl satisfies some calls without trapping (measured: aerobackup's own output filename proves it read the clock, and the trace shows no clock syscall). The shim has no vDSO (design §5.2), so those calls become real there. Declare them, or the literal 'none'."
    command -v strace >/dev/null || abort "strace not found — the census is 'measured with strace, not guessed' (H2 §2 item #2), and there is no substitute that measures SYSCALLS rather than libc calls: a static binary does not use the dynamic loader at all, so LD_PRELOAD-class interception sees nothing."
    [ -f "$UNISTD" ] || abort "kernel syscall-number header not found at $UNISTD — the artifact is keyed by NUMBER, and names alone cannot build a dispatch table"

    # The workload's inputs have to exist, or the trace is of a failure. Setup is
    # DECLARED rather than invented: a tool that quietly created an input file
    # would be measuring its own scaffolding instead of the user's work — the
    # placeholder failure again, one level up. Runs before the build so a wrong
    # invocation is caught without paying for a compile.
    if [ -n "$CANDIDATE_SETUP" ]; then
        echo "setup: $CANDIDATE_SETUP"
        bash -c "$CANDIDATE_SETUP" || fail "candidate setup failed: $CANDIDATE_SETUP"
    fi

    if [ -n "$CANDIDATE_BUILD" ]; then
        echo "building: $CANDIDATE_BUILD"
        bash -c "$CANDIDATE_BUILD" || fail "candidate build failed: $CANDIDATE_BUILD"
    fi
    [ -f "$CANDIDATE_BIN" ] || fail "build produced no $CANDIDATE_BIN"
    check_image_contract

    local tmp trace sha rows=0 name num count vn vnum
    tmp="$(mktemp -d)"
    trap 'rm -rf "${tmp:-}"' EXIT
    trace="$tmp/trace"
    # shellcheck disable=SC2086  # CANDIDATE_ARGS is intentionally word-split
    strace -f -qq -o "$trace" "$CANDIDATE_BIN" $CANDIDATE_ARGS \
        || fail "the candidate did not run cleanly under strace — a census of a failing run measures the failure, not the work"

    # Every traced line is "<pid> name(args) = ret" (or "name(args) = ret" when
    # single-process). Take the text before the first '(' on lines that look
    # like a syscall entry; signal/exit lines have no '('. Then key each name to
    # its NUMBER from the kernel's own header, because the number is what the
    # shim dispatches on.
    : > "$tmp/rows"
    while read -r count name; do
        [ -n "$name" ] || continue
        num="$(sed -n "s/^#define __NR_${name}[[:space:]]*\([0-9]*\).*/\1/p" "$UNISTD" | head -1)"
        [ -n "$num" ] || { echo "WARN: no syscall number for '$name' in $UNISTD — omitted" >&2; continue; }
        printf '%s %s %s\n' "$num" "$name" "$count" >> "$tmp/rows"
        rows=$((rows + 1))
    done < <(sed -E 's/^(\[[0-9]+\][[:space:]]+|[0-9]+[[:space:]]+)//' "$trace" \
             | grep -E '^[a-z_0-9]+\(' \
             | sed -E 's/^([a-z_0-9]+)\(.*/\1/' \
             | sort | uniq -c \
             | awk '{print $1, $2}')

    [ "$rows" -gt 0 ] || fail "the trace produced no syscall rows — an empty census is not a census"

    sha="$(sha256sum "$CANDIDATE_BIN" | cut -d' ' -f1)"
    {
        echo "# AeroSLS E7 Linux syscall census -- generated by tools/linux_syscall_census.sh"
        echo "# candidate: $CANDIDATE_NAME"
        echo "# provenance: $CANDIDATE_PROVENANCE"
        echo "# binary: $CANDIDATE_BIN"
        echo "# target: $CANDIDATE_TARGET"
        echo "# sha256: $sha"
        echo "# host: $(uname -srm)"
        echo "# date: $(date -u +%Y-%m-%dT%H:%M:%SZ)"
        echo "# method: strace -f full trace, entry-counted"
        echo "# columns: <syscall-number> <name> <count>"
    } > "$ART"

    # The declared vDSO backstop, recorded in the artifact rather than kept in
    # the config: the artifact is what the shim's table is built from, so a
    # requirement that lives only in a sibling file is a requirement the table
    # can silently miss.
    if [ "$CANDIDATE_VDSO_SYSCALLS" = "none" ]; then
        echo "# vdso-backstopped: none" >> "$ART"
    else
        for vn in $CANDIDATE_VDSO_SYSCALLS; do
            vnum="$(sed -n "s/^#define __NR_${vn}[[:space:]]*\([0-9]*\).*/\1/p" "$UNISTD" | head -1)"
            [ -n "$vnum" ] || fail "CANDIDATE_VDSO_SYSCALLS names '$vn', which has no number in $UNISTD"
            echo "# vdso-backstopped: $vnum $vn" >> "$ART"
        done
    fi
    sort -n -k1,1 "$tmp/rows" >> "$ART"

    echo "wrote $ART — $rows syscalls, candidate $CANDIDATE_NAME"
    echo "top syscalls:"
    awk '{print $3, $2}' "$tmp/rows" | sort -rn | head -10 | sed 's/^/  /'
}

# ─── Teeth: the refusals above, observed to fire ───────────────────────────
selftest() {
    local tmp pass=0 failn=0
    tmp="$(mktemp -d)"
    trap 'rm -rf "${tmp:-}"' EXIT

    case_one() {  # label, expected-rc, expected-substring, conf-body, [artifact-body]
        local label="$1" want_rc="$2" want_msg="$3" conf="$4" art="${5-}"
        printf '%s\n' "$conf" > "$tmp/c.conf"
        if [ -n "$art" ]; then printf '%s\n' "$art" > "$tmp/a.txt"; else rm -f "$tmp/a.txt"; fi
        local out rc
        out="$(LINUX_CENSUS_CONF="$tmp/c.conf" LINUX_CENSUS_ARTIFACT="$tmp/a.txt" bash "$0" --check 2>&1)"
        rc=$?
        if [ "$rc" != "$want_rc" ]; then
            echo "FAIL: $label — expected rc=$want_rc, got rc=$rc"; echo "$out" | sed 's/^/      /'; failn=$((failn+1)); return
        fi
        if ! printf '%s' "$out" | grep -q "$want_msg"; then
            echo "FAIL: $label — rc correct, message did not contain '$want_msg'"; echo "$out" | sed 's/^/      /'; failn=$((failn+1)); return
        fi
        echo "ok:   $label (rc=$rc)"; pass=$((pass+1))
    }

    local designated='CANDIDATE_STATE=designated
CANDIDATE_NAME=aerobackup
CANDIDATE_PROVENANCE=backup/backup.sh
CANDIDATE_BIN=tools/nonexistent-bin
CANDIDATE_TARGET=x86_64-unknown-linux-musl'

    # 1. The placeholder case: a candidate named but never designated. This is
    #    what makes a census of "something lying around" impossible.
    case_one "undesignated is refused, not censused" 2 "no candidate designated" \
        'CANDIDATE_STATE=undesignated
CANDIDATE_NAME=hello-world'
    # 2. Designated but with NO provenance — a name is not a provenance.
    case_one "designation without provenance is refused" 1 "PROVENANCE is empty" \
        'CANDIDATE_STATE=designated
CANDIDATE_NAME=aerobackup'
    # 3. Provenance that does not exist in the tree — the shape a toy takes.
    case_one "provenance that does not exist is refused" 1 "does not exist in the tree" \
        'CANDIDATE_STATE=designated
CANDIDATE_NAME=toy
CANDIDATE_PROVENANCE=does/not/exist.sh
CANDIDATE_BIN=tools/nonexistent-bin'
    # 4. Properly designated with no artifact yet: a NAMED skip, not a pass,
    #    because passing here would mean passing without inspecting anything.
    case_one "designated but uncensused is a skip, not a pass" 2 "not yet censused" "$designated"
    # 5. An artifact that measured nothing.
    case_one "an empty artifact is not a census" 1 "contains no syscall rows" "$designated" \
        '# candidate: aerobackup
# target: x86_64-unknown-linux-musl
# sha256: 00'
    # 6. A malformed row.
    case_one "a malformed row is refused" 1 "neither a comment nor" "$designated" \
        '# candidate: aerobackup
# target: x86_64-unknown-linux-musl
# sha256: 00
257 openat 12
this is not a row'
    # 7. The anti-rot tooth: an artifact belonging to a DIFFERENT candidate.
    case_one "an artifact from another candidate is refused" 1 "was produced for candidate" "$designated" \
        '# candidate: some-other-thing
# target: x86_64-unknown-linux-musl
# sha256: 00
257 openat 12'
    # 8. Duplicate numbers — a table that cannot become a dispatch.
    case_one "duplicate syscall numbers are refused" 1 "duplicate syscall number" "$designated" \
        '# candidate: aerobackup
# target: x86_64-unknown-linux-musl
# sha256: 00
257 openat 12
257 openat 3'
    # 9. A stale census: the artifact's sha256 no longer describes the binary.
    #    This is the tooth that makes "re-run it" a guarantee rather than advice.
    local stale="$tmp/stale-bin"
    printf 'not the censused bytes\n' > "$stale"
    case_one "a stale artifact is refused" 1 "has changed since the census" \
        'CANDIDATE_STATE=designated
CANDIDATE_NAME=aerobackup
CANDIDATE_PROVENANCE=backup/backup.sh
CANDIDATE_BIN='"$tmp"'/stale-bin
CANDIDATE_TARGET=x86_64-unknown-linux-musl' \
        '# candidate: aerobackup
# target: x86_64-unknown-linux-musl
# sha256: 0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef
257 openat 12'
    # 10. The negative control for 9: the SAME artifact, whose sha256 really
    #     does describe the binary, must PASS — otherwise the tool fails
    #     everything and proves nothing.
    local real_sha
    real_sha="$(sha256sum "$stale" | cut -d' ' -f1)"
    case_one "the same artifact with a matching sha256 passes" 0 "sha256 re-verified" \
        'CANDIDATE_STATE=designated
CANDIDATE_NAME=aerobackup
CANDIDATE_PROVENANCE=backup/backup.sh
CANDIDATE_BIN='"$tmp"'/stale-bin
CANDIDATE_TARGET=x86_64-unknown-linux-musl' \
        '# candidate: aerobackup
# target: x86_64-unknown-linux-musl
# sha256: '"$real_sha"'
# vdso-backstopped: none
257 openat 12'

    # 11. The real-invocation requirement: an argument-less run traces a usage
    #     message, not the workload. Refused before strace is looked for, which
    #     is what lets this tooth bite on a host that has no strace.
    local out rc
    printf '%s\n' 'CANDIDATE_STATE=designated
CANDIDATE_NAME=aerobackup
CANDIDATE_PROVENANCE=backup/backup.sh
CANDIDATE_BIN=tools/nonexistent-bin
CANDIDATE_TARGET=x86_64-unknown-linux-musl
CANDIDATE_ARGS=' > "$tmp/c11.conf"
    out="$(LINUX_CENSUS_CONF="$tmp/c11.conf" LINUX_CENSUS_ARTIFACT="$tmp/a11.txt" bash "$0" --run 2>&1)"; rc=$?
    if [ "$rc" -eq 1 ] && printf '%s' "$out" | grep -q "CANDIDATE_ARGS is empty"; then
        echo "ok:   an argument-less census run is refused (rc=1)"; pass=$((pass+1))
    else
        echo "FAIL: argument-less run — expected rc=1 naming CANDIDATE_ARGS, got rc=$rc"
        printf '%s\n' "$out" | sed 's/^/      /'; failn=$((failn+1))
    fi

    # 12. A multi-word value must survive the parse. This is the tooth for the
    #     bug that made the file parsed rather than sourced: `CANDIDATE_BUILD=
    #     cargo build ...` was an assignment plus a command called `build`, and
    #     every value with spaces in it was lost. A captured CANDIDATE_ARGS gets
    #     past the empty-args check and stops at the missing strace instead (rc
    #     2); an empty one would stop at rc 1 naming CANDIDATE_ARGS.
    printf '%s\n' 'CANDIDATE_STATE=designated
CANDIDATE_NAME=aerobackup
CANDIDATE_PROVENANCE=backup/backup.sh
CANDIDATE_BIN=tools/nonexistent-bin
CANDIDATE_TARGET=x86_64-unknown-linux-musl
CANDIDATE_VDSO_SYSCALLS=none
CANDIDATE_ARGS=--storage /tmp/e7-census/sls_storage.img --kind hourly' > "$tmp/c12.conf"
    out="$(LINUX_CENSUS_CONF="$tmp/c12.conf" LINUX_CENSUS_ARTIFACT="$tmp/a12.txt" bash "$0" --run 2>&1)"; rc=$?
    if [ "$rc" -eq 2 ] && printf '%s' "$out" | grep -q "strace not found"; then
        echo "ok:   a multi-word value survives the parse (rc=2 at strace, not rc=1 at args)"; pass=$((pass+1))
    else
        echo "FAIL: multi-word value — expected rc=2 naming strace, got rc=$rc"
        printf '%s\n' "$out" | sed 's/^/      /'; failn=$((failn+1))
    fi

    # 13. An artifact that is otherwise perfect but never states the vDSO
    #     decision. This is the one omission that would silently SHRINK the
    #     shim's syscall table, because a requirement nobody wrote down looks
    #     exactly like a requirement nobody needed.
    case_one "an artifact with no vDSO decision is refused" 1 "does not record the vDSO decision" \
        'CANDIDATE_STATE=designated
CANDIDATE_NAME=aerobackup
CANDIDATE_PROVENANCE=backup/backup.sh
CANDIDATE_BIN='"$tmp"'/stale-bin
CANDIDATE_TARGET=x86_64-unknown-linux-musl' \
        '# candidate: aerobackup
# target: x86_64-unknown-linux-musl
# sha256: '"$real_sha"'
257 openat 12'

    # 14. And the negative control for 13: the same, with the line present.
    case_one "the same artifact with the vDSO line stated passes" 0 "sha256 re-verified" \
        'CANDIDATE_STATE=designated
CANDIDATE_NAME=aerobackup
CANDIDATE_PROVENANCE=backup/backup.sh
CANDIDATE_BIN='"$tmp"'/stale-bin
CANDIDATE_TARGET=x86_64-unknown-linux-musl' \
        '# candidate: aerobackup
# target: x86_64-unknown-linux-musl
# sha256: '"$real_sha"'
# vdso-backstopped: none
257 openat 12'

    # 15. A vDSO declaration whose number disagrees with the kernel header —
    #     the exact drift that would put the shim's table on the wrong syscall.
    case_one "a vDSO number that disagrees with the header is refused" 1 "numbers clock_gettime" \
        'CANDIDATE_STATE=designated
CANDIDATE_NAME=aerobackup
CANDIDATE_PROVENANCE=backup/backup.sh
CANDIDATE_BIN='"$tmp"'/stale-bin
CANDIDATE_TARGET=x86_64-unknown-linux-musl' \
        '# candidate: aerobackup
# target: x86_64-unknown-linux-musl
# sha256: '"$real_sha"'
# vdso-backstopped: 999 clock_gettime
257 openat 12'

    echo
    echo "linux_syscall_census selftest: $pass passed, $failn failed"
    [ "$failn" -eq 0 ] || exit 1
}

load_conf
case "$MODE" in
    check)    require_designated; check_artifact ;;
    run)      run_census ;;
    selftest) selftest ;;
esac
