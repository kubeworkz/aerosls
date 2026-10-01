#!/usr/bin/env bash
# tests/sidecar_stamp_check_smoke.sh — the TEETH for
# tests/sidecar_stamp_check.sh.
#
# ─── What a teeth smoke is for ─────────────────────────────────────────────
# A guard that has never been seen to fail is a guard nobody can trust. Each
# tooth below breaks ONE property of the stamp rule in a throwaway copy of the
# tree and requires the guard to (a) exit 1 and (b) name THAT clause. Requiring
# the right named clause is the half that matters: a guard that reddened on
# everything, or on the wrong clause, would pass a tooth that only checked the
# exit code.
#
# The first B tooth is the whole reason this pair exists: it is a `user/` change
# with the archive left alone — the incident, in one line — and no other guard in
# the tree could see it, because every other guard reads the sources or the
# kernel and the stale thing is a COMMITTED ARTEFACT.
#
# The H teeth are its mirror: the ARCHIVE changes and `user/` does not -- a
# repack or a swap out of band -- which the source digest cannot see and the
# archive's own sha256 (sidecars.cpio.sha256) can. The I and J teeth hold the two
# ends that keep that hash honest: the packer writes it after packing, and
# x86-iso checks it before it copies.
#
# The K teeth are the coupling, and the first of them is the reason the record
# exists: edit a `user/` source and hand-run the source-digest tool over the old
# archive, and B is green again (the stamp DOES match the sources), H is green
# (the bytes ARE the stamped ones) -- only K can see that no run wrote both
# files together, because sidecars.cpio.stamps is the record both stamps are
# projected from. Neither of the other two checks can: each is only ever asked
# about its own subject. The rest of K's teeth break the record itself (missing,
# edited), its derivation (a stamp recomputed on its own again), its writer
# (the packer stops writing it), the build's verification (x86-iso stops
# checking it), and its per-archive name (the E3 image would be checked against
# the default's).
#
# The L teeth go one level deeper: the v2 record NAMES the six packed binaries,
# and L re-extracts each one from the archive with coreutils alone. That is the
# clause a record cannot argue with -- even an instrument that lies, or a
# record derived from files that are not the packed ones, is caught by the
# independent extraction. Its teeth swap an entry out of the archive, edit a
# binary line, swap two lines, drop a binary from the record, and replace the
# entry instrument with one that answers without reading.
#
# The M teeth close the loop the record alone cannot: a record and its stamps
# can ALL be rewritten from a stale archive, and K and L stay green because
# they compare those files to each other. M compares the archive to something
# outside that set -- the flattened `.bin` files `selfhost-bootimage` produced,
# which the seed writes too (its copy excludes `target/`, so the six files the
# stub entries would have been flattened from are created explicitly). The
# teeth move one build output, rewrite the whole record consistently from a
# stale archive (the residual state K and L cannot see), delete the recipe's
# call, and write an E3 record that merely NAMES the flattened init without an
# archive to recompute it from -- a record is not an authority by itself. The
# green arm is the e3_envs pack overwriting the shared init.bin: an init that
# IS the E3 archive's entry, with the E3 record verified against a live
# recomputation of that archive, must be skipped, not refused, or
# `make x86-iso` would block after every E3 pack.
#
# The N teeth hold the E3 pack's OWN four files to the one-run rule wherever
# they exist: the two E3 stamps must be fields of a record that recomputes to
# the E3 archive, so a hand-refreshed E3 stamp or a hand-written E3 record
# cannot be the file M leans on. The quartet is .gitignored build output, so
# the vacuity control's tree has none of it (N skips); the green arm is an E3
# archive with no record -- half a quartet is not assumed to be one run's.
#
# ─── The hermetic seam, and why the copy is a whole tree ───────────────────
# The guard's optional ROOT argument is the seam: it inspects a repository root,
# defaulting to its own parent. The copy therefore has to be real enough for the
# instrument it calls — the digest tool walks `user/`, and the stamp it is
# compared against is computed from those files — so the seed copies the whole
# `user/` tree (3.2 MiB, no `target/`) rather than a handful of files. A copy
# that was missing one covered source would make the digest differ for a reason
# the tooth did not intend, which is how a smoke starts lying.
#
# No network, no build, no Rust: bash/sed/tar/python3 and the tree's own tool.
# That puts it in tests/run_source_smokes.sh's set by construction, so its teeth
# are proven on every push.
#
# The last arm is the vacuity control: the guard must be GREEN on the real,
# unmutated tree. Without it, a guard that failed unconditionally would pass
# every tooth in this file.
#
# Exit: 0 every tooth bit, 1 a tooth did not.
set -u
cd "$(dirname "$0")/.." || exit 2
ROOT="$(pwd)"
GUARD="$ROOT/tests/sidecar_stamp_check.sh"

