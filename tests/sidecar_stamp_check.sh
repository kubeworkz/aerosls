#!/usr/bin/env bash
# tests/sidecar_stamp_check.sh — the committed `sidecars.cpio` must have been
# packed from the committed `user/` sources, its two stamps and its pack-run
# record must all be one run's, and its six binaries must be this tree's own
# build outputs wherever those outputs exist.
#
# ─── Why this guard exists ────────────────────────────────────────────────
# `make x86-iso` now REFUSES to ship a sidecar archive whose source stamp does
# not match the tree (see the Makefile and tools/sidecar_source_digest.sh — the
# stamp is a digest of every source the six sidecars are built from). That
# refusal is the right place for the check, but it is only as good as its
# presence: the rule lives in one recipe, and a rule that can be deleted, or
# turned into a warning, or bypassed by a host that never runs `make x86-iso`
# (CI ships the COMMITTED archive; so does deploy) protects nothing.
#
# This guard is that rule's teeth, and it asks the question one level earlier
# than the build does: does the archive in the INDEX still correspond to the
# sources in the INDEX? It is also the only place that question is asked with no
# Rust toolchain, no ISO and no boot — which is what makes it answerable on
# every push.
#
# The archive's OWN BYTES are checked the same way (`sidecars.cpio.sha256`): a
# repack or a swap that leaves `user/` untouched still changes them, and the
# source stamp cannot see it. Both are plain coreutils, so both are answerable
# on the hosts that actually ship the archive.
#
# ─── What it asserts ──────────────────────────────────────────────────────
#   A. THE INSTRUMENT ANSWERS, AND ANSWERS THE SAME WAY TWICE. The tool prints
#      exactly one sha256 line for this tree, and two consecutive runs agree.
#      A digest that is not stable, or not a digest, cannot attest anything —
#      and every clause below is only as trustworthy as this one.
#   B. THE STAMP MATCHES THE SOURCES. `sidecars.cpio.digest` equals the tool's
#      answer for the committed `user/`. This is THE clause: it is red exactly
#      when a `user/` change was committed without re-packing the archive,
#      which is the failure that cost a session (the kernel refused a 28-byte
#      answer to a 216-byte registration, because the ISO's init predated
#      ENV_REGISTER) and which no source-level guard could see.
#   C. `x86-iso` REFUSES, and refuses BEFORE it copies. The recipe calls the
#      tool, compares against `$(SIDECAR_STAMP)`, exits non-zero, and does all
#      of that above the `cp ... isodir/boot/sidecars.cpio` line — a check that
#      runs after the copy could still fail the build, but "verified the thing
#      it ships" is the property, so the order is asserted, not assumed.
#   D. THE PACKER WRITES IT. `selfhost-bootimage` is the only target that packs
#      an archive, so it is the only place the stamp can honestly be written.
#   E. THE STAMP IS PER-ARCHIVE. `SIDECAR_STAMP` derives from `SIDECAR_CPIO`,
#      so the E3 image (sidecars_e3.cpio) gets its own and cannot be verified
#      against the default image's stamp.
#   F. THE COMMITTED STAMP IS NOT IGNORED. `.gitignore` swallows the E3
#      stamps and record (build artifacts, like their archive) but not the
#      default ones: if a pattern ever swallowed a default stamp or the record,
#      the archive would ship without the evidence that binds it and the
#      refusals would stop seeing a mismatch. Pattern matching here is a
#      deliberate approximation of .gitignore semantics (no anchoring, no
#      `**`), erring toward flagging.
#   G. THE STAMP ONLY COVERS BINARIES THE PACKER BUILT. The stamp attests the
#      packed BINARIES as well as the sources, and the only thing that makes
#      that true is that `selfhost-bootimage`'s sidecar builds are FATAL: a
#      failed build must stop the target before it packs and stamps, or a run
#      that reused whatever ELFs were on disk (the x86_64-unknown-none target
#      missing) stamps them as if this tree produced them. That is the
#      stale-archive failure one layer down -- the SOURCES match, only the
#      binaries inside are old -- so the builds must not be allowed to fall
#      through, and the stamp write must come after them.
#   H. THE ARCHIVE'S OWN BYTES ARE STAMPED, AND THE STAMP MATCHES. B ties the
#      archive to the sources; it cannot see an archive re-packed or replaced
#      OUT OF BAND, where `user/` is untouched and only the bytes inside change
#      (a hand-run `cpio` over stale images, a `cp` of yesterday's archive).
#      `sidecars.cpio.sha256` records the archive's own sha256 and this clause
#      recomputes it -- with coreutils alone, so it holds wherever the archive is
#      shipped. This is the "THE clause" for the bytes half of the stamp.
#   I. THE PACKER WRITES THE BYTES STAMP. `selfhost-bootimage` is the only
#      target that packs, so it is the only place the bytes stamp can be honest:
#      written as `$(SIDECAR_ARCHIVE_STAMP)`, derived per-archive from
#      `$(SIDECAR_CPIO)`, and written AFTER the pack -- a stamp written first
#      describes the PREVIOUS archive.
#   J. `x86-iso` VERIFIES THE BYTES STAMP, before it copies. The property C
#      asserts for the sources, one layer down: the recipe hashes the archive,
#      compares against `$(SIDECAR_ARCHIVE_STAMP)`, exits non-zero on a
#      mismatch, and does it above the `cp ... isodir/boot/sidecars.cpio` line.
#   K. THE THREE FILES ARE ONE PACK RUN'S. B and H each verify a stamp against
#      its own subject, and two independent writes can satisfy both while
#      disagreeing with each other: edit a `user/` source and hand-run the
#      source-digest tool over the old archive, and B is green again while the
#      archive still carries the previous binaries (the incident, reached past
#      its own stamp). Both stamps are now PROJECTIONS of one record
#      (`sidecars.cpio.stamps`) that a single tool invocation computes from
#      both instruments, and this clause recomputes that record, requires the
#      committed one to match, and requires each stamp to equal its field --
#      so neither stamp can move without the other's knowledge. The record is
#      also the manifest of the six packed binaries (`binary boot/*.bin
#      <sha256>` lines); clause L holds those.
#   L. THE RECORD'S SIX BINARIES ARE THE ARCHIVE'S. The record names the six
#      `boot/*.bin` images and their digests, so "these are the packed
#      binaries" is spelled out, not merely implied by the archive's hash.
#      This clause re-extracts each entry with coreutils and requires the
#      record's line, the entry instrument's answer and its own extraction to
#      agree -- the record cannot misstate what is inside even if the tool
#      that wrote it lies. What no cargo-free clause can prove is that those
#      images were BUILT from the current sources: the packer's fatal builds
#      and its `--verify` inputs (clause K) are what make that true at pack
#      time, and a hand can still re-derive every file from a stale archive.
#   M. THE ARCHIVE'S BINARIES ARE THIS TREE'S BUILD OUTPUTS, WHEN THEY EXIST.
#      A record and its two stamps can be rewritten TOGETHER from a stale
#      archive, and K and L stay green because they only compare those files
#      to each other. This clause compares the archive to something outside
#      that set: the flattened `.bin` files `selfhost-bootimage` produces,
#      read from the Makefile's own `SIDECAR_*_BIN` defaults and compared
#      natively, so the answer does not depend on the check tool the recipe
#      calls. The one exemption is an `init.bin` that is the E3 archive's
#      entry -- the e3_envs pack overwrites the shared flattened file -- and
#      even that record is trusted only after a live recomputation of
#      sidecars_e3.cpio equals it, so a hand-written record that merely names
#      the flattened file cannot mask a stale one. On a host with no build
#      outputs (CI's ISO job, deploy, a fresh clone) there is nothing to
#      compare against and the clause holds without pretending: the claim is
#      made at pack time, and re-made here wherever the pack output survives.
#      The recipe half requires `x86-iso` to run
#      tools/sidecar_build_output_check.sh over the same six paths, above the
#      copy of the archive.
#   N. THE E3 TUPLE IS ONE PACK RUN'S TOO, WHEN IT IS HERE. The e3_envs pack
#      writes four files beside the default one (sidecars_e3.cpio, its two
#      stamps and its record), all .gitignored build artifacts (clause F), so
#      on CI, a fresh clone or any host that never ran that pack they are
#      simply absent and the clause holds without pretending -- an ABORT here
#      would report every such tree as rot. Where they DO exist, K's one-run
#      rule applies one archive over: the committed E3 record must equal a
#      live recomputation of sidecars_e3.cpio (v2 nine-line shape), and each
#      E3 stamp must be a field of that record. M leans on the E3 record
#      whenever it excuses an e3_envs init mismatch and K and L never look at
#      the E3 files, so this is the clause that keeps the file M trusts from
#      being a hand-written one.
#
# ─── Hermetic seam ────────────────────────────────────────────────────────
# The optional ROOT argument is the same one tests/env_checkpoint_restore_check.sh
# uses: it inspects a repository root, defaulting to its own parent. The smoke
# builds a copy of the tree, plants one mutation, and runs this guard against
# it — no network, no build, no Rust.
#
# GUARD-KIND: host (plain bash + coreutils: find/sort/sha256sum/grep/dd/tail/head,
# plus awk to read the Makefile recipes). There is
# deliberately NO `build` or `runtime` marker: this guard needs no build
# artefact, so an ABORT here is rot and run_checks.sh reports it as a FAILURE
# rather than an owed skip. "Could not check" must not resemble "it is fine".
#
# Exit: 0 every clause holds, 1 one failed, 2 a precondition is missing.
set -u

