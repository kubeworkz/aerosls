#!/usr/bin/env bash
# tools/sidecar_stamp_record.sh — the pack-run record for a sidecar archive:
# the source digest, the archive's own sha256, and the digest of each of the
# six packed sidecar binaries — every half computed in ONE invocation.
#
# ─── Why this exists ───────────────────────────────────────────────────────
# tools/sidecar_source_digest.sh attests the SOURCES an archive was packed
# from; tools/sidecar_archive_digest.sh attests the archive's own BYTES. Each
# is verified against its own subject, and that is exactly the hole: stamps
# written by independent commands can DRIFT APART while both stay green. Edit
# a `user/` source and hand-run
#
#     tools/sidecar_source_digest.sh > sidecars.cpio.digest
#
# and the source stamp matches the new sources again -- while the archive still
# carries the binaries built from the OLD ones and the bytes stamp still
# matches those bytes. `make selfhost-bootimage` never ran, so nothing was
# re-packed, and yet `make x86-iso` and tests/sidecar_stamp_check.sh both pass:
# the incident the source stamp exists for (a stale archive booted and blamed
# on the kernel), reached past the stamp by refreshing one file.
#
# This tool closes that by making the stamps two VIEWS of one computation. It
# computes every half in a single invocation and prints them as one record;
# `selfhost-bootimage` writes that record and then projects each stamp out of
# it (`sed -n 's/^sources //p'` / `sed -n 's/^bytes //p'`). The guard in
# tests/sidecar_stamp_check.sh (clause K) recomputes the record, requires the
# committed record to match, and requires each committed stamp to equal its
# field of the record -- so refreshing one stamp on its own is red, and so is a
# record no run could have written.
#
# ─── v2: the six packed binaries are NAMED ─────────────────────────────────
# The source digest cannot see WHICH binaries are inside; the archive-bytes
# hash covers them but does not name them. The record now does: six
# `binary boot/*.bin <sha256>` lines, read out of the archive by
# tools/sidecar_archive_entry_digest.sh. `selfhost-bootimage` also hands this
# tool the six flattened `.bin` files it just packed (`--verify ENTRY=PATH`),
# so the record it writes is checked against the pack's INPUTS and refuses to
# be written at all if the archive holds something else — the packer must not
# bless binaries other than the ones this run built. The guard's clause L then
# re-extracts each entry itself, so the record cannot misstate what is inside
# even if this tool is wrong.
#
# What this still is NOT: a signature, and not a proof that the packed
# binaries were built from the current sources. A hand can re-derive all four
# files (this record plus the two stamps) from a stale archive, and no
# cargo-free check -- CI's ISO job and deploy have no toolchain -- can rebuild
# to tell. The FATAL builds in `selfhost-bootimage` are what make "packed from
# this tree's sources" true at pack time; naming the binaries here makes the
# images that claim is about explicit, and makes a record that describes a
# different archive than the one on disk red. And wherever the pack's own
# output survives (the flattened `.bin` files), tools/sidecar_build_output_check.sh
# compares the archive with it -- the one rewrite a hand cannot make has to be
# part of the state for a stale pack to agree with itself.
#
# ─── Reuse, not reimplementation ───────────────────────────────────────────
# Every field is computed BY an instrument beside this one -- the source
# digest, the archive digest and the entry digest tools -- never re-derived
# here: whatever the guard compares a stamp against is the same instrument the
# record was built from, so the record cannot disagree with the tools about
# what "the source digest", "the archive's sha256" or "the entry's bytes" mean.
#
# Usage:  tools/sidecar_stamp_record.sh [--verify ENTRY=PATH]... [ARCHIVE]
#           --verify  compare the archive's ENTRY with the file at PATH before
#                     writing anything (selfhost-bootimage passes the six
#                     flattened binaries it just packed)
# Output: nine lines -- a version tag, sources, bytes, and the six binaries.
# Exit:   0 printed, 1 no hash tool or an entry/verify mismatch,
#         2 usage error or archive missing/empty or outside the repo.
set -u