[ -f "$GUARD" ] || { echo "ABORT: $GUARD not found — run from the repo root." >&2; exit 2; }
[ -f "$ROOT/sidecars.cpio.digest" ] || { echo "ABORT: sidecars.cpio.digest not found — the guard has nothing to be about." >&2; exit 2; }
[ -f "$ROOT/sidecars.cpio.sha256" ] || { echo "ABORT: sidecars.cpio.sha256 not found — the guard has nothing to bind the archive's own bytes to." >&2; exit 2; }
[ -f "$ROOT/sidecars.cpio.stamps" ] || { echo "ABORT: sidecars.cpio.stamps not found — the guard has nothing to bind the two stamps to each other." >&2; exit 2; }
[ -f "$ROOT/tools/sidecar_archive_digest.sh" ] || { echo "ABORT: tools/sidecar_archive_digest.sh not found — the guard cannot hash the archive." >&2; exit 2; }
[ -f "$ROOT/tools/sidecar_stamp_record.sh" ] || { echo "ABORT: tools/sidecar_stamp_record.sh not found — the guard cannot recompute the pack-run record." >&2; exit 2; }
[ -f "$ROOT/tools/sidecar_archive_entry_digest.sh" ] || { echo "ABORT: tools/sidecar_archive_entry_digest.sh not found — the guard cannot read the packed binaries out of the archive." >&2; exit 2; }
[ -f "$ROOT/tools/sidecar_build_output_check.sh" ] || { echo "ABORT: tools/sidecar_build_output_check.sh not found — the seed cannot carry the check the recipe calls." >&2; exit 2; }

W="$(mktemp -d)"
cleanup() { rm -rf "$W"; }
trap cleanup EXIT
trap 'cleanup; exit 1' TERM INT

# ─── The stub archive, and why it is a real newc ─────────────────────────
# The v2 record NAMES the six packed binaries and clause L re-extracts each one
# from the archive, so a text stub can no longer stand in for an archive: a
# seeded tree must hold a valid `newc` image or the guard could not pass on it.
# This builds the smallest one that does -- the six boot/*.bin entries the
# record names, plus a `boot/layout` entry that comes FIRST and whose text
# LISTS all six names, exactly like the real one. That last part matters: both
# the entry tool and the guard's own extraction find an entry by searching for
# its name, so the layout's text is a built-in false positive that must be
# skipped by checking the 110-byte header that precedes a real entry (see
# user/bootimage/src/newc.rs: c_filesize at +54, c_namesize at +94, name and
# data each padded to a 4-byte boundary).
#
# newc_stub <archive> [drop:ENTRY]... [edit:ENTRY=TEXT]...
#   The spec arguments let a tooth change exactly one entry: `drop:` rebuilds
#   the archive without it, `edit:` rebuilds it with different bytes for it.
newc_stub() {   # newc_stub <archive> [spec]...
    python3 - "$@" <<'PY'
import sys

out, specs = sys.argv[1], sys.argv[2:]
drop, edits = set(), {}
for spec in specs:
    kind, _, rest = spec.partition(":")
    if kind == "drop":
        drop.add(rest)
    elif kind == "edit":
        name, sep, text = rest.partition("=")
        if not sep:
            sys.exit("newc_stub: edit: expects ENTRY=TEXT")
        edits[name] = text
    else:
        sys.exit("newc_stub: unknown spec '%s'" % spec)

names = ["boot/init.bin", "boot/dm.bin", "boot/posix.bin",
         "boot/ramdisk.bin", "boot/net.bin", "boot/e1000.bin"]

def entry(name, data):
    namez = name.encode() + b"\0"
    hdr = b"070701"
    hdr += b"%08x" % 1           # c_ino
    hdr += b"%08x" % 0o100644    # c_mode
    hdr += b"%08x" % 0           # c_uid
    hdr += b"%08x" % 0           # c_gid
    hdr += b"%08x" % 1           # c_nlink
    hdr += b"%08x" % 0           # c_mtime
    hdr += b"%08x" % len(data)   # c_filesize (at +54)
    hdr += b"%08x" % 0           # c_devmajor
    hdr += b"%08x" % 0           # c_devminor
    hdr += b"%08x" % 0           # c_rdevmajor
    hdr += b"%08x" % 0           # c_rdevminor
    hdr += b"%08x" % len(namez)  # c_namesize (at +94)
    hdr += b"%08x" % 0           # c_check
    blob = hdr + namez
    blob += b"\0" * ((4 - len(namez) % 4) % 4)
    blob += data
    blob += b"\0" * ((4 - len(data) % 4) % 4)
    return blob

data = {"boot/layout": "".join("bin %s\n" % n for n in names).encode()}
for n in names:
    data[n] = ("stub %s\n" % n).encode()
for n, text in edits.items():
    data[n] = text.encode()
order = ["boot/layout"] + [n for n in names if n not in drop]
blob = b"".join(entry(n, data[n]) for n in order)
blob += entry("TRAILER!!!", b"")
open(out, "wb").write(blob)
PY
}