ROOT=""
while [ $# -gt 0 ]; do
    case "$1" in
        -*) echo "ABORT: unknown argument '$1' (usage: $0 [ROOT])" >&2; exit 2 ;;
        *)  ROOT="$1"; shift ;;
    esac
done
ROOT="${ROOT:-$(cd "$(dirname "$0")/.." && pwd)}"
cd "$ROOT" || exit 2

fail=0
ok()   { echo "ok:   $*"; }
bad()  { echo "FAIL: $*"; fail=1; }

CPIO="${SIDECAR_CPIO:-sidecars.cpio}"
STAMP="$CPIO.digest"
TOOL="tools/sidecar_source_digest.sh"
ARCHIVE_STAMP="$CPIO.sha256"
ARCHIVE_TOOL="tools/sidecar_archive_digest.sh"
RECORD_TOOL="tools/sidecar_stamp_record.sh"
RECORD="$CPIO.stamps"
ENTRY_TOOL="tools/sidecar_archive_entry_digest.sh"
BUILD_CHECK_TOOL="tools/sidecar_build_output_check.sh"
# The six sidecars the record names, in pack order (boot/rootfs.bin is built by
# the packer itself and the manifests/layout are derived from the same images;
# the archive's own sha256 covers those, these six are the built ones).
BIN_ENTRIES="boot/init.bin boot/dm.bin boot/posix.bin boot/ramdisk.bin boot/net.bin boot/e1000.bin"
E3_CPIO="sidecars_e3.cpio"
E3_STAMP="sidecars_e3.cpio.digest"
E3_ARCHIVE_STAMP="sidecars_e3.cpio.sha256"
E3_RECORD="sidecars_e3.cpio.stamps"

# ── Preconditions (exit 2: the guard could not be evaluated at all) ────────
[ -f "$TOOL" ] || {
    echo "ABORT: $TOOL is missing — nothing in this tree can digest the sidecar sources." >&2
    exit 2
}
[ -x "$TOOL" ] || {
    echo "ABORT: $TOOL is not executable — the Makefile calls it as a command." >&2
    exit 2
}
[ -f "$ARCHIVE_TOOL" ] || {
    echo "ABORT: $ARCHIVE_TOOL is missing — nothing in this tree can hash the archive's own bytes." >&2
    exit 2
}
[ -x "$ARCHIVE_TOOL" ] || {
    echo "ABORT: $ARCHIVE_TOOL is not executable — the Makefile calls it as a command." >&2
    exit 2
}
[ -f "$RECORD_TOOL" ] || {
    echo "ABORT: $RECORD_TOOL is missing — nothing in this tree can bind the two stamps into one pack-run record." >&2
    exit 2
}
[ -x "$RECORD_TOOL" ] || {
    echo "ABORT: $RECORD_TOOL is not executable — the Makefile calls it as a command." >&2
    exit 2
}
[ -f "$ENTRY_TOOL" ] || {
    echo "ABORT: $ENTRY_TOOL is missing — nothing in this tree can read the packed binaries out of the archive." >&2
    exit 2
}
[ -x "$ENTRY_TOOL" ] || {
    echo "ABORT: $ENTRY_TOOL is not executable — the record tool calls it as a command." >&2
    exit 2
}
[ -s "$CPIO" ] || {
    echo "ABORT: $CPIO is missing or empty — this tree does not ship the archive the stamp is about." >&2
    exit 2
}
[ -f Makefile ] || { echo "ABORT: Makefile is missing." >&2; exit 2; }
if ! command -v sha256sum >/dev/null 2>&1; then
    echo "ABORT: sha256sum is not on PATH — there is no digest to compare." >&2
    exit 2
fi

# Recipe body of a Makefile target, as text (blank lines dropped, and it stops
# at the first line that is not a TAB continuation).
recipe() {   # recipe <target:>
    awk -v hdr="$1" '
        $0 ~ "^"hdr { f = 1; next }
        f && /^\t/ { print; next }
        f && /^[[:space:]]*$/ { next }
        f { exit }
    ' Makefile
}

# The recipe's LOGICAL commands: backslash-continuations joined into one line,
# so a command's `||` handler can be inspected even when it is written on the
# NEXT physical line. The builds are, and the whole point of clause G is that a
# two-line `|| echo` swallows a failed build -- reading physical lines would see
# a bare build and call it fatal.
flatten() {
    awk '
        { line = $0
          while (line ~ /\\[[:space:]]*$/) {
              sub(/\\[[:space:]]*$/, "", line)
              if ((getline more) <= 0) break
              line = line " " more
          }
          print line
        }'
}

