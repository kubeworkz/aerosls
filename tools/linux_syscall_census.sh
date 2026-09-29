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
# ─── Why the declared vDSO set is PROVED, not inferred ─────────────────────
# CANDIDATE_VDSO_SYSCALLS is the census's statement about syscalls it cannot
# see: the trace runs on a Linux whose kernel maps a vDSO, and the shim has
# none (§5.2 omits AT_SYSINFO_EHDR). The evidence behind the one entry was a
# FILENAME that is a timestamp, i.e. an inference — and an inference is what
# this tool refuses everywhere else.
#
# `--prove-vdso` measures it instead. It starts the candidate under
# tools/e7_stack_launcher.c, which loads the ET_EXEC itself and builds the
# initial stack, leaving AT_SYSINFO_EHDR out of the auxv; each declared syscall
# must then (a) appear in that trace as a real call AND (b) not appear in the
# census at all. And nothing else may move: the no-vDSO row set must equal the
# census's rows minus the execve that started the target, plus the declared
# calls. `--check` refuses a declaration with no proof beside it, so the
# requirement travels with the artifact instead of living in a doc.
#
# The launcher does not remove the vDSO image — the kernel mapped it when it
# exec'd the LAUNCHER. It removes the auxv ENTRY, which is the only handle musl
# uses to reach one (`__vdsosym` scans `libc.auxv`). That is the condition
# AeroSLS presents, and it is deliberately not overstated as "a machine without
# a vDSO": a libc that found the vDSO by scanning /proc/self/maps would still
# find one here, and the proof would not catch it.
#
# (PTRACE is the obvious way to remove one auxv entry and it cannot work here:
# the run is already traced by strace, and PTRACE_TRACEME while already traced
# fails with EPERM. Hence a launcher, and hence the marker line it writes —
# everything after that marker in the trace is the target's own syscalls, so
# the proof's rows are comparable with the census's row for row.)
#
# ─── Modes ─────────────────────────────────────────────────────────────────
#   --check     (default) verify the designation, the checked-in artifact and
#               the vDSO proof are consistent — that the artifact still belongs
#               to the binary the candidate names, by sha256, and that every
#               declared vDSO backstop was measured. The mode a guard would run.
#   --run       build the candidate, enforce the image contract, run it under
#               `strace -f`, and write the artifact. Needs `strace` and the
#               candidate's toolchain, so it runs on a Linux host, not in CI's
#               Windows guard set.
#   --prove-vdso  run the candidate again with an initial stack that has no
#               AT_SYSINFO_EHDR and write tools/linux_syscall_census_novdso.txt,
#               which states, per declared syscall, that it became a real call
#               while nothing else in the row set changed.
#   --selftest  exercise the refusal paths. Every guarantee in this header is a
#               refusal, and a refusal that has never been observed to fire is
#               not a guarantee. Runs anywhere; needs no candidate, no strace.
#
# ─── Where the guard is, and why it is not this file ───────────────────────
# `--run` and `--check` both refuse to proceed until a candidate is DESIGNATED,
# and there is no honest `# GUARD-KIND:` classification for "no first user has
# been designated yet": that missing input is a business decision, not a build
# artefact and not a live cluster. So this tool was not a guard while it had
# nothing to check — wiring it in then would either have reddened CI for a
# reason nothing in CI could fix, or added a permanent silent SKIP.
#
# That is no longer the state of the tree. The guard is
# tests/linux_abi_census_check.sh, which execs `--check` and adds the two things
# a tool cannot assert about itself: that a pass actually reported its
# conclusions (the vacuity control), and that exiting 2 without an `ABORT:` line
# is a broken instrument rather than an owed skip. It is SOURCE-ONLY — three
# checked-in files plus the kernel's syscall-number header, no strace, no cargo,
# no built binary — so CI checks the artifacts on every push while the two modes
# that MEASURE (`--run`, `--prove-vdso`) stay hand-run on a census host.
# tests/linux_abi_census_check_smoke.sh proves the guard's teeth.
#
# Exit: 0 pass, 1 fail (the designation or the artifact is wrong), 2 prerequisite
# missing (no candidate designated, no strace, no toolchain).
set -u
cd "$(dirname "$0")/.."   # repo root: every path in this script is repo-relative