seed() {   # seed <dir> — a tree the guard must PASS on
    rm -rf "$1"
    mkdir -p "$1/tools" "$1/user"
    cp "$ROOT/Makefile" "$ROOT/.gitignore" "$ROOT/sidecars.cpio.digest" "$1/" || return 1
    cp "$ROOT/tools/sidecar_source_digest.sh" "$ROOT/tools/sidecar_archive_digest.sh" \
       "$ROOT/tools/sidecar_archive_entry_digest.sh" "$ROOT/tools/sidecar_stamp_record.sh" \
       "$ROOT/tools/sidecar_build_output_check.sh" "$1/tools/" || return 1
    # The whole user/ tree, so the digest over the copy is the digest over the
    # real one. target/ is the cross-build output and is neither covered nor
    # small (3.2 GiB against 3.2 MiB).
    ( cd "$ROOT/user" && tar cf - --exclude=./target . ) | ( cd "$1/user" && tar xf - ) || return 1
    # The archive is a minimal but REAL newc image holding the six boot/*.bin
    # entries the record names (newc_stub above): the seeded tree is one the
    # guard's clause L passes on, its layout's textual hit is exercised, and
    # K's live recomputation has a real archive to recompute over. It stays a
    # few hundred bytes rather than a copy of the 1.2 MiB archive, and the
    # H/K/L teeth repack, edit or drop exactly one of its entries.
    newc_stub "$1/sidecars.cpio" || return 1
    sha256sum -- "$1/sidecars.cpio" | cut -d' ' -f1 > "$1/sidecars.cpio.sha256" || return 1
    # The flattened build outputs clause M compares the archive against, at
    # the makefile's own paths (two do not share their entry's basename:
    # boot/net.bin -> network.bin, boot/e1000.bin -> e1000_driver.bin).
    m_flat="$1/user/target/x86_64-unknown-none/release"
    mkdir -p "$m_flat" || return 1
    printf 'stub boot/init.bin\n'    > "$m_flat/init.bin" || return 1
    printf 'stub boot/dm.bin\n'      > "$m_flat/dm.bin" || return 1
    printf 'stub boot/posix.bin\n'   > "$m_flat/posix.bin" || return 1
    printf 'stub boot/ramdisk.bin\n' > "$m_flat/ramdisk.bin" || return 1
    printf 'stub boot/net.bin\n'     > "$m_flat/network.bin" || return 1
    printf 'stub boot/e1000.bin\n'   > "$m_flat/e1000_driver.bin" || return 1
    # The pack-run record must describe the stub too: K recomputes it from the
    # copied tools (sources over the copied user/, bytes and the six entries
    # over the stub), so a seeded tree is one the guard passes on, and the K
    # teeth break exactly that.
    ( cd "$1" && tools/sidecar_stamp_record.sh sidecars.cpio > sidecars.cpio.stamps ) || return 1
    return 0
}

passed=0
failed=0

tooth() {   # tooth <label> <expected-clause> <dir>
    local label="$1" clause="$2" dir="$3" out rc
    out="$(bash "$GUARD" "$dir" 2>&1)"; rc=$?
    if [ "$rc" -eq 1 ] && printf '%s\n' "$out" | grep -q "^FAIL: ${clause}\."; then
        echo "ok:   $label (guard exits 1 naming '$clause')"
        passed=$((passed + 1))
    else
        echo "FAIL: $label did NOT bite — exit $rc, expected 1 with 'FAIL: $clause.'"
        printf '%s\n' "$out" | sed 's/^/      /'
        failed=$((failed + 1))
    fi
}

echo "sidecar_stamp_check_smoke — teeth for the committed-archive stamp guard"
echo "======================================================================"
echo

# ── A. the instrument itself ──────────────────────────────────────────────
# A digest that is not a digest, and one that is not stable, both make every
# clause below meaningless -- so both are asserted, not assumed.
seed "$W/a1"
sed -i "s/^} | sha256sum | cut -d' ' -f1$/} | sha256sum | cut -c1-10/" "$W/a1/tools/sidecar_source_digest.sh"
tooth "A: the tool answers with something that is not a sha256 line" A "$W/a1"

seed "$W/a2"
printf 'echo extra-line\n' >> "$W/a2/tools/sidecar_source_digest.sh"
tooth "A: the tool answers with more than one line (the stamp is one line)" A "$W/a2"

seed "$W/a3"
printf '# a source the tool must notice\n' >> "$W/a3/user/init/src/env_manager.rs"
tooth "B: a source the tool already covers was edited" B "$W/a3"

# ── B. THE clause: the committed stamp against the committed sources ──────
seed "$W/b1"
printf '\n// tooth\n' >> "$W/b1/user/init/src/env_manager.rs"
tooth "B: a user/ source was committed without re-packing the archive (the incident)" B "$W/b1"

seed "$W/b2"
rm -f "$W/b2/sidecars.cpio.digest"
tooth "B: the stamp is missing, so nothing says what the archive was packed from" B "$W/b2"

seed "$W/b3"
printf '0000000000000000000000000000000000000000000000000000000000000000\n' > "$W/b3/sidecars.cpio.digest"
tooth "B: the stamp records a different (stale) digest" B "$W/b3"

seed "$W/b4"
mkdir -p "$W/b4/user/newcrate/src"
printf 'pub fn new_thing() {}\n' > "$W/b4/user/newcrate/src/lib.rs"
tooth "B: a NEW covered source was added and the stamp was not refreshed" B "$W/b4"

seed "$W/b5"
# The stamp's own line ending must not matter, and neither must an archive that
# the stamp has nothing to say about: the pair is what is checked.
printf '%s' "$(cat "$W/b5/sidecars.cpio.digest")" > "$W/b5/sidecars.cpio.digest"
out="$(bash "$GUARD" "$W/b5" 2>&1)"; rc=$?
if [ "$rc" -eq 0 ]; then
    echo "ok:   B: a stamp with no trailing newline still matches (the comparison is textual)"
    passed=$((passed + 1))
else
    echo "FAIL: B was red for a stamp whose only difference is a trailing newline"
    printf '%s\n' "$out" | sed 's/^/      /'
    failed=$((failed + 1))
fi

# ── C. x86-iso refuses, before it copies ──────────────────────────────────
seed "$W/c1"
python3 - "$W/c1/Makefile" <<'PY'
import sys
p = sys.argv[1]
lines = open(p).read().split("\n")
# Drop the line that computes the digest the recipe compares against: the
# refusal step is then still there, and still copies the archive.
out = [l for l in lines if 'want="$$(tools/sidecar_source_digest.sh)"' not in l]
open(p, "w").write("\n".join(out))
PY
tooth "C: the recipe stops computing the digest it compares the stamp against" C "$W/c1"