# sha256 of one `newc` entry's DATA, extracted with coreutils alone. This is
# the guard's OWN extraction (clause L), deliberately not the entry tool's:
# the record's binary lines are only trustworthy if something that did not
# write them can still read the archive. Mirrors user/bootimage/src/newc.rs:
# the name is preceded by the 110-byte header (c_filesize at +54, c_namesize
# at +94), and the data starts at header + 110 + align4(namesize).
newc_entry_sha() {   # newc_entry_sha <archive> <entry>  -> sha256 or empty
    local a="$1" want="$2" off hdr size_hex nsize_hex namesize size data_off
    for off in $(grep -abo -F -- "$want" "$a" 2>/dev/null | cut -d: -f1); do
        hdr=$((off - 110))
        [ "$hdr" -ge 0 ] || continue
        [ "$(dd if="$a" bs=1 skip="$hdr" count=6 2>/dev/null)" = "070701" ] || continue
        size_hex=$(dd if="$a" bs=1 skip=$((hdr + 54)) count=8 2>/dev/null)
        nsize_hex=$(dd if="$a" bs=1 skip=$((hdr + 94)) count=8 2>/dev/null)
        case "$size_hex$nsize_hex" in *[!0-9a-fA-F]*) return 1 ;; esac
        namesize=$((16#$nsize_hex))
        size=$((16#$size_hex))
        data_off=$((hdr + 110 + namesize + ((4 - (namesize % 4)) % 4)))
        tail -c +$((data_off + 1)) "$a" | head -c "$size" | sha256sum | cut -d' ' -f1
        return 0
    done
    return 1
}

# ── A. the instrument answers, and answers the same way twice ─────────────
h1="$("$TOOL" 2>/dev/null || true)"
h2="$("$TOOL" 2>/dev/null || true)"
# Command substitution strips the TRAILING newline only, so any newline left in
# here is an embedded one -- i.e. more than one line of output. (Asked with wc
# and not a grep for a newline: a pattern containing a newline is several
# patterns to grep, one of them empty, and an empty pattern matches anything.)
h1_lines=$(printf '%s' "$h1" | wc -l)
a_ok=1
if [ -z "$h1" ]; then
    a_ok=0
    bad "A. $TOOL printed nothing — the stamp would be compared against an empty string, which any missing stamp file also reads as"
elif [ "$h1_lines" -ne 0 ]; then
    a_ok=0
    bad "A. $TOOL printed more than one line — the stamp is one line, so a multi-line answer can never match one (and a stamp written from it would be a file comparison, not a digest)"
elif ! printf '%s' "$h1" | grep -Eq '^[0-9a-f]{64}$'; then
    a_ok=0
    bad "A. $TOOL printed '$h1', which is not a sha256 line — a stamp compared against this could only ever mismatch, so the gate would be noise rather than a check"
elif [ "$h1" != "$h2" ]; then
    a_ok=0
    bad "A. $TOOL answered $h1 and then $h2 for the same tree — a digest that is not stable cannot attest that two things are the same"
else
    ok "A. the tool answers with one stable sha256 line for this tree ($h1)"
fi

# ── B. THE clause: the committed stamp matches the committed sources ──────
if [ "$a_ok" -eq 0 ]; then
    bad "B. cannot be evaluated: A failed, so there is no trustworthy digest to compare the stamp against"
elif [ ! -f "$STAMP" ]; then
    bad "B. $STAMP is missing — nothing in this tree says which sources $CPIO was packed from, so nothing can stop a stale archive from being shipped (make selfhost-bootimage writes it)"
elif [ "$(cat "$STAMP")" != "$h1" ]; then
    bad "B. $STAMP records $(cat "$STAMP") but the user/ sources in this tree digest to $h1 — $CPIO was packed from different sources. Re-pack and commit both: make selfhost-bootimage (the refusal in make x86-iso is what this makes visible before an ISO is built)"
else
    ok "B. $CPIO is from the user/ sources in this tree — the committed stamp matches the committed sources ($h1)"
fi

# ── C. x86-iso refuses on a mismatch, and before it copies ─────────────────
xbody="$(recipe 'x86-iso:')"
if [ -z "$xbody" ]; then
    bad "C. the Makefile has no x86-iso recipe — the refusal this guard exists for was deleted (or the target was renamed)"
else
    missing=""
    case "$xbody" in *"$TOOL"*)                     ;; *) missing="$missing digest-tool" ;; esac
    case "$xbody" in *'$(SIDECAR_STAMP)'*)          ;; *) missing="$missing stamp" ;; esac
    printf '%s\n' "$xbody" | grep -qE 'exit [1-9]'  ||     missing="$missing refuse"
    if [ -n "$missing" ]; then
        bad "C. the x86-iso recipe is missing:$missing — without the tool call, the comparison against \$(SIDECAR_STAMP) and a non-zero exit on a mismatch, the build ships whatever archive is on disk, which is the failure this guard exists for"
    else
        c_line=$(printf '%s\n' "$xbody" | grep -nF "$TOOL" | head -1 | cut -d: -f1)
        p_line=$(printf '%s\n' "$xbody" | grep -nF 'isodir/boot/sidecars.cpio' | head -1 | cut -d: -f1)
        if [ -z "${p_line:-}" ]; then
            bad "C. the x86-iso recipe no longer copies the archive into isodir/boot/sidecars.cpio — the guard's anchor has moved, so it cannot say whether the check runs before the copy"
        elif [ "${c_line:-0}" -ge "$p_line" ]; then
            bad "C. x86-iso verifies the archive (line $c_line) at or after it copies it (line $p_line) — the check must gate the copy, not report on it afterwards"
        else
            ok "C. x86-iso calls the tool (line $c_line), compares it with \$(SIDECAR_STAMP) and exits non-zero on a mismatch, all before it copies the archive (line $p_line)"
        fi
    fi
fi

# ── D. the one target that packs writes the stamp ─────────────────────────
sbody="$(recipe 'selfhost-bootimage:')"
if [ -z "$sbody" ]; then
    bad "D. the Makefile has no selfhost-bootimage recipe — nothing packs a sidecar archive, so nothing can write a stamp for one"
else
    missing=""
    case "$sbody" in *"$RECORD_TOOL"*)              ;; *) missing="$missing record-tool" ;; esac
    case "$sbody" in *'> "$(SIDECAR_STAMP)"'*)      ;; *) missing="$missing stamp-write" ;; esac
    case "$sbody" in *'-o "$(SIDECAR_CPIO)"'*|*'-o $(SIDECAR_CPIO)'*) ;; *) missing="$missing pack" ;; esac
    if [ -n "$missing" ]; then
        bad "D. selfhost-bootimage is missing:$missing — the stamps are only honest if the target that PACKS the archive writes them (here, as projections of the run record); a stamp written anywhere else (or nowhere) describes a pack that did not happen"
    else
        ok "D. selfhost-bootimage packs $CPIO and writes \$(SIDECAR_STAMP) in the same run, projected from the record $RECORD_TOOL computes"
    fi
fi

# ── E. the stamp is per-archive ───────────────────────────────────────────
if grep -Eq '^SIDECAR_STAMP[[:space:]]*\?=[[:space:]]*\$\(SIDECAR_CPIO\)\.digest[[:space:]]*$' Makefile; then
    ok "E. SIDECAR_STAMP derives from SIDECAR_CPIO, so each archive has its own stamp (the E3 image cannot be verified against the default one's)"
