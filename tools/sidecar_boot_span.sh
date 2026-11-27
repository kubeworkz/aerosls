#!/usr/bin/env bash
# tools/sidecar_boot_span.sh — the sidecar boot image's reserved physical
# span, read out of the archive's own `boot/layout` table.
#
# ─── Why this exists ───────────────────────────────────────────────────────
# kernel/boot_image.c does not relocate the sidecars: it reserves
# [base, base + total_size) from the frame pool and copies every image to
# the physical address the archive's manifests declare. The base is 512 MiB
# (BOOT_IMAGE_BASE_PHYS), so a guest whose RAM ends inside that span copies
# init, DM, POSIX, ... into unbacked physical memory. The failure is not a
# clean "out of memory": init starts at an address no page backs and faults
# at ring-3 entry, and the kernel answers by restarting the boot. Measured
# 2026-11-09: `run-cluster.sh --nodes 3 --ram 512M` produced 27 init deaths
# per node and no HTTP server; 768M booted the same archive.
#
# The span is a property of the ARCHIVE, not a constant — binary sizes move
# it. So it is read from `boot/layout`, the builder's fixed-width region
# table (user/bootimage/src/builder.rs), rather than hardcoded in the
# launcher. run-cluster.sh compares the guest's RAM against it before it
# builds or launches anything.
#
# ─── The newc walk ────────────────────────────────────────────────────────
# The same coreutils walk as tools/sidecar_archive_entry_digest.sh: find the
# entry name, require the 110-byte newc header immediately before it (the
# layout text itself repeats every entry name, so a bare textual match is
# not enough), read c_filesize/c_namesize, and cut the data. No cpio, no
# Rust — this answers on the hosts that ship the committed archive.
#
# Usage:  tools/sidecar_boot_span.sh ARCHIVE
# Output: "<base> <end>" in decimal, e.g. "536870912 578158592".
# Exit:   0 printed, 1 no readable boot/layout (not a newc archive, the
#         entry is missing, or the table is malformed), 2 usage/archive
#         missing.
set -u

if [ $# -ne 1 ]; then
    echo "usage: $0 ARCHIVE" >&2
    exit 2
fi
archive=$1
entry=boot/layout

[ -s "$archive" ] || {
    echo "sidecar_boot_span: $archive is missing or empty — nothing to read" >&2
    exit 2
}

# Locate the entry header: the name must be preceded by the 110-byte fixed
# header, whose first six bytes are the newc magic. Later occurrences are
# skipped (the layout text repeats every name).
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
    echo "sidecar_boot_span: $archive has no '$entry' entry (or it is not a newc archive)" >&2
    exit 1
fi

# c_filesize at header+54, c_namesize at header+94 — both 8 ASCII hex digits.
size_hex=$(dd if="$archive" bs=1 skip=$((found + 54)) count=8 2>/dev/null)
nsize_hex=$(dd if="$archive" bs=1 skip=$((found + 94)) count=8 2>/dev/null)
if [ "${#size_hex}" -ne 8 ] || [ "${#nsize_hex}" -ne 8 ]; then
    echo "sidecar_boot_span: $archive's header for '$entry' is truncated — not a newc archive?" >&2
    exit 1
fi
case "$size_hex$nsize_hex" in
    *[!0-9a-fA-F]*)
        echo "sidecar_boot_span: $archive's header for '$entry' is not hexadecimal — not a newc archive?" >&2
        exit 1 ;;
esac
namesize=$((16#$nsize_hex))
size=$((16#$size_hex))

# The name is NUL-terminated (c_namesize includes the NUL) and padded to a
# 4-byte boundary; the data starts right after. Mirrors newc.rs's writer —
# `name_pad = align4(namesize) - namesize` — exactly.
data_off=$((found + 110 + namesize + ((4 - (namesize % 4)) % 4)))
layout="$(tail -c +$((data_off + 1)) "$archive" | head -c "$size")"

if [ "${layout%%$'\n'*}" != "AEROSLS-BOOT-LAYOUT 1" ]; then
    echo "sidecar_boot_span: $archive's '$entry' does not start with AEROSLS-BOOT-LAYOUT 1 — malformed or not the builder's table" >&2
    exit 1
fi

# The table is fixed-width ASCII:
#   base <phys>
#   region <name> <phys> <size>
#   file <path> <offset> <size>
# The span is the largest region end; `base` anchors it.
base=""
end=""
malformed() {
    echo "sidecar_boot_span: $archive's '$entry' has a malformed line: $1" >&2
    exit 1
}
while IFS=' ' read -r kind f1 f2 f3; do
    case "$kind" in
        ''|file|AEROSLS-BOOT-LAYOUT) continue ;;
        base)
            [ -n "$f1" ] || malformed "$kind"
            case "$f1" in *[!0-9a-fA-F]*) malformed "base $f1" ;; esac
            base=$((16#$f1)) ;;
        region)
            [ -n "$f1" ] && [ -n "$f2" ] && [ -n "$f3" ] || malformed "$kind"
            case "$f2$f3" in *[!0-9a-fA-F]*) malformed "region $f1 $f2 $f3" ;; esac
            phys=$((16#$f2))
            rsize=$((16#$f3))
            [ -n "$base" ] || malformed "region before base"
            [ "$phys" -ge "$base" ] || malformed "region $f1 starts below base"
            r_end=$((phys + rsize))
            if [ -z "$end" ] || [ "$r_end" -gt "$end" ]; then end=$r_end; fi ;;
        *) malformed "$kind" ;;
    esac
done <<< "$layout"

[ -n "$base" ] && [ -n "$end" ] || {
    echo "sidecar_boot_span: $archive's '$entry' has no base/region records" >&2
    exit 1
}
printf '%s %s\n' "$base" "$end"