root="$(cd "$(dirname "$0")/.." && pwd)" || exit 2
[ -d "$root/user" ] || { echo "sidecar_stamp_record: $root/user is missing — run from the repo" >&2; exit 2; }
cd "$root" || exit 2

if ! command -v sha256sum >/dev/null 2>&1; then
    echo "sidecar_stamp_record: sha256sum not found (coreutils) — cannot write the pack-run record" >&2
    exit 1
fi

archive="sidecars.cpio"
archive_set=0
verify=()
while [ $# -gt 0 ]; do
    case "$1" in
        --verify)
            shift
            if [ $# -eq 0 ]; then
                echo "sidecar_stamp_record: --verify needs ENTRY=PATH" >&2
                exit 2
            fi
            verify+=("$1")
            ;;
        -*)
            echo "sidecar_stamp_record: unknown option '$1' (usage: $0 [--verify ENTRY=PATH]... [ARCHIVE])" >&2
            exit 2
            ;;
        *)
            if [ "$archive_set" -eq 1 ]; then
                echo "sidecar_stamp_record: more than one archive given ('$archive' and '$1')" >&2
                exit 2
            fi
            archive=$1
            archive_set=1
            ;;
    esac
    shift
done

[ -s "$archive" ] || {
    echo "sidecar_stamp_record: $archive is missing or empty — nothing to record (run from the repo, or pass the archive path)" >&2
    exit 2
}

here="$(cd "$(dirname "$0")" && pwd)"

# The six sidecars, in pack order: the `boot/*.bin` entries the packer writes
# from the flattened build outputs. `boot/rootfs.bin` is built by the packer
# itself (rootfs.rs) from no input this tree builds, and the manifests/layout
# are derived from the same images — the archive's own sha256 covers all of
# them; these six are the ones the sources produce.
BINARIES=(boot/init.bin boot/dm.bin boot/posix.bin boot/ramdisk.bin boot/net.bin boot/e1000.bin)

if [ "${#verify[@]}" -gt 0 ]; then
    for pair in "${verify[@]}"; do
        entry_name=${pair%%=*}
        path=${pair#*=}
        if [ "$entry_name" = "$pair" ] || [ -z "$path" ]; then
            echo "sidecar_stamp_record: --verify expects ENTRY=PATH (got '$pair')" >&2
            exit 2
        fi
        want="$("$here/sidecar_archive_entry_digest.sh" "$archive" "$entry_name")" || exit 1
        if [ ! -f "$path" ]; then
            echo "sidecar_stamp_record: --verify $entry_name: $path is missing — cannot check what was packed" >&2
            exit 1
        fi
        got="$(sha256sum -- "$path" | cut -d' ' -f1)"
        if [ "$want" != "$got" ]; then
            echo "sidecar_stamp_record: REFUSING to write the record: $archive's '$entry_name' is not the binary at $path." >&2
            echo "         the archive entry hashes to $want, the file to $got — this run did not pack the" >&2
            echo "         binary it just built, and stamping that as this tree's own would be a lie." >&2
            exit 1
        fi
    done
fi

sources="$("$here/sidecar_source_digest.sh")" || exit 1
bytes="$("$here/sidecar_archive_digest.sh" "$archive")" || exit 1
bin_digests=()
for name in "${BINARIES[@]}"; do
    digest="$("$here/sidecar_archive_entry_digest.sh" "$archive" "$name")" || exit 1
    bin_digests+=("$digest")
done

# One record, every half. The version tag comes first for the same reason the
# source digest has one: a future change to what is recorded changes this line,
# so every record written by the old format mismatches -- a re-pack, not a
# false pass. The `sources `/`bytes ` labels are what the Makefile's two
# projections key on (`sed -n 's/^sources //p'`); the `binary ` lines name the
# packed images and are what the guard's clause L re-extracts.
printf 'sidecar-stamp-record-v2\n'
printf 'sources %s\n' "$sources"
printf 'bytes %s\n' "$bytes"
i=0
for name in "${BINARIES[@]}"; do
    printf 'binary %s %s\n' "$name" "${bin_digests[$i]}"
    i=$((i + 1))
done