else
    bad "E. SIDECAR_STAMP is not \$(SIDECAR_CPIO).digest — with a fixed stamp name the E3 image (SIDECAR_CPIO=sidecars_e3.cpio) would be verified against the DEFAULT archive's stamp, which is a check that can only pass for the wrong reason or fail for one"
fi

# ── F. the committed stamp is not ignored ─────────────────────────────────
if [ ! -f .gitignore ]; then
    bad "F. .gitignore is missing — this guard cannot tell whether the stamp is committable, and an ignored stamp means the archive ships alone"
else
    swallowed=""
    e3_listed=0
    e3_archive_listed=0
    e3_record_listed=0
    while IFS= read -r pat; do
        case "$pat" in ''|'#'*|'!'*) continue ;; esac
        case "$STAMP" in $pat) swallowed="$pat" ;; esac
        case "$ARCHIVE_STAMP" in $pat) [ -n "$swallowed" ] || swallowed="$pat" ;; esac
        case "$RECORD" in $pat) [ -n "$swallowed" ] || swallowed="$pat" ;; esac
        [ "$pat" = "$E3_STAMP" ] && e3_listed=1
        [ "$pat" = "$E3_ARCHIVE_STAMP" ] && e3_archive_listed=1
        [ "$pat" = "$E3_RECORD" ] && e3_record_listed=1
    done < .gitignore
    if [ -n "$swallowed" ]; then
        bad "F. .gitignore pattern '$swallowed' ignores $STAMP (or $ARCHIVE_STAMP, or $RECORD) — an ignored stamp or record cannot be committed, so the archive would ship without the evidence of what it was packed from (or which bytes it was, or that all three are one run's) and make x86-iso would refuse every clean checkout"
    elif [ "$e3_listed" -eq 0 ] || [ "$e3_archive_listed" -eq 0 ] || [ "$e3_record_listed" -eq 0 ]; then
        bad "F. .gitignore does not list all three E3 build-artifact stamps ($E3_STAMP, $E3_ARCHIVE_STAMP, $E3_RECORD) — the E3 archive is a build artifact (sidecars_e3.cpio is ignored), so its stamps and record are too; unignored they show up as untracked files in every tree that builds an E3 image"
    else
        ok "F. .gitignore lists all three build-artifact stamps ($E3_STAMP, $E3_ARCHIVE_STAMP, $E3_RECORD) and swallows neither $STAMP nor $ARCHIVE_STAMP nor $RECORD, nor a pattern that would"
    fi
fi

# ── G. the stamp only covers binaries the packer BUILT ────────────────────
# Clauses C and D keep the stamp honest about WHERE it is written; this one
# keeps it honest about what it took to get there. `selfhost-bootimage` is the
# only writer and the only packer, so the claim "packed from this tree's
# sources" can only be false if it packed binaries it did NOT build -- no cross
# target, a compile error -- and stamped them anyway. Every build must therefore
# be fatal (a `||` fallback must exit non-zero, and make must not be told to
# ignore the error), and the stamp step (the run record and the stamps projected
# from it) must sit after them. `-@$(CARGO) build` and `|| echo warning` are the
# two shapes of a fall-through, and both are red.
if [ -n "$sbody" ]; then
    g_builds=0
    g_last_build=0
    g_stamp=""
    g_fault=""
    g_idx=0
    while IFS= read -r cmd; do
        g_idx=$((g_idx + 1))
        # Recipe lines are shell commands, so `@`/`-` are make prefixes and a
        # `#` after them is a comment: normalise, then skip comments.
        norm="${cmd//@/}"
        norm="${norm#${norm%%[![:space:]]*}}"
        case "$norm" in '#'*) continue ;; esac
        # Any of the three stamp-step writes counts: the record itself and the
        # two projections. The FIRST of them must sit after the last build.
        case "$norm" in
            *'sidecar_stamp_record.sh'*|*'s/^sources //p'*|*'s/^bytes //p'*)
                [ -n "$g_stamp" ] || g_stamp=$g_idx ;;
        esac
        case "$norm" in *'$(CARGO) build'*) ;; *) continue ;; esac
        g_builds=$((g_builds + 1))
        g_last_build=$g_idx
        # `-cmd`: make's ignore-error prefix, which turns a failed build into a
        # recipe that carries on to the pack.
        case "$norm" in
            -*'$(CARGO) build'*) g_fault="a \$(CARGO) build is prefixed with '-' (make ignores a failed command)" ;;
        esac
        # A `||` is a fallback path; it is only safe if it ends the target.
        case "$norm" in
            *'||'*)
                case "$norm" in
                    *'exit '[1-9]*) ;;
                    *) [ -n "$g_fault" ] || g_fault="a \$(CARGO) build has a '||' that does not exit non-zero" ;;
                esac ;;
        esac
    done < <(printf '%s\n' "$sbody" | flatten)

    if [ "$g_builds" -eq 0 ]; then
        bad "G. selfhost-bootimage no longer runs a \$(CARGO) build — the target that writes the stamp must build the binaries it packs, or the stamp covers images this run did not produce"
    elif [ -n "$g_fault" ]; then
        bad "G. selfhost-bootimage's sidecar builds are not fatal ($g_fault) — a failed build falls through to the pack and the stamp, which is exactly how a run that reused existing binaries would bless them as this tree's own. End the '||' with 'exit 1' (or drop it entirely)."
    elif [ -n "$g_stamp" ] && [ "$g_last_build" -ge "$g_stamp" ]; then
        bad "G. selfhost-bootimage writes the run record / a stamp (command $g_stamp) at or before its last sidecar build (command $g_last_build) — the stamp step must only be reached after every build succeeds, or it records binaries this run did not just build"
    else
        ok "G. selfhost-bootimage builds the sidecars fatally and writes the run record (and the stamps projected from it) only afterwards, so no stamp covers binaries the run did not build"
    fi
fi

# ── H. THE ARCHIVE'S OWN BYTES ARE STAMPED, AND THE STAMP MATCHES ─────────
# B ties the archive to the SOURCES; nothing in B can see the archive's BYTES
# change while the sources stay put, which is exactly a repack or a swap out of
# band. This clause recomputes the archive's sha256 with coreutils and requires
# the committed stamp to agree, so it holds on the hosts that ship the archive.
h_sha="$(sha256sum -- "$CPIO" | cut -d' ' -f1)"
h_tool="$(bash "$ARCHIVE_TOOL" "$CPIO" 2>/dev/null || true)"
if [ "$h_tool" != "$h_sha" ]; then
    bad "H. $ARCHIVE_TOOL did not answer with $CPIO's sha256 (got '${h_tool:-<nothing>}', expected $h_sha) — the instrument for the archive-bytes stamp is not answering, so the comparison below could be against the wrong thing"
elif [ ! -f "$ARCHIVE_STAMP" ]; then
    bad "H. $ARCHIVE_STAMP is missing — nothing records which archive BYTES were packed, so a repack or a swap out of band (user/ untouched, the source stamp still matching) would ship. make selfhost-bootimage writes it"