seed "$W/c2"
python3 - "$W/c2/Makefile" <<'PY'
import sys
p = sys.argv[1]
lines = open(p).read().split("\n")
start = next(i for i, l in enumerate(lines) if l.startswith("x86-iso:"))
end = next(i for i in range(start + 1, len(lines)) if lines[i] and not lines[i].startswith("\t"))
kept = []
for l in lines[start:end]:
    if l.lstrip().startswith("exit 1"):
        continue
    # Refusals written as `cmd || exit 1` on one line: drop the exit too, so
    # the mutation really makes every refusal a warning (a tool line added
    # later must not leave one behind for the guard to find).
    kept.append(l.replace("|| exit 1;", ";"))
open(p, "w").write("\n".join(lines[:start] + kept + lines[end:]))
PY
tooth "C: the refusal becomes a warning (the step no longer exits non-zero)" C "$W/c2"

seed "$W/c3"
python3 - "$W/c3/Makefile" <<'PY'
import sys
p = sys.argv[1]
lines = open(p).read().split("\n")
start = next(i for i, l in enumerate(lines) if l.startswith("x86-iso:"))
cp = next(l for l in lines if "isodir/boot/sidecars.cpio" in l)
# A second copy of the archive, as the FIRST thing the recipe does: the guard
# reads the first copy line, so the check now appears to run after shipping.
lines.insert(start + 1, cp)
open(p, "w").write("\n".join(lines))
PY
tooth "C: the archive is copied before it is verified" C "$W/c3"

# ── D. the target that packs writes the stamp ─────────────────────────────
seed "$W/d1"
python3 - "$W/d1/Makefile" <<'PY'
import sys
p = sys.argv[1]
lines = open(p).read().split("\n")
out = [l for l in lines if '> "$(SIDECAR_STAMP)"' not in l]
open(p, "w").write("\n".join(out))
PY
tooth "D: the packing target stops writing the stamp" D "$W/d1"

# ── E. the stamp is per-archive ───────────────────────────────────────────
seed "$W/e1"
sed -i 's/^SIDECAR_STAMP[[:space:]]*?=[[:space:]]*$(SIDECAR_CPIO)\.digest$/SIDECAR_STAMP      ?= sidecars.cpio.digest/' "$W/e1/Makefile"
tooth "E: the stamp name is fixed, so the E3 image would be verified against the default's" E "$W/e1"

# ── F. the committed stamp is not ignored ─────────────────────────────────
seed "$W/f1"
printf '*.digest\n' >> "$W/f1/.gitignore"
tooth "F: a wildcard .gitignore pattern swallows the committed stamp" F "$W/f1"

seed "$W/f2"
sed -i '/^sidecars_e3\.cpio\.digest$/d' "$W/f2/.gitignore"
tooth "F: the E3 stamp stops being ignored and turns up as an untracked file" F "$W/f2"

seed "$W/f3"
printf '*.sha256\n' >> "$W/f3/.gitignore"
tooth "F: a wildcard .gitignore pattern swallows the archive-bytes stamp" F "$W/f3"

seed "$W/f4"
sed -i '/^sidecars_e3\.cpio\.sha256$/d' "$W/f4/.gitignore"
tooth "F: the E3 archive-bytes stamp stops being ignored and turns up as an untracked file" F "$W/f4"

seed "$W/f5"
printf '*.stamps\n' >> "$W/f5/.gitignore"
tooth "F: a wildcard .gitignore pattern swallows the pack-run record" F "$W/f5"

seed "$W/f6"
sed -i '/^sidecars_e3\.cpio\.stamps$/d' "$W/f6/.gitignore"
tooth "F: the E3 record stops being ignored and turns up as an untracked file" F "$W/f6"

# ── G. only a run that BUILT the binaries may stamp ───────────────────────
# The regression this clause exists for: the packer's build fails (the cross
# target is missing) and rather than stopping, it warns and reuses whatever
# ELFs are on disk, then packs and STAMPS them as this tree's own. This is the
# historical shape of that line, restored verbatim.
seed "$W/g1"
python3 - "$W/g1/Makefile" <<'PY'
import re, sys
p = sys.argv[1]
t = open(p).read()
t2 = re.sub(r'\|\| \{ echo "\[SELFHOST\] REFUSING.*?exit 1; \}',
            '|| echo "[SELFHOST] warning: x86_64-unknown-none target not installed; using existing binaries"',
            t, flags=re.S)
assert t2 != t, "the fatal build handler was not found to replace"
open(p, "w").write(t2)
PY
tooth "G: a failed build warns and reuses existing binaries instead of stopping" G "$W/g1"

# The other shape of a fall-through: make told to ignore the build's exit code.
seed "$W/g2"
python3 - "$W/g2/Makefile" <<'PY'
import sys
p = sys.argv[1]
t = open(p).read()
t2 = t.replace("@$(CARGO) build", "-@$(CARGO) build", 1)
assert t2 != t, "no $(CARGO) build line to prefix with '-'"
open(p, "w").write(t2)
PY
tooth "G: the sidecar build is prefixed with '-' so make ignores its failure" G "$W/g2"

# Stamped before it was built: the stamp stops being a consequence of the build.
seed "$W/g3"
python3 - "$W/g3/Makefile" <<'PY'
import sys
p = sys.argv[1]
lines = open(p).read().split("\n")
stamp = [i for i, l in enumerate(lines)
         if 'sidecar_stamp_record.sh' in l and '> "$(SIDECAR_STAMP_RECORD)"' in l]
