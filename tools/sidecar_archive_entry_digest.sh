#!/usr/bin/env bash
# tools/sidecar_archive_entry_digest.sh — one line of hex: the sha256 of ONE
# named entry's DATA inside a `newc` cpio archive.
#
# ─── Why this exists ───────────────────────────────────────────────────────
# tools/sidecar_archive_digest.sh hashes the WHOLE archive: that binds the
# archive's bytes but does not say WHICH files those bytes are. The pack-run
# record (tools/sidecar_stamp_record.sh) therefore names the six packed sidecar
# binaries and their digests, and this is the instrument that reads one of them
# out of the archive — the `boot/*.bin` entries `selfhost-bootimage` packs from
# the flattened build outputs. Point it at any other entry and it answers for
# that one; the record's list of six is what makes the claim about the pack.
#
# ─── What it is, and is not ────────────────────────────────────────────────
# A `newc` walk in coreutils: `grep -abo` finds the name's byte offset, `dd`
# reads the fixed 110-byte header's c_filesize/c_namesize, `tail`+`head` cut
# the entry's data, `sha256sum` digests it. The layout is the one
# user/bootimage/src/newc.rs writes and kernel/boot_image.h walks; it is
# deliberately independent of the Rust toolchain, so it answers on the hosts
# that ship the committed archive (CI's ISO job, deploy) and in a fresh clone.
#
# Scanning for the name alone is not enough: `boot/layout` lists every entry
# name in its text, so the first textual match may be a line in another entry's
# data. The scan accepts only an occurrence whose 110 bytes of header are there
# and start with the newc magic.
#
# `--to-stdout` writes the entry's data instead of its digest. That is for
# callers that need the BYTES -- tests/m4_std_thread_probe_check.sh reads the
# probe image's boot/posix.bin out of the packed archive with it. Keeping that
# read here, on the walk the record is built from, is what leaves one parser
# for the packer's c_namesize padding; a second reader would be a second place
# for it to drift.
#
# It is an instrument, not a guarantee: a hand can run it too. What it cannot
# do is misread the archive — the guard re-extracts the entry itself.
#
# Usage:  tools/sidecar_archive_entry_digest.sh ARCHIVE ENTRY
#         tools/sidecar_archive_entry_digest.sh --to-stdout ARCHIVE ENTRY
# Output: one line of hex, or with --to-stdout the entry's bytes verbatim.
# Exit:   0 printed/extracted, 1 no hash tool or no such entry / not a newc
#         archive, 2 usage error or archive missing/empty.
set -u

to_stdout=0
archive=""
entry=""
while [ $# -gt 0 ]; do
    case "$1" in
        --to-stdout) to_stdout=1 ;;
        -*) echo "$0: unknown option '$1'" >&2; exit 2 ;;
        *)
            if [ -z "$archive" ]; then archive=$1
            elif [ -z "$entry" ]; then entry=$1
            else echo "$0: too many operands ('$archive', '$entry', '$1')" >&2; exit 2
            fi ;;
    esac
    shift
done
if [ -z "$archive" ] || [ -z "$entry" ]; then
    echo "usage: $0 [--to-stdout] ARCHIVE ENTRY" >&2
    exit 2
fi

[ -s "$archive" ] || {
    echo "sidecar_archive_entry_digest: $archive is missing or empty — nothing to read" >&2
    exit 2
}

if [ "$to_stdout" = 0 ] && ! command -v sha256sum >/dev/null 2>&1; then
    echo "sidecar_archive_entry_digest: sha256sum not found (coreutils) — cannot hash the entry" >&2
    exit 1
fi

# Locate the entry header: the name must be preceded by the 110-byte fixed
# header, whose first six bytes are the magic. Later occurrences are skipped
# (the layout text repeats every name).
found=""
for off in $(grep -abo -F -- "$entry" "$archive" 2>/dev/null | cut -d: -f1); do
    hdr=$((off - 110))
    [ "$hdr" -ge 0 ] || continue
    if [ "$(dd if="$archive" bs=1 skip="$hdr" count=6 2>/dev/null)" = "070701" ]; then
        found=$hdr
        break
    fi
done
if [ -z "$found" ]; then
    echo "sidecar_archive_entry_digest: $archive has no entry '$entry' (or it is not a newc archive)" >&2
    exit 1
fi

# c_filesize at header+54, c_namesize at header+94 — both 8 ASCII hex digits.
size_hex=$(dd if="$archive" bs=1 skip=$((found + 54)) count=8 2>/dev/null)
nsize_hex=$(dd if="$archive" bs=1 skip=$((found + 94)) count=8 2>/dev/null)
case "$size_hex$nsize_hex" in
    *[!0-9a-fA-F]*)
        echo "sidecar_archive_entry_digest: $archive's header for '$entry' is not hexadecimal — not a newc archive?" >&2
        exit 1 ;;
esac
namesize=$((16#$nsize_hex))
size=$((16#$size_hex))

# The name is NUL-terminated (c_namesize includes the NUL) and padded to a
# 4-byte boundary; the data starts right after. Mirrors newc.rs's writer —
# `name_pad = align4(namesize) - namesize` — exactly.
data_off=$((found + 110 + namesize + ((4 - (namesize % 4)) % 4)))
if [ "$to_stdout" = 1 ]; then
    tail -c +$((data_off + 1)) "$archive" | head -c "$size"
else
    tail -c +$((data_off + 1)) "$archive" | head -c "$size" | sha256sum | cut -d' ' -f1
fi