elif [ "$(cat "$ARCHIVE_STAMP")" != "$h_sha" ]; then
    bad "H. $ARCHIVE_STAMP records $(cat "$ARCHIVE_STAMP") but $CPIO hashes to $h_sha — the archive's BYTES are not the ones that were packed and stamped. Re-pack and commit both: make selfhost-bootimage (the refusal in make x86-iso is what this makes visible before an ISO is built)"
else
    ok "H. the committed $CPIO's own bytes are the stamped ones — $ARCHIVE_STAMP matches sha256sum ($h_sha)"
fi

# ── I. the target that packs writes the bytes stamp ───────────────────────
# D's property one layer down: a hash of the archive is only honest if the run
# that PACKED it wrote it, after the pack, under a per-archive name -- here, as
# the bytes field of the run record projected into the stamp.
if [ -z "$sbody" ]; then
    bad "I. cannot be evaluated: selfhost-bootimage has no recipe (see D), so there is nowhere a bytes stamp could be written"
else
    i_missing=""
    case "$sbody" in *'> "$(SIDECAR_ARCHIVE_STAMP)"'*) ;; *) i_missing="$i_missing bytes-stamp-write" ;; esac
    if [ -n "$i_missing" ]; then
        bad "I. selfhost-bootimage is missing:$i_missing — the archive's own bytes are only attested if the target that PACKS writes their hash as \$(SIDECAR_ARCHIVE_STAMP) in the same run (projected from the run record)"
    elif ! grep -Eq '^SIDECAR_ARCHIVE_STAMP[[:space:]]*\?=[[:space:]]*\$\(SIDECAR_CPIO\)\.sha256[[:space:]]*$' Makefile; then
        bad "I. SIDECAR_ARCHIVE_STAMP is not \$(SIDECAR_CPIO).sha256 — with a fixed name the E3 image (SIDECAR_CPIO=sidecars_e3.cpio) would be checked against the DEFAULT archive's bytes, which can only pass for the wrong archive or fail for the right one"
    else
        i_tool=$(printf '%s\n' "$sbody" | flatten | grep -nF '> "$(SIDECAR_ARCHIVE_STAMP)"' | head -1 | cut -d: -f1)
        i_pack=$(printf '%s\n' "$sbody" | flatten | grep -nF -e '-o "$(SIDECAR_CPIO)"' | head -1 | cut -d: -f1)
        if [ -z "${i_pack:-}" ]; then
            bad "I. selfhost-bootimage no longer packs with -o \"\$(SIDECAR_CPIO)\" — the guard's anchor has moved, so it cannot say whether the bytes stamp is written after the pack"
        elif [ "${i_tool:-0}" -le "$i_pack" ]; then
            bad "I. selfhost-bootimage writes \$(SIDECAR_ARCHIVE_STAMP) (command $i_tool) at or before it packs the archive (command $i_pack) — a stamp written first describes the PREVIOUS archive, not the one this run just made"
        else
            ok "I. selfhost-bootimage packs $CPIO and then writes \$(SIDECAR_ARCHIVE_STAMP), per-archive (\$(SIDECAR_CPIO).sha256), projected from the record whose writer hashes the bytes"
        fi
    fi
fi

# ── J. x86-iso verifies the bytes stamp, before it copies ─────────────────
# C's property one layer down: the recipe must hash the archive, compare it
# with the bytes stamp and refuse, above the copy.
if [ -z "$xbody" ]; then
    bad "J. cannot be evaluated: the Makefile has no x86-iso recipe (see C)"
else
    j_missing=""
    case "$xbody" in *"$ARCHIVE_TOOL"*) ;; *) j_missing="$j_missing archive-tool" ;; esac
    case "$xbody" in *'$(SIDECAR_ARCHIVE_STAMP)'*) ;; *) j_missing="$j_missing bytes-stamp" ;; esac
    printf '%s\n' "$xbody" | grep -qE 'exit [1-9]' || j_missing="$j_missing refuse"
    if [ -n "$j_missing" ]; then
        bad "J. the x86-iso recipe is missing:$j_missing — without hashing the archive's bytes, comparing them with \$(SIDECAR_ARCHIVE_STAMP) and exiting non-zero on a mismatch, a repack or swap done out of band ships while the source stamp still matches"
    else
        j_tool=$(printf '%s\n' "$xbody" | flatten | grep -nF "$ARCHIVE_TOOL" | head -1 | cut -d: -f1)
        j_copy=$(printf '%s\n' "$xbody" | flatten | grep -nF 'isodir/boot/sidecars.cpio' | head -1 | cut -d: -f1)
        if [ -z "${j_copy:-}" ]; then
            bad "J. the x86-iso recipe no longer copies the archive into isodir/boot/sidecars.cpio — the guard's anchor has moved, so it cannot say whether the bytes check runs before the copy"
        elif [ "${j_tool:-0}" -ge "$j_copy" ]; then
            bad "J. x86-iso verifies the archive's bytes (command $j_tool) at or after it copies it (command $j_copy) — the check must gate the copy, not report on it afterwards"
        else
            ok "J. x86-iso hashes the archive's bytes (command $j_tool), compares them with \$(SIDECAR_ARCHIVE_STAMP) and exits non-zero on a mismatch, all before it copies the archive (command $j_copy)"
        fi
    fi
fi

# ── K. the three files come from ONE pack run ─────────────────────────────
# B and H each verify a stamp against its own subject, and two independent
# writes can satisfy both while disagreeing with each other: edit a `user/`
# source and hand-run the source-digest tool over the old archive, and B is
# green again while the archive still carries the previous binaries. The record
# ($CPIO.stamps) is written by ONE tool invocation that computes every half
# (sources, archive bytes, the six binaries), and the two stamps are projected
# out of it; this clause recomputes the record, requires the committed one to
# match, requires the record to have the v2 shape, and requires each stamp to
# equal its field.
k_err=""
k_want="$("$RECORD_TOOL" "$CPIO" 2>/dev/null || true)"
k_want2="$("$RECORD_TOOL" "$CPIO" 2>/dev/null || true)"
k_count=$(printf '%s\n' "$k_want" | wc -l)
k_l1=$(printf '%s\n' "$k_want" | sed -n 1p)
k_l2=$(printf '%s\n' "$k_want" | sed -n 2p)
k_l3=$(printf '%s\n' "$k_want" | sed -n 3p)
k_sources="${k_l2#sources }"
k_bytes="${k_l3#bytes }"
k_bins_ok=1
for k_name in $BIN_ENTRIES; do
    printf '%s\n' "$k_want" | grep -Eq "^binary ${k_name} [0-9a-f]{64}$" || k_bins_ok=0
done
if [ -z "$k_want" ]; then
    k_err="cannot be evaluated: $RECORD_TOOL printed nothing for $CPIO — there is no record to bind the stamps with"
elif [ "$k_count" -ne 9 ] || [ "$k_l1" != "sidecar-stamp-record-v2" ] || [ "$k_bins_ok" -ne 1 ] \
     || ! printf '%s' "$k_l2" | grep -Eq '^sources [0-9a-f]{64}$' \
     || ! printf '%s' "$k_l3" | grep -Eq '^bytes [0-9a-f]{64}$'; then
    k_err="cannot be evaluated: $RECORD_TOOL no longer answers with the nine-line record (tag v2, 'sources <sha256>', 'bytes <sha256>', and the six 'binary boot/*.bin <sha256>' lines)"