assert len(stamp) == 1, "expected exactly one record write"
line = lines.pop(stamp[0])
anchor = next(i for i, l in enumerate(lines) if 'building the init + DM' in l)
lines.insert(anchor + 1, line)
open(p, "w").write("\n".join(lines))
PY
tooth "G: the record (and the stamps projected from it) is written before the sidecar build" G "$W/g3"

# No build at all: the packer packs and stamps whatever is on disk.
seed "$W/g4"
python3 - "$W/g4/Makefile" <<'PY'
import sys
p = sys.argv[1]
t = open(p).read()
t2 = t.replace("$(CARGO) build", "true")
assert t2 != t, "no $(CARGO) build line to remove"
open(p, "w").write(t2)
PY
tooth "G: the packer stops building the sidecars it stamps" G "$W/g4"

# ── H. the archive's own bytes are stamped, and the stamp matches ────────
# The bytes half of the rule: the source stamp (B) cannot see an archive that
# was repacked or replaced with `user/` untouched. This is that tooth -- the
# archive changes, the sources do not, and the source stamp still matches.
seed "$W/h1"
printf 'a completely different archive, repacked by hand\n' > "$W/h1/sidecars.cpio"
tooth "H: the archive was repacked out of band (sources untouched, source stamp still matches)" H "$W/h1"

seed "$W/h2"
rm -f "$W/h2/sidecars.cpio.sha256"
tooth "H: the archive-bytes stamp is missing, so nothing says which bytes were packed" H "$W/h2"

seed "$W/h3"
printf '0000000000000000000000000000000000000000000000000000000000000000\n' > "$W/h3/sidecars.cpio.sha256"
tooth "H: the archive-bytes stamp records a different (stale) hash" H "$W/h3"

seed "$W/h4"
printf '%s' "$(cat "$W/h4/sidecars.cpio.sha256")" > "$W/h4/sidecars.cpio.sha256"
out="$(bash "$GUARD" "$W/h4" 2>&1)"; rc=$?
if [ "$rc" -eq 0 ]; then
    echo "ok:   H: an archive-bytes stamp with no trailing newline still matches"
    passed=$((passed + 1))
else
    echo "FAIL: H was red for an archive-bytes stamp whose only difference is a trailing newline"
    printf '%s\n' "$out" | sed 's/^/      /'
    failed=$((failed + 1))
fi

# ── I. the target that packs writes the bytes stamp ───────────────────────
seed "$W/i1"
python3 - "$W/i1/Makefile" <<'PY'
import sys
p = sys.argv[1]
lines = open(p).read().split("\n")
out = [l for l in lines if '> "$(SIDECAR_ARCHIVE_STAMP)"' not in l]
open(p, "w").write("\n".join(out))
PY
tooth "I: the packing target stops writing the bytes stamp" I "$W/i1"

seed "$W/i2"
python3 - "$W/i2/Makefile" <<'PY'
import sys
p = sys.argv[1]
lines = open(p).read().split("\n")
idx = [i for i, l in enumerate(lines) if '> "$(SIDECAR_ARCHIVE_STAMP)"' in l]
assert len(idx) == 1, "expected exactly one archive-bytes stamp write"
line = lines.pop(idx[0])
anchor = next(i for i, l in enumerate(lines) if l.startswith("selfhost-bootimage:"))
lines.insert(anchor + 1, line)
open(p, "w").write("\n".join(lines))
PY
tooth "I: the bytes stamp is written before the archive is packed" I "$W/i2"

seed "$W/i3"
sed -i 's/^SIDECAR_ARCHIVE_STAMP[[:space:]]*?=[[:space:]]*$(SIDECAR_CPIO)\.sha256$/SIDECAR_ARCHIVE_STAMP ?= sidecars.cpio.sha256/' "$W/i3/Makefile"
tooth "I: the bytes stamp name is fixed, so the E3 image would be checked against the default's bytes" I "$W/i3"

# ── J. x86-iso verifies the bytes stamp, before it copies ─────────────────
seed "$W/j1"
python3 - "$W/j1/Makefile" <<'PY'
import sys
p = sys.argv[1]
lines = open(p).read().split("\n")
out = [l for l in lines if 'want_sha="$$(tools/sidecar_archive_digest.sh "$(SIDECAR_CPIO)")"' not in l]
open(p, "w").write("\n".join(out))
PY
tooth "J: x86-iso stops hashing the archive's bytes" J "$W/j1"

seed "$W/j2"
python3 - "$W/j2/Makefile" <<'PY'
import sys
p = sys.argv[1]
lines = open(p).read().split("\n")
idx = [i for i, l in enumerate(lines) if 'want_sha="$$(tools/sidecar_archive_digest.sh' in l]
assert len(idx) == 1, "expected exactly one archive-bytes hash line"
line = lines.pop(idx[0]).strip().rstrip("\\").rstrip()
start = next(i for i, l in enumerate(lines) if l.startswith("x86-iso:"))
end = next(i for i in range(start + 1, len(lines)) if lines[i] and not lines[i].startswith("\t"))
lines.insert(end, "\t" + line)
open(p, "w").write("\n".join(lines))
PY
tooth "J: x86-iso hashes the archive only after it has already copied it" J "$W/j2"

# ── K. the three files come from one pack run ─────────────────────────────
# B and H each hold a stamp against its own subject; K is what makes them one
# run's, so these teeth break the coupling rather than a subject. The first is
# the failure the record exists for: a stamp refreshed on its own.
seed "$W/k1"
printf '\n// edited without re-packing\n' >> "$W/k1/user/init/src/env_manager.rs"
( cd "$W/k1" && tools/sidecar_source_digest.sh > sidecars.cpio.digest )
tooth "K: a source was edited and the source stamp hand-refreshed without re-packing" K "$W/k1"

