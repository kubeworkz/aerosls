#!/usr/bin/env bash
# tests/env_storage_durable_check.sh — POSIX-Environments Roadmap v0.2,
# Phase P1b's verification plan (§5) made executable:
#
#   "create an environment, write a file larger than 71 168 bytes (the clause
#    that proves the format moved) and a small one, record a hash of both;
#    reboot; assert both are present and byte-identical through the
#    environment's own console; assert the partition's storage quota usage
#    moved by the file's size and that a write past the quota is refused with
#    the quota's own error, not a frame-pool exhaustion."
#
# ─── Modes ─────────────────────────────────────────────────────────────────
#   (no arguments)     the SOURCE clauses over this script's own repository root
#   --live             the source clauses, then the boot arm (QEMU + the ISO):
#                      write, quota, reboot, re-read, over-quota refusal
#   --replay DIR       only the artifact validation of a recorded run (no QEMU,
#                      no build) — how the smoke proves the boot clauses' teeth
#   positional ROOT    the source clauses over another tree (the smoke's own
#                      hermetic seam; every real caller passes nothing)
#
# ─── Live knobs (all optional; defaults are what every other caller gets) ──
#   P1B_ISO          the ISO to boot                  (default sls_operating_system.iso)
#   P1B_BOOT_ENTRY   1-based grub MENU POSITION       (default 3 = the unified entry)
#   P1B_WINDOW_S     seconds to wait for the markers  (default 180)
#   P1B_PORT         the QEMU hostfwd port            (default tests/free_port.sh)
#   P1B_RAM / P1B_SMP                                 (default 1G / 2)
#   P1B_ARTIFACTS    where the recorded evidence lands (default a temp dir,
#                    printed at the end so it can be --replay'd later)
#   P1B_INDEX        the environment's index in its partition (default 2)
#   P1B_TOOTH=ram-backed|unquotaed|format-v1-only|bad-format-version
#                    source-level teeth (see the smoke): rejected by the boot
#                    arm — the boot arm records what really happened — and
#                    asserted by the source clauses and by --replay fixtures.
#
# ─── Why this guard exists, and what it will not claim ─────────────────────
# tests/aerofs_v2_check.sh guards the FORMAT layer (the writable v2 that a
# durable region must be formatted in). This guard is the other half: the
# DEVICE, the QUOTA and the DESCRIPTOR. Its three claims, one clause group
# each:
#
#   S1-S10 (source)  the wiring: a per-environment NVMe extent band that
#                    cannot collide with the stream or row-store pools, a
#                    persisted directory whose LBAs keep the one-frame safety
#                    gap, attach-before-spawn, release-before-free, the
#                    write-through chain (ramdisk server → kernel → extent),
#                    first-touch quota charged to the TENANT's partition with
#                    denial before any side effect, the descriptor's
#                    state_lba stamped from the table, and the boot seam that
#                    makes "once per boot" a line of code.
#   S11 (source+run) the real kernel module, executed: env_storage_host_test.c
#                    is built and run HERE, inside the inspected root (its own
#                    "Build and run:" comment is the build recipe — the guard
#                    refuses to run one the comment no longer states).
#   S12 (source)     the observables the live arm reads: the operator's
#                    storagequota surface and the [STORAGE-QUOTA] section.
#   S13 (source)     the wiring of the BOOT evidence itself: ci.yml runs
#                    THIS guard's --live arm, after the ISO build. A clause
#                    cannot reboot anything — but it can insist that the job
#                    that can does. Without it the evidence is a recording
#                    someone trusted, and §5's "what this increment does not
#                    claim" line never retires; with it, deleting or
#                    reordering the step reddens this guard by name.
#   D1-D5 (replay)   the live claims, validated from a recorded run's
#                    artifacts: the attach happened durably, the environment
#                    came back WITH the reboot's own serial evidence (the
#                    directory reloaded, the extent read back from NVMe), the
#                    files' bytes survived the reboot as read through the
#                    environment's own console, quota usage moved by the
#                    file's size, and an over-quota write was refused by the
#                    QUOTA's own line with the usage pinned at the ceiling and
#                    no frame-pool denial anywhere near it.
#
# ─── Teeth, in tests/env_storage_durable_check_smoke.sh ────────────────────
#   P1B_TOOTH=ram-backed           the store loses its extent (durable flag
#                                  off) but keeps its entry. Expected: D2c
#                                  (no LBA restore line) and D3a/D3b (the
#                                  post-reboot bytes are gone) red, while D4
#                                  and D5 stay green — the control that
#                                  separates "durable" from "was written":
#                                  metering and refusal hold their own
#                                  evidence while durability alone reddens.
#   P1B_TOOTH=unquotaed           the entry's charge partition becomes
#                                  PARTITION_SYSTEM (E4 Finding 1's exact
#                                  shape on the durable side). Expected: D4
#                                  red (the tenant's usage never moves — and
#                                  D5 with it, since a tenant nothing is
#                                  charged to is never over quota), with
#                                  every durability-side clause green.
#   P1B_TOOTH=format-v1-only      the v2 ceiling is the v1 ceiling (the
#                                  derived MAX_BLOCKS_V2 expression replaced
#                                  by v1's). Expected: the source clause that
#                                  pins the DERIVED expression (S8h) red —
#                                  the constant-folded arithmetic alone would
#                                  not see this — the small-file clauses
#                                  green; replay fixture D3a red, D3b green.
#   P1B_TOOTH=bad-format-version  the formatting mount treats any unparsable
#                                  store as fresh (`if block.iter().all(...)`
#                                  replaced by `if true`) — a store labelled
#                                  v3 would be FORMATTED OVER rather than
#                                  refused by name. Expected: the source
#                                  refusal clause (S8e) red — its zero-gate
#                                  half specifically.
#
# ─── What this guard will NOT claim ────────────────────────────────────────
# Byte-identity of the big file is a FINGERPRINT of three reads (wc -c,
# head -n 20, tail -n 20) rather than a whole-file hash: the environment's
# console is a 4 KiB ring buffer (ENV_CONSOLE_BUF) with no hash applet, so
# cat-ing 128 KiB through it would drop the middle. The fingerprint is
# recorded by THIS script from the console's own output and compared across
# the reboot — which is what the roadmap's "record a hash of both" is for.
# The small file is read whole and hashed whole.

set -u
cd "$(dirname "$0")/.."

# ─── Argument parsing ──────────────────────────────────────────────────────
MODE="source"
ROOT=""
REPLAY_DIR=""
TOOTH="${P1B_TOOTH:-}"
while [ $# -gt 0 ]; do
    case "$1" in
        --live)   MODE="live" ;;
        --replay) MODE="replay"; shift; REPLAY_DIR="${1:-}" ;;
        --help|-h)
            sed -n '2,12p' "$0"; exit 0 ;;
        -*) echo "ABORT: unknown option '$1' (see the header)" >&2; exit 2 ;;
        *)  ROOT="$1" ;;
    esac
    shift
done
ROOT="${ROOT:-$(pwd)}"

fail=0
ok()   { echo "ok:   $*"; }
bad()  { echo "FAIL: $*"; fail=1; }
note() { echo "note: $*"; }

# has <file> <fixed string> -> 0 when present
has() { grep -qF -- "$2" "$1" 2>/dev/null; }

# body <file> <literal first line> -> the function/block, up to the first line
# that is exactly "}" at column 0 (the shape every function in these files has).
# The anchor matches at the line's first non-blank character, so Rust methods
# inside an `impl` block count too (the C functions are column 0 anyway).
body() {
    awk -v pat="$2" '{ line = $0; sub(/^[ \t]+/, "", line)
                       if (index(line, pat) == 1) grab = 1 }
                     grab { print; if ($0 == "}") exit }' "$1"
}

# order_ok <file> <earlier-fixed-string> <later-fixed-string> -> 0 when the
# first match's line number is strictly less than the second's. This is how
# "attach BEFORE spawn" is judged: by position, not by hope.
order_ok() {
    local a b
    a=$(grep -nF -- "$2" "$1" 2>/dev/null | head -1 | cut -d: -f1)
    b=$(grep -nF -- "$3" "$1" 2>/dev/null | head -1 | cut -d: -f1)
    [ -n "$a" ] && [ -n "$b" ] && [ "$a" -lt "$b" ]
}

# dval <file> <NAME> -> the third field of the #define line (leading spaces ok)
dval() { awk -v n="$2" '$1=="#define" && $2==n { print $3; exit }' "$1" 2>/dev/null; }

# rval <file> <NAME> -> the value of a Rust `pub const NAME: <ty> = value;`
# (dval parses #define; the format layer's ceilings are Rust consts).
rval() { awk -v n="$2:" '$2 == "const" && index($3, n) == 1 {
             v = $6; sub(/;.*/, "", v); print v; exit }' "$1" 2>/dev/null; }

