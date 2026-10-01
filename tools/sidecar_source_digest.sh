#!/usr/bin/env bash
# tools/sidecar_source_digest.sh — one line of hex: the digest of every source
# file the sidecar archive is packed FROM.
#
# ─── Why this exists ───────────────────────────────────────────────────────
# `sidecars.cpio` is a TRACKED artifact (the default `make x86-iso` ships it
# without building one, which is what lets CI's ISO job run with no Rust
# cross-toolchain step — see .github/workflows/ci.yml). The consequence is that
# a `user/` change that is never re-packed is invisible: the ISO boots an init
# built from the previous sources, and the failure surfaces as a KERNEL-side
# defect. That happened: the P1a registration's `ENV_REGISTER` round trip was
# answered by an init that had never heard of the opcode, so the kernel
# honestly reported "ENV_REGISTER reply carries 28 bytes, short of the 216-byte
# registration -- refused", and the afternoon went into the kernel.
#
# So the archive carries the digest of what it was packed from, and
# `make x86-iso` REFUSES to ship an archive whose digest no longer matches the
# tree. `make selfhost-bootimage` writes it (it is the thing that packs).
#
# It attests the packed BINARIES too, and NOT by hashing them: a binary hash
# written by the same run that packs cannot tell a rebuild from a fallback (the
# archive and its new hash agree either way). What makes the binaries part of
# the claim is that `selfhost-bootimage`'s cargo builds are FATAL, so the stamp
# below is only reachable by a run that rebuilt all six sidecars from these
# sources; a run that reused existing binaries because the x86_64-unknown-none
# target was missing refuses to write it rather than blessing them. That is why
# the digest stays over the SOURCES: it is the part any host, cargo or none, can
# recompute, which is what lets `x86-iso` verify the COMMITTED archive.
#
# It does NOT cover the archive's own BYTES: a repack or a swap done out of band
# leaves the sources untouched and this digest matching, so `x86-iso` would ship
# the substituted archive. That half is tools/sidecar_archive_digest.sh -- a
# sha256 of the archive file, tracked beside this digest, and also plain
# coreutils so it too is recomputable on a host with no cargo. The two are
# projections of one pack-run record (tools/sidecar_stamp_record.sh,
# `$(SIDECAR_CPIO).stamps`), so neither can be refreshed without the other --
# and that record also NAMES the six packed binaries
# (tools/sidecar_archive_entry_digest.sh), with the packer passing the six
# flattened outputs it just built for verification, so the archive the record
# describes is the one the packer made, not one assembled from stale images.
#
# ─── Not a timestamp, and that is the whole point ──────────────────────────
# "is sidecars.cpio older than user/*.rs?" is the obvious rule and it is
# unusable: a fresh `git clone` writes the index in path order, so
# `sidecars.cpio` lands BEFORE `user/...` and every clean checkout would
# refuse. A digest of the sources answers the question that was actually being
# asked ("was this archive packed from these sources?") and answers it in a
# tarball, in a worktree, and in a fresh clone identically.
#
# ─── What is in it, and why the list is a superset ─────────────────────────
# Everything a build of the six sidecars can read: Rust sources, the manifests
# and lockfile, the linker scripts and `crt0.S` each sidecar is linked with,
# and the C headers it builds against (`.h`), plus the gui `.slint` sources.
# cargo's REAL dependency closure needs `cargo metadata`; this is the plain
# superset of it, deliberately. Over-inclusion costs a re-pack (about a
# minute), under-inclusion ships the bug above.
#
# Deliberately NOT in it: anything a `clean` or a kernel build writes
# (`user/target/`, `user/shell.x86.o` — hashing build output would refuse the
# next ISO after every `make`), and prose (`user/README.md`, the `user/examples`
# guest fixtures, which no sidecar links).
#
# Usage:  tools/sidecar_source_digest.sh          # prints the digest
# Exit:   0 printed, 1 no hash tool, 2 run outside the repo.
set -u

root="$(cd "$(dirname "$0")/.." && pwd)" || exit 2
[ -d "$root/user" ] || { echo "sidecar_source_digest: $root/user is missing — run from the repo" >&2; exit 2; }
cd "$root" || exit 2

if ! command -v sha256sum >/dev/null 2>&1; then
    echo "sidecar_source_digest: sha256sum not found (coreutils) — cannot digest the sidecar sources" >&2
    exit 1
fi

# A version tag first, so a future change to the file SET cannot silently
# compare equal against a stamp written by the old set: the line changes shape
# and every existing stamp mismatches, which is a re-pack, not a false pass.
{
    printf 'sidecar-sources-v1\n'
    find user \
        -path user/target -prune -o \
        -type f \( -name '*.rs' -o -name '*.toml' -o -name 'Cargo.lock' \
                   -o -name '*.S' -o -name '*.ld' -o -name '*.h' \
                   -o -name '*.json' -o -name '*.slint' \) -print |
        LC_ALL=C sort |
        while IFS= read -r f; do
            printf '%s ' "$f"
            sha256sum < "$f" | cut -d' ' -f1
        done
} | sha256sum | cut -d' ' -f1
