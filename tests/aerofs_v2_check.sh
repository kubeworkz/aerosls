#!/usr/bin/env bash
# tests/aerofs_v2_check.sh — aerofs-lite v2: the writable format, and the
# version rule that decides who may write it (POSIX-Environments v0.2 §5,
# P1b part 1).
#
# ─── What this pins, and why the host tests alone do not ───────────────────
# user/vfs/tests/vfs_tests.rs proves the format WORKS: an empty store formats
# v2, a 71,200-byte file (past v1's 71,168-byte ceiling) is written, the
# store's own bytes are re-booted as a fresh kernel and driver, and both files
# read back byte-identical. What a test cannot state is the RULE around that:
# which version is READ, which is WRITTEN, and what a third version gets. A
# future edit can rewrite `AEROFS_VERSION`, or repurpose the builder, and every
# test still passes — a test that formats its own store is happy whichever
# version the formatter writes, and a v1 image mounts without a write ever
# being attempted. That silent drift is what this guard exists for, clause by
# clause:
#
#   A. the version rule is real — both versions named, the version this build
#      WRITES is v2, and an unknown version is a NAMED refusal rather than
#      "this is not an image"
#   B. the layout moved — 10 direct + two indirect blocks (266 blocks,
#      136,192 B per file), v1's ceiling still written down as 71,168 B, and
#      the v2 inode's two pointer offsets are the ones the code uses
#   C. the builder still writes v1 — every existing image, the system rootfs
#      included, keeps its version and therefore its read-only mount
#   D. the fs layer is version-dispatched — v1 answers `ERofs` from the
#      mount's own gate AND from every mutating entry point, and each version
#      parses its own inode layout
#   E. an empty store formats v2, sized by the DEVICE's block count, writing
#      only the non-zero metadata blocks (the v1 builder is NOT what happens
#      to an empty store any more)
#   F. the host tests that carry the end-to-end teeth exist — and, on a tree
#      that has the user workspace (the real repository), they PASS, with the
#      byte-identity test's name read back out of the run
#
# ─── The hermetic seam ────────────────────────────────────────────────────
# The guard takes an optional repository root (default: its own parent) and
# reads only files inside it. That seam is what lets
# tests/aerofs_v2_check_smoke.sh copy a minimal tree, break one thing, and
# require THIS guard to exit 1 naming THAT clause. A tree with no
# user/Cargo.toml (the smoke's) runs A–E and F's existence half and says why
# the cargo half did not run; the real tree runs all of F.
#
# Exit: 0 pass, 1 fail — each violation printed as "FAIL: <clause>." — or
# 2 abort (a prerequisite is missing, and the guard says which).
set -u

ROOT="${1:-$(cd "$(dirname "$0")/.." && pwd)}"
AEROFS="$ROOT/user/vfs/src/aerofs.rs"
VFS="$ROOT/user/vfs/src/vfs.rs"
TESTS="$ROOT/user/vfs/tests/vfs_tests.rs"

missing=""
for f in "$AEROFS" "$VFS" "$TESTS"; do
    [ -f "$f" ] || missing="$missing $f"
done
if [ -n "$missing" ]; then
    echo "ABORT: missing:$missing" >&2
    echo "       pass a repository root as \$1 (the default is the guard's parent)" >&2
    exit 2
fi

fails=0
ok()   { printf 'ok:   %s\n' "$1"; }
fail() { printf 'FAIL: %s.\n' "$1"; fails=$((fails + 1)); }
has()  { grep -Fq -- "$2" "$1"; }        # has <file> <fixed-string>

echo "aerofs_v2_check — the writable format and the version rule"
echo "root: $ROOT"
echo "========================================================================"

# ── A. The version rule ─────────────────────────────────────────────────────
# Both versions are named, the writer's is v2, and an unknown version is a
# refusal BY NAME. All three are load-bearing: `AEROFS_VERSION` is what the
# formatter writes and what `mount_aerofs_or_format` mounts read-write, so
# moving it to 1 would silently turn every formatted store read-only.
if has "$AEROFS" 'pub const AEROFS_VERSION_V1: u32 = 1;' \
   && has "$AEROFS" 'pub const AEROFS_VERSION_V2: u32 = 2;' \
   && has "$AEROFS" 'pub const AEROFS_VERSION: u32 = AEROFS_VERSION_V2;'; then
    ok "A. v1 is 1, v2 is 2, and the version this build writes is v2"
else
    fail "A. the version constants no longer say read-v1/write-v2"
fi

if has "$AEROFS" 'pub enum SuperblockRefusal' \
   && has "$AEROFS" 'found => return Err(SuperblockRefusal::Version { found }),'; then
    ok "A. an unknown version is a NAMED refusal (SuperblockRefusal::Version)"
