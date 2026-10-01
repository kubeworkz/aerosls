#!/usr/bin/env bash
# tools/sidecar_archive_digest.sh — one line of hex: the sha256 of the packed
# sidecar archive's OWN BYTES.
#
# ─── Why this exists ───────────────────────────────────────────────────────
# tools/sidecar_source_digest.sh answers "was this archive packed from these
# sources?", and it answers from the SOURCES alone — which is exactly what makes
# it recomputable on a host with no cargo. That is its strength and its hole: an
# archive re-packed or replaced OUT OF BAND (a hand-run `cpio` over stale `.bin`
# images, a `cp` of an older `sidecars.cpio` over this one) leaves the sources
# untouched, so the source stamp still matches and `make x86-iso` would ship the
# substituted bytes. The failure is the same one the source stamp exists for —
# an ISO whose init predates the kernel it runs against — reached without anyone
# editing `user/`.
#
# This tool records the archive's own sha256 alongside the source digest, and
# `make x86-iso` refuses to ship an archive whose bytes do not hash to it. Both
# halves are tracked and both are plain coreutils (`sha256sum`), so the check
# still runs wherever the committed archive is shipped — CI's ISO job, deploy —
# with no Rust toolchain and no build.
#
# Not a substitute for tools/sidecar_source_digest.sh: that one ties the archive
# to `user/` (and, through the fatal builds in `selfhost-bootimage`, to the
# binaries those sources produce). This one only ties the stamp to the exact
# bytes on disk, so a repack that skips the make target is caught even though
# the sources still agree. Ship both, and re-pack through `selfhost-bootimage`:
# it writes tools/sidecar_stamp_record.sh's pack-run record first and derives
# both stamps from it, so the two cannot be refreshed independently. That
# record also names the six packed binaries and is verified against the six
# flattened `.bin` files the packer just built (`--verify`), so the bytes this
# stamp covers are the run's own output.
#
# Usage:  tools/sidecar_archive_digest.sh [ARCHIVE]     # default sidecars.cpio
# Exit:   0 printed, 1 no hash tool, 2 archive missing/empty or outside the repo.
set -u

root="$(cd "$(dirname "$0")/.." && pwd)" || exit 2
[ -d "$root/user" ] || { echo "sidecar_archive_digest: $root/user is missing — run from the repo" >&2; exit 2; }
cd "$root" || exit 2

if ! command -v sha256sum >/dev/null 2>&1; then
    echo "sidecar_archive_digest: sha256sum not found (coreutils) — cannot hash the sidecar archive" >&2
    exit 1
fi

archive="${1:-sidecars.cpio}"
[ -s "$archive" ] || {
    echo "sidecar_archive_digest: $archive is missing or empty — nothing to hash (run from the repo, or pass the archive path)" >&2
    exit 2
}

# `--` so an archive whose name begins with '-' is still a filename, and cut on
# the first space so the leading hash is taken whether sha256sum prints one or
# two spaces (it prints two, plus the name).
sha256sum -- "$archive" | cut -d' ' -f1