elif [ "$k_want" != "$k_want2" ]; then
    k_err="cannot be evaluated: $RECORD_TOOL answered differently on two runs of the same tree — a record that is not stable cannot bind anything"
elif [ ! -f "$RECORD" ]; then
    k_err="$RECORD is missing — nothing shows that $STAMP and $ARCHIVE_STAMP were written by one packer run, and each can be refreshed on its own (a user/ edit plus a hand-run source digest over the old archive leaves B and H green while the old binaries ship). make selfhost-bootimage writes it"
elif [ "$(cat "$RECORD")" != "$k_want" ]; then
    k_err="$RECORD does not match what the instruments compute now — the record and the tree have drifted apart, so the stamps cannot both be one pack run's (or the record was edited; it is written only by the packer's stamp step)"
elif [ "$(cat "$STAMP")" != "$k_sources" ]; then
    k_err="$STAMP is not the sources field of $RECORD — that stamp was refreshed without the record, which is exactly a stamp drifting independently of the archive it belongs to"
elif [ "$(cat "$ARCHIVE_STAMP")" != "$k_bytes" ]; then
    k_err="$ARCHIVE_STAMP is not the bytes field of $RECORD — the bytes stamp was refreshed without the record, so the three files are not one run's"
else
    k_missing=""
    case "$sbody" in *"$RECORD_TOOL"*) ;; *) k_missing="$k_missing record-tool" ;; esac
    case "$sbody" in *'> "$(SIDECAR_STAMP_RECORD)"'*) ;; *) k_missing="$k_missing record-write" ;; esac
    grep -Eq '^SIDECAR_STAMP_RECORD[[:space:]]*\?=[[:space:]]*\$\(SIDECAR_CPIO\)\.stamps[[:space:]]*$' Makefile \
        || k_missing="$k_missing per-archive-name"
    grep -qF 'sidecar_source_digest.sh' "$RECORD_TOOL" || k_missing="$k_missing sources-instrument"
    grep -qF 'sidecar_archive_digest.sh' "$RECORD_TOOL" || k_missing="$k_missing archive-instrument"
    grep -qF 'sidecar_archive_entry_digest.sh' "$RECORD_TOOL" || k_missing="$k_missing entry-instrument"
    if [ -n "$k_missing" ]; then
        k_err="selfhost-bootimage is missing the coupling:$k_missing — the record must be written by the packing target, derive its name from \$(SIDECAR_CPIO), and be computed from the three instruments; a record from anywhere else (or one that reimplements the halves) binds a different claim than the one x86-iso recomputes"
    else
        k_flat=$(printf '%s\n' "$sbody" | flatten)
        k_pack=$(printf '%s\n' "$k_flat" | grep -nF -e '-o "$(SIDECAR_CPIO)"' | head -1 | cut -d: -f1)
        k_rec=$(printf '%s\n' "$k_flat" | grep -nF '> "$(SIDECAR_STAMP_RECORD)"' | head -1 | cut -d: -f1)
        k_dig=$(printf '%s\n' "$k_flat" | grep -nF '> "$(SIDECAR_STAMP)"' | head -1 | cut -d: -f1)
        k_bts=$(printf '%s\n' "$k_flat" | grep -nF '> "$(SIDECAR_ARCHIVE_STAMP)"' | head -1 | cut -d: -f1)
        if [ -z "${k_pack:-}" ] || [ -z "${k_rec:-}" ] || [ -z "${k_dig:-}" ] || [ -z "${k_bts:-}" ]; then
            k_err="selfhost-bootimage's record/pack/stamp anchors have moved (pack '${k_pack:-none}', record '${k_rec:-none}', digest '${k_dig:-none}', bytes '${k_bts:-none}') — the guard cannot say what is written after what"
        elif [ "$k_rec" -le "$k_pack" ]; then
            k_err="selfhost-bootimage writes the record (command $k_rec) at or before it packs the archive (command $k_pack) — a record written first describes the previous archive"
        elif [ "$k_dig" -le "$k_rec" ] || [ "$k_bts" -le "$k_rec" ]; then
            k_err="selfhost-bootimage writes a stamp (digest $k_dig, bytes $k_bts) at or before the record it must be projected from (command $k_rec)"
        else
            k_dline=$(printf '%s\n' "$k_flat" | sed -n "${k_dig}p")
            k_bline=$(printf '%s\n' "$k_flat" | sed -n "${k_bts}p")
            case "$k_dline" in
                *'$(SIDECAR_STAMP_RECORD)'*) ;;
                *) k_err="the line writing $STAMP does not read $RECORD — the stamp is computed on its own again, which is the drift this clause exists for" ;;
            esac
            if [ -z "$k_err" ]; then
                case "$k_dline" in
                    *'sidecar_source_digest.sh'*) k_err="the line writing $STAMP recomputes the source digest directly instead of projecting it from $RECORD" ;;
                esac
            fi
            if [ -z "$k_err" ]; then
                case "$k_bline" in
                    *'$(SIDECAR_STAMP_RECORD)'*) ;;
                    *) k_err="the line writing $ARCHIVE_STAMP does not read $RECORD — the bytes stamp is computed on its own again" ;;
                esac
            fi
            if [ -z "$k_err" ]; then
                case "$k_bline" in
                    *'sidecar_archive_digest.sh'*) k_err="the line writing $ARCHIVE_STAMP hashes the archive directly instead of projecting it from $RECORD" ;;
                esac
            fi
            if [ -z "$k_err" ]; then
                k_rline=$(printf '%s\n' "$k_flat" | sed -n "${k_rec}p")
                k_vfy_missing=""
                case "$k_rline" in *'--verify'*) ;; *) k_vfy_missing=" --verify" ;; esac
                for k_binvar in SIDECAR_INIT_BIN SIDECAR_DM_BIN SIDECAR_POSIX_BIN SIDECAR_RAMDISK_BIN SIDECAR_NET_BIN SIDECAR_E1000_BIN; do
                    case "$k_rline" in *"$k_binvar"*) ;; *) k_vfy_missing="$k_vfy_missing \$$k_binvar" ;; esac
                done
                if [ -n "$k_vfy_missing" ]; then
                    k_err="the record step does not hand the pack's flattened binaries for verification:$k_vfy_missing — the record must be checked against the files this run produced, not only derived from the archive it wrote"
                fi
            fi
            if [ -z "$k_err" ]; then
                kx_missing=""
                if [ -z "$xbody" ]; then
                    kx_missing="$kx_missing x86-iso-recipe"
                else
                    case "$xbody" in *"$RECORD_TOOL"*) ;; *) kx_missing="$kx_missing record-tool" ;; esac
                    case "$xbody" in *'$(SIDECAR_STAMP_RECORD)'*) ;; *) kx_missing="$kx_missing record-file" ;; esac
                    printf '%s\n' "$xbody" | grep -qE 'exit [1-9]' || kx_missing="$kx_missing refuse"
                fi
                if [ -n "$kx_missing" ]; then
                    k_err="x86-iso is missing:$kx_missing — without recomputing the record and refusing on a mismatch, the build ships an archive whose stamps may have been refreshed independently of each other"
                else
                    kx_tool=$(printf '%s\n' "$xbody" | flatten | grep -nF "$RECORD_TOOL" | head -1 | cut -d: -f1)
                    kx_copy=$(printf '%s\n' "$xbody" | flatten | grep -nF 'isodir/boot/sidecars.cpio' | head -1 | cut -d: -f1)
                    if [ -z "${kx_copy:-}" ]; then
                        k_err="the x86-iso recipe no longer copies the archive into isodir/boot/sidecars.cpio — the guard's anchor has moved, so it cannot say whether the record check runs before the copy"
                    elif [ "${kx_tool:-0}" -ge "$kx_copy" ]; then
                        k_err="x86-iso verifies the record (command $kx_tool) at or after it copies the archive (command $kx_copy) — the check must gate the copy, not report on it afterwards"
                    fi
                fi
            fi
        fi
    fi