seed "$W/k2"
# A repack that differs only where the six binaries do NOT: the record's binary
# lines stay true (L is green), so this tooth isolates what K alone sees -- the
# bytes field of a record written for a different archive, with the bytes stamp
# hand-refreshed to match the one now on disk.
newc_stub "$W/k2/sidecars.cpio" "edit:boot/layout=layout rebuilt by hand"
sha256sum -- "$W/k2/sidecars.cpio" | cut -d' ' -f1 > "$W/k2/sidecars.cpio.sha256"
tooth "K: the archive was repacked (the layout differs, the six binaries do not) and the bytes stamp was hand-refreshed without a new record" K "$W/k2"

seed "$W/k3"
rm -f "$W/k3/sidecars.cpio.stamps"
tooth "K: the pack-run record is missing, so the stamps cannot be shown to be one run's" K "$W/k3"

seed "$W/k4"
printf 'a line no packer run writes\n' >> "$W/k4/sidecars.cpio.stamps"
tooth "K: the record was edited after the pack" K "$W/k4"

seed "$W/k5"
python3 - "$W/k5/Makefile" <<'PY'
import sys
p = sys.argv[1]
lines = open(p).read().split("\n")
idx = [i for i, l in enumerate(lines) if '> "$(SIDECAR_STAMP)"' in l]
assert len(idx) == 1, "expected exactly one digest projection"
lines[idx[0]] = '\t@tools/sidecar_source_digest.sh > "$(SIDECAR_STAMP)"'
open(p, "w").write("\n".join(lines))
PY
tooth "K: the packer computes the source stamp on its own again instead of projecting it from the record" K "$W/k5"

seed "$W/k6"
python3 - "$W/k6/Makefile" <<'PY'
import sys
p = sys.argv[1]
lines = open(p).read().split("\n")
out = [l for l in lines if '> "$(SIDECAR_STAMP_RECORD)"' not in l]
assert len(out) < len(lines), "no record write to delete"
open(p, "w").write("\n".join(out))
PY
tooth "K: the packing target stops writing the record the stamps are derived from" K "$W/k6"

seed "$W/k7"
python3 - "$W/k7/Makefile" <<'PY'
import sys
p = sys.argv[1]
lines = open(p).read().split("\n")
start = next(i for i, l in enumerate(lines) if l.startswith("x86-iso:"))
end = next(i for i in range(start + 1, len(lines)) if lines[i] and not lines[i].startswith("\t"))
kept = [l for l in lines[start:end] if 'sidecar_stamp_record.sh' not in l]
assert len(kept) < end - start, "no record check to delete"
open(p, "w").write("\n".join(lines[:start] + kept + lines[end:]))
PY
tooth "K: x86-iso stops checking the record, so an independently refreshed stamp ships" K "$W/k7"

seed "$W/k8"
sed -i 's/^SIDECAR_STAMP_RECORD[[:space:]]*?=[[:space:]]*$(SIDECAR_CPIO)\.stamps$/SIDECAR_STAMP_RECORD ?= sidecars.cpio.stamps/' "$W/k8/Makefile"
tooth "K: the record name is fixed, so the E3 image would be checked against the default's" K "$W/k8"

seed "$W/k9"
python3 - "$W/k9/Makefile" <<'PY'
import sys
p = sys.argv[1]
lines = open(p).read().split("\n")
idx = [i for i, l in enumerate(lines)
       if '> "$(SIDECAR_STAMP_RECORD)"' in l and 'sidecar_stamp_record.sh' in l]
assert len(idx) == 1, "expected exactly one record write"
lines[idx[0]] = '\t@tools/sidecar_stamp_record.sh "$(SIDECAR_CPIO)" > "$(SIDECAR_STAMP_RECORD)"'
open(p, "w").write("\n".join(lines))
PY
tooth "K: the record step stops verifying the record against the binaries this run just packed" K "$W/k9"

# ── L. the record's six binaries are the archive's ────────────────────────
# K holds the record against the instruments that wrote it; L holds it against
# the ARCHIVE, re-extracting each boot/*.bin entry with coreutils alone. These
# teeth break one claim at a time: the archive's bytes, the record's lines, and
# the instrument itself.
seed "$W/l1"
newc_stub "$W/l1/sidecars.cpio" "edit:boot/ramdisk.bin=a DIFFERENT ramdisk, packed out of band"
tooth "L: the archive holds a different boot/ramdisk.bin than the record names (a stale image repacked)" L "$W/l1"

seed "$W/l2"
python3 - "$W/l2/sidecars.cpio.stamps" <<'PY'
import sys
p = sys.argv[1]
lines = open(p).read().split("\n")
idx = [i for i, l in enumerate(lines) if l.startswith("binary boot/net.bin ")]
assert len(idx) == 1, "expected exactly one boot/net.bin line"
lines[idx[0]] = "binary boot/net.bin " + "0" * 64
open(p, "w").write("\n".join(lines))
PY
tooth "L: the record's boot/net.bin line names a digest the archive does not hold (a hand-edited record)" L "$W/l2"

