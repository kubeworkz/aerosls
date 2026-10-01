#!/usr/bin/env bash
# tools/sidecar_build_output_check.sh — the archive's six `boot/*.bin` entries
# against the flattened build outputs in this tree, when those outputs exist.
#
# ─── Why this exists ───────────────────────────────────────────────────────
# The pack-run record names the six packed binaries and the guard re-extracts
# them, but a record and an archive are one story: a stale archive with its
# record and both stamps REWRITTEN from it is internally consistent, and every
# record-vs-archive check above stays green (B, H, K and L alike). What a hand
# cannot rewrite is this tree's own BUILD OUTPUT — the flattened `*.bin` files
# `selfhost-bootimage` produced. On a host where they exist, the archive's
# entries must be those files: a repack from stale images is refused here even
# though its record and stamps agree with each other.
#
# On a host with no build outputs (CI's ISO job, deploy, a fresh clone) there
# is nothing to compare against, and this tool says so and exits 0: it cannot
# make the claim, and it must not pretend to. That is the honest limit of a
# cargo-free check — the claim is made at pack time by the fatal builds and
# `--verify` in `selfhost-bootimage`, and re-made here wherever the pack's
# output is still in the tree.
#
# ─── The E3 exception, and why it is not a hole ────────────────────────────
# `make selfhost-bootimage-e3` builds init with the e3_envs feature and
# OVERWRITES the same flattened init.bin the default pack uses, so after an E3
# pack the default archive legitimately holds a default init the on-disk file
# no longer matches. A flattened `init.bin` that is the E3 archive's entry is
# a known build, not a stale image, and is skipped -- and "is the E3 archive's
# entry" is CHECKED, not taken from a record: the variant record named by the
# Makefile (or --variant-record) must equal a live recomputation of its own
# archive, and that archive must exist and must not be the archive being
# checked. A hand-written record that merely names some other init therefore
# excuses nothing. Only init can differ between the variants; the five shared
# entries are always compared absolutely.
#
# ─── Reuse, not reimplementation ───────────────────────────────────────────
# Entries are read with tools/sidecar_archive_entry_digest.sh — the same
# instrument the record is built from — and files with `sha256sum`. The six
# paths default to the Makefile's own `SIDECAR_*_BIN ?=` values, so the check
# compares what the build target would have produced, and `--build` pairs
# override one entry at a time exactly as the recipe passes them.
#
# Usage:  tools/sidecar_build_output_check.sh [--variant-record FILE]
#                                             [--build ENTRY=PATH]... [ARCHIVE]
# Output: one summary line to stdout; a refusal block to stderr on a mismatch.
# Exit:   0 every present output is the archive's (or none are present),
#         1 a mismatch / no hash tool / an entry the archive does not hold,
#         2 usage error or archive missing/empty.
set -u

root="$(cd "$(dirname "$0")/.." && pwd)" || exit 2
[ -d "$root/user" ] || { echo "sidecar_build_output_check: $root/user is missing — run from the repo" >&2; exit 2; }
cd "$root" || exit 2

if ! command -v sha256sum >/dev/null 2>&1; then
    echo "sidecar_build_output_check: sha256sum not found (coreutils) — cannot hash the build outputs" >&2
    exit 1
fi

here="$(cd "$(dirname "$0")" && pwd)"
entry_tool="$here/sidecar_archive_entry_digest.sh"
if [ ! -x "$entry_tool" ]; then
    echo "sidecar_build_output_check: $entry_tool is missing or not executable — nothing here can read the archive's entries" >&2
    exit 2
fi

archive="sidecars.cpio"
archive_set=0
variant_record=""
builds=()
while [ $# -gt 0 ]; do
    case "$1" in
        --variant-record)
            shift
            if [ $# -eq 0 ]; then
                echo "sidecar_build_output_check: --variant-record needs a FILE" >&2
                exit 2
            fi
            variant_record="$1"
            ;;
        --build)
            shift
            if [ $# -eq 0 ]; then
                echo "sidecar_build_output_check: --build needs ENTRY=PATH" >&2
                exit 2
            fi
            builds+=("$1")
            ;;
        -*)
            echo "sidecar_build_output_check: unknown option '$1' (usage: $0 [--variant-record FILE] [--build ENTRY=PATH]... [ARCHIVE])" >&2
            exit 2
            ;;
        *)
            if [ "$archive_set" -eq 1 ]; then
                echo "sidecar_build_output_check: more than one archive given ('$archive' and '$1')" >&2
                exit 2
            fi
            archive="$1"
            archive_set=1
            ;;
    esac
    shift
done

[ -s "$archive" ] || {
    echo "sidecar_build_output_check: $archive is missing or empty — nothing to check (run from the repo, or pass the archive path)" >&2
    exit 2
}

# The six sidecars, in pack order — the same list the record names.
BINARIES=(boot/init.bin boot/dm.bin boot/posix.bin boot/ramdisk.bin boot/net.bin boot/e1000.bin)

# The Makefile's `?=` default for a variable, so the tool and the build
# resolve the same paths and a clean checkout needs no arguments at all.
makefile_default() {   # makefile_default <VARNAME> -> value or empty
    sed -n "s/^$1[[:space:]]*?=[[:space:]]*//p" Makefile | head -1
}