else
    fail "A. an unknown version no longer has a named refusal"
fi

if has "$AEROFS" 'read v1, write v2, refuse'; then
    ok "A. the migration rule is written down where it is enforced"
else
    fail "A. the read-v1/write-v2/refuse-the-rest rule is not stated in aerofs.rs"
fi

# ── B. The layout moved ─────────────────────────────────────────────────────
# v2's inode has ten direct pointers and TWO indirect blocks. The exact
# constants matter — not just "some bigger number" — because the max-file
# clause of the phase is 71,168 < size, and a wrong count would silently
# shrink the ceiling back or overrun the inode's 64 bytes.
if has "$AEROFS" 'pub const NDIRECT_V2: usize = 10;' \
   && has "$AEROFS" 'pub const MAX_BLOCKS_V2: u64 = (NDIRECT_V2 + 2 * NINDIRECT) as u64;' \
   && has "$AEROFS" 'pub const MAX_FILE_BYTES_V1: u64 = MAX_BLOCKS * aerosls_proto::BLOCK_SIZE as u64;' \
   && has "$AEROFS" 'pub const MAX_FILE_BYTES_V2: u64 = MAX_BLOCKS_V2 * aerosls_proto::BLOCK_SIZE as u64;'; then
    # The guard carries the arithmetic itself: 10 + 2*128 = 266 blocks; 266
    # blocks * 512 B = 136,192; v1's 11 + 128 = 139 blocks * 512 = 71,168.
    if [ $(( (10 + 2 * 128) * 512 )) -eq 136192 ] \
       && [ $(( (11 + 128) * 512 )) -eq 71168 ] \
       && [ $(( 10 + 2 * 128 )) -eq 266 ]; then
        ok "B. v2 is 10 direct + two indirect blocks: 266 blocks, 136,192 B (v1: 139 blocks, 71,168 B)"
    else
        fail "B. the guard's own block arithmetic no longer matches 136,192 / 71,168"
    fi
else
    fail "B. the second-indirect-block layout or one of the two ceilings is gone"
fi

# The two indirect pointers' offsets ARE the layout: v2's indirect1 lives
# where v1's eleventh direct pointer lived (54..58) and indirect2 where v1
# kept its only indirect (58..62). A reader of the wrong version follows the
# wrong one, silently, which is why the offsets are pinned here.
if has "$AEROFS" 'indirect: u32le(&slot[54..]),' \
   && has "$AEROFS" 'indirect2: u32le(&slot[58..]),' \
   && has "$AEROFS" 'put_u32le(&mut slot[54..], ino.indirect);' \
   && has "$AEROFS" 'put_u32le(&mut slot[58..], ino.indirect2);'; then
    ok "B. the v2 inode's two indirect pointers are at 54 and 58"
else
    fail "B. the v2 inode's pointer offsets moved"
fi

# ── C. The builder still writes v1 ──────────────────────────────────────────
# `ImageBuilder::build()` is what writes the system rootfs image and every
# image the tests seed. It must keep writing v1: a built image is read-only,
# and a v2 builder would turn every existing image into a writable store.
if has "$AEROFS" 'put_u32le(&mut sb_raw[4..], AEROFS_VERSION_V1);'; then
    ok "C. the image builder still writes v1 (every built image stays read-only)"
else
    fail "C. the image builder stopped writing v1 — built images would change version"
fi

# ── D. The fs layer is version-dispatched ───────────────────────────────────
d_ok=1
has "$VFS" 'writable: sb.version == crate::aerofs::AEROFS_VERSION_V2,' || d_ok=0
has "$VFS" '!self.writable' || d_ok=0
has "$VFS" 'Fs::Aerofs(f) => f.read_only(),' || d_ok=0
if [ "$d_ok" -eq 1 ]; then
    ok "D. the mount's version decides writability, and the read-only gate uses it"
else
    fail "D. the mount no longer derives writability from the version (or the gate ignores it)"
fi

# Both inode layouts are read, each where its version says: the v2 parse on a
# writable store, the v1 parse otherwise. Both the inode table read and the
# block-address walk dispatch on the same flag, so neither can quietly start
# assuming the other version's offsets.
if has "$VFS" 'let inode = if self.writable {' \
   && has "$VFS" 'parse_inode_v2(&block[in_blk..])' \
   && has "$VFS" 'parse_inode(&block[in_blk..])' \
   && has "$VFS" 'let (indirect, idx) = if self.writable {'; then
    ok "D. each version's inode layout is the one its reads use"
else
    fail "D. the inode parse or the block-address walk stopped being version-dispatched"
fi