CONF="${LINUX_CENSUS_CONF:-tools/linux_abi_candidate.conf}"
ART="${LINUX_CENSUS_ARTIFACT:-tools/linux_syscall_census.txt}"
PROOF="${LINUX_CENSUS_PROOF:-tools/linux_syscall_census_novdso.txt}"
LAUNCHER_SRC="${LINUX_CENSUS_LAUNCHER:-tools/e7_stack_launcher.c}"
UNISTD="${LINUX_CENSUS_UNISTD:-/usr/include/x86_64-linux-gnu/asm/unistd_64.h}"

# The kernel's own number for a syscall NAME. The artifact is keyed by number
# because numbers are the ABI, so every name has to come back through here.
nr_of() { sed -n "s/^#define __NR_$1[[:space:]]*\([0-9]*\).*/\1/p" "$UNISTD" | head -1; }

abort() { echo "ABORT: $*" >&2; exit 2; }
fail()  { echo "FAIL: $*" >&2; exit 1; }

MODE=check
for a in "$@"; do
    case "$a" in
        --run)        MODE=run ;;
        --check)      MODE=check ;;
        --prove-vdso) MODE=prove ;;
        --selftest)   MODE=selftest ;;
        # The header IS the manual, and it grows: print it up to the first
        # non-comment line rather than to a line number that goes stale.
        -h|--help)    awk 'NR>1 && $0 !~ /^#/ {exit} NR>1 {sub(/^# ?/, ""); print}' "$0"; exit 0 ;;
        *)            abort "unknown argument '$a' (try --help)" ;;
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

# ─── The vDSO proof ────────────────────────────────────────────────────────
# The claim being checked: "the shim has no vDSO (§5.2), so syscall X is
# satisfied in userspace here and will be a real trap there." The no-vDSO run's
# row set must be the census's row set, minus the execve that started the
# target (the launcher run is exec'd as the LAUNCHER, which is the one thing the
# two runs cannot share), plus every declared syscall as a real call.
# stdout is the row set the proof must have; rc=1 if a declared syscall has no
# row of its own (the callers word that failure, so this stays message-free).
proof_expected() {   # $1 = the proof's own rows
    local proof="$1" dnum dname execve_n rc=0
    execve_n="$(nr_of execve)"
    [ -n "$execve_n" ] || return 1
    awk -v x="$execve_n" '/^[0-9]+ /{ if ($1 != x) print $1, $2, $3 }' "$ART"
    while read -r dnum dname; do
        [ -n "$dname" ] || continue
        if grep -qE "^$dnum $dname [0-9]+$" "$proof"; then
            grep -E "^$dnum $dname [0-9]+$" "$proof"
        else
            rc=1
        fi
    done < <(sed -n 's/^# vdso-backstopped: //p' "$ART")
    return $rc
}