fi
if [ -n "$k_err" ]; then
    bad "K. $k_err"
else
    ok "K. $CPIO, $STAMP and $ARCHIVE_STAMP all come from one pack run — the committed record matches a live recomputation of every half ($k_sources / $k_bytes / the six binaries), it has the v2 shape, and each stamp is a field of it"
fi

# ── L. the record's six binaries are the archive's ────────────────────────
# K verifies the record against the instruments; this clause verifies it
# against the ARCHIVE with the guard's own coreutils extraction, so a record
# that names different bytes than the archive holds is red even if the
# instrument that wrote it agrees with itself. It is a content check, not a
# provenance check: cargo-free, the guard can see WHICH binaries are packed,
# not whether the current sources built them.
l_err=""
for l_name in $BIN_ENTRIES; do
    l_own=$(newc_entry_sha "$CPIO" "$l_name" 2>/dev/null || true)
    l_tool=$(bash "$ENTRY_TOOL" "$CPIO" "$l_name" 2>/dev/null || true)
    l_rec=$(sed -n "s|^binary $l_name ||p" "$RECORD" 2>/dev/null | head -1)
    if [ -z "$l_own" ]; then
        l_err="$CPIO has no extractable entry '$l_name' — the archive the record names is not the archive on disk (truncated, a binary removed, or not a newc archive at all)"
        break
    fi
    if [ "$l_tool" != "$l_own" ]; then
        l_err="$ENTRY_TOOL answered '${l_tool:-<nothing>}' for $l_name where an independent extraction of $CPIO gives $l_own — the instrument that names the packed binaries does not read them"
        break
    fi
    if [ "$l_rec" != "$l_own" ]; then
        l_err="$RECORD's line for $l_name records '${l_rec:-<missing>}' where the archive entry hashes to $l_own — the record does not describe the bytes on disk"
        break
    fi
done
if [ -n "$l_err" ]; then
    bad "L. $l_err"
else
    ok "L. the six binaries the record names are the archive's — every boot/*.bin entry matches both $ENTRY_TOOL and an independent coreutils extraction, so the record cannot misstate what is packed"
fi

# ── M. the archive's binaries are the flattened build outputs, when present ─
# The record, its two stamps and the archive can be rewritten together from a
# stale pack, and K and L stay green because they compare the files to each
# other. This clause compares the archive to something outside that set: the
# flattened `.bin` files `selfhost-bootimage` produces, read from the Makefile
# exactly as the recipe passes them. The one exemption is an init.bin that IS
# the E3 archive's entry, and only when the committed E3 record equals a live
# recomputation of sidecars_e3.cpio: the e3_envs pack overwrites the shared
# flattened file, and a record alone cannot excuse a mismatch.
m_err=""
m_cmp=0
m_var=0
# The E3 record is the only record allowed to explain an init mismatch, and
# only after it has been ANCHORED: a live recomputation of sidecars_e3.cpio
# must equal the committed E3 record. A hand-written record that merely names
# the flattened init would otherwise excuse a stale one in the archive.
m_e3_ok=0
if [ -s "$E3_CPIO" ] && [ -f "$E3_RECORD" ]; then
    m_e3_shot=$("$RECORD_TOOL" "$E3_CPIO" 2>/dev/null || true)
    if [ -n "$m_e3_shot" ] && [ "$m_e3_shot" = "$(cat "$E3_RECORD")" ]; then
        m_e3_ok=1
    fi
fi
for m_name in $BIN_ENTRIES; do
    case "$m_name" in
        boot/init.bin)    m_var_name=SIDECAR_INIT_BIN ;;
        boot/dm.bin)      m_var_name=SIDECAR_DM_BIN ;;
        boot/posix.bin)   m_var_name=SIDECAR_POSIX_BIN ;;
        boot/ramdisk.bin) m_var_name=SIDECAR_RAMDISK_BIN ;;
        boot/net.bin)     m_var_name=SIDECAR_NET_BIN ;;
        boot/e1000.bin)   m_var_name=SIDECAR_E1000_BIN ;;
        *) continue ;;
    esac
    m_path=$(sed -n "s/^${m_var_name}[[:space:]]*?=[[:space:]]*//p" Makefile | head -1)
    [ -n "$m_path" ] || continue
    [ -f "$m_path" ] || continue
    m_own=$(newc_entry_sha "$CPIO" "$m_name" 2>/dev/null || true)
    [ -n "$m_own" ] || continue   # an entry the archive does not hold is clause L's failure
    m_file=$(sha256sum -- "$m_path" 2>/dev/null | cut -d' ' -f1)
    if [ -z "$m_file" ]; then
        m_err="cannot hash $m_path — the flattened output the Makefile names for $m_name is unreadable"
        break
    fi
    if [ "$m_file" = "$m_own" ]; then
        m_cmp=$((m_cmp + 1))
        continue
    fi
    m_variant=""
    if [ "$m_name" = "boot/init.bin" ] && [ "$m_e3_ok" -eq 1 ] \
        && grep -q "^binary ${m_name} ${m_file}$" "$E3_RECORD"; then
        # Only init can differ between this tree's pack variants (e3_envs
        # rewrites it and overwrites the shared file); the five shared entries
        # are compared absolutely, and the record above had to be anchored.
        m_variant="$E3_RECORD"
    fi
    if [ -n "$m_variant" ]; then
        m_var=$((m_var + 1))
        continue
    fi
    m_err="$m_path is the flattened output this tree built for $m_name and it hashes to $m_file, while $CPIO's entry hashes to $m_own — the archive holds a different image than the build did. A record and two stamps rewritten from a stale archive agree with each other; the build output is the half they cannot rewrite. (An init named by a hand-written E3 record is not exempt: the record must match a live recomputation of $E3_CPIO.) Re-pack from these sources: make selfhost-bootimage"
    break
done
if [ -n "$m_err" ]; then
    bad "M. $m_err"
elif [ -z "$xbody" ]; then
    bad "M. cannot be evaluated: the Makefile has no x86-iso recipe (see C)"