# EVERY mutating entry point answers ERofs on a v1 mount. The count is the
# clause: write, truncate, create_file, create_dir, unlink, rmdir and rename
# are seven, and one that forgets is a v1 image with one writable door.
d_guards=$(grep -Fc 'if !self.writable {' "$VFS")
if [ "$d_guards" -ge 7 ]; then
    ok "D. every mutating entry point refuses v1 for itself ($d_guards v1 guards)"
else
    fail "D. only $d_guards mutating entry points refuse v1 (expected at least 7)"
fi

# ── E. An empty store formats v2, by the device's size ─────────────────────
e_ok=1
has "$VFS" 'let total = cache.blocks() as u32;' || e_ok=0
has "$VFS" 'format_v2(total).ok_or(Errno::EInval)?' || e_ok=0
has "$VFS" 'if blk.iter().all(|&x| x == 0) {' || e_ok=0
# The v1 builder must NOT be what an empty store gets any more: that was the
# read-only format-time image, and formatting it here would make every fresh
# tenant store v1 — and there is no in-place upgrade in this cut.
if grep -Fq -- 'ImageBuilder::new().build()' "$VFS"; then
    e_ok=0
fi
if [ "$e_ok" -eq 1 ]; then
    ok "E. an empty store formats the WRITABLE v2, sized by the device, non-zero blocks only"
else
    fail "E. the format-on-empty path no longer writes v2 (or writes the v1 builder image)"
fi

# ── F. The end-to-end teeth exist (and, here, pass) ─────────────────────────
# The name is pinned WITH its parameter list and brace ("fn name() {"), not
# as a bare substring: a test renamed to "..._renamed" still CONTAINS the old
# name, and a guard that accepted that would pass a tooth that renamed it
# away. This is the same trap the pin-check smoke documents for opcodes.
f_missing=""
for name in \
    v2_empty_store_formats_writable \
    v2_large_file_survives_a_reboot \
    v2_allocator_frees_and_refuses_clearly \
    v1_image_mounts_read_only_through_the_formatting_mount
do
    has "$TESTS" "fn $name() {" || f_missing="$f_missing $name"
done
for name in \
    v2_inode_roundtrip_and_field_offsets \
    v2_ceilings_moved \
    v2_format_layout_and_allocation_state \
    version_rule_refusals_are_named \
    builder_still_writes_v1
do
    has "$AEROFS" "fn $name() {" || f_missing="$f_missing $name"
done
if [ -n "$f_missing" ]; then
    fail "F. the format's teeth are missing:$f_missing"
else
    ok "F. the v2 host tests exist (format round-trip, refusals, byte-identity survival)"
fi

if [ -z "$f_missing" ] && has "$TESTS" 'byte-identical'; then
    ok "F. the survival clause is stated as byte-identity in the test"
else
    [ -z "$f_missing" ] && fail "F. the survival test no longer asserts byte-identity"
fi

if [ -f "$ROOT/user/Cargo.toml" ]; then
    if ! command -v cargo >/dev/null 2>&1; then
        echo "ABORT: cargo is not available, but $ROOT/user/Cargo.toml exists —" >&2
        echo "       the host tests are half of clause F and must be able to run." >&2
        exit 2
    fi
    echo "--- running the host tests (cargo test -p aerosls-vfs)…"
    # Deliberately NOT `-q`: cargo's quiet mode makes libtest print progress
    # dots instead of test names, and the names are half of what is checked
    # here (a suite that ran zero of these tests must not pass for having run
    # others).
    out="$(cd "$ROOT" && cargo test -p aerosls-vfs --manifest-path user/Cargo.toml 2>&1)"
    rc=$?
    if [ "$rc" -eq 0 ] \
       && printf '%s\n' "$out" | grep -q 'v2_large_file_survives_a_reboot ... ok' \
       && printf '%s\n' "$out" | grep -q 'v2_inode_roundtrip_and_field_offsets ... ok' \
       && printf '%s\n' "$out" | grep -q 'version_rule_refusals_are_named ... ok' \
       && printf '%s\n' "$out" | grep -q 'test result: ok.'; then
        ok "F. the v2 host tests PASS on this tree (cargo test -p aerosls-vfs)"
    else
        fail "F. the v2 host tests did not pass (cargo rc=$rc; the failure output follows)"
        printf '%s\n' "$out" | tail -30 | sed 's/^/      /'
    fi
else
    echo "note: no user/Cargo.toml under the given root — the host-test half of"
    echo "      clause F was not run (this is the smoke's seeded-tree seam)."
fi

echo
if [ "$fails" -eq 0 ]; then
    echo "aerofs_v2_check: PASS"
    exit 0
fi
echo "aerofs_v2_check: FAIL ($fails clause(s))"
exit 1