build_path() {   # build_path <entry> -> the --build path, or empty
    local entry="$1" pair
    if [ "${#builds[@]}" -gt 0 ]; then
        for pair in "${builds[@]}"; do
            if [ "${pair%%=*}" = "$entry" ]; then
                printf '%s\n' "${pair#*=}"
                return 0
            fi
        done
    fi
    return 1
}

path_var() {   # path_var <entry> -> the Makefile variable naming its path
    case "$1" in
        boot/init.bin)    printf 'SIDECAR_INIT_BIN\n' ;;
        boot/dm.bin)      printf 'SIDECAR_DM_BIN\n' ;;
        boot/posix.bin)   printf 'SIDECAR_POSIX_BIN\n' ;;
        boot/ramdisk.bin) printf 'SIDECAR_RAMDISK_BIN\n' ;;
        boot/net.bin)     printf 'SIDECAR_NET_BIN\n' ;;
        boot/e1000.bin)   printf 'SIDECAR_E1000_BIN\n' ;;
    esac
}

# The records that may explain an init mismatch: the E3 archive's (whose build
# overwrites the shared init.bin) and any explicitly named variant. The record
# of the archive being checked is never one of them -- a record of THIS archive
# cannot excuse this archive's entries.
e3_cpio="$(makefile_default SIDECAR_E3_CPIO)"; [ -n "$e3_cpio" ] || e3_cpio=sidecars_e3.cpio
records=("$e3_cpio.stamps")
if [ -n "$variant_record" ]; then
    records+=("$variant_record")
fi

# A variant record counts only when it is a live view of its own archive: the
# archive exists, the record is what tools/sidecar_stamp_record.sh computes
# from it now, and it is not the record of the archive under check. That is
# what keeps a hand-written record from masking a stale init.
anchored_variant() {   # anchored_variant <record> -> 0 if anchored
    local rec="$1" arch shot
    case "$rec" in *.stamps) arch="${rec%.stamps}" ;; *) return 1 ;; esac
    [ "$arch" = "$archive" ] && return 1
    [ -s "$arch" ] || return 1
    [ -f "$rec" ] || return 1
    shot="$("$here/sidecar_stamp_record.sh" "$arch" 2>/dev/null)" || return 1
    [ -n "$shot" ] || return 1
    [ "$shot" = "$(cat "$rec")" ] || return 1
    return 0
}

checked=0
variants=0
skipped=""
for entry in "${BINARIES[@]}"; do
    path="$(build_path "$entry" || true)"
    if [ -z "$path" ]; then
        path="$(makefile_default "$(path_var "$entry")")"
    fi
    [ -n "$path" ] || continue
    [ -f "$path" ] || continue
    if ! file_sha="$(sha256sum -- "$path" | cut -d' ' -f1)"; then
        echo "sidecar_build_output_check: cannot hash $path" >&2
        exit 1
    fi
    entry_sha="$("$entry_tool" "$archive" "$entry" 2>/dev/null)" || entry_sha=""
    if [ -z "$entry_sha" ]; then
        echo "sidecar_build_output_check: REFUSING: $archive has no entry '$entry' to compare with the build output at $path." >&2
        exit 1
    fi
    if [ "$file_sha" = "$entry_sha" ]; then
        checked=$((checked + 1))
        continue
    fi
    matched_variant=""
    if [ "$entry" = "boot/init.bin" ]; then
        # Only init can differ between this tree's pack variants (e3_envs
        # rewrites it and overwrites the shared flattened file), and only an
        # anchored record may say so.
        for rec in "${records[@]}"; do
            if anchored_variant "$rec" && grep -q "^binary $entry $file_sha$" "$rec"; then
                matched_variant="$rec"
                break
            fi
        done
    fi
    if [ -n "$matched_variant" ]; then
        variants=$((variants + 1))
        skipped="$skipped $entry"
        continue
    fi
    echo "sidecar_build_output_check: REFUSING: $archive's '$entry' is not the build output at $path." >&2
    echo "         the archive entry hashes to $entry_sha" >&2
    echo "         the file on disk hashes to $file_sha" >&2
    if [ "$entry" = "boot/init.bin" ]; then
        echo "         and no verified packed-variant record in this tree names that file (${records[*]})" >&2
    fi
    echo "         — so $path is what THIS tree's build produced and the archive holds another" >&2
    echo "         image. The record and the two stamps can be rewritten from a stale archive so" >&2
    echo "         that they agree with each other; the build output is the half a hand cannot" >&2
    echo "         rewrite. Re-pack from this tree and retry:" >&2
    echo "             make selfhost-bootimage && make x86-iso" >&2
    exit 1
done

if [ "$checked" -eq 0 ] && [ "$variants" -eq 0 ]; then
    echo "sidecar_build_output_check: no flattened sidecar outputs in this tree — nothing to compare against $archive"
elif [ "$variants" -gt 0 ]; then
    echo "sidecar_build_output_check: $archive holds all $checked flattened sidecar outputs compared, and$skipped matched a packed variant's record (the e3_envs pack overwrites the shared init.bin)"
else
    echo "sidecar_build_output_check: $archive holds all $checked flattened sidecar outputs present in this tree"
fi