# The offline half: every check here is file-only, which is what lets a guard
# run it on a host with no strace and no candidate binary.
check_proof() {
    local vdso
    vdso="$(sed -n 's/^# vdso-backstopped: //p' "$ART" | head -1)"
    [ "$vdso" != none ] || return 0   # nothing declared, so nothing to prove
    [ -f "$PROOF" ] || fail "$ART declares vdso-backstopped '$vdso' but there is no proof beside it
       at $PROOF. A declaration nobody measured is the guess this tool exists to
       refuse: the shim's table is built from the artifact, so an unproved entry
       is a syscall the shim may implement that never needed implementing — or,
       worse, one it never implements that always did. Run '$0 --prove-vdso'."

    local p_name p_bin p_sha a_sha art_target
    p_name="$(sed -n 's/^# candidate: //p' "$PROOF" | head -1)"
    p_bin="$(sed -n 's/^# binary: //p' "$PROOF" | head -1)"
    p_sha="$(sed -n 's/^# sha256: //p' "$PROOF" | head -1)"
    a_sha="$(sed -n 's/^# sha256: //p' "$ART" | head -1)"
    art_target="$(sed -n 's/^# target: //p' "$ART" | head -1)"
    [ "$p_name" = "$CANDIDATE_NAME" ] || fail "the proof at $PROOF is for candidate '$p_name', but $CONF designates '$CANDIDATE_NAME'"
    [ "$p_bin" = "$CANDIDATE_BIN" ] || fail "the proof at $PROOF is for '$p_bin', but $CONF designates '$CANDIDATE_BIN'"
    [ "$p_sha" = "$a_sha" ] || fail "the proof and the census were taken of different binaries (proof $p_sha, census $a_sha) — regenerate both with '$0 --run' then '$0 --prove-vdso'"
    [ "$(sed -n 's/^# target: //p' "$PROOF" | head -1)" = "$art_target" ] || fail "the proof at $PROOF is for target '$(sed -n 's/^# target: //p' "$PROOF" | head -1)', but $ART is for '$art_target'"

    local dnum dname c_plain c_novdso n=0
    while read -r dnum dname; do
        [ -n "$dname" ] || continue
        # Already refused by check_artifact; re-stated here so the proof can never
        # stand in for the contradiction.
        c_plain="$(sed -n "s/^$dnum $dname \([0-9]*\)$/\1/p" "$ART" | head -1)"
        [ -z "$c_plain" ] || fail "$ART lists $dname ($dnum) both as a traced syscall and as vdso-backstopped"
        c_novdso="$(sed -n "s/^# vdso-proved: $dnum $dname plain=0 novdso=\([0-9]*\)$/\1/p" "$PROOF" | head -1)"
        [ -n "$c_novdso" ] || fail "$PROOF does not record '$dnum $dname' as proved — it was declared, not measured. Re-run '$0 --prove-vdso'."
        [ "$c_novdso" -gt 0 ] || fail "$PROOF records '$dname' as proved with a count of 0, which proves nothing"
        n=$((n + 1))
    done < <(sed -n 's/^# vdso-backstopped: //p' "$ART")

    # The stronger half: if the launcher had changed the workload in any other
    # way, the row sets would differ here rather than quietly agreeing.
    local tmp; tmp="$(mktemp -d)"; trap 'rm -rf "${tmp:-}"' EXIT
    proof_expected "$PROOF" > "$tmp/expected.raw" || fail "$PROOF states a vDSO backstop in its header but has no row for it — the proof contradicts itself"
    sort -n -k1,1 "$tmp/expected.raw" > "$tmp/expected"
    awk '/^[0-9]+ /{ print $1, $2, $3 }' "$PROOF" | sort -n -k1,1 > "$tmp/actual"
    [ -s "$tmp/actual" ] || fail "$PROOF contains no syscall rows — an empty census is not a census"
    diff -u "$tmp/expected" "$tmp/actual" > "$tmp/d" || fail "$PROOF and $ART disagree in more than the declared vDSO syscalls — one of them is stale, or the no-vDSO run changed the workload:\n$(sed 's/^/       /' "$tmp/d")"

    echo "proof: ok — $n declared vDSO-restored syscall(s) observed as real calls with no AT_SYSINFO_EHDR, and no other row in the census moved"
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

# ─── Trace -> rows ─────────────────────────────────────────────────────────
# Every traced line is "<pid> name(args) = ret" (or "name(args) = ret" when
# single-process). Take the text before the first '(' on lines that look like a
# syscall entry; signal/exit lines have no '('. Then key each name to its NUMBER
# from the kernel's own header, because the number is what the shim dispatches
# on. Shared by --run and --prove-vdso so both sides of the proof are counted
# by the same code — a proof counted differently from the thing it proves is
# not a proof.
count_rows() {   # $1 = trace file, $2 = rows file to write
    local trace="$1" rows_file="$2" name num
    : > "$rows_file"
    while read -r count name; do
        [ -n "$name" ] || continue
        num="$(nr_of "$name")"
        [ -n "$num" ] || { echo "WARN: no syscall number for '$name' in $UNISTD — omitted" >&2; continue; }
        printf '%s %s %s\n' "$num" "$name" "$count" >> "$rows_file"
    done < <(sed -E 's/^(\[[0-9]+\][[:space:]]+|[0-9]+[[:space:]]+)//' "$trace" \
             | grep -E '^[a-z_0-9]+\(' \
             | sed -E 's/^([a-z_0-9]+)\(.*/\1/' \
             | sort | uniq -c \
             | awk '{print $1, $2}')
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

    local tmp trace sha rows name num vn vnum
    tmp="$(mktemp -d)"
    trap 'rm -rf "${tmp:-}"' EXIT
    trace="$tmp/trace"
    # shellcheck disable=SC2086  # CANDIDATE_ARGS is intentionally word-split
    strace -f -qq -o "$trace" "$CANDIDATE_BIN" $CANDIDATE_ARGS \
        || fail "the candidate did not run cleanly under strace — a census of a failing run measures the failure, not the work"

    # Every traced line is counted by the same code --prove-vdso uses, so the
    # census and its proof are comparable row for row by construction.
    count_rows "$trace" "$tmp/rows"
    rows="$(wc -l < "$tmp/rows")"

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

# ─── Prove the vDSO declaration ────────────────────────────────────────────
prove_vdso() {
    require_designated
    check_artifact          # the census must exist and describe THIS binary
    if [ -z "$CANDIDATE_VDSO_SYSCALLS" ] || [ "$CANDIDATE_VDSO_SYSCALLS" = none ]; then
        abort "candidate '$CANDIDATE_NAME' declares no vDSO-backstopped syscalls
       (CANDIDATE_VDSO_SYSCALLS=${CANDIDATE_VDSO_SYSCALLS:-unset}), so there is nothing to prove.
       Set the key, re-run '$0 --run', then run this."
    fi
    [ -n "$CANDIDATE_ARGS" ] || fail "CANDIDATE_ARGS is empty in $CONF — the proof run has to be the candidate's real invocation"
    [ -f "$CANDIDATE_BIN" ] || fail "$CANDIDATE_BIN is not built, so the proof cannot start it"
    [ -f "$LAUNCHER_SRC" ] || abort "no stack launcher at $LAUNCHER_SRC — the proof needs one that removes AT_SYSINFO_EHDR from the auxv"
    command -v strace >/dev/null || abort "strace not found — the proof IS a measurement; there is no way to assert this from a file"
    command -v cc >/dev/null || abort "no C compiler (cc) — $LAUNCHER_SRC has to be built to run the candidate without a vDSO"
    [ -f "$UNISTD" ] || abort "kernel syscall-number header not found at $UNISTD"

    local tmp; tmp="$(mktemp -d)"; trap 'rm -rf "${tmp:-}"' EXIT

    echo "building launcher: cc -O2 -o $tmp/e7_stack_launcher $LAUNCHER_SRC"
    cc -O2 -Wall -Wextra -o "$tmp/e7_stack_launcher" "$LAUNCHER_SRC" \
        || fail "$LAUNCHER_SRC did not build — the proof cannot run"

    if [ -n "$CANDIDATE_SETUP" ]; then
        echo "setup: $CANDIDATE_SETUP"
        bash -c "$CANDIDATE_SETUP" || fail "candidate setup failed: $CANDIDATE_SETUP"
    fi

    echo "tracing: $CANDIDATE_BIN $CANDIDATE_ARGS, started on a stack with no AT_SYSINFO_EHDR"
    # shellcheck disable=SC2086
    strace -f -qq -o "$tmp/trace" "$tmp/e7_stack_launcher" "$CANDIDATE_BIN" $CANDIDATE_ARGS \
        || fail "the candidate did not run cleanly with no vDSO — the run that is supposed to prove the declaration failed, so it proves nothing"

    # The marker is how the loader's own syscalls are kept out of the row set.
    # Without it there is no boundary, so there is no comparable measurement.
    local mk
    mk="$(grep -n 'e7:enter-no-vdso' "$tmp/trace" | head -1 | cut -d: -f1)"
    [ -n "$mk" ] || fail "the launcher's entry marker is not in the trace — the launcher never reached the target's entry point, so this is not a no-vDSO run of the candidate"
    awk -v m="$mk" 'NR>m' "$tmp/trace" > "$tmp/target-trace"
    count_rows "$tmp/target-trace" "$tmp/rows"
    [ -s "$tmp/rows" ] || fail "the no-vDSO run produced no syscall rows for the target"
    if ! awk -v m="$mk" 'NR<=m' "$tmp/trace" | grep -qE '(^|[[:space:]])execve\('; then
        echo "WARN: the launcher's own execve is not in the trace before the marker — nothing is hidden by the split, but that is unexpected" >&2
    fi

    # The claim, both halves. Present in the no-vDSO run is half of it; absent
    # from the census is the other half, and a syscall that was already visible
    # to the trace is not backstopped by anything.
    local dnum dname c_novdso c_plain proved="" n=0
    while read -r dnum dname; do
        [ -n "$dname" ] || continue
        c_novdso="$(sed -n "s/^$dnum $dname \([0-9]*\)$/\1/p" "$tmp/rows" | head -1)"
        [ -n "$c_novdso" ] || fail "'$dnum $dname' is declared vdso-backstopped, but a run with no AT_SYSINFO_EHDR did not call it. Either the declaration is wrong or the vDSO was not really removed — a declaration nothing confirms is the guess this tool exists to refuse."
        c_plain="$(sed -n "s/^$dnum $dname \([0-9]*\)$/\1/p" "$ART" | head -1)"
        [ -z "$c_plain" ] || fail "'$dnum $dname' is declared vdso-backstopped, but the census traced it $c_plain time(s) — it is not backstopped by the vDSO, so it must not be declared"
        proved="${proved}# vdso-proved: $dnum $dname plain=0 novdso=$c_novdso
"
        n=$((n + 1))
    done < <(sed -n 's/^# vdso-backstopped: //p' "$ART")

    # And nothing else may have moved: with the vDSO absent the workload must do
    # the same work, plus the declared calls. A launcher that quietly changed
    # the run would show up right here rather than as a plausible row set.
    proof_expected "$tmp/rows" > "$tmp/expected.raw" || fail "the no-VDSO run has no row for a syscall it just declared proved — this is a bug in the proof, not in the candidate"
    sort -n -k1,1 "$tmp/expected.raw" > "$tmp/expected"
    sort -n -k1,1 "$tmp/rows" > "$tmp/actual"
    diff -u "$tmp/expected" "$tmp/actual" > "$tmp/d" || fail "the no-vDSO run differs from the census in more than the declared syscalls — the launcher changed the workload, or the census was taken on a different fixture. Re-run '$0 --run' and then this. (-expected +actual)\n$(sed 's/^/       /' "$tmp/d")"

    {
        echo "# AeroSLS E7 vDSO-backstop proof -- generated by tools/linux_syscall_census.sh --prove-vdso"
        echo "# candidate: $CANDIDATE_NAME"
        echo "# provenance: $CANDIDATE_PROVENANCE"
        echo "# binary: $CANDIDATE_BIN"
        echo "# target: $CANDIDATE_TARGET"
        echo "# sha256: $(sed -n 's/^# sha256: //p' "$ART" | head -1)"
        echo "# census: $ART"
        echo "# launcher: $LAUNCHER_SRC"
        echo "# launcher-sha256: $(sha256sum "$LAUNCHER_SRC" | cut -d' ' -f1)"
        echo "# launcher-auxv: AT_SYSINFO_EHDR omitted (the shim has no vDSO; design §5.2)"
        echo "# method: strace -f, candidate started by the launcher on a stack it built; rows are the target's own"
        echo "# split: the launcher's write(2) of its entry marker; its own execve is above that line, not in these rows"
        echo "# host: $(uname -srm)"
        echo "# date: $(date -u +%Y-%m-%dT%H:%M:%SZ)"
        echo "# columns: <syscall-number> <name> <count>"
        printf '%s' "$proved"
    } > "$PROOF"
    sort -n -k1,1 "$tmp/rows" >> "$PROOF"

    echo "wrote $PROOF — $n declared syscall(s) proved, $(wc -l < "$tmp/rows") rows"
    check_proof
}

# ─── Teeth: the refusals above, observed to fire ───────────────────────────
selftest() {
    local tmp pass=0 failn=0
    tmp="$(mktemp -d)"
    trap 'rm -rf "${tmp:-}"' EXIT

    case_one() {  # label, expected-rc, expected-substring, conf-body, [artifact], [proof]
        local label="$1" want_rc="$2" want_msg="$3" conf="$4" art="${5-}" prf="${6-}"
        printf '%s\n' "$conf" > "$tmp/c.conf"
        if [ -n "$art" ]; then printf '%s\n' "$art" > "$tmp/a.txt"; else rm -f "$tmp/a.txt"; fi
        # A stale proof from the previous case must not leak into this one.
        if [ -n "$prf" ]; then printf '%s\n' "$prf" > "$tmp/p.txt"; else rm -f "$tmp/p.txt"; fi
        local out rc
        out="$(LINUX_CENSUS_CONF="$tmp/c.conf" LINUX_CENSUS_ARTIFACT="$tmp/a.txt" LINUX_CENSUS_PROOF="$tmp/p.txt" bash "$0" --check 2>&1)"
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
    # The invariant is that the parsed VALUE arrived — not what the run then
    # tripped over. This tooth used to hard-code "no strace on this host", which
    # made the selftest host-dependent: put strace on PATH and the same
    # assertion failed while the parse was working perfectly. So accept either
    # end of the run and refuse only the failure that means the value was lost.
    local lost=0 why=""
    printf '%s' "$out" | grep -q "CANDIDATE_ARGS is empty" && lost=1
    if [ "$rc" -eq 2 ] && printf '%s' "$out" | grep -q "strace not found"; then
        why="reached the missing strace (no strace on this host)"
    elif [ "$rc" -eq 1 ] && printf '%s' "$out" | grep -q "build produced no"; then
        why="reached the missing binary (strace is on this host's PATH)"
    else
        lost=1
    fi
    if [ "$lost" -eq 0 ]; then
        echo "ok:   a multi-word value survives the parse ($why, not lost at the args check)"; pass=$((pass+1))
    else
        echo "FAIL: multi-word value — rc=$rc, and the run did not show the value surviving the parse"
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

    # 16-19. The vDSO declaration is PROVED, not inferred. A declaration with no
    #        proof beside it is the guess this tool refuses everywhere else, and
    #        the negative control (19) is what stops this from being a tool that
    #        reddens whatever it is shown.
    local vconf='CANDIDATE_STATE=designated
CANDIDATE_NAME=aerobackup
CANDIDATE_PROVENANCE=backup/backup.sh
CANDIDATE_BIN='"$tmp"'/stale-bin
CANDIDATE_TARGET=x86_64-unknown-linux-musl'
    local vart='# candidate: aerobackup
# target: x86_64-unknown-linux-musl
# sha256: '"$real_sha"'
# vdso-backstopped: 228 clock_gettime
0 read 5
59 execve 1
257 openat 12'
    local vproof_head='# candidate: aerobackup
# binary: '"$tmp"'/stale-bin
# target: x86_64-unknown-linux-musl
# sha256: '"$real_sha"''
    local vproof_rows='0 read 5
228 clock_gettime 1
257 openat 12'

    case_one "a declared vDSO backstop with no proof is refused" 1 "no proof beside it" \
        "$vconf" "$vart"
    case_one "a proof that never states the syscall was proved is refused" 1 "was declared, not measured" \
        "$vconf" "$vart" "$vproof_head
$vproof_rows"
    case_one "a proof that states the syscall but has no row for it is refused" 1 "contradicts itself" \
        "$vconf" "$vart" "$vproof_head
# vdso-proved: 228 clock_gettime plain=0 novdso=1
0 read 5"
    case_one "a proof whose other rows disagree with the census is refused" 1 "disagree in more than the declared" \
        "$vconf" "$vart" "$vproof_head
# vdso-proved: 228 clock_gettime plain=0 novdso=1
0 read 5
228 clock_gettime 1"
    case_one "a proof that states the syscall and matches the census passes" 0 "proof: ok" \
        "$vconf" "$vart" "$vproof_head
# vdso-proved: 228 clock_gettime plain=0 novdso=1
$vproof_rows"

    echo
    echo "linux_syscall_census selftest: $pass passed, $failn failed"
    [ "$failn" -eq 0 ] || exit 1
}

load_conf
case "$MODE" in
    check)    require_designated; check_artifact; check_proof ;;
    run)      run_census ;;
    prove)    prove_vdso ;;
    selftest) selftest ;;
esac