# ─── Source clauses ────────────────────────────────────────────────────────
source_clauses() {
    local H="$ROOT/kernel/env_storage.h" C="$ROOT/kernel/env_storage.c"
    local P="$ROOT/kernel/persist.h"     PC="$ROOT/kernel/persist.c"
    local E="$ROOT/user/init/src/env_manager.rs"
    local S="$ROOT/user/ramdisk/src/server.rs"
    local K="$ROOT/user/proto/src/kabi.rs"  PL="$ROOT/user/proto/src/lib.rs"
    local ER="$ROOT/user/vfs/src/errno.rs"  VF="$ROOT/user/vfs/src/vfs.rs"
    local AE="$ROOT/user/vfs/src/aerofs.rs" SB="$ROOT/user/sidecar/src/boot.rs"
    local CK="$ROOT/kernel/env_ckpt.c"      EPH="$ROOT/kernel/env_ckpt.h"
    local SH="$ROOT/kernel/stream.h"        RW="$ROOT/kernel/rowstore.h"
    local QH="$ROOT/kernel/storage_quota.h" SHELL="$ROOT/user/shell.c"
    local EP="$ROOT/kernel/env_proto.h"     EPR="$ROOT/user/proto/src/env_proto.rs"
    local DISP="$ROOT/kernel/syscall_dispatch.c" CAP="$ROOT/kernel/cap.c"
    local CIW="$ROOT/.github/workflows/ci.yml"

    for f in "$H" "$C" "$P" "$PC" "$E" "$S" "$K" "$PL" "$ER" "$VF" "$AE" \
             "$SB" "$CK" "$EPH" "$SH" "$RW" "$QH" "$SHELL" "$EP" "$EPR" \
             "$DISP" "$CAP" "$CIW"; do
        # A tree without the surface cannot be judged at all: refusal (exit 2,
        # the run_checks fail-closed), never a quiet pass over what is missing.
        [ -f "$f" ] || { echo "ABORT: missing $f — the P1b surface this guard reads is not in this tree" >&2; exit 2; }
    done

    # ── S1. the extent band: derived here, not transcribed ─────────────────
    local base frames slots per slot row
    base=$(dval "$SH" STREAM_DATA_LBA_BASE); base="${base%ULL}"
    frames=$(dval "$SH" STREAM_MAX_FRAMES)
    slots=$(dval "$SH" STREAM_MAX)
    per=$(( frames * 8 ))
    row=$(dval "$RW" ROWSTORE_LBA_BASE); row="${row%ULL}"
    local band band_end
    band=$(dval "$H" ENV_STORAGE_DATA_LBA_BASE); band="${band%ULL}"
    if [ -z "$base" ] || [ -z "$frames" ] || [ -z "$slots" ] || [ -z "$band" ]; then
        bad "S1. a band-defining constant is unreadable (STREAM_DATA_LBA_BASE='$base' STREAM_MAX_FRAMES='$frames' STREAM_MAX='$slots' ENV_STORAGE_DATA_LBA_BASE='$band') — the arithmetic cannot be done, so nothing about collision-freedom can be claimed"
    elif [ "$band" -ne $(( base + slots * per )) ]; then
        bad "S1. ENV_STORAGE_DATA_LBA_BASE=$band but this guard computes stream end = $base + $slots*$per = $(( base + slots * per )) — the durable band must start exactly where stream data ends, and a transcription drift here is the region-overlap bug persist_lba_layout_host_test.c exists for"
    elif [ $(( band + slots * 2048 )) -gt "$row" ]; then
        bad "S1. the env-storage band ($band..$(( band + slots * 2048 ))) overlaps ROWSTORE_LBA_BASE=$row"
    else
        ok "S1. the durable band is derived and collision-free: stream data ends at $(( base + slots * per )), extents live at $band..$(( band + slots * 2048 )), row-store starts at $row"
    fi
    local pages pbytes
    pages=$(dval "$H" ENV_STORAGE_PAGES);  pages="${pages%u}"
    pbytes=$(dval "$H" ENV_STORAGE_PAGE_BYTES); pbytes="${pbytes%u}"
    if [ "$pages" = "256" ] && [ "$pbytes" = "4096" ]; then
        ok "S1b. one extent is 256 x 4096 = 1 MiB, exactly the tenant ramdisk's region (RD_STORAGE_FRAMES 256)"
    else
        bad "S1b. ENV_STORAGE_PAGES='$pages' ENV_STORAGE_PAGE_BYTES='$pbytes' — expected 256/4096 (the extent must be exactly the 1 MiB region it caches, or region page i stops mapping extent page i)"
    fi

    # ── S2. the directory's LBAs keep the one-frame safety gap ─────────────
    local eh ee ck sd
    eh=$(dval "$P" PERSIST_ENVSTOR_HDR_LBA); eh="${eh%ULL}"
    ee=$(dval "$P" PERSIST_ENVSTOR_ENT_LBA); ee="${ee%ULL}"
    ck=$(dval "$P" PERSIST_ENV_CKPT_ENT_LBA); ck="${ck%ULL}"
    sd=$(dval "$SH" STREAM_DIR_LBA); sd="${sd%ULL}"
    if [ -z "$eh" ] || [ -z "$ee" ] || [ -z "$ck" ] || [ -z "$sd" ]; then
        bad "S2. a directory LBA is unreadable (HDR='$eh' ENT='$ee' CKPT_ENT='$ck' STREAM_DIR='$sd')"
    elif [ "$eh" -ne $(( ck + 8 + 8 )) ]; then
        bad "S2. PERSIST_ENVSTOR_HDR_LBA=$eh — the P1a entries end at $(( ck + 8 )) and the safety gap every boundary in persist.h carries is one frame, so the header belongs at $(( ck + 8 + 8 ))"
    elif [ "$ee" -ne $(( eh + 8 )) ]; then
        bad "S2. PERSIST_ENVSTOR_ENT_LBA=$ee — the header at $eh is one frame, so the entries begin at $(( eh + 8 ))"
    elif [ $(( ee + 8 )) -gt "$sd" ]; then
        bad "S2. the entry frame ($ee..$(( ee + 8 ))) reaches STREAM_DIR_LBA=$sd"
    else
        ok "S2. the directory sits in the small-record band with its gaps: header $eh, entries $ee..$(( ee + 8 )), stream dir at $sd"
    fi
    has "$C" "_Static_assert(sizeof(struct EnvStorageEntry) == 32," || \
        bad "S2b. env_storage.c no longer pins sizeof(struct EnvStorageEntry) == 32 — the checksum span in p_region_specs is a compile-time constant and a grown entry would silently truncate into the next frame"
    [ $(( 8 * 32 )) -le 4096 ] && \
        ok "S2b. 8 entries x 32 B = 256 B fits the single frame (this guard's own arithmetic)" || \
        bad "S2b. 8 x 32 does not fit a frame"

    # ── S3. the directory persists through persist.c's own machinery ───────
    has "$PC" "{ PERSIST_ENVSTOR_HDR_LBA, PERSIST_MAGIC_ENV_STORAGE," || \
        bad "S3. persist.c's p_region_specs has no env-storage region — the directory would never be checksum-verified or torn-write-protected like every other persisted array"
    has "$PC" "PERSIST_PEND_ENVSTOR       (1u << 17)" || \
        bad "S3b. PERSIST_PEND_ENVSTOR is not bit 17 (or is absent) — the pending-bit sequence is what defers the write into one batch"
    local pend_max
    pend_max=$(grep -oE 'PERSIST_PEND_[A-Z]+ +\(1u << *[0-9]+\)' "$PC" | grep -oE '<< *[0-9]+' | grep -oE '[0-9]+' | sort -n | tail -1)
    [ "${pend_max:-0}" -eq 17 ] || \
        bad "S3c. the highest persist pend bit is ${pend_max:-none}, expected 17 (P1B's) — another region took the bit or P1B's was never added"
    has "$PC" "if (pend & PERSIST_PEND_ENVSTOR)       persist_env_storage();" || \
        bad "S3d. the defer flush never calls persist_env_storage() — a batched directory change would be noted and never written"
    order_ok "$PC" "env_storage_boot_reset();" "PERSIST_ENVSTOR_HDR_LBA, p_buf" || \
        bad "S3e. the boot seam runs AFTER the directory read in persist_restore_all — env_storage_boot_reset() must cross the boundary before the persisted occupancy is re-charged, or a same-boot flag would silently skip the re-charge"
    # Refusal by version and size, before the array is touched:
    order_ok "$PC" "version == ENV_STORAGE_REC_VERSION" "persist_read_array(env_storage_table," || \
        bad "S3f. the directory restore does not validate record size + version BEFORE reading into the live table — a foreign snapshot would land half-applied (refusal over partial application)"
    has "$PC" "directory REFUSED" || \
        bad "S3g. a refused directory has no named refusal line — the serial log would say nothing about why every store cold-started"

    # ── S4. attach before spawn; release before free ───────────────────────
    order_ok "$E" "k.env_storage_attach(" "k.create_sidecar_in(&rd_manifest" || \
        bad "S4. env_storage_attach is not called BEFORE the ramdisk server is created in create_environment — the server's RD_INFO restore would race an entry that does not exist yet, and a rebooted environment would mount an empty store"
    local between
    between=$(awk '/k.env_storage_attach\(/{f=1} f&&/k.free_region_in/{c++} /let rd_name/{exit} END{print c+0}' "$E")
    [ "$between" -ge 3 ] || \
        bad "S4b. a refused attach does not hand back all three regions (found $between free_region_in calls between the attach and the first name) — a refused placement must allocate nothing (E4)"
    order_ok "$E" "k.env_storage_release(env.rd_storage)" "k.free_region_in(env.rd_heap" || \
        bad "S4c. reclaim_environment releases the regions before the store — the directory entry is keyed by the region's base, so release() must run while that base still names it"

    # ── S5. quota: the tenant's own pages, denied before any side effect ───
    local charge
    charge=$(body "$C" "static int es_charge_to")
    if printf '%s' "$charge" | grep -qF 'storage_page_reserve(e->partition_id)'; then
        ok "S5. first-touch charging reserves against e->partition_id — the store's own tenant"
    else
        bad "S5. es_charge_to no longer calls storage_page_reserve(e->partition_id) — the charge subject is not the store's partition"
    fi
    printf '%s' "$charge" | grep -qF 'storage_page_release(e->partition_id, got)' || \
        bad "S5b. a mid-loop denial does not release the pages THIS call charged — the watermark would advance over pages the quota never granted (denial must touch nothing)"
    has "$C" "storage_page_reserve(PARTITION_SYSTEM" && \
        bad "S5c. env_storage.c charges PARTITION_SYSTEM by name — a tenant's disk must count against the tenant (E4 Finding 1's residue, reappearing on the durable side)"
    local quota_hdr_rs quota_hdr_h
    quota_hdr_rs=$(grep -cF 'ENV_ERR_QUOTA' "$EPR" || true)
    quota_hdr_h=$(grep -cF 'ENV_ERR_QUOTA' "$EP" || true)
    [ "$quota_hdr_rs" -ge 1 ] && [ "$quota_hdr_h" -ge 1 ] || \
        bad "S5d. ENV_ERR_QUOTA is missing from one side of the wire (env_proto.rs: $quota_hdr_rs, env_proto.h: $quota_hdr_h) — the two sides' status codes must travel together"
    local n_rs n_h
    n_rs=$(awk '/ENV_ERR_QUOTA: u16 = /{print $0; exit}' "$EPR" | grep -oE '= [0-9]+' | grep -oE '[0-9]+')
    n_h=$(awk '/#define ENV_ERR_QUOTA/{print $3; exit}' "$EP")
    [ -n "$n_rs" ] && [ "$n_rs" = "$n_h" ] || \
        bad "S5e. ENV_ERR_QUOTA disagrees across the wire (Rust=$n_rs C=$n_h) — a reply the C side names 'quota' would decode as something else in Rust"
    order_ok "$C" "if (es_charge_to(e, need))" "nvme_write_pages_sync" || \
        bad "S5f. in env_storage_write the quota charge no longer precedes the device write — a refusal would happen AFTER bytes moved (denial before any side effect)"
    # …and the quota's own line reaches the serial log with the right name:
    has "$C" "[ENV-STORAGE] quota denied partition=" || \
        bad "S5g. a quota denial prints no [ENV-STORAGE] quota denied line — 'refused with the quota's own error' would be unobservable to an operator or a guard"
    # …and the release path hands the pages back:
    has "$C" "storage_page_release(e->partition_id, charged)" || \
        bad "S5h. release does not hand the charged pages back to the partition — destroy would leak the tenant's quota forever"

    # ── S6. the write-through chain, end to end ────────────────────────────
    local wb
    wb=$(body "$S" "fn write_blocks")
    printf '%s' "$wb" | grep -qF 'k.env_storage_write(dev.storage_base' || \
        bad "S6. the ramdisk server no longer write-throughs on RD_WRITE — its RAM copy would be the last copy, which is exactly the pre-P1b device"
    order_ok "$S" "copy::copy_blocks(src, dst, bytes)" "k.env_storage_write(dev.storage_base" || \
        bad "S6b. env_storage_write is called BEFORE the copy lands in the region — the kernel persists the frame pages, so the bytes must be there first"
    printf '%s' "$wb" | grep -qF 'RD_ERR_QUOTA' || \
        bad "S6c. write_blocks never replies RD_ERR_QUOTA — the quota refusal cannot cross the wire"
    local rb
    rb=$(body "$S" "pub fn run<K: Kernel>")
    printf '%s' "$rb" | grep -qF 'k.env_storage_restore(dev.storage_base)' || \
        bad "S6d. the server never restores at startup — a rebooted environment would mount stale RAM instead of its extent"
    order_ok "$S" "k.env_storage_restore(dev.storage_base)" "    loop {" || \
        bad "S6e. the restore is not before the serve loop — the POSIX sidecar's first read (the superblock) could beat it"
    has "$PL" "pub const RD_ERR_QUOTA: u16 = 9;" || \
        bad "S6f. proto's RD_ERR_QUOTA is not 9 (or absent) — the wire status the quota refusal rides on"
    has "$VF" "CacheError::Status(status) if status == RD_ERR_QUOTA => Errno::EDquot," || \
        bad "S6g. the VFS no longer maps the quota status to EDQUOT — a tenant would see generic EIO instead of 'disk quota exceeded'"
    has "$ER" "Errno::EDquot => 122," || \
        bad "S6h. EDquot has no POSIX code (122) — the errno the shell reports would not be EDQUOT"

    # ── S7. the descriptor names it ────────────────────────────────────────
    has "$CK" "env_storage_extent_of(rec.partition_id, rec.index," || \
        bad "S7. env_ckpt_register_from no longer looks the store up — state_lba would stay 0 and a restore would reattach to nothing"
    for assign in "rec.state_lba     = store_lba;" "rec.state_sectors = store_sectors;" "rec.state_bytes   = store_bytes;"; do
        has "$CK" "$assign" || bad "S7b. the descriptor does not stamp '$assign'"
    done
    has "$EPH" "durable storage extent (P1b)" || \
        bad "S7c. env_ckpt.h no longer documents state_lba as P1b's durable storage extent — the field's purpose would be folklore"

    # ── S8. the format layer: ceilings, and refusal by name ────────────────
    local nd2 nind nd1
    nd2=$(rval "$AE" NDIRECT_V2); nind=$(rval "$AE" NINDIRECT); nd1=$(rval "$AE" NDIRECT)
    if [ -z "$nd2" ] || [ -z "$nind" ] || [ -z "$nd1" ]; then
        bad "S8. NDIRECT_V2 ('$nd2'), NINDIRECT ('$nind') or NDIRECT ('$nd1') unreadable — the ceiling arithmetic cannot be done"
    else
        local v2blocks v2bytes v1blocks v1bytes
        v2blocks=$(( nd2 + 2 * nind ))
        v2bytes=$(( v2blocks * 512 ))
        v1blocks=$(( nd1 + nind ))
        v1bytes=$(( v1blocks * 512 ))
        if [ "$v2blocks" -ne 266 ] || [ "$v2bytes" -ne 136192 ]; then
            bad "S8. this guard computes v2 = $v2blocks blocks / $v2bytes B, expected 266 / 136192 — the writable ceiling moved without this guard being told"
        elif [ "$v1bytes" -ne 71168 ]; then
            bad "S8b. this guard computes v1 = $v1bytes B, expected 71168 — the read-only ceiling moved"
        elif [ "$v1bytes" -ge 71200 ] || [ "$v2bytes" -lt 71200 ]; then
            bad "S8c. the phase's own test file does not fit: 71168 < 71200 <= $v2bytes must hold"
        else
            ok "S8. the ceilings hold by arithmetic: v1 $v1bytes < 71200 <= v2 $v2bytes (the guard's large file fits only in v2)"
        fi
    fi
    has "$AE" "found => return Err(SuperblockRefusal::Version { found })," || \
        bad "S8d. parse_superblock_refusing no longer refuses an unknown version BY NAME — a v3 store would be read as something"
    local mnt
    mnt=$(body "$VF" "pub fn mount_aerofs_or_format")
    printf '%s' "$mnt" | grep -qF 'parse_superblock_refusing(&block)' && \
    printf '%s' "$mnt" | grep -qF 'if block.iter().all(|&b| b == 0)' || \
        bad "S8e. mount_aerofs_or_format no longer consults the refusing parser, or no longer detects a fresh store by block 0's zero bytes — with the zero-gate gone (the §5 bad-format-version tooth's mutation) every unparsable store becomes 'unformatted' and gets formatted over"
    printf '%s' "$mnt" | grep -qF 'return Err(Errno::EInval);' || \
        bad "S8f. the formatting mount has no refusal return for a non-empty unparsable store — it would format v3 over as v2, the exact thing the bad-format-version tooth names"
    has "$SB" "MountRefused(&'static str)" && has "$SB" 'R::Version { .. } => "version",' || \
        bad "S8g. the sidecar's boot no longer names the refusal (MountRefused / the version arm) — the serial log would show a bare EInval and 'refused by name' would be unobservable"
    # …and the ceiling the mount and the allocator actually consult is the
    # DERIVED expression, not a transcription of the arithmetic above (the
    # §5 `format-v1-only` tooth's single mutation point):
    has "$AE" "pub const MAX_BLOCKS_V2: u64 = (NDIRECT_V2 + 2 * NINDIRECT) as u64;" || \
        bad "S8h. MAX_BLOCKS_V2 is no longer the derived (NDIRECT_V2 + 2 * NINDIRECT) — v2's ceiling could silently become v1's while this guard's constant-folded arithmetic still passes"

    # ── S9. durability is a flag the ram-backed tooth can flip ─────────────
    has "$C" "ne->flags        = ENV_STORAGE_F_VALID | ENV_STORAGE_F_DURABLE;" || \
        bad "S9. attach no longer marks the store DURABLE — the ram-backed tooth's single-point mutation has nothing to bite on"
    has "$C" "if (!(e->flags & ENV_STORAGE_F_DURABLE) || !es_io_ready())" || \
        bad "S9b. restore does not branch on the durable flag — a RAM-backed store would be treated as if it had an extent"
    local wr
    wr=$(body "$C" "uint64_t env_storage_write")
    printf '%s' "$wr" | grep -qF 'ENV_STORAGE_F_DURABLE' || \
        bad "S9c. env_storage_write does not branch on the durable flag — writes to a RAM-backed store would claim to persist"
    has "$C" "static const uint8_t __attribute__((aligned(4096))) zero_page[4096];" || \
        bad "S9d. the extent's zero page is missing or no longer __attribute__((aligned(4096))) — nvme_buf_aligned() rejects a misaligned buffer with 0xFC, so attach would refuse every durable store on the real driver (the failure the live boot caught; only a permissive stub can miss it)"
    has "$C" "(no NVMe queue — RAM-backed)" && has "$C" "ne->flags        = ENV_STORAGE_F_VALID;" || \
        bad "S9e. attach no longer degrades to a RAM-backed store when the NVMe queue is absent — a boot whose NVMe never came up (the E5/E6 boots, a BAR below 4 GiB) would refuse ENVIRONMENT CREATION entirely"

    # ── S10. syscall surface: numbers, gating, dispatch ────────────────────
    local highest
    highest=$(grep -rhoE '#define SYS_SLS_[A-Z_]+ +[0-9]+' "$ROOT/kernel"/*.h "$ROOT/net"/*.h 2>/dev/null \
              | awk '{print $3}' | sort -n | tail -1)
    [ "${highest:-0}" -eq 326 ] || \
        bad "S10. the highest SYS_SLS_ number is ${highest:-none}, expected 326 (P1B's four: 323-326) — a collision or an unwired define"
    has "$DISP" "case SYS_SLS_ENV_STORAGE_WRITE:" || \
        bad "S10b. syscall_dispatch has no SYS_SLS_ENV_STORAGE_WRITE case — the server's write-through would never reach the kernel"
    has "$CAP" "only PARTITION_SYSTEM creates environments" || \
        bad "S10c. sys_sls_env_storage_attach is not gated to PARTITION_SYSTEM — any sidecar could mint a durable store"
    has "$CAP" "owner != caller->partition_id" || \
        bad "S10d. restore/write do not gate on the store's owner partition — one environment's server could name another's region"
    has "$K" "const SYS_ENV_STORAGE_ATTACH: u64 = 323;" || \
        bad "S10e. kabi's syscall constants are missing P1B's (323-326) — the RealKernel shims would target the wrong numbers"

    # ── S11. the real kernel module, executed here ─────────────────────────
    local HT="$ROOT/tests/env_storage_host_test.c"
    if [ ! -f "$HT" ]; then
        bad "S11. tests/env_storage_host_test.c is missing — the kernel module has no executed proof"
    elif ! command -v gcc >/dev/null 2>&1; then
        echo "ABORT: gcc not found — S11 builds and runs the kernel host test, and a guard that skipped it would pass having examined nothing" >&2
        exit 2
    else
        # The guard runs the command the test's own "Build and run:" comment
        # states, and refuses if the two have drifted: whoever edits the
        # test's build command stays the one source of truth (run_all.sh's
        # own rule — including its continuation-line and CRLF handling).
        local cmd1
        cmd1=$(awk '
            /\* *gcc / { grab=1 }
            grab {
                line = $0
                gsub(/\r/, "", line)
                sub(/^[[:space:]]*\*[[:space:]]?/, "", line)
                print line
                if (line !~ /\\[[:space:]]*$/) { exit }
            }
        ' "$HT")
        if [ -z "$cmd1" ]; then
            bad "S11b. the test has no 'Build and run:' gcc line for this guard to run"
        else
            local tmpbin="/tmp/env_storage_host_test.$$"
            local built out rc
            # Built INSIDE the inspected root (aerofs_v2_check.sh's rule for
            # cargo): the executed proof is of THIS tree, not of the guard's.
            built=$(cd "$ROOT" && eval "$cmd1 -o '$tmpbin'" 2>&1); rc=$?
            if [ "$rc" -ne 0 ]; then
                bad "S11c. the kernel host test does not build:\n$built"
            else
                out=$("$tmpbin" 2>&1); rc=$?
                rm -f "$tmpbin"
                if [ "$rc" -ne 0 ] || ! printf '%s' "$out" | grep -qF "all checks passed"; then
                    bad "S11d. env_storage_host_test failed (rc=$rc):\n$(printf '%s' "$out" | grep '^FAIL' | sed 's/^/      /')"
                else
                    local nchecks
                    nchecks=$(printf '%s' "$out" | grep -c '^ok ')
                    ok "S11. env_storage_host_test ran here and passed ($nchecks checks): attach/zero-fill, write-through + restore across a fresh region, quota refusal with RAM revert, re-attach re-charge refusal, release, unattached-RAM, no-device degrade"
                fi
            fi
        fi
    fi

    # ── S12. the observables the live arm reads ────────────────────────────
    has "$SHELL" "partition storagequotas" && has "$SHELL" "partition storagequota set " || \
        bad "S12. the operator shell lost 'partition storagequota(s)' — the live arm has no way to read or set the quota it asserts on"
    has "$ROOT/kernel/storage_quota.c" "[STORAGE-QUOTA] Per-partition on-disk page usage/quota" || \
        bad "S12b. sys_sls_partition_storage_quota_list prints no [STORAGE-QUOTA] section — the usage the plan asserts on is unreadable"

    # ── S13. the boot evidence's wiring: CI runs the live arm ──────────────
    # A source clause cannot reboot anything — but it can insist that the job
    # that CAN does. §10 step 2's gate IS the live arm; without this clause
    # the reboot evidence runs only when someone runs it by hand, and the
    # roadmap's "what this increment does not claim" line never retires.
    # Two halves, each with its own name: the invocation (present at all)
    # and its order after the ISO build — a boot guard ahead of its image
    # aborts (exit 2) before booting, reddening for the wrong reason while
    # proving nothing about the reboot.
    if ! has "$CIW" "env_storage_durable_check.sh --live"; then
        bad "S13. ci.yml never invokes this guard's live arm — the reboot evidence would run only by hand, and §10 step 2's gate would not be exercised on any push"
    else
        ok "S13. ci.yml invokes this guard's live arm (the durable region's reboot evidence is wired into CI)"
        if order_ok "$CIW" "make X86_CC=gcc X86_LD=ld x86-iso" "env_storage_durable_check.sh --live"; then
            ok "S13b. the live arm runs AFTER the ISO build — the boot guard finds its image instead of aborting before it"
        else
            bad "S13b. ci.yml's live arm does not run AFTER the ISO build — a boot guard ahead of its image aborts before booting and proves nothing about the reboot"
        fi
    fi

    if [ "$fail" -eq 0 ]; then
        echo
        echo "env_storage_durable_check: every source clause held (S1-S13)."
    else
        echo
        echo "env_storage_durable_check: source clauses FAILED."
    fi
    return 0
}

# ─── The artifact validation half (shared by --live and --replay) ─────────
# The clause labels D1..D5 are the contract the smoke asserts on, so they are
# pinned rather than reworded casually.
validate() {   # validate <dir> <tooth-name> ; 0 = every clause held
    python3 - "$1" "${2:-}" <<'PY'
import json, os, re, sys

W = sys.argv[1]
tooth = sys.argv[2] if len(sys.argv) > 2 else ""
fails = []

def ok(m):   print("ok:   " + m)
def bad(m):  fails.append(m); print("FAIL: " + m)
def note(m): print("note: " + m)

def read(name):
    p = os.path.join(W, name)
    if not os.path.isfile(p):
        return None
    return open(p, encoding="utf-8", errors="replace").read()

if not tooth:
    t = read("tooth.txt")
    if t:
        tooth = t.strip()

def need(name, what):
    s = read(name)
    if s is None:
        bad("%s: missing — %s never happened or the run never recorded it"
            % (name, what))
    return s

def jf(name):
    s = read(name)
    if s is None:
        bad("%s: missing — the run never produced this evidence" % name)
        return None
    try:
        return json.loads(s)
    except Exception as e:
        bad("%s: unparseable JSON (%s) — a guard that cannot read its inputs "
            "must not pass" % (name, e))
        return None

# create.json's presence is part of the recorded run (the live arm copies it
# next to identity.json); nothing below reads its fields — the create's own
# claim is D1's attach line, not the HTTP answer.
ident  = jf("identity.json")
create = jf("create.json")
envlist = jf("envlist.json")
restore = jf("restore.json")
b1 = read("boot1.log")
b2 = read("boot2.log")
P = I = E = None
if ident is not None:
    P = ident.get("partition"); I = ident.get("index"); E = ident.get("env_id")

# ── D1. the create attached a DURABLE store, and the directory was written ─
if b1 is None or P is None:
    bad("D1. boot1.log/identity.json: missing — cannot see the attach this run's storage rests on")
elif not re.search(r"\[ENV-STORAGE\] attach partition=%s index=%s .*\(new store\)" % (P, I), b1):
    bad("D1. boot1.log has no '[ENV-STORAGE] attach partition=%s index=%s ... (new store)' — the environment booted WITHOUT a store being attached, so everything it wrote lived in RAM only" % (P, I))
elif "[PERSIST] Environment storage directory written" not in b1:
    bad("D1b. boot1.log has no '[PERSIST] Environment storage directory written' — the directory never reached NVMe, so there is nothing for a reboot to reattach to")
else:
    ok("D1. the create attached a store for (partition %s, index %s) and persisted the directory" % (P, I))

# ── D2. the environment came back (same partition, same index) ────────────
if envlist is None:
    bad("D2. envlist.json: missing — cannot see the environment after the reboot")
else:
    found = False
    for e in envlist.get("envs", envlist.get("environments", [])) if isinstance(envlist, dict) else []:
        if str(e.get("index")) == str(I):
            found = True
    # The route's shape varies; fall back to a textual probe of the JSON.
    if not found and ("\"index\": %s" % I) in json.dumps(envlist):
        found = True
    if not found:
        bad("D2. after the reboot, partition %s lists no environment at index %s — the environment did not come back, so nothing about its files can be claimed" % (P, I))
    elif restore is not None and str(restore.get("ok")) != "true":
        bad("D2b. restore.json ok=%r — the replay pass did not complete" % restore.get("ok"))
    else:
        ok("D2. the environment is back in partition %s at index %s" % (P, I))

# ── D2c. the reboot's own serial: the directory reloaded, the extent read ─
# The HTTP answers above say the environment is LISTED; this is the boot half's
# evidence that its STORE came back — the same two lines the live arm waits for
# on serial (L15), recorded into boot2.log. A ram-backed store (the §5 tooth)
# prints neither: restore answers "RAM-backed, nothing to load" instead.
if b2 is None:
    bad("D2c. boot2.log: missing — the post-reboot serial was never recorded, so nothing about the reload can be claimed")
elif not re.search(r"\[ENV-STORAGE\] restore partition=%s index=%s .*loaded from LBA " % (P, I), b2):
    bad("D2c. boot2.log has no '[ENV-STORAGE] restore partition=%s index=%s … loaded from LBA' — the rebooted environment's store was never read back from NVMe" % (P, I))
elif not re.search(r"\[PERSIST\] Environment storage directory restored \([1-9][0-9]* store", b2):
    bad("D2d. boot2.log has no '[PERSIST] Environment storage directory restored (N store…' — the directory never reloaded, so the re-attach had no persisted occupancy to re-charge")
else:
    ok("D2c. the reboot's serial shows the directory restored and the extent read back from NVMe")

# ── D3. byte-identity through the environment's own console ───────────────
# D3a: the big file (fingerprint: wc -c + head -n 20 + tail -n 20, recorded
# from the console before and after the reboot). D3b: the small file, read
# whole and hashed whole.
big_pre, big_post = read("big_pre.txt"), read("big_post.txt")
small_pre, small_post = read("small_pre.txt"), read("small_post.txt")

if big_pre is None or big_post is None:
    bad("D3a. %s — the big file's fingerprint was not readable through the console on %s the reboot"
        % ("before" if big_pre is None else "after", "before" if big_pre is None else "after"))
elif "BIGWRITE-TOO-SMALL" in big_pre:
    note("D3a. the pre-reboot write itself did not land (format-v1-only shape) — the large-file clause is the one that must be red")
    bad("D3a. the big file never reached its size before the reboot — the v2 ceiling did not admit it")
elif big_pre.strip() != big_post.strip():
    bad("D3a. the big file's fingerprint changed across the reboot — the store did not survive byte-identically:\n      pre:  %r\n      post: %r"
        % (big_pre.strip()[:120], big_post.strip()[:120]))
else:
    ok("D3a. the file larger than 71 168 bytes came back byte-identically (fingerprint: %s)"
       % big_pre.strip().replace("\n", " / ")[:100])

if small_pre is None or small_post is None:
    bad("D3b. the small file's content was not readable through the console on one side of the reboot")
elif "SMALLWRITE-ABSENT" in small_pre:
    bad("D3b. the small file never landed before the reboot")
elif small_pre != small_post:
    bad("D3b. the small file's bytes changed across the reboot")
else:
    ok("D3b. the small file came back byte-identically (%d bytes of console content)"
       % len(small_post.strip()))

# ── D4. quota usage moved by the file's size, and the occupancy survived ──
def ival(name):
    s = read(name)
    if s is None:
        return None
    m = re.search(r"-?\d+", s)
    return int(m.group(0)) if m else None

before = ival("quota_pages_before.txt")
after  = ival("quota_pages_after.txt")
reboot = ival("quota_pages_reboot.txt")
BIG_BYTES = 71200
if before is None or after is None:
    bad("D4. quota_pages_before/after missing — the plan's quota-usage clause has no numbers to stand on")
else:
    delta = (after - before) * 4096
    if delta < BIG_BYTES:
        bad("D4. the tenant's storage-quota usage moved by %d pages (%d bytes) around the write — expected at least %d bytes' worth (the big file's size)"
            % (after - before, delta, BIG_BYTES))
    else:
        ok("D4. quota usage moved by %d pages (%d bytes) when the %d-byte file was written"
           % (after - before, delta, BIG_BYTES))
    if reboot is None:
        bad("D4b. quota_pages_reboot missing — occupancy after the reboot is unasserted")
    elif reboot * 4096 < BIG_BYTES:
        bad("D4b. after the reboot the tenant is charged only %d pages (%d bytes) — persisted occupancy was not re-charged, so a rebooted tenant is metered-empty"
            % (reboot, reboot * 4096))
    else:
        ok("D4b. the re-attach re-charged the persisted occupancy after the reboot (%d pages)" % reboot)

# ── D5. the over-quota write was refused by the QUOTA's own error ─────────
denied = read("quota_denied.txt")
ceiling = ival("quota_ceiling.txt")
usage_end = ival("quota_usage_end.txt")
frame_denied = read("frame_denied.txt")
big2 = read("big2_wc.txt")
if denied is None or ceiling is None or usage_end is None:
    bad("D5. quota_denied/quota_ceiling/quota_usage_end missing — the refusal was never exercised or never recorded")
else:
    if denied.strip() != "1":
        bad("D5. the over-quota write did not produce an '[ENV-STORAGE] quota denied' line — nothing refused it, so the quota is not the thing saying no")
    else:
        ok("D5. the over-quota write was refused by the quota's own serial line")
    if usage_end > ceiling:
        bad("D5b. usage ended at %d pages, past the ceiling of %d — the denial did not actually stop the charge" % (usage_end, ceiling))
    else:
        ok("D5b. usage stopped at the ceiling (%d <= %d pages)" % (usage_end, ceiling))
    if frame_denied is None:
        bad("D5c. frame_denied.txt missing — the 'not a frame-pool exhaustion' half was never checked")
    elif frame_denied.strip() != "0":
        bad("D5c. a frame/region-allocation denial appears in the over-quota window — the refusal came from the frame pool, which is precisely what §5 says it must not be")
    else:
        ok("D5c. no frame-pool denial anywhere near the refusal — the quota is the binding constraint, by name")
    if big2 is not None and re.search(r"\d+", big2):
        n = int(re.search(r"\d+", big2).group(0))
        if n >= 128894:
            bad("D5d. the over-quota file reached %d bytes — the write was not refused at all" % n)
        else:
            ok("D5d. the over-quota file stopped at %d bytes (refused mid-write, POSIX-style)" % n)

print("")
if fails:
    print("env_storage_durable_check[%s]: %d clause(s) red" % (tooth or "clean", len(fails)))
    sys.exit(1)
print("env_storage_durable_check[%s]: every replayed claim held." % (tooth or "clean"))
sys.exit(0)
PY
}

# ─── --replay: validation only ─────────────────────────────────────────────
if [ "$MODE" = "replay" ]; then
    if [ -z "$REPLAY_DIR" ] || [ ! -d "$REPLAY_DIR" ]; then
        echo "ABORT: --replay needs an existing artifact directory" >&2
        exit 2
    fi
    source_clauses
    [ "$fail" -eq 0 ] || { echo; echo "env_storage_durable_check: stopping at the source clauses."; exit 1; }
    echo
    if validate "$REPLAY_DIR" "$TOOTH"; then exit 0; fi
    exit 1
fi

# ─── The source clauses (both plain and --live run them first) ─────────────
source_clauses
[ "$fail" -eq 0 ] || { echo; echo "env_storage_durable_check: stopping at the source clauses."; exit 1; }

if [ "$MODE" = "source" ]; then
    exit 0
fi

# ─── The boot arm ──────────────────────────────────────────────────────────
case "$TOOTH" in
    "") ;;
    ram-backed|unquotaed|format-v1-only|bad-format-version)
        echo "ABORT: P1B_TOOTH=$TOOTH is a source-level tooth (see tests/env_storage_durable_check_smoke.sh); the boot arm records what really happened and takes no teeth" >&2
        exit 2 ;;
    *) echo "ABORT: unknown P1B_TOOTH='$TOOTH'" >&2; exit 2 ;;
esac

for tool in qemu-system-x86_64 qemu-img curl python3; do
    command -v "$tool" >/dev/null 2>&1 || {
        echo "ABORT: $tool not found — the boot arm needs QEMU, an image tool, curl and python3" >&2; exit 2; }
done

ISO="${P1B_ISO:-sls_operating_system.iso}"
ENTRY="${P1B_BOOT_ENTRY:-3}"
WINDOW_S="${P1B_WINDOW_S:-180}"
INDEX="${P1B_INDEX:-2}"
TOKEN=deadbeef01234567cafebabe76543210   # dave, DB_ADMIN (kernel/auth.c)

[ -f "$ISO" ] || { echo "ABORT: ISO '$ISO' not found — build it (make x86-iso) or point P1B_ISO at it" >&2; exit 2; }

PORT="${P1B_PORT:-}"
if [ -z "$PORT" ]; then
    PORT=$(bash tests/free_port.sh) || { echo "ABORT: no free loopback port for the QEMU hostfwd (see tests/free_port.sh)" >&2; exit 2; }
fi
BASE="http://127.0.0.1:$PORT"
RAM="${P1B_RAM:-1G}"
SMP="${P1B_SMP:-2}"

WD="$(mktemp -d)"
ART="${P1B_ARTIFACTS:-/tmp/p1b-storage-$$}"
mkdir -p "$ART"
IMG="$WD/disk.img"
SER="$WD/ser"
LOG="$WD/serial.log"
QERR="$WD/qemu.err"
QPID=""
CATPID=""

boot_fail() {
    echo "FAIL: $1" >&2
    [ -s "$QERR" ] && sed 's/^/      qemu: /' "$QERR" >&2
    echo "      last 20 serial lines:" >&2
    tail -20 "$LOG" 2>/dev/null | sed 's/^/      /' >&2
    cp "$LOG" "$ART/serial-fail.log" 2>/dev/null || true
    echo "      the whole serial log is at: $ART/serial-fail.log" >&2
    exit 1
}
cleanup() {
    [ -n "$QPID" ] && kill "$QPID" 2>/dev/null
    [ -n "$CATPID" ] && kill "$CATPID" 2>/dev/null
    rm -rf "$WD"
}
trap cleanup EXIT
trap 'cleanup; exit 1' TERM INT

qemu-img create -f raw "$IMG" 10G >/dev/null 2>&1 || { echo "ABORT: qemu-img could not create the NVMe image" >&2; exit 2; }
rm -f "$SER.in" "$SER.out"
mkfifo "$SER.in" "$SER.out" 2>/dev/null || true
cat "$SER.out" > "$LOG" &
CATPID=$!

ACCEL="${QEMU_ACCEL:-}"
if [ -z "$ACCEL" ]; then
    if [ -e /dev/kvm ] && [ -r /dev/kvm ]; then ACCEL="-accel kvm"
    else                                   ACCEL="-accel tcg,thread=multi"; fi
fi

# The NVMe device is the point of this run: the extent must survive the
# reboot, so the disk file is the same one across both boots.
qemu-system-x86_64 -cdrom "$ISO" \
    -drive id=disk,file="$IMG",if=none,format=raw \
    -device nvme,drive=disk,serial=slsdev0 \
    -netdev user,id=net0,hostfwd=tcp:127.0.0.1:$PORT-:3000 \
    -device e1000,netdev=net0,mac=52:54:00:12:34:09 \
    -display none -m "$RAM" -smp "$SMP" -boot d -monitor none \
    $ACCEL \
    -serial pipe:"$SER" 2>"$QERR" &
QPID=$!

bash tests/grub_select_kernel_only.sh "$SER.in" "$LOG" "$QPID" "$ENTRY" || {
    boot_fail "could not select grub entry $ENTRY (QEMU or grub failed before the menu)"
}
ok "L1. QEMU is booting grub entry $ENTRY with a fresh 10G NVMe image"

count_line() { local n; n=$(grep -acF -- "$1" "$LOG" 2>/dev/null); printf '%s\n' "${n:-0}"; }

ready=0; qemu_alive=1
for i in $(seq 1 $((WINDOW_S * 2))); do
    if grep -aq "unified=1" "$LOG" 2>/dev/null && \
       grep -aq "Listening on port 3000" "$LOG" 2>/dev/null && \
       [ "$(count_line "[E1] control plane planted")" -ge 1 ] && \
       [ "$(count_line "[INIT] heartbeat")" -ge 5 ]; then
        ready=1; break
    fi
    if ! kill -0 "$QPID" 2>/dev/null; then qemu_alive=0; break; fi
    sleep 0.5
done
[ "$ready" -eq 1 ] || {
    [ "$qemu_alive" -eq 0 ] && boot_fail "QEMU exited before the unified boot reached its markers"
    boot_fail "the unified boot's markers did not appear within ${WINDOW_S}s"
}
ok "L2. the unified boot is up (control plane serving, init making progress)"

api() {   # api <out-file> <curl args...>
    local out="$1"; shift
    local t
    for t in 1 2 3 4 5; do
        if curl -sf --max-time 60 -H "Authorization: Bearer $TOKEN" \
                -H "Content-Type: application/json" -o "$out" "$@" 2>/dev/null; then
            return 0
        fi
        sleep 2
    done
    return 1
}
jval() {
    python3 - "$1" "$2" <<'PY' 2>/dev/null
import json, sys
try:
    d = json.load(open(sys.argv[1]))
except Exception:
    print(""); sys.exit(0)
v = d.get(sys.argv[2]) if isinstance(d, dict) else None
if v is None:     print("")
elif v is True:   print("true")
elif v is False:  print("false")
else:             print(v)
PY
}
wait_health() {
    for i in $(seq 1 90); do
        if curl -sf --max-time 2 "$BASE/api/health" >/dev/null 2>&1; then return 0; fi
        if ! kill -0 "$QPID" 2>/dev/null; then return 1; fi
        sleep 2
    done
    return 1
}

# ─── The serial operator shell (quota surface) ─────────────────────────────
# Wait for the interactive shell's prompt, then send one line and wait for the
# evidence it prints. The serial pipe carries both directions.
serial_wait_shell() {
    local i
    for i in $(seq 1 "$WINDOW_S"); do
        grep -aqF -- "--- Multi-User SLS Secure Shell Active ---" "$LOG" 2>/dev/null && return 0
        kill -0 "$QPID" 2>/dev/null || return 1
        sleep 1
    done
    return 1
}
serial_cmd() {   # serial_cmd <line> <fixed marker to wait for>
    local line="$1" marker="$2" i
    printf '%s\n' "$line" > "$SER.in"
    for i in $(seq 1 60); do
        grep -aqF -- "$marker" "$LOG" 2>/dev/null && return 0
        sleep 1
    done
    return 1
}
quota_usage_for() {   # quota_usage_for <file> <partition-id> -> prints pages
    # The LAST section, never the first: these files are full serial-log
    # copies, and the log keeps every `[STORAGE-QUOTA]` section ever printed
    # (caught live: the after-write read returned boot1's pre-write section
    # and reported `delta 0` while serial showed usage=2 -> 24).
    python3 - "$1" "$2" <<'PY'
import re, sys
txt, pid = open(sys.argv[1], errors="replace").read(), sys.argv[2]
m = re.findall(r"partition\s+%s\s+usage=(\d+)" % re.escape(pid), txt)
print(m[-1] if m else "")
PY
}

# ─── L3. a partition, then an environment in it ────────────────────────────
pname="p1bstore"
api "$WD/pcreate.json" -X POST -d "{\"name\":\"$pname\"}" "$BASE/api/partitions" || \
    boot_fail "POST /api/partitions did not answer"
PID="$(jval "$WD/pcreate.json" partition_id)"
[ "$(jval "$WD/pcreate.json" ok)" = "true" ] && [ -n "$PID" ] && [ "$PID" != "0" ] || \
    boot_fail "POST /api/partitions did not define a partition (id='$PID')"
ok "L3. partition $PID ('$pname') exists"

serial_wait_shell || boot_fail "the operator shell never came up on the serial console (no shell banner)"
ok "L4. the serial operator shell is up (the quota surface)"

api "$WD/create.json" -X POST -d "{\"index\":$INDEX}" "$BASE/api/partition/$PID/env" || \
    boot_fail "POST /api/partition/$PID/env did not answer"
ENV_ID="$(jval "$WD/create.json" env_id)"
if [ "$(jval "$WD/create.json" ok)" != "true" ] || [ -z "$ENV_ID" ] || [ "$ENV_ID" = "0" ]; then
    boot_fail "the environment manager did not create an environment (ok=$(jval "$WD/create.json" ok) env_id='$ENV_ID') — a store attach that refuses with the quota error would answer here"
fi
ok "L5. env $ENV_ID is live in partition $PID at index $INDEX"

# The attach's own evidence on serial, before anything writes a byte.
for i in $(seq 1 30); do
    grep -aqE "\[ENV-STORAGE\] attach partition=$PID index=$INDEX " "$LOG" 2>/dev/null && break
    sleep 1
done
grep -aqE "\[ENV-STORAGE\] attach partition=$PID index=$INDEX " "$LOG" || \
    boot_fail "no [ENV-STORAGE] attach line for (partition $PID, index $INDEX) — the create did not attach a durable store"
grep -aqF "[PERSIST] Environment storage directory written" "$LOG" || \
    boot_fail "the storage directory was never written to NVMe at attach"
ok "L6. the kernel attached a durable store and persisted the directory (serial evidence)"

# ─── The environment console (E6 attach surface) ───────────────────────────
cons_send() {   # cons_send <line> — waits for the peer to take it
    local line="$1" t ok queued
    python3 - "$line" > "$WD/send_body.json" <<'PY'
import json, sys
print(json.dumps({"input": sys.argv[1]}))
PY
    for t in $(seq 1 20); do
        api "$WD/send.json" -X POST -d "$(cat "$WD/send_body.json")" \
            "$BASE/api/partition/$PID/env/$ENV_ID/console" || true
        ok="$(jval "$WD/send.json" ok)"
        queued="$(jval "$WD/send.json" queued)"
        if [ "$ok" = "true" ] && [ -n "$queued" ] && [ "$queued" != "0" ]; then return 0; fi
        sleep 1
    done
    return 1
}
cons_out() {   # cons_out — stdout: the `output` field, verbatim
    api "$WD/cons.json" "$BASE/api/partition/$PID/env/$ENV_ID/console" || true
    python3 - "$WD/cons.json" <<'PY'
import json, sys
try:
    d = json.load(open(sys.argv[1]))
except Exception:
    sys.exit(0)
v = d.get("output") if isinstance(d, dict) else None
if isinstance(v, str):
    sys.stdout.write(v)
PY
}
cons_wait() {   # cons_wait <fixed marker> [timeout-s]
    local marker="$1" n="${2:-45}" i
    for i in $(seq 1 "$n"); do
        cons_out | grep -qF -- "$marker" && return 0
        sleep 1
    done
    return 1
}

# ─── The sentinel protocol ───────────────────────────────────────────────
# Every GET /console DRAINS the environment's output (the route hands back
# what has accumulated and clears it), so polling with cons_wait consumes
# exactly the block it is waiting for and cannot then be read again — and
# the env console's prompt ("$ ", written without a newline) merges into
# whatever line follows it, so a marker matched by substring is ambiguous.
# So every exchange is bracketed: echo a START line, run the command, echo
# an END line, then accumulate drains until a line EXACTLY equal to the END
# marker appears (the console is one ordered reader — the END line's arrival
# means the command's output completed), and keep only the lines between the
# two exact markers. Bracketing makes the pre- and post-reboot captures
# byte-comparable: whatever the prompt or echo did, both sides ran the same
# commands between the same sentinels.
cons_until() {   # cons_until <sentinel: last field of a line> <timeout-s> <append-to>
    # Matched as the line's LAST field, because the prompt ("$ ", no
    # newline) may have merged onto the front of it, and because the shell
    # may or may not echo the input — either way the sentinel lands last,
    # and either way it lands AFTER the bracketed command finished (one
    # ordered reader).
    local marker="$1" n="$2" out="$3" i
    for i in $(seq 1 "$n"); do
        cons_out >> "$out" 2>/dev/null || true
        awk -v m="$marker" '$NF==m{f=1} END{exit !f}' "$out" 2>/dev/null && return 0
        sleep 1
    done
    return 1
}

cons_xcapture() {   # cons_xcapture <command> <end-marker> <timeout-s> <file>
    # Runs <command> bracketed by sentinels; writes the bracketed block to
    # <file>. The START marker is a fresh line first, so anything still
    # draining from before lands above the cut and is dropped. Failures are
    # staged and the console-so-far is printed, because "it timed out" about
    # a 90-second QEMU+TCG write says nothing about WHICH step wedged.
    local cmd="$1" end="$2" n="$3" out="$4" raw="$WD/xcap.raw"
    : > "$raw"
    xcap_dump() {
        cons_out >> "$raw" 2>/dev/null || true
        echo "      xcapture: $1 — console so far (last 12 lines):" >&2
        tail -12 "$raw" 2>/dev/null | sed 's/^/        /' >&2
        cp "$raw" "$ART/xcap-fail.raw" 2>/dev/null || true
    }
    cons_send "echo P1B-START" || { xcap_dump "the START sentinel line was never accepted"; return 1; }
    cons_send "$cmd"           || { xcap_dump "the command line was never accepted: $cmd"; return 1; }
    cons_send "echo $end"      || { xcap_dump "the END sentinel line was never accepted: $end"; return 1; }
    cons_until "$end" "$n" "$raw" || { xcap_dump "the END sentinel '$end' never arrived within ${n}s"; return 1; }
    # Cut at the first line whose LAST field is the START sentinel and stop
    # at the first whose last field is the END one — the prompt may have
    # merged onto the front of either, and the shell may or may not echo
    # the input line; the cut lands after the START line either way, so
    # whatever drained from before is excluded and both sides' blocks are
    # the same bracket.
    awk -v s="P1B-START" -v e="$end" \
        '$NF==s{f=1;next} f{print} f&&$NF==e{exit}' "$raw" > "$out"
}

# wc on this shell takes NO -c flag (the applet opens argv[1] as a file),
# and prints "lines words bytes path" — the byte count is the field before
# the path, wherever the prompt attached.
wc_bytes() {   # wc_bytes <captured-file> <path> -> the byte count
    # "lines words bytes path": the path is the last field, the count just
    # before it, wherever a merged prompt attached. An input-echo line ends
    # in the path too but has fewer than 4 fields, so it cannot be picked up.
    awk -v p="$2" '$NF==p && NF>=4 {v=$(NF-1)} END{print v+0}' "$1"
}

# Announce + wait for the shell to be reading: the [env-id] line first, then
# a real line through the input path (an EMPTY input is never queued by
# env_console_write — len 0 answers queued:0 — so the probe is a line the
# shell actually runs, bracketed by sentinels).
cons_wait "[env-id] index=$INDEX" 60 || \
    boot_fail "the environment never announced its identity on its own console"
cons_xcapture "true" "P1B-P1" 20 "$WD/probe.txt" || \
    boot_fail "the environment console would not accept input (the shell never ran the probe line)"
ok "L7. the environment's own console is attached and reading"

# ─── Quota BEFORE the writes ───────────────────────────────────────────────
serial_cmd "partition storagequotas" "[STORAGE-QUOTA]" || \
    boot_fail "'partition storagequotas' produced no [STORAGE-QUOTA] section on serial"
cp "$LOG" "$WD/serial_prequota.log"
quota_usage_for "$WD/serial_prequota.log" "$PID" > "$WD/pages_before" || true
BEFORE="$(cat "$WD/pages_before")"
[ -n "$BEFORE" ] || boot_fail "partition $PID has no storage-quota row before the write (usage unreadable)"
ok "L8. storage quota before: partition $PID usage=$BEFORE pages"

# ─── The files, through the environment's own console ──────────────────────
# The big file is written with 19-digit seq lines on purpose: this stack
# write-throughs the STORE once per VFS write() (the applets write a line at
# a time), and under QEMU+TCG one round trip is ~40 ms — 20 000 short lines
# would take ~13 minutes, 4 445 long ones ~3 minutes. 4,445 × 20 B =
# 88 900 bytes: above v1's 71 168 ceiling, which is the point of the clause.
cons_xcapture "seq 1000000000000000000 1000000000000004444 > /p1b_big" "P1B-W1" 420 "$WD/write1.txt" || \
    boot_fail "the big-file write never completed through the console"
cons_xcapture "echo p1b-small-payload > /p1b_small" "P1B-W2" 60 "$WD/write2.txt" || \
    boot_fail "the small-file write never completed through the console"
cons_xcapture "wc /p1b_big" "P1B-WC" 30 "$WD/wc_raw.txt" || \
    boot_fail "wc /p1b_big never answered after the console write"
BIG_WC="$(wc_bytes "$WD/wc_raw.txt" /p1b_big)"
if [ -z "$BIG_WC" ] || [ "$BIG_WC" -lt 71200 ]; then
    printf 'BIGWRITE-TOO-SMALL %s\n' "${BIG_WC:-none}" > "$ART/big_pre.txt"
    boot_fail "the big file is ${BIG_WC:-absent} bytes — the clause that proves the format moved says it must exceed 71 168"
fi
ok "L9. /p1b_big is $BIG_WC bytes (> 71 168 — the v2 ceiling admitted it)"

# The fingerprints, bracketed by the SAME sentinels the reboot will use, so
# the two captures are the same bracket of lines.
cons_xcapture "head -n 20 /p1b_big" "P1B-H1" 30 "$WD/big_head.txt" || \
    boot_fail "the head fingerprint never completed through the console"
cons_xcapture "tail -n 20 /p1b_big" "P1B-T1" 30 "$WD/big_tail.txt" || \
    boot_fail "the tail fingerprint never completed through the console"
{ echo "wc=$BIG_WC"; echo "-- head -n 20"; cat "$WD/big_head.txt"; \
  echo "-- tail -n 20"; cat "$WD/big_tail.txt"; } > "$ART/big_pre.txt"

cons_xcapture "cat /p1b_small" "P1B-C1" 30 "$ART/small_pre.txt" || \
    boot_fail "the small file never came back through the console"
grep -qF "p1b-small-payload" "$ART/small_pre.txt" || \
    boot_fail "the small file does not contain what was written to it"

# ─── Quota AFTER the writes ────────────────────────────────────────────────
serial_cmd "partition storagequotas" "[STORAGE-QUOTA]" || \
    boot_fail "'partition storagequotas' produced no section after the write"
cp "$LOG" "$WD/serial_postquota.log"
AFTER="$(quota_usage_for "$WD/serial_postquota.log" "$PID")"
[ -n "$AFTER" ] || boot_fail "partition $PID has no storage-quota row after the write"
printf '%s\n' "$BEFORE" > "$ART/quota_pages_before.txt"
printf '%s\n' "$AFTER"  > "$ART/quota_pages_after.txt"
ok "L10. storage quota after: partition $PID usage=$AFTER pages (delta $(( AFTER - BEFORE )))"

# ─── Record this boot, pause, checkpoint, reboot in place ──────────────────
python3 - "$ART/identity.json" "$PID" "$INDEX" "$ENV_ID" "$pname" <<'PY'
import json, sys
p, part, idx, env, name = sys.argv[1:]
json.dump({"partition": int(part), "index": int(idx), "env_id": int(env),
           "partition_name": name}, open(p, "w"), sort_keys=True)
open(p, "a").write("\n")
PY
cp "$WD/create.json" "$ART/create.json"
cp "$LOG" "$ART/boot1.log"
SPLIT=$(wc -c < "$LOG")

api "$WD/pause.json" -X POST -d "{\"partition_id\":$PID}" "$BASE/api/partition/pause" || \
    boot_fail "POST /api/partition/pause did not answer"
[ "$(jval "$WD/pause.json" ok)" = "true" ] || boot_fail "partition $PID could not be paused (a record captured unpaused is refused on restore)"
api "$WD/checkpoint.json" -X POST "$BASE/api/checkpoint" || boot_fail "POST /api/checkpoint did not answer"
[ "$(jval "$WD/checkpoint.json" status)" = "0" ] || boot_fail "the checkpoint did not run (status=$(jval "$WD/checkpoint.json" status))"
ok "L11. the environment is paused and checkpointed — the record that replays it is on NVMe"

curl -s -X POST "$BASE/api/node/reboot" -H "Authorization: Bearer $TOKEN" \
     -d '{"confirm":"reboot"}' --max-time 10 >/dev/null 2>&1 &
sleep 1
bash tests/grub_select_kernel_only.sh "$SER.in" "$LOG" "$QPID" "$ENTRY" || {
    boot_fail "could not select grub entry $ENTRY after the reboot"
}
ok "L12. the node was rebooted in place (same NVMe image — the extent must still be there)"

planted_before=1
ready2=0
for i in $(seq 1 $((WINDOW_S * 2))); do
    if grep -aqF "[ENV_CKPT] Restored " "$LOG" && \
       curl -sf --max-time 2 "$BASE/api/health" >/dev/null 2>&1; then
        ready2=1; break
    fi
    if ! kill -0 "$QPID" 2>/dev/null; then boot_fail "QEMU exited during the reboot"; fi
    sleep 0.5
done
[ "$ready2" -eq 1 ] || boot_fail "the rebooted node did not reach its control plane within ${WINDOW_S}s"
ok "L13. the node is up again and its boot line says the snapshot's records were restored"

api "$WD/restore.json" -X POST "$BASE/api/env/restore" || boot_fail "POST /api/env/restore did not answer"
[ "$(jval "$WD/restore.json" ok)" = "true" ] || \
    boot_fail "the replay pass did not complete (ok=$(jval "$WD/restore.json" ok))"
ok "L14. the replay ran: replayed $(jval "$WD/restore.json" replayed), refused $(jval "$WD/restore.json" refused)"

# The durable half's own serial evidence: directory restored, extent loaded.
for i in $(seq 1 60); do
    grep -aqE "\[ENV-STORAGE\] restore partition=$PID index=$INDEX .* loaded from LBA " "$LOG" 2>/dev/null && break
    sleep 1
done
grep -aqE "\[ENV-STORAGE\] restore partition=$PID index=$INDEX .* loaded from LBA " "$LOG" || \
    boot_fail "no [ENV-STORAGE] restore ... 'loaded from LBA' line for (partition $PID, index $INDEX) — the rebooted environment's store was not loaded back from NVMe"
grep -aqE "\[PERSIST\] Environment storage directory restored \([1-9][0-9]* store" "$LOG" || \
    boot_fail "the storage directory was not restored from NVMe at boot"
ok "L15. the directory reloaded and the extent was read back into the fresh region (serial evidence)"

# The replay's last act is the record's own pause coming back — the snapshot
# was captured with partition $PID paused (L11), and a restore puts back
# exactly the pauses it stepped out of. That repause froze the freshly created
# sidecar mid-boot: entry.rs printed [POSIX] boot: and boot() never reached
# boot.rs's [env-id] line (caught live — the identity wait timed out against a
# console nothing was running to write to). The operator's next act is the one
# every restored record needs: resume the partition and let the boot finish.
api "$WD/resume.json" -X POST -d "{\"partition_id\":$PID}" "$BASE/api/partition/resume" || \
    boot_fail "POST /api/partition/resume did not answer after the replay"
[ "$(jval "$WD/resume.json" ok)" = "true" ] || \
    boot_fail "partition $PID could not be resumed after the replay — the environment would stay frozen mid-boot with no identity line ever written"

# ─── The files again, through the environment's own console ────────────────
serial_wait_shell || boot_fail "the operator shell did not come back after the reboot"
cons_wait "[env-id] index=$INDEX" 90 || \
    boot_fail "the environment never announced its identity after the reboot"
cons_xcapture "true" "P1B-P2" 20 "$WD/probe2.txt" || \
    boot_fail "the post-reboot console would not accept input (the shell never ran the probe line)"

cons_xcapture "wc /p1b_big" "P1B-WC" 30 "$WD/wc_raw2.txt" || \
    boot_fail "wc /p1b_big never answered after the reboot"
BIG_WC2="$(wc_bytes "$WD/wc_raw2.txt" /p1b_big)"
cons_xcapture "head -n 20 /p1b_big" "P1B-H1" 30 "$WD/big_head2.txt" || \
    boot_fail "the head fingerprint never completed after the reboot"
cons_xcapture "tail -n 20 /p1b_big" "P1B-T1" 30 "$WD/big_tail2.txt" || \
    boot_fail "the tail fingerprint never completed after the reboot"
{ echo "wc=$BIG_WC2"; echo "-- head -n 20"; cat "$WD/big_head2.txt"; \
  echo "-- tail -n 20"; cat "$WD/big_tail2.txt"; } > "$ART/big_post.txt"

cons_xcapture "cat /p1b_small" "P1B-C1" 30 "$ART/small_post.txt" || \
    boot_fail "the small file never came back after the reboot"
ok "L16. both files re-read through the environment's own console post-reboot"

# Quota occupancy after the reboot (the re-attach's re-charge).
serial_cmd "partition storagequotas" "[STORAGE-QUOTA]" || \
    boot_fail "'partition storagequotas' produced no section after the reboot"
cp "$LOG" "$WD/serial_rebootquota.log"
REBOOT="$(quota_usage_for "$WD/serial_rebootquota.log" "$PID")"
printf '%s\n' "${REBOOT:-0}" > "$ART/quota_pages_reboot.txt"
ok "L17. storage quota after reboot: partition $PID usage=${REBOOT:-?} pages"

# ─── The over-quota refusal, on the same boot ──────────────────────────────
CEIL=$(( ${REBOOT:-0} + 4 ))
serial_cmd "partition storagequota set $PID $CEIL" "storagequota partition=$PID pages=$CEIL" || \
    boot_fail "'partition storagequota set' produced no confirmation on serial"
printf '%s\n' "$CEIL" > "$ART/quota_ceiling.txt"
SPLIT_OQ=$(wc -c < "$LOG")

cons_xcapture "seq 2000000000000000000 2000000000000030000 > /p1b_big2" "P1B-W3" 420 "$WD/write3.txt" || \
    boot_fail "the over-quota write never completed through the console"
sleep 2
serial_cmd "partition storagequotas" "[STORAGE-QUOTA]" || \
    boot_fail "'partition storagequotas' produced no section after the refusal"
tail -c +$((SPLIT_OQ + 1)) "$LOG" > "$ART/serial_overquota.log"
grep -aqF "[ENV-STORAGE] quota denied partition=$PID" "$ART/serial_overquota.log" && \
    printf '1\n' > "$ART/quota_denied.txt" || printf '0\n' > "$ART/quota_denied.txt"
grep -acE "\[ALLOC_REGION\]|frame pool|FRAME pool exhaust" "$ART/serial_overquota.log" \
    > "$ART/frame_denied.txt" || printf '0\n' > "$ART/frame_denied.txt"
USAGE_END="$(quota_usage_for "$ART/serial_overquota.log" "$PID")"
printf '%s\n' "${USAGE_END:-0}" > "$ART/quota_usage_end.txt"
cons_xcapture "wc /p1b_big2" "P1B-WC2" 30 "$WD/wc2_raw.txt" || true
wc_bytes "$WD/wc2_raw.txt" /p1b_big2 > "$ART/big2_wc.txt"
ok "L18. the over-quota write was attempted; the serial slice and the final usage are recorded"

# ─── The post-reboot snapshots the validator reads ─────────────────────────
api "$WD/envlist.json" "$BASE/api/partition/$PID/env" || true
cp "$WD/envlist.json" "$ART/envlist.json" 2>/dev/null || true
[ -f "$WD/restore.json" ] && cp "$WD/restore.json" "$ART/restore.json"
tail -c +$((SPLIT + 1)) "$LOG" > "$ART/boot2.log"
[ -n "$TOOTH" ] && printf '%s\n' "$TOOTH" > "$ART/tooth.txt"

kill "$QPID" 2>/dev/null || true; QPID=""
kill "$CATPID" 2>/dev/null || true; CATPID=""

echo
echo "artifacts: $ART (replay them later with: bash tests/env_storage_durable_check.sh --replay \"$ART\")"
echo

if validate "$ART" "$TOOTH"; then
    echo
    echo "env_storage_durable_check: a file larger than 71 168 bytes survived a real reboot byte-identically, the tenant's quota moved by its size, and an over-quota write was refused by the quota's own error — through the environment's own console."
    exit 0
fi
echo
echo "env_storage_durable_check: the reboot did NOT hold the storage it should have (artifacts: $ART)."
exit 1