seed "$W/l3"
python3 - "$W/l3/sidecars.cpio.stamps" <<'PY'
import sys
p = sys.argv[1]
# The digests of two entries swapped: every line is a valid 64-hex digest and
# the record still has nine lines -- only the archive can say the digests are
# on the wrong entries.
lines = open(p).read().split("\n")
dm = next(i for i, l in enumerate(lines) if l.startswith("binary boot/dm.bin "))
px = next(i for i, l in enumerate(lines) if l.startswith("binary boot/posix.bin "))
dm_digest, px_digest = lines[dm].split()[-1], lines[px].split()[-1]
lines[dm] = "binary boot/dm.bin " + px_digest
lines[px] = "binary boot/posix.bin " + dm_digest
open(p, "w").write("\n".join(lines))
PY
tooth "L: two binary lines were swapped between entries (a well-formed record that misstates the pack)" L "$W/l3"

seed "$W/l4"
newc_stub "$W/l4/sidecars.cpio" "drop:boot/posix.bin"
tooth "L: the archive no longer holds boot/posix.bin (a binary removed) while the record still names it" L "$W/l4"

seed "$W/l5"
printf '#!/bin/sh\necho 0000000000000000000000000000000000000000000000000000000000000000\n' > "$W/l5/tools/sidecar_archive_entry_digest.sh"
chmod +x "$W/l5/tools/sidecar_archive_entry_digest.sh"
tooth "L: the entry instrument answers every name without reading the archive (the writer and reader agree, but neither hashes the packed bytes)" L "$W/l5"

seed "$W/l6"
sed -i '/^binary boot\/e1000.bin /d' "$W/l6/sidecars.cpio.stamps"
tooth "L: the record names only five of the six binaries (a binary silently dropped from the record)" L "$W/l6"

# ── M. the archive's binaries are the flattened build outputs ─────────────
# K and L compare the record, the stamps and the archive to one another; these
# break the one subject they cannot see -- the tree's own build output.
seed "$W/m1"
printf 'a newer build of init, not yet packed\n' > "$W/m1/user/target/x86_64-unknown-none/release/init.bin"
tooth "M: the tree's flattened init.bin moved on and the archive was not re-packed" M "$W/m1"

seed "$W/m2"
# THE tooth: the state the record alone cannot close. The archive is repacked
# from a stale init and the record AND both stamp files are rewritten so that
# every record-vs-archive check agrees -- B, H, K and L all stay green -- and
# only the build output left in the tree disagrees.
newc_stub "$W/m2/sidecars.cpio" "edit:boot/init.bin=a STALE init image, packed by hand"
( cd "$W/m2" && tools/sidecar_stamp_record.sh sidecars.cpio > sidecars.cpio.stamps )
sha256sum -- "$W/m2/sidecars.cpio" | cut -d' ' -f1 > "$W/m2/sidecars.cpio.sha256"
tooth "M: the archive was repacked from a stale image and its record and stamps rewritten consistently (the state K and L cannot see)" M "$W/m2"

seed "$W/m3"
python3 - "$W/m3/Makefile" <<'PY'
import sys
p = sys.argv[1]
lines = open(p).read().split("\n")
start = next(i for i, l in enumerate(lines) if l.startswith("x86-iso:"))
end = next(i for i in range(start + 1, len(lines)) if lines[i] and not lines[i].startswith("\t"))
kept = [l for l in lines[start:end] if 'sidecar_build_output_check.sh' not in l]
assert len(kept) < end - start, "no build-output check call to delete"
open(p, "w").write("\n".join(lines[:start] + kept + lines[end:]))
PY
tooth "M: x86-iso stops checking the archive against the tree's build outputs" M "$W/m3"

# ── M green arm: the E3 pack overwriting the shared init.bin ──────────────
# `make selfhost-bootimage-e3` overwrites the flattened init.bin with the
# e3_envs build, so the default archive legitimately holds an init the file no
# longer matches. The file is the E3 ARCHIVE's init entry and the E3 record
# recomputes to that archive, so M must skip it and stay green -- a refusal
# here would block `make x86-iso` after every E3 pack. The quartet is written
# whole (e3_pack): clause N holds the E3 pair to the same one-run rule, so an
# E3 pack that is only half here would be red on its own.
#
# e3_pack <dir> [newc spec]... -- a VALID E3 quartet: the archive, its record
# (one record-tool invocation, the way the packer writes it) and the two
# stamps PROJECTED out of that record (`sed -n 's/^sources //p'` / `'s/^bytes
# //p'`, exactly the Makefile's projections), so a seeded tree is one clause N
# passes on and each tooth below can break exactly one half of it.
e3_pack() {   # e3_pack <dir> [newc spec]...
    local d="$1"
    shift
    newc_stub "$d/sidecars_e3.cpio" "$@" || return 1
    ( cd "$d" && tools/sidecar_stamp_record.sh sidecars_e3.cpio > sidecars_e3.cpio.stamps ) || return 1
    sed -n 's/^sources //p' "$d/sidecars_e3.cpio.stamps" > "$d/sidecars_e3.cpio.digest" || return 1
    sed -n 's/^bytes //p' "$d/sidecars_e3.cpio.stamps" > "$d/sidecars_e3.cpio.sha256" || return 1
    return 0
}

seed "$W/m4"
m_flat="$W/m4/user/target/x86_64-unknown-none/release"
e3_text='stub boot/init.bin built with e3_envs'
printf '%s' "$e3_text" > "$m_flat/init.bin"
e3_pack "$W/m4" "edit:boot/init.bin=$e3_text"
out="$(bash "$GUARD" "$W/m4" 2>&1)"; rc=$?
if [ "$rc" -eq 0 ]; then
    echo "ok:   M green: the e3_envs init is the E3 archive's entry and the E3 record recomputes to it, so M skips it instead of refusing the default archive"
    passed=$((passed + 1))
else
    echo "FAIL: M refuses the default archive after an E3 pack overwrites the shared init.bin"
    printf '%s\n' "$out" | sed 's/^/      /'
    failed=$((failed + 1))