else
    m_wire=""
    case "$xbody" in *"$BUILD_CHECK_TOOL"*) ;; *) m_wire="$m_wire check-tool" ;; esac
    case "$xbody" in *--build*) ;; *) m_wire="$m_wire build-paths" ;; esac
    for m_binvar in SIDECAR_INIT_BIN SIDECAR_DM_BIN SIDECAR_POSIX_BIN SIDECAR_RAMDISK_BIN SIDECAR_NET_BIN SIDECAR_E1000_BIN; do
        case "$xbody" in *"$m_binvar"*) ;; *) m_wire="$m_wire \$$m_binvar" ;; esac
    done
    printf '%s\n' "$xbody" | grep -qE 'exit [1-9]' || m_wire="$m_wire refuse"
    if [ -n "$m_wire" ]; then
        bad "M. the x86-iso recipe is missing:$m_wire — without running $BUILD_CHECK_TOOL over the six flattened paths and refusing on a mismatch, a stale archive whose record and stamps were all rewritten from it ships while every record-vs-archive check stays green"
    else
        m_line=$(printf '%s\n' "$xbody" | flatten | grep -nF "$BUILD_CHECK_TOOL" | head -1 | cut -d: -f1)
        m_copy=$(printf '%s\n' "$xbody" | flatten | grep -nF 'isodir/boot/sidecars.cpio' | head -1 | cut -d: -f1)
        if [ -z "${m_copy:-}" ]; then
            bad "M. the x86-iso recipe no longer copies the archive into isodir/boot/sidecars.cpio — the guard's anchor has moved, so it cannot say whether the build-output check runs before the copy"
        elif [ "${m_line:-0}" -ge "$m_copy" ]; then
            bad "M. x86-iso checks the archive against the build outputs (command $m_line) at or after it copies it (command $m_copy) — the check must gate the copy, not report on it afterwards"
        else
            ok "M. the archive's six binaries are this tree's flattened outputs wherever those exist — $m_cmp compared, $m_var skipped as a packed variant (the e3_envs init, anchored to the E3 archive), and x86-iso runs the check (command $m_line) before it copies the archive (command $m_copy)"
        fi
    fi
fi

# ── N. the E3 tuple is one pack run's too, when it is here ────────────────
# The e3_envs pack is a second archive written by the same target under a
# different name (SIDECAR_CPIO is overridden), and all four of its files are
# .gitignored build artifacts (F). On CI, a fresh clone or any host that never
# ran it they are absent, and the clause says so instead of aborting: "could
# not check" must not look like "it is fine", but neither must a pack that is
# not here look like drift. Where the files ARE here, the rule is K's, one
# archive over -- one tool invocation computes every half of the E3 record,
# and the two E3 stamps are projections of it. That matters beyond tidiness:
# M is allowed to skip an init mismatch when this record names the flattened
# init, and M's own anchor only recomputes the record at the moment it needs
# it; N is what keeps the committed record honest even on a tree where no
# init mismatch happens to be present.
n_err=""
if [ ! -s "$E3_CPIO" ] || [ ! -f "$E3_RECORD" ]; then
    ok "N. no E3 pack in this tree ($E3_CPIO and $E3_RECORD are ignored build files; at least one of them is absent) — the E3 one-run rule is skipped, not assumed"
else
    n_want=$("$RECORD_TOOL" "$E3_CPIO" 2>/dev/null || true)
    n_want2=$("$RECORD_TOOL" "$E3_CPIO" 2>/dev/null || true)
    n_count=$(printf '%s\n' "$n_want" | wc -l)
    n_l1=$(printf '%s\n' "$n_want" | sed -n 1p)
    n_l2=$(printf '%s\n' "$n_want" | sed -n 2p)
    n_l3=$(printf '%s\n' "$n_want" | sed -n 3p)
    n_sources="${n_l2#sources }"
    n_bytes="${n_l3#bytes }"
    n_bins_ok=1
    for n_name in $BIN_ENTRIES; do
        printf '%s\n' "$n_want" | grep -Eq "^binary ${n_name} [0-9a-f]{64}$" || n_bins_ok=0
    done
    if [ -z "$n_want" ]; then
        n_err="cannot be evaluated: $RECORD_TOOL printed nothing for $E3_CPIO — there is no recomputation to hold the committed E3 record to"
    elif [ "$n_count" -ne 9 ] || [ "$n_l1" != "sidecar-stamp-record-v2" ] || [ "$n_bins_ok" -ne 1 ] \
         || ! printf '%s' "$n_l2" | grep -Eq '^sources [0-9a-f]{64}$' \
         || ! printf '%s' "$n_l3" | grep -Eq '^bytes [0-9a-f]{64}$'; then
        n_err="cannot be evaluated: $RECORD_TOOL no longer answers with the nine-line record for $E3_CPIO (tag v2, 'sources <sha256>', 'bytes <sha256>', and the six 'binary boot/*.bin <sha256>' lines)"
    elif [ "$n_want" != "$n_want2" ]; then
        n_err="cannot be evaluated: $RECORD_TOOL answered differently on two runs over $E3_CPIO — a record that is not stable cannot bind anything"
    elif [ "$(cat "$E3_RECORD")" != "$n_want" ]; then
        n_err="$E3_RECORD does not match what the instruments compute over $E3_CPIO now — the committed E3 record and the E3 archive have drifted apart, and that record is the file clause M accepts as proof that an e3_envs init.bin is the packed one (a hand-written record must not read as a verified variant). Re-pack the E3 image (make selfhost-bootimage-e3) or remove its four files"
    elif [ "$(cat "$E3_STAMP" 2>/dev/null || true)" != "$n_sources" ]; then
        n_err="$E3_STAMP is absent or is not the sources field of $E3_RECORD — it was written or removed without the record that computes it, so the four E3 files are not one pack run's. make selfhost-bootimage-e3 writes all four together"
    elif [ "$(cat "$E3_ARCHIVE_STAMP" 2>/dev/null || true)" != "$n_bytes" ]; then
        n_err="$E3_ARCHIVE_STAMP is absent or is not the bytes field of $E3_RECORD — the E3 bytes stamp was written or removed without the record, so the four E3 files are not one pack run's"
    else
        ok "N. the E3 tuple is one pack run's too — $E3_RECORD matches a live recomputation of $E3_CPIO (the v2 nine-line record, all six binaries), and $E3_STAMP and $E3_ARCHIVE_STAMP are its sources and bytes fields"
    fi
    if [ -n "$n_err" ]; then
        bad "N. $n_err"
    fi
fi

echo
if [ "$fail" -eq 0 ]; then
    echo "sidecar_stamp_check: the committed sidecar archive is the one the committed user/ sources produce, its bytes are the ones that were packed, all three files are one packer run's, the six binaries those files name are the archive's, the archive matches this tree's flattened build outputs wherever those exist, only a run that BUILT the sidecars can stamp them, the ignored E3 pack is held to that same one-run rule wherever it is present, and make x86-iso enforces all of it."
    exit 0
fi
echo "sidecar_stamp_check: the committed archive, its bytes, its stamps, its named binaries, this tree's flattened outputs and the committed sources do not all agree (or the rule that enforces them is gone)."
exit 1