fi

# ── M teeth: a fabricated E3 record must not exempt anything ──────────────
# The narrowing the anchoring exists for: a hand writes a record that merely
# NAMES the flattened init. Without an anchor it would excuse a stale one in
# the archive; with one, M is red because the record cannot be recomputed.
fake_variant_record() {   # fake_variant_record <dir> — an E3 record naming the flattened files
    {
        printf 'sidecar-stamp-record-v2\n'
        printf 'sources %s\n' "$(cat "$1/sidecars.cpio.digest")"
        printf 'bytes %s\n' "$(cat "$1/sidecars.cpio.sha256")"
        for pair in "boot/init.bin=init.bin" "boot/dm.bin=dm.bin" "boot/posix.bin=posix.bin" \
                    "boot/ramdisk.bin=ramdisk.bin" "boot/net.bin=network.bin" "boot/e1000.bin=e1000_driver.bin"; do
            printf 'binary %s %s\n' "${pair%%=*}" "$(sha256sum -- "$1/user/target/x86_64-unknown-none/release/${pair#*=}" | cut -d' ' -f1)"
        done
    } > "$1/sidecars_e3.cpio.stamps"
}

seed "$W/m5"
printf '%s' "$e3_text" > "$W/m5/user/target/x86_64-unknown-none/release/init.bin"
fake_variant_record "$W/m5"
tooth "M: the E3 record names the flattened init but no E3 archive backs it (a fabricated record)" M "$W/m5"

seed "$W/m6"
printf '%s' "$e3_text" > "$W/m6/user/target/x86_64-unknown-none/release/init.bin"
newc_stub "$W/m6/sidecars_e3.cpio"
fake_variant_record "$W/m6"
# N bites here too, and honestly so: the fabricated record does not recompute
# over the E3 archive, which is exactly N's property one clause over. The
# tooth requires M's line, which is the anchoring it is about.
tooth "M: the E3 record disagrees with a live recomputation of the E3 archive (a mismatched variant record)" M "$W/m6"

# ── N. the E3 tuple is one pack run's, when it is here ────────────────────
# The E3 quartet is ignored build output (F), so these teeth are built on a
# valid one and break a single half each. Every state here leaves the default
# side green -- M only reads the E3 record when an init mismatches, and in
# these trees it does not -- so a red is N's alone (m6 above is the exception:
# a record that does not recompute is both M's anchor failing and N's rule).
seed "$W/n1"
e3_pack "$W/n1"
printf '0000000000000000000000000000000000000000000000000000000000000000\n' > "$W/n1/sidecars_e3.cpio.digest"
tooth "N: the E3 sources stamp was refreshed without the E3 record (one stamp drifting on its own)" N "$W/n1"

seed "$W/n2"
e3_pack "$W/n2"
printf '0000000000000000000000000000000000000000000000000000000000000000\n' > "$W/n2/sidecars_e3.cpio.sha256"
tooth "N: the E3 bytes stamp was refreshed without the E3 record" N "$W/n2"

seed "$W/n3"
e3_pack "$W/n3" "edit:boot/net.bin=a different net image"
cp "$W/n3/sidecars.cpio.stamps" "$W/n3/sidecars_e3.cpio.stamps"
tooth "N: the E3 record is the default archive's, which does not recompute over the E3 archive" N "$W/n3"

seed "$W/n4"
e3_pack "$W/n4"
rm -f "$W/n4/sidecars_e3.cpio.digest"
tooth "N: the E3 archive and record are here but the E3 sources stamp is gone (an incomplete tuple is not one run's)" N "$W/n4"

# ── N green arm: half a quartet is skipped, not assumed ──────────────────
# A host can hold part of the ignored E3 output (an archive from an older pack
# whose record was removed). With no record there is nothing that claims to be
# one run's, and the clause must say so instead of going red -- M anchors the
# record itself the moment it needs it.
seed "$W/n5"
newc_stub "$W/n5/sidecars_e3.cpio"
out="$(bash "$GUARD" "$W/n5" 2>&1)"; rc=$?
if [ "$rc" -eq 0 ]; then
    echo "ok:   N green: an E3 archive with no record is skipped, not assumed (only the archive of the ignored quartet is here)"
    passed=$((passed + 1))
else
    echo "FAIL: N goes red for an E3 archive whose ignored record is absent"
    printf '%s\n' "$out" | sed 's/^/      /'
    failed=$((failed + 1))
fi

# ── The vacuity control ───────────────────────────────────────────────────
# Without this arm a guard that failed unconditionally would pass every tooth
# above. It is also where a drifted copy shows up: if the seed stopped copying a
# covered source, the digest would differ and THIS arm would fail -- and if the
# seed's stub stopped being a readable newc (or its layout's textual hit stopped
# being skipped), L would disagree with the entry tool and THIS arm would fail,
# as would M if the seed's flattened outputs stopped matching the stub entries.
out="$(bash "$GUARD" "$ROOT" 2>&1)"; rc=$?
if [ "$rc" -eq 0 ]; then
    echo "ok:   control: the untouched tree passes ($(printf '%s\n' "$out" | grep -c '^ok:') clauses green)"
    passed=$((passed + 1))
else
    echo "FAIL: the guard is RED on the real, unmutated tree — it cannot be trusted on a mutation"
    printf '%s\n' "$out" | sed 's/^/      /'
    failed=$((failed + 1))
fi

echo
echo "$passed passed, $failed failed"
[ "$failed" -eq 0 ] || exit 1
exit 0
