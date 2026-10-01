#!/usr/bin/env bash
# tests/env_checkpoint_restore_check.sh — guards POSIX-Environments v0.2 Phase
# P1a's RESTORE side: the source wiring of restore-through-create, and — with
# --live — the round trip itself: create an environment, checkpoint the node,
# reboot against the same NVMe image, and assert the environment is back in the
# same partition with the pause state the snapshot recorded.
#
# ─── Modes ─────────────────────────────────────────────────────────────────
#   (no arguments)     the SOURCE clauses over this script's own repository root
#   --live             the source clauses, then the boot arm (QEMU + the ISO)
#   --replay DIR       only the artifact validation of a recorded run (no QEMU,
#                      no build) — how the smokes prove the boot clauses' teeth
#   positional ROOT    the source clauses over another tree (the smoke's own
#                      hermetic seam; every real caller passes nothing)
#
# Live knobs (all optional; the defaults are what every other caller gets):
#   P1A_ISO          the ISO to boot                  (default sls_operating_system.iso)
#   P1A_BOOT_ENTRY   1-based grub MENU POSITION       (default 3 = the unified entry)
#   P1A_WINDOW_S     seconds to wait for the markers  (default 180)
#   P1A_PORT         the QEMU hostfwd port            (default tests/free_port.sh)
#   P1A_RAM / P1A_SMP                                 (default 1G / 2 — see below)
#   P1A_ARTIFACTS    where the recorded evidence lands (default a temp dir,
#                    printed at the end so it can be --replay'd later)
#   P1A_TOOTH=no-restore  withhold the replay and require the LIVENESS clause
#                    to go red (the vacuity control the roadmap's tooth names)
#
# ─── Why this guard exists ────────────────────────────────────────────────
# tests/env_ckpt_check.sh guards the *capture* wiring ("is the record layer
# reachable from a boot, and does the writer keep its promises"). This guard is
# the other half, and it exists for the reason the third increment's addendum
# names: an environment record that is written, read back, validated — and then
# never replayed — is not a restore. It is a file that survives a reboot.
#
# The roadmap (docs/AeroSLS-POSIX-Environments-Roadmap-v0.2.md §4) names this
# check and its four teeth: `P1A_TOOTH=no-restore`, `stale-descriptor`,
# `leak-unpause` and `dirty-capture`. Each tooth is one of the invariants
# below, and tests/env_checkpoint_restore_check_smoke.sh makes each one bite by
# breaking exactly that property in a throwaway copy of the tree.
#
# ─── The two halves, and the one thing this guard will not claim ───────────
#   * The DESCRIPTOR half — source clauses T1–T6, and the live round trip. The
#     live arm proves, across a real reboot against the same NVMe image, that:
#     the first boot is COLD for the environment region (a reused image would
#     make the restore unattributable), the create registered a record, the
#     checkpoint wrote it, the operator's pause was recorded and came back at
#     the next boot, the replay ran through the manager's own create and
#     reproduced the identity, the environment is LIVE again in the same
#     partition (its console bound, its POSIX sidecar carrying the partition),
#     and the operator's pause was put back after the create.
#   * The PAYLOAD half does not exist yet. The sidecars' page tables, mapped
#     frames, register save areas and the console's buffered bytes are not
#     captured, so the environment that comes back is EMPTY: a file written
#     before the checkpoint is gone. The guard says that out loud (a `note:`
#     line) instead of asserting a byte it cannot produce, and the clause that
#     will assert it — a file's bytes surviving, a shell variable set before
#     the reboot visible after it — is deliberately NOT written here yet.
#
# `P1A_TOOTH=no-restore` is the vacuity control for the live arm: with the
# replay withheld the environment must NOT come back, so the liveness clause
# (B8) must go red and every other clause must stay green. Today that is the
# honest shape of the tooth — with no payload, the DESCRIPTOR *is* the restore,
# so withholding it takes the environment's liveness red. When the payload
# lands, B8 stays green under this tooth (an empty environment is still an
# environment) and the CONTENTS clauses become the ones that go red.
#
# ─── The source invariants, and what breaks if one is dropped ─────────────
#   A.  Preconditions (else exit 2 — the guard could not be evaluated at all).
#   T1. no-restore: the replay EXISTS, is REACHABLE from a boot (the HTTP route
#       registers it and calls it), goes through the EXISTING create path (a
#       second create path is the drift this project deletes on sight), counts
#       a replay only after a create acknowledged, arms itself from
#       after_restore() — so a boot that adopted nothing has nothing pending —
#       and REPORTS what it did, including `pending` and `remaining`, which is
#       what keeps "came back empty" from being described as "restored".
#   T2. stale-descriptor: the record the create produced is compared with the
#       one that was adopted (identity — partition, index, task names/kinds,
#       region kinds and frame counts), and a mismatch is refused with its own
#       reason AND the environment the create made is destroyed: refusal over
#       partial application, so a refused restore leaves no environment behind.
#   T3. leak-unpause: a partition the snapshot left paused is resumed for the
#       create (the create path refuses a paused target) and re-paused
#       afterwards — from the pass's own ledger, never "every partition with a
#       record" — with no `return` between the resume and the re-pause, so a
#       failed restore cannot leave a partition an operator had frozen RUNNING.
#       The capture's own interval (quiesce → commit → release, no early
#       return) and the restore's ordering (pauses re-applied AFTER the live
#       set is settled) are re-asserted here because the pause is this guard's
#       subject.
#   T4. dirty-capture: a record captured WITHOUT a quiesce is refused, not
#       replayed — unless it names PARTITION_SYSTEM, the one partition the
#       capture never freezes. The gate lives in env_ckpt_restore_next(), so no
#       caller can forget it; the refusal has a code, a rendering, a counter,
#       and it clears the record from the pending set (a refusal that left the
#       record pending would be an infinite pass).
#   T5. env_service.c's new dependency on the record layer is satisfiable:
#       every host test that links kernel/env_service.c links kernel/env_ckpt.c
#       too, and has the partition stand-ins env_ckpt.c needs.
#   T6. Every entry point this guard asserts on is exercised by
#       tests/env_ckpt_host_test.c: an API with no observed behaviour is a
#       claim, not a feature.
#
# ─── The live clauses, in the order the run produces them ────────────────
#   L1  a fresh 10G NVMe image, grub entry 3 (the unified boot), cold for the
#       environment region;  L2 the control plane is up;  L3 a partition;
#   L4 an environment in it at P1A_INDEX;  L5 the partition admin-paused;
#   L6 the checkpoint writes the region (the [PERSIST] line, not just status 0);
#   L7 reboot IN PLACE, same disk, same grub entry;  L8 the control plane is
#       back and the boot says it restored the snapshot's records;  L9 the
#       recorded pause is back BEFORE any replay;  L10 the replay runs (or is
#       withheld, under P1A_TOOTH=no-restore).
# The validator then reads the recorded artifacts as B1..B9 in the same order,
# ending with B8 (liveness: the console registry AND the process table agree)
# and B9 (the pause the pass stepped out of was put back).
#
# ─── What the FIRST live runs of this arm had to be taught ───────────────
# Written down because each one presented as a restore bug and was not one:
#   1. `make` does not track headers, so checkpoint_delta.x86.o predated
#      CKPT_REGION_ENV and its ckpt_mark_all_dirty() cleared region 17 on every
#      full checkpoint (`dirty=0x1ffff` — one bit short). See the Makefile.
#   2. `make x86-iso` ships the CHECKED-IN sidecars.cpio and does not rebuild
#      it, so the ISO carried an init with no ENV_REGISTER arm and the kernel
#      refused a 28-byte answer to a 216-byte registration. The archive is now
#      stamped with the digest of the user/ sources it was packed from
#      (tools/sidecar_source_digest.sh) AND with the sha256 of its own bytes
#      (tools/sidecar_archive_digest.sh), the two being projections of one
#      pack-run record (tools/sidecar_stamp_record.sh, sidecars.cpio.stamps),
#      and `make x86-iso` REFUSES to ship one
#      that matches neither (or whose stamps and record are not one run's) —
#      the second stamp is what catches a repack or a swap done out of band,
#      with user/ untouched, and the record is what catches a stamp refreshed
#      on its own. This arm notes a mismatched
#      stamp (or record) before it boots, for the run that points P1A_ISO at
#      an older image anyway.
#   3. GET /api/partitions did not expose the pause state at all, and
#      GET /api/partition/{id}/env puts the partition at the TOP level of the
#      response, not on each entry. B4 had no field to read and B8 read an
#      invented one — both caught only by running the arm, never by a
#      hand-written fixture.
#
# GUARD-KIND: host (plain bash over the tree; --replay adds python3). NOT
# `build`, deliberately: neither the default mode nor --replay can abort for a
# missing build artefact, so an rc=2 from either is rot and must fail-closed —
# which is exactly what markers-other-than-runtime/build get in run_checks.sh.
# The third mode, --live, does need sls_operating_system.iso and QEMU; being
# opt-in is what lets the smoke prove the boot clauses' teeth on every push
# without a build.
#
# Exit: 0 all clauses hold, 1 one failed, 2 precondition missing.
set -u

ROOT=""
LIVE=0
REPLAY=""
while [ $# -gt 0 ]; do
    case "$1" in
        --live)   LIVE=1; shift ;;
        --replay) REPLAY="${2:-}"; shift 2 ;;
        -*)       echo "ABORT: unknown argument '$1' (usage: $0 [ROOT] [--live] [--replay DIR])" >&2; exit 2 ;;
        *)        ROOT="$1"; shift ;;
    esac
done
ROOT="${ROOT:-$(cd "$(dirname "$0")/.." && pwd)}"

fail=0
ok()   { echo "ok:   $*"; }
bad()  { echo "FAIL: $*"; fail=1; }
note() { echo "note: $*"; }

# has <file> <fixed string> -> 0 when present
has() { grep -qF -- "$2" "$1" 2>/dev/null; }

# line_of <text> <fixed string> -> the 1-based line number of the first match.
# Fixed-string on purpose: every anchor below is source text, not a pattern.
line_of() { printf '%s\n' "$1" | grep -nF -- "$2" | head -1 | cut -d: -f1; }

# body <file> <literal first line> -> the function, up to the first line that is
# exactly "}" at column 0 (the shape every function in these files has).
# index() rather than a regex: the literals carry `*`, `(`, `)` and `{`.
body() {
    awk -v pat="$2" 'index($0, pat) == 1 { grab = 1 }
                     grab { print; if ($0 == "}") exit }' "$1"
}

# ─── The artifact validation half. Shared by the live run and --replay. ─────
# One python pass over a run's evidence; every failure is a line, and a missing
# or unparseable artifact is a failure too — a guard that cannot read its
# inputs must not pass. The clause labels (B1..B9) are the contract the smoke
# asserts on, so they are pinned rather than reworded casually.
validate() {   # validate <dir> <tooth-name> ; 0 = every clause held
    python3 - "$1" "${2:-}" <<'PY'
import json, os, re, sys

W    = sys.argv[1]
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

# A recorded run is self-describing: a tooth run writes tooth.txt next to its
# evidence, so --replay knows which arm it is looking at even when the caller
# does not pass the tooth along. (A caller that does pass it wins: that is how
# the live run and the smoke name the arm explicitly.)
if not tooth:
    t = read("tooth.txt")
    if t:
        tooth = t.strip()

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

ident   = jf("identity.json")
create  = jf("create.json")
pause   = jf("pause.json")
chk     = jf("checkpoint.json")
pre     = jf("partitions_pre.json")
post    = jf("partitions_post.json")
envlist = jf("envlist.json")
procs   = jf("processes.json")
b1 = read("boot1.log")
b2 = read("boot2.log")

P = I = E = None
if ident is not None:
    P = ident.get("partition"); I = ident.get("index"); E = ident.get("env_id")

# ── B1. the first boot is COLD for the environment region, on real NVMe ─────
if b1 is None:
    bad("B1. boot1.log: missing — the cold boot's own evidence is not in the set")
elif "no snapshot" not in b1:
    bad("B1. boot1.log has no '[ENV_CKPT] no snapshot': the first boot was not "
        "cold for the environment region, so an environment seen after the "
        "reboot could have come from an older snapshot than this run's")
elif re.search(r"MMIO above 4 GiB|NVMe unavailable", b1):
    bad("B1. boot1.log shows the NVMe stack did not come up (the BAR landed "
        "above 4 GiB): the persistence path cold-started and the round trip "
        "never happened")
else:
    ok("B1. the first boot read no environment snapshot and reached the NVMe "
       "(a reused disk would make the reboot's restore unattributable)")

# ── B2. the create registered a record for the identity the run asserts ─────
if b1 is None or P is None:
    bad("B2. boot1.log/identity.json: missing — cannot tie the record to an identity")
elif ("[ENV] checkpoint record registered: env %d (partition %d, index %d)"
      % (E, P, I)) not in b1:
    bad("B2. boot1.log has no registration line for env %s (partition %s, index "
        "%s): the environment existed without a record, so there was nothing to "
        "checkpoint" % (E, P, I))
else:
    ok("B2. the create registered the record the checkpoint would write "
       "(env %s, partition %s, index %s)" % (E, P, I))

# ── B3. the checkpoint wrote it ─────────────────────────────────────────────
written = None
if chk is None or str(chk.get("status")) != "0":
    bad("B3. checkpoint.json status=%r — expected 0; the checkpoint did not run "
        "and nothing was written to NVMe" % (chk.get("status") if chk else None))
elif b1 is None:
    bad("B3. boot1.log: missing — cannot see the capture's own report")
else:
    m = re.search(r"\[PERSIST\] Environment checkpoint snapshot written "
                  r"\((\d+) live record\(s\), (\d+) frozen, (\d+) dropped\)\.", b1)
    if not m:
        bad("B3. boot1.log has no '[PERSIST] Environment checkpoint snapshot "
            "written' line: the region was never written, so the reboot could "
            "not restore anything")
    elif int(m.group(1)) < 1 or int(m.group(3)) != 0:
        bad("B3. the capture wrote %s live record(s) with %s dropped — expected "
            "at least 1 live and 0 dropped" % (m.group(1), m.group(3)))
    else:
        ok("B3. the checkpoint wrote the region (%s live record(s), %s frozen, "
           "0 dropped)" % (m.group(1), m.group(2)))

# ── B4. the operator's pause went in with the snapshot and came back ────────
if pause is None or str(pause.get("ok")) != "true":
    bad("B4. pause.json ok=%r — the partition was never admin-paused before the "
        "checkpoint, so the run cannot say anything about a pause being restored"
        % (pause.get("ok") if pause else None))
elif pre is None:
    bad("B4. partitions_pre.json: missing — cannot see the pause state the "
        "snapshot came back with")
else:
    row = None
    for p in pre.get("partitions", []):
        if str(p.get("id")) == str(P):
            row = p
            break
    if row is None:
        bad("B4. the restored partition table has no partition %s at all — the "
            "partition did not survive the reboot, so nothing could land in it" % P)
    elif str(row.get("paused")) not in ("true", "1", "True"):
        bad("B4. partition %s came back RUNNING after the reboot, but it was "
            "admin-paused when the checkpoint ran: the recorded pause was not "
            "replayed" % P)
    else:
        ok("B4. the partition is paused again after the reboot — the pause the "
           "snapshot recorded came back with the snapshot (before any replay)")

# ── B5. the snapshot's records were adopted, and the pause re-applied ───────
if b2 is None:
    bad("B5. boot2.log: missing — the reboot's own evidence is not in the set")
else:
    m = re.search(r"\[ENV_CKPT\] Restored (\d+) environment checkpoint "
                  r"record\(s\) \((\d+) partition\(s\) re-paused\)\.", b2)
    if not m:
        bad("B5. boot2.log has no '[ENV_CKPT] Restored ...' line: the rebooted "
            "kernel adopted no environment record, so there was nothing to replay")
    elif int(m.group(1)) < 1 or int(m.group(2)) != 1:
        bad("B5. the reboot restored %s record(s) and re-paused %s partition(s) "
            "— expected at least 1 record and exactly 1 re-paused partition (the "
            "one the operator had paused)" % (m.group(1), m.group(2)))
    else:
        ok("B5. the reboot adopted the record and re-applied the recorded pause "
           "(%s record(s), %s partition(s) re-paused)" % (m.group(1), m.group(2)))

# ── B6. the replay went through create and reproduced the identity ──────────
# B6 is a REPLAY claim, so under the no-restore tooth there is no replay to have
# a report — a note, exactly as B7 and B9 are notes. Reading "the replay's own
# line is missing" as a failure here would make the tooth take TWO clauses red
# and destroy the thing the tooth is for: attributability. The first live tooth
# run did exactly that (B6 and B8), which is why this is a note and why the
# smoke's tooth fixture now strips the replay lines the way a real tooth run
# does.
if tooth == "no-restore":
    note("B6. P1A_TOOTH=no-restore: the replay was withheld, so there is no "
         "identity-intact line and no claim to check")
elif b2 is None or P is None:
    bad("B6. boot2.log/identity.json: missing — cannot see the replay's own report")
else:
    replayed_line = re.search(
        r"\[ENV_RESTORE\] record \(partition (\d+), index (\d+)\) replayed "
        r"through create: env (\d+) -> (\d+), identity intact", b2)
    if replayed_line is None:
        bad("B6. boot2.log has no '[ENV_RESTORE] record (partition %s, index %s) "
            "replayed through create ... identity intact' line: either the replay "
            "never ran or it did not re-register the environment" % (P, I))
    elif (int(replayed_line.group(1)), int(replayed_line.group(2))) != (P, I):
        bad("B6. the replay reports (partition %s, index %s), not the (partition "
            "%s, index %s) that was checkpointed" % (replayed_line.group(1),
            replayed_line.group(2), P, I))
    elif "came back as a different environment" in b2:
        bad("B6. the replay refused an environment for its identity — the "
            "create did not reproduce what was written down")
    else:
        ok("B6. the replay went through the manager's create and re-registered "
           "env %s as env %s under the same (partition %s, index %s)"
           % (replayed_line.group(3), replayed_line.group(4), P, I))

# ── B7. the pass reported itself, and refused nothing ───────────────────────
res = None
if tooth == "no-restore":
    note("B7. P1A_TOOTH=no-restore withheld the replay on purpose — there is no "
         "restore.json to read, and B8 is the clause that must go red")
else:
    res = jf("restore.json")
    if res is not None:
        if str(res.get("ok")) != "true":
            bad("B7. restore.json ok=%r — the replay pass did not complete"
                % res.get("ok"))
        elif int(res.get("replayed", 0)) < 1 or int(res.get("refused", -1)) != 0 \
                or int(res.get("remaining", -1)) != 0:
            bad("B7. the pass reported replayed=%s refused=%s remaining=%s — "
                "expected at least 1 replay, 0 refused and nothing still pending"
                % (res.get("replayed"), res.get("refused"), res.get("remaining")))
        else:
            ok("B7. the replay pass reported %s replayed, %s refused, %s still "
               "pending (pending at entry: %s)"
               % (res.get("replayed"), res.get("refused"), res.get("remaining"),
                  res.get("pending")))

# ── B8. THE LIVENESS CLAUSE: the environment is back in the same partition ──
# This is the clause P1A_TOOTH=no-restore exists to trip: with the replay
# withheld, an environment that "comes back" would mean the guard was reading
# something other than a restore. Two independent surfaces must agree — the
# console registry says an environment with this (partition, index) is live and
# bound, and the process table says its POSIX sidecar carries that partition —
# so a listing that merely repeats the request cannot satisfy this clause.
# GET /api/partition/{id}/env is PARTITION-SCOPED, so the partition is the
# route's own key at the TOP level of the response (`"partition": N`), and each
# entry carries the environment's (index, env_id) inside it -- there is no
# per-entry partition field, and there should not be. The clause reads the two
# halves where the route actually puts them: "same partition" is the response's
# `partition`, "same environment" is an entry with this index and a bound
# env_id. (The first live run of this guard was red on exactly the invented
# per-entry field -- which is the class of mistake only a boot arm catches,
# since a hand-written fixture happily carries whatever it was told to.)
sidecar = "aerosls.posix.%s" % (I,)
console_live = envlist is not None and \
    str(envlist.get("partition")) == str(P) and any(
        str(e.get("index")) == str(I) and
        str(e.get("env_id")) not in ("0", "None", "")
        for e in envlist.get("envs", []))
side_live = procs is not None and any(
    p.get("name") == sidecar and str(p.get("partition_id")) == str(P)
    for p in procs.get("processes", []))
if tooth == "no-restore":
    bad("B8. P1A_TOOTH=no-restore: the replay was withheld, so the environment is "
        "NOT live again after the reboot — that is the tooth working, and this is "
        "the clause that must go red for it (with a payload it would stay green, "
        "because an empty environment is still an environment)")
elif console_live and side_live:
    ok("B8. the environment is LIVE again in partition %s at index %s: its "
       "console is bound again and its POSIX sidecar carries the partition it "
       "was checkpointed in" % (P, I))
elif not console_live and not side_live:
    bad("B8. the environment is NOT live again in partition %s at index %s: no "
        "console is bound for it and no %s process carries the partition — "
        "whatever the replay reported, no environment came back" % (P, I, sidecar))
elif not console_live:
    bad("B8. env %s (partition %s, index %s) does not appear as a live, bound "
        "console after the replay: the environment came back without an address"
        % (E, P, I))
else:
    bad("B8. no process named %s carries partition %s: the environment's POSIX "
        "sidecar is not running where it was checkpointed" % (sidecar, P))

# ── B9. the pause the replay stepped out of was put back ────────────────────
if tooth == "no-restore":
    note("B9. P1A_TOOTH=no-restore: the pass never ran, so it never owed a pause "
         "back (no claim to check)")
elif res is None:
    bad("B9. restore.json: missing — cannot see whether the pause was put back")
elif int(res.get("resumed", -1)) != 1 or int(res.get("repaused", -1)) != 1:
    bad("B9. the pass reported resumed=%s repaused=%s — the create cannot be "
        "placed into a paused partition, so it must resume exactly one partition "
        "and put exactly that one back"
        % (res.get("resumed"), res.get("repaused")))
elif post is None:
    bad("B9. partitions_post.json: missing — cannot see the pause state after "
        "the replay")
else:
    row = None
    for p in post.get("partitions", []):
        if str(p.get("id")) == str(P):
            row = p
            break
    if row is None or str(row.get("paused")) not in ("true", "1", "True"):
        bad("B9. partition %s is RUNNING after the replay — the pass resumed it "
            "for the create and did not put the operator's pause back (the "
            "leak-unpause failure, on the restore side)" % P)
    else:
        ok("B9. the partition is paused again after the replay: the pass stepped "
           "out of the pause for the create and put exactly that pause back")

print("note: this pass restored the environment's IDENTITY, not its contents. The")
print("note: payload (the sidecars' frames, their page tables, the console's bytes)")
print("note: is not captured yet, so a file written before the checkpoint is gone —")
print("note: the clause that will assert it arrives with the payload increment.")
sys.exit(1 if fails else 0)
PY
}

# ─── Replay mode: validate recorded evidence, no boot, no build. ────────────
if [ -n "$REPLAY" ]; then
    if [ ! -d "$REPLAY" ]; then
        echo "ABORT: --replay dir '$REPLAY' is not a directory" >&2
        exit 2
    fi
    command -v python3 >/dev/null 2>&1 || {
        echo "ABORT: python3 not found — the artifact validation runs in python." >&2
        exit 2
    }
    if validate "$REPLAY" "${P1A_TOOTH:-}"; then
        echo
        echo "env_checkpoint_restore_check: the recorded reboot held the descriptor-level restore."
        exit 0
    fi
    echo
    echo "env_checkpoint_restore_check: the recorded reboot did NOT hold the restore it claims."
    exit 1
fi

# ═══ The source clauses (see the header's invariant list) ═══════════════════
cd "$ROOT"

# ── A. Preconditions ──────────────────────────────────────────────────────
for f in kernel/env_ckpt.h kernel/env_ckpt.c kernel/env_service.h \
         kernel/env_service.c kernel/persist.c net/http.c \
         tests/env_ckpt_host_test.c tests/partition_host_stubs.h; do
    if [ ! -f "$f" ]; then
        echo "ABORT: $f is missing — cannot evaluate the P1a restore path at all" >&2
        exit 2
    fi
done
ok "A. every file the restore-through-create path lives in is present"

drv=$(body kernel/env_service.c "int env_service_restore_pending(struct EnvSvcRestoreReport* out) {")
nextb=$(body kernel/env_ckpt.c "int env_ckpt_restore_next(uint32_t* partition, uint32_t* index, uint32_t* env_id) {")
adm=$(body kernel/env_ckpt.c "int env_ckpt_restore_admissible(const struct EnvCkptRecord* rec) {")
ident=$(body kernel/env_ckpt.c "int env_ckpt_restore_same_identity(const struct EnvCkptRecord* want,")
repb=$(body kernel/env_ckpt.c "uint32_t env_ckpt_restore_repause(void) {")
resb=$(body kernel/env_ckpt.c "int env_ckpt_restore_resume_for_create(uint32_t partition_id) {")
refb=$(body kernel/env_ckpt.c "void env_ckpt_restore_refuse(uint32_t partition_id, uint32_t index,")
after=$(body kernel/env_ckpt.c "void env_ckpt_after_restore(void) {")

if [ -z "$drv" ]; then
    bad "A. env_service_restore_pending() is not defined in env_service.c — the restore would have no driver"
fi
if ! has kernel/env_service.h "int env_service_restore_pending(struct EnvSvcRestoreReport* out);"; then
    bad "A. env_service.h does not declare env_service_restore_pending() — a driver nothing can call is a file, not a feature"
fi

# ── T1. no-restore: the replay exists, is reachable, and creates ───────────
# The driver must go through the SAME create path the create route uses, and it
# must not count a replay before that create acknowledged: a pass that reported
# replays it did not make is the failure this tooth names.
if [ -n "$drv" ]; then
    missing=""
    case "$drv" in *"env_service_create(p, idx, &status, &new_id)"*) ;; *) missing="$missing create";; esac
    case "$drv" in *"env_ckpt_restore_next(&p, &idx, &old_id)"*) ;; *) missing="$missing next";; esac
    case "$drv" in *"env_ckpt_restore_settle(p, idx)"*) ;; *) missing="$missing settle";; esac
    if printf '%s' "$drv" | grep -qF "cap_send_msg(" ; then
        missing="$missing second-create-path"
    fi
    if [ -n "$missing" ]; then
        bad "T1. env_service_restore_pending() is missing:$missing — a restore must replay a record through the environment manager's existing create (env_service_create), and a driver that speaks the channel itself has grown the second create path v0.2 §2 forbids"
    else
        cr_line=$(line_of "$drv" "env_service_create(p, idx, &status, &new_id)")
        rp_line=$(line_of "$drv" "out->n_replayed++;")
        if [ -z "${cr_line:-}" ] || [ -z "${rp_line:-}" ]; then
            bad "T1. could not locate the driver's create or its replay counter — the guard's own anchors have moved"
        elif [ "$rp_line" -le "$cr_line" ]; then
            bad "T1. the pass counts a replay (line $rp_line) before the create it claims (line $cr_line) — a pass that counts creates it did not make is the no-restore tooth's failure wearing a report's name"
        else
            ok "T1. the pass replays each record through env_service_create() (line $cr_line) and counts it as replayed only after that create returns (line $rp_line)"
        fi
    fi
else
    bad "T1. there is no env_service_restore_pending() body to check (see A)"
fi

# Reachable from a boot: a route calls it, and the route is registered.
if ! has net/http.c '"/api/env/restore"'; then
    bad "T1. net/http.c does not register POST /api/env/restore — the replay would be correct and called from nowhere, the exact class of failure persist.c's own comments record twice"
elif ! has net/http.c "env_service_restore_pending(&rep)"; then
    bad "T1. the /api/env/restore handler does not call env_service_restore_pending() — the route would answer without restoring anything"
else
    ok "T1. POST /api/env/restore calls the replay, so the restore is reachable from a boot"
fi

# The pass reports what it did, including what it did NOT do: `pending` and
# `remaining` are what make "nothing to restore" distinguishable from
# "everything restored" — the vacuity control the roadmap's tooth reads.
if has net/http.c 'jb_uint(&j,"replayed", rep.n_replayed)' && \
   has net/http.c 'jb_uint(&j,"remaining", rep.n_remaining)'; then
    ok "T1. the handler reports replayed/refused/remaining, so a pass that restored nothing cannot be read as one that restored everything"
else
    bad "T1. the /api/env/restore response does not report what the pass did — a restore that replays nothing and one that replays everything would look alike"
fi

# Armed by the restore, not by chance: after_restore() is what makes the
# adopted set the pending set.
if [ -n "$after" ] && printf '%s' "$after" | grep -qF "env_ckpt_restore_begin();"; then
    ok "T1. env_ckpt_after_restore() arms the pending set, so what is replayed is exactly what the snapshot adopted"
else
    bad "T1. env_ckpt_after_restore() does not arm the replay set — nothing would ever be pending, and the route would report a clean pass forever"
fi

# ── T2. stale-descriptor: identity is checked, and a mismatch is undone ────
if [ -n "$drv" ]; then
    same_line=$(line_of "$drv" "if (!env_ckpt_restore_same_identity(&w, env_ckpt_find(p, idx))) {")
    keep_line=$(line_of "$drv" "struct EnvCkptRecord w = *want;")
    if [ -z "${same_line:-}" ]; then
        bad "T2. the pass never compares the environment the create produced with the record it adopted — a stale descriptor would be adopted as if it were this environment"
    elif [ -z "${keep_line:-}" ] || [ "$keep_line" -ge "$same_line" ]; then
        bad "T2. the pass does not keep the ADOPTED record (a copy taken before the create overwrites it), so there is nothing to compare a stale descriptor against"
    elif ! has kernel/env_ckpt.h "#define ENV_CKPT_REFUSE_IDENTITY"; then
        bad "T2. ENV_CKPT_REFUSE_IDENTITY is not defined, so an identity mismatch has no reason a guard or an operator can read"
    else
        # Containment: the mismatch block must both record the refusal and END
        # the environment the create just made. Refusing without undoing would
        # leave a running environment nobody can account for — the half-restore
        # this phase refuses.
        blk=$(printf '%s\n' "$drv" | tail -n +"$same_line" | awk '{ print; if ($0 == "        }") exit }')
        missing=""
        case "$blk" in *"ENV_CKPT_REFUSE_IDENTITY"*) ;; *) missing="$missing refusal-reason";; esac
        case "$blk" in *"env_service_destroy("*) ;; *) missing="$missing undo";; esac
        if [ -n "$missing" ]; then
            bad "T2. the identity-mismatch arm is missing:$missing — a create that came back as a different environment must be refused with its reason AND destroyed, or the refused restore leaves an environment behind (refusal over partial application)"
        else
            ok "T2. an identity mismatch is refused with its own reason and the environment the create made is destroyed (a refused restore leaves nothing behind)"
        fi
    fi
else
    bad "T2. there is no driver body to check (see A)"
fi

# The comparison itself: identity, not placement.
if [ -n "$ident" ]; then
    missing=""
    case "$ident" in *"want->partition_id != got->partition_id"*) ;; *) missing="$missing partition";; esac
    case "$ident" in *"want->index        != got->index"*) ;; *) missing="$missing index";; esac
    case "$ident" in *"want->tasks[i].kind != got->tasks[i].kind"*) ;; *) missing="$missing task-kind";; esac
    case "$ident" in *"want->tasks[i].name[c] != got->tasks[i].name[c]"*) ;; *) missing="$missing task-name";; esac
    case "$ident" in *"want->regions[i].kind   != got->regions[i].kind"*) ;; *) missing="$missing region-kind";; esac
    case "$ident" in *"want->regions[i].frames != got->regions[i].frames"*) ;; *) missing="$missing region-size";; esac
    if [ -n "$missing" ]; then
        bad "T2. env_ckpt_restore_same_identity() no longer compares:$missing — corners of the environment's identity that a create does not legitimately re-decide (a different sidecar name, a swapped region kind and a resized heap are each a different environment)"
    else
        ok "T2. identity compares partition, index, task names and kinds, and the region kinds and sizes — and deliberately not the per-boot bases, channels, env_id, console key or pids"
    fi
else
    bad "T2. env_ckpt_restore_same_identity() is not defined in env_ckpt.c"
fi

# ── T3. leak-unpause: the pause is stepped out of, and put back ────────────
if [ -n "$drv" ]; then
    res_line=$(line_of "$drv" "int can = env_ckpt_restore_resume_for_create(p);")
    cr_line2=$(line_of "$drv" "env_service_create(p, idx, &status, &new_id)")
    rep_line=$(line_of "$drv" "out->n_repaused  = env_ckpt_restore_repause();")
    loop_line=$(line_of "$drv" "while (env_ckpt_restore_pending() > 0) {")
    if [ -z "${res_line:-}" ] || [ -z "${rep_line:-}" ]; then
        bad "T3. the pass does not step out of a restored pause and/or never puts it back — a partition the snapshot left paused cannot even be created into (the create path refuses a paused target)"
    elif [ -z "${cr_line2:-}" ] || [ -z "${loop_line:-}" ]; then
        bad "T3. could not locate the pass's loop or its create — the guard's own anchors have moved"
    elif [ "$res_line" -ge "$cr_line2" ]; then
        bad "T3. the pass resumes the target after it has already issued the create (resume line $res_line, create line $cr_line2) — the create would be refused by the paused-partition gate, and the restore would fail for a reason that says nothing about the environment"
    elif [ "$rep_line" -le "$cr_line2" ]; then
        bad "T3. the pass puts the pause back before the create (repause line $rep_line, create line $cr_line2) — the create would then be refused by the pause the pass just restored"
    else
        returns_between=$(printf '%s\n' "$drv" | sed -n "${res_line},${rep_line}p" | grep -cE "^[[:space:]]*(return|goto)")
        if [ "$returns_between" -ne 0 ]; then
            bad "T3. $returns_between return(s) sit between the resume (line $res_line) and the repause (line $rep_line) — a pass that bailed out there leaves a partition an operator had frozen RUNNING (v0.2 §4's leak-unpause tooth, on the restore side)"
        else
            ok "T3. the pass resumes a restored pause for the create (line $res_line) and re-pauses the pass's ledger afterwards (line $rep_line) on a single straight-line exit"
        fi
    fi
else
    bad "T3. there is no driver body to check (see A)"
fi

# The ledger is the thing that gets put back — never "every partition that has
# a record". This is rule 1's mirror: a capture does not resume what it did not
# pause, and a restore does not re-pause what it did not resume.
if [ -n "$repb" ] && [ -n "$resb" ]; then
    missing=""
    case "$resb" in *"if (!partition_exists(partition_id)) return -1;"*) ;; *) missing="$missing exists";; esac
    case "$resb" in *"if (!partition_is_paused(partition_id)) return 0;"*) ;; *) missing="$missing is-paused";; esac
    case "$resb" in *"partition_resume(partition_id)"*) ;; *) missing="$missing resume";; esac
    case "$repb" in *"ec_restore_resumed[i]"*) ;; *) missing="$missing ledger";; esac
    case "$repb" in *"partition_pause(p)"*) ;; *) missing="$missing repause";; esac
    if [ -n "$missing" ]; then
        bad "T3. the pause dance is incomplete:$missing — only what was actually resumed may be put back, and a partition that is gone or already running again must be left alone (a cleanup path must not be the thing that faults)"
    else
        ok "T3. env_ckpt_restore_resume_for_create() resumes only a paused, existing partition and env_ckpt_restore_repause() re-pauses exactly its ledger"
    fi
else
    bad "T3. the resume/repause pair is not both defined in env_ckpt.c"
fi

# The capture's half of the same contract, re-asserted where the pause lives:
# quiesce before the write, release after the commit, no early return between.
wbody=$(body kernel/persist.c "void persist_environments(void) {")
if [ -n "$wbody" ]; then
    q_line=$(line_of "$wbody" "env_ckpt_quiesce_for_capture(&q);")
    rr_line=$(line_of "$wbody" "env_ckpt_release_capture(&q);")
    cc_line=$(line_of "$wbody" "persist_region_commit();")
    if [ -z "${q_line:-}" ] || [ -z "${rr_line:-}" ] || [ -z "${cc_line:-}" ]; then
        bad "T3. persist_environments() no longer has its quiesce/release interval — the capture this restore replays would describe a moving target"
    elif [ "$q_line" -ge "$cc_line" ] || [ "$rr_line" -le "$cc_line" ]; then
        bad "T3. the capture's interval is inverted (quiesce $q_line, commit $cc_line, release $rr_line): it must freeze before it writes and thaw only after the snapshot is on disk"
    elif [ "$(printf '%s\n' "$wbody" | tail -n +"$q_line" | grep -cE '^[[:space:]]*return')" -ne 0 ]; then
        bad "T3. a return sits between the capture's quiesce and its release — a failed capture would leave a tenant frozen with nothing on disk to show for it (the leak-unpause tooth on the capture side)"
    else
        ok "T3. the capture still freezes before it stages (line $q_line), commits (line $cc_line) and thaws (line $rr_line) with no early return, so both sides of the pause keep the same contract"
    fi
fi

# Restore ordering: the pause comes back AFTER the live set is settled.
restore=$(body kernel/persist.c "void persist_restore_all(void) {")
if [ -n "$restore" ]; then
    ar_line=$(line_of "$restore" "env_ckpt_after_restore();")
    ap_line=$(line_of "$restore" "env_ckpt_apply_restored_pauses();")
    if [ -z "${ap_line:-}" ]; then
        bad "T3. the restore never calls env_ckpt_apply_restored_pauses() — a partition the snapshot recorded as paused comes back RUNNING"
    elif [ -z "${ar_line:-}" ] || [ "$ap_line" -le "$ar_line" ]; then
        bad "T3. the snapshot's pauses are re-applied before after_restore() settles the live set (line $ar_line) — the pass would act on records that did not survive compaction"
    else
        ok "T3. the snapshot's pauses are re-applied (line $ap_line) after the live set is settled (line $ar_line)"
    fi
else
    bad "T3. persist_restore_all() is not defined in kernel/persist.c"
fi

# ── T4. dirty-capture: a record the capture never froze is not replayed ────
if [ -n "$adm" ] && [ -n "$nextb" ]; then
    missing=""
    case "$adm" in *"!(rec->flags & ENV_CKPT_FLAG_QUIESCED)"*) ;; *) missing="$missing quiesced-flag";; esac
    case "$adm" in *"rec->partition_id != PARTITION_SYSTEM"*) ;; *) missing="$missing system-exemption";; esac
    case "$adm" in *"ENV_CKPT_REFUSE_UNQUIESCED"*) ;; *) missing="$missing unquiesced";; esac
    case "$adm" in *"!partition_exists(rec->partition_id)"*) ;; *) missing="$missing partition-gone";; esac
    case "$adm" in *"ENV_CKPT_REFUSE_NO_PARTITION"*) ;; *) missing="$missing no-partition";; esac
    # The gate must be APPLIED by the hand-out, not offered as advice: a caller
    # that forgot it would replay a moving target's descriptor.
    case "$nextb" in *"env_ckpt_restore_admissible(rec)"*) ;; *) missing="$missing gate-not-applied";; esac
    if [ -n "$missing" ]; then
        bad "T4. the replay's admissibility gate is incomplete:$missing — a record captured without a quiesce (except PARTITION_SYSTEM's, which the capture never freezes) describes a moving target and must be refused BY env_ckpt_restore_next(), so no caller can forget it"
    elif ! has kernel/env_ckpt.h "#define ENV_CKPT_REFUSE_UNQUIESCED" || \
         ! has kernel/env_ckpt.h "#define ENV_CKPT_REFUSE_NO_PARTITION"; then
        bad "T4. the restore-side refusal codes are not both defined in env_ckpt.h"
    elif ! has kernel/env_ckpt.c "case ENV_CKPT_REFUSE_UNQUIESCED:" || \
         ! has kernel/env_ckpt.c "case ENV_CKPT_REFUSE_NO_PARTITION:" || \
         ! has kernel/env_ckpt.c "case ENV_CKPT_REFUSE_IDENTITY:"; then
        bad "T4. a restore refusal has a code but no rendering — the serial transcript would say nothing about why an environment did not come back"
    elif ! has kernel/env_ckpt.c "ec_restore_refused_count++"; then
        bad "T4. a refused replay is not counted — a pass that refused everything would report the same thing as one that had nothing to do"
    elif [ -z "$refb" ] || ! printf '%s' "$refb" | grep -qF "ec_restore_clear_pending("; then
        bad "T4. env_ckpt_restore_refuse() does not clear the record from the pending set — the driver's loop would hand the same refused record out forever"
    else
        ok "T4. the hand-out refuses an unquiesced or partition-less record with its own code, rendering and counter, and the refusal clears it from the pending set"
    fi
else
    bad "T4. the admissibility gate and/or env_ckpt_restore_next() are not defined in env_ckpt.c"
fi

# ── T5. env_service.c's new dependency is satisfiable where it is linked ───
missing_link=""
for t in tests/*_host_test.c; do
    cmd=$(awk '
        /[*] *gcc / { grab=1 }
        grab {
            line = $0
            gsub(/\r/, "", line)
            sub(/^[[:space:]]*\*[[:space:]]?/, "", line)
            print line
            if (line !~ /[\\][[:space:]]*$/) { exit }
        }
    ' "$t")
    case "$cmd" in
        *"kernel/env_service.c"*)
            case "$cmd" in
                *"kernel/env_ckpt.c"*) ;;
                *) missing_link="$missing_link $(basename "$t")" ;;
            esac
            case "$cmd" in
                *"kernel/partition.c"*) ;;
                *) grep -qF "tests/partition_host_stubs.h" "$t" || \
                       missing_link="$missing_link $(basename "$t")" ;;
            esac ;;
    esac
done
if [ -n "$missing_link" ]; then
    bad "T5. these host tests link kernel/env_service.c without what the record layer it now calls needs (kernel/env_ckpt.c, and the partition stand-ins when kernel/partition.c is not linked), so they fail at LINK time on env_ckpt_restore_next()/_settle():$missing_link"
else
    ok "T5. every host test that links env_service.c can resolve the record layer and the partition primitives it now uses"
fi

# ── T6. Every entry point this guard asserts on is driven by the host test ─
missing_test=""
for sym in env_ckpt_restore_begin env_ckpt_restore_next env_ckpt_restore_settle \
           env_ckpt_restore_refuse env_ckpt_restore_admissible \
           env_ckpt_restore_same_identity env_ckpt_restore_resume_for_create \
           env_ckpt_restore_repause env_ckpt_restore_pending env_ckpt_restore_refused \
           env_ckpt_restore_resumed; do
    has tests/env_ckpt_host_test.c "$sym" || missing_test="$missing_test $sym"
done
if [ -n "$missing_test" ]; then
    bad "T6. tests/env_ckpt_host_test.c never exercises:$missing_test — an entry point this guard asserts on that no test drives is a claim, not a feature"
else
    ok "T6. every restore entry point this guard asserts on is exercised against the real kernel/env_ckpt.c"
fi

# ─── Default mode ends here: the source clauses. ───────────────────────────
if [ "$LIVE" -ne 1 ]; then
    if [ "$fail" -ne 0 ]; then
        echo
        echo "env_checkpoint_restore_check: the P1a restore path does not do what it is written to do."
        exit 1
    fi
    echo
    echo "env_checkpoint_restore_check: the P1a restore-through-create path is wired and its four refusals are in place."
    exit 0
fi

# ═══ The boot arm: the round trip, on the real target ══════════════════════
# Everything below is one boot of the unified entry, one checkpoint, one
# in-place reboot against the SAME NVMe image, and one replay — the property
# the phase exists for, measured instead of designed.
ISO="${P1A_ISO:-sls_operating_system.iso}"
ENTRY="${P1A_BOOT_ENTRY:-3}"          # 3 = the unified entry (grub menu order)
WINDOW_S="${P1A_WINDOW_S:-180}"
# -m 1G is deliberate, not a default: at 4G the NVMe device's 64-bit BAR lands
# above 4 GiB, outside the kernel's identity map, and the whole persistence
# stack honestly cold-starts ("[NVME] MMIO above 4 GiB") — which would make
# every run here a no-op-shaped pass (the pitfall tests/tcache_roundtrip_check.sh
# documents at length, and B1 asserts against).
RAM="${P1A_RAM:-1G}"
SMP="${P1A_SMP:-2}"
TOOTH="${P1A_TOOTH:-}"
INDEX="${P1A_INDEX:-2}"               # the environment's identity in its partition
TOKEN=deadbeef01234567cafebabe76543210   # dave, DB_ADMIN (kernel/auth.c)
BASE=""

[ "$fail" -eq 0 ] || { echo; echo "env_checkpoint_restore_check: source clauses failed — not booting"; exit 1; }

for tool in qemu-system-x86_64 qemu-img curl python3; do
    command -v "$tool" >/dev/null 2>&1 || {
        echo "ABORT: $tool not found — the boot arm boots the ISO under QEMU." >&2
        exit 2
    }
done
[ -f "$ISO" ] || { echo "ABORT: $ISO missing — build it with: make x86-iso (with sidecars.cpio present)" >&2; exit 2; }
[ -f tests/grub_select_kernel_only.sh ] || { echo "ABORT: tests/grub_select_kernel_only.sh missing — the grub menu cannot be driven" >&2; exit 2; }

# ─── A stale sidecar looks EXACTLY like a broken restore ───────────────────
# $ISO embeds sidecars.cpio, which is a TRACKED artifact, so a `user/` change
# that was never re-packed (`make selfhost-bootimage`) can leave the ISO
# carrying an init built before it. The first thing that goes wrong then is
# real but misattributed: the create SUCCEEDS, its ENV_REGISTER round trip is
# answered by a manager that has never heard of the opcode, and the kernel
# honestly reports
#
#   [ENV] env 1 (partition 1): ENV_REGISTER reply carries 28 bytes, short of
#         the 216-byte registration -- refused, not half-applied
#
# which reads as a restore-layer defect and is nothing of the sort. It cost
# this guard's first passing-shaped run its whole diagnosis.
#
# `make x86-iso` now REFUSES such an archive outright, so a stale one cannot be
# built into a fresh ISO at all — but P1A_ISO can name an older ISO, and a
# refusal in the build is exactly the kind of thing that gets worked around by
# pointing at yesterday's image. So the arm still says it out loud, from the
# same digest the build verifies: a NOTE and not an abort, because the archive
# is a committed artifact and this must not block a run that has a good ISO.
if [ -f sidecars.cpio ] && [ -x tools/sidecar_source_digest.sh ]; then
    _want=$(tools/sidecar_source_digest.sh 2>/dev/null || true)
    _have=$(cat sidecars.cpio.digest 2>/dev/null || true)
    if [ -n "$_want" ] && [ "$_have" != "$_want" ]; then
        note "sidecars.cpio is not from the user/ sources in this tree (stamp '$_have' vs '$_want') — $ISO may carry an init from before them. If the run reports 'ENV_REGISTER reply carries ... short of the ... registration', re-pack first: make selfhost-bootimage && make x86-iso"
    fi
fi
# The source digest cannot see an archive that was repacked or replaced with
# user/ untouched (a hand-run `cpio`, a `cp` of an older sidecars.cpio), so the
# archive's own bytes are noted too — the same second stamp the build verifies.
if [ -f sidecars.cpio ] && [ -x tools/sidecar_archive_digest.sh ]; then
    _want_sha=$(tools/sidecar_archive_digest.sh 2>/dev/null || true)
    _have_sha=$(cat sidecars.cpio.sha256 2>/dev/null || true)
    if [ -n "$_want_sha" ] && [ "$_have_sha" != "$_want_sha" ]; then
        note "sidecars.cpio's own bytes are not the ones that were packed and stamped (sha256 '$_have_sha' vs '$_want_sha') — the archive was repacked or replaced after it was stamped, so $ISO may carry an init this tree did not build. Re-pack first: make selfhost-bootimage && make x86-iso"
    fi
fi
# Each stamp above can still be refreshed on its own; the pack-run record is
# what says they and the archive came from one run. A mismatch here means no
# single `make selfhost-bootimage` wrote all three, which the build now refuses
# too — noted before a boot for the run that points at an older image anyway.
if [ -f sidecars.cpio ] && [ -x tools/sidecar_stamp_record.sh ]; then
    _want_rec=$(tools/sidecar_stamp_record.sh 2>/dev/null || true)
    _have_rec=$(cat sidecars.cpio.stamps 2>/dev/null || true)
    if [ -n "$_want_rec" ] && [ "$_have_rec" != "$_want_rec" ]; then
        note "sidecars.cpio's stamps are not all from one packer run (the pack-run record sidecars.cpio.stamps differs from a live recomputation) — one stamp was refreshed without the other, so $ISO may carry an init the packed sources no longer describe. Re-pack first: make selfhost-bootimage && make x86-iso"
    fi
fi

case "$TOOTH" in
    ""|no-restore) ;;
    stale-descriptor|leak-unpause|dirty-capture)
        echo "ABORT: P1A_TOOTH=$TOOTH is a source-level tooth (see tests/env_checkpoint_restore_check_smoke.sh); the boot arm knows only no-restore" >&2
        exit 2 ;;
    *) echo "ABORT: unknown P1A_TOOTH='$TOOTH' — expected no-restore" >&2; exit 2 ;;
esac

PORT="${P1A_PORT:-}"
if [ -z "$PORT" ]; then
    PORT=$(bash tests/free_port.sh) || { echo "ABORT: no free loopback port for the QEMU hostfwd (see tests/free_port.sh)" >&2; exit 2; }
fi
BASE="http://127.0.0.1:$PORT"

# The artifacts live OUTSIDE the work dir on purpose: $WD is removed on exit
# (success or failure), and the evidence a run records is the thing --replay
# reads later — a path that is deleted before the caller can look at it would
# make the printout below a lie. Default to a per-run temp dir, printed at the
# end; P1A_ARTIFACTS puts it somewhere the caller owns.
WD="$(mktemp -d)"
ART="${P1A_ARTIFACTS:-/tmp/p1a-restore-$$}"
mkdir -p "$ART"
IMG="$WD/disk.img"
SER="$WD/ser"
LOG="$WD/serial.log"
QERR="$WD/qemu.err"
QPID=""
CATPID=""

boot_fail() {   # boot_fail <message>
    echo "FAIL: $1" >&2
    [ -s "$QERR" ] && sed 's/^/      qemu: /' "$QERR" >&2
    echo "      last 20 serial lines:" >&2
    tail -20 "$LOG" 2>/dev/null | sed 's/^/      /' >&2
    cp "$LOG" "$ART/serial-fail.log" 2>/dev/null || true
    echo "      the whole serial log is at: $ART/serial-fail.log" >&2
    echo "      (env/init lines: grep -aE '\[ENV\]|\[INIT\]' $ART/serial-fail.log | tail -20)" >&2
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

# The NVMe device is the point of this run: the snapshot must survive the
# reboot, so the disk is the same file across both boots. No -no-reboot: the
# reboot endpoint resets the machine in place, which -no-reboot would turn into
# a QEMU exit.
qemu-system-x86_64 -cdrom "$ISO" \
    -drive id=disk,file="$IMG",if=none,format=raw \
    -device nvme,drive=disk,serial=slsdev0 \
    -netdev user,id=net0,hostfwd=tcp:127.0.0.1:$PORT-:3000 \
    -device e1000,netdev=net0,mac=52:54:00:12:34:07 \
    -display none -m "$RAM" -smp "$SMP" -boot d -monitor none \
    $ACCEL \
    -serial pipe:"$SER" 2>"$QERR" &
QPID=$!

bash tests/grub_select_kernel_only.sh "$SER.in" "$LOG" "$QPID" "$ENTRY" || {
    boot_fail "could not select grub entry $ENTRY (QEMU or grub failed before the menu)"
}
ok "L1. QEMU is booting grub entry $ENTRY (the unified boot) with a fresh 10G NVMe image"

count_line() {   # count_line <fixed string> — a number, always
    local n
    n=$(grep -acF -- "$1" "$LOG" 2>/dev/null)
    printf '%s\n' "${n:-0}"
}

# The unified boot's readiness, the same markers tests/env_create_boot_check.sh
# waits on, plus the contradiction arms that name a wrong boot as soon as the
# kernel makes it visible instead of waiting the window out.
ready=0
qemu_alive=1
for i in $(seq 1 $((WINDOW_S * 2))); do
    if grep -aq "unified=1" "$LOG" 2>/dev/null && \
       grep -aq "Listening on port 3000" "$LOG" 2>/dev/null && \
       [ "$(count_line "[E1] control plane planted")" -ge 1 ] && \
       [ "$(count_line "[INIT] heartbeat")" -ge 5 ]; then
        ready=1
        break
    fi
    if grep -aq "[BOOT] command line:" "$LOG" 2>/dev/null && \
       ! grep -aq "unified=1" "$LOG" 2>/dev/null; then
        boot_fail "grub entry $ENTRY booted without unified=1 on the kernel command line (this is not the unified boot)"
    fi
    if ! kill -0 "$QPID" 2>/dev/null; then
        qemu_alive=0
        break
    fi
    sleep 0.5
done
[ "$ready" -eq 1 ] || {
    if [ "$qemu_alive" -eq 0 ]; then
        boot_fail "QEMU exited before the unified boot reached its markers — the boot crashed"
    fi
    boot_fail "the unified boot's markers did not appear within ${WINDOW_S}s (heartbeats: $(count_line "[INIT] heartbeat"))"
}
ok "L2. the unified boot is up (control plane serving, init making progress)"

# ─── HTTP helpers (retried: one refused connection is a host-side hiccup) ──
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
jval() {   # jval <json-file> <key> — the value, or empty when absent/not JSON
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

wait_health() {   # wait until the CURRENT boot's control plane answers
    for i in $(seq 1 90); do
        if curl -sf --max-time 2 "$BASE/api/health" >/dev/null 2>&1; then return 0; fi
        if ! kill -0 "$QPID" 2>/dev/null; then return 1; fi
        sleep 2
    done
    return 1
}

# ─── L3. a partition over HTTP, then an environment in it ─────────────────
pname="p1arestore"
api "$WD/pcreate.json" -X POST -d "{\"name\":\"$pname\"}" "$BASE/api/partitions" || \
    boot_fail "POST /api/partitions did not answer"
PID="$(jval "$WD/pcreate.json" partition_id)"
[ "$(jval "$WD/pcreate.json" ok)" = "true" ] && [ -n "$PID" ] && [ "$PID" != "0" ] || \
    boot_fail "POST /api/partitions did not define a partition (id='$PID')"
ok "L3. partition $PID ('$pname') exists"

api "$WD/create.json" -X POST -d "{\"index\":$INDEX}" "$BASE/api/partition/$PID/env" || \
    boot_fail "POST /api/partition/$PID/env did not answer"
ENV_ID="$(jval "$WD/create.json" env_id)"
if [ "$(jval "$WD/create.json" ok)" != "true" ] || [ -z "$ENV_ID" ] || [ "$ENV_ID" = "0" ]; then
    boot_fail "the environment manager did not create an environment in partition $PID"
fi
ok "L4. env $ENV_ID is live in partition $PID at index $INDEX (the identity to restore)"

# ─── L5. pause the partition, so a pause is part of what is checkpointed ───
api "$WD/pause.json" -X POST -d "{\"partition_id\":$PID}" "$BASE/api/partition/pause" || \
    boot_fail "POST /api/partition/pause did not answer"
[ "$(jval "$WD/pause.json" ok)" = "true" ] || boot_fail "partition $PID could not be paused"
ok "L5. partition $PID is admin-paused — the checkpoint now has a pause to record"

# ─── L6. checkpoint: the region is written to NVMe ────────────────────────
api "$WD/checkpoint.json" -X POST "$BASE/api/checkpoint" || boot_fail "POST /api/checkpoint did not answer"
[ "$(jval "$WD/checkpoint.json" status)" = "0" ] || boot_fail "the checkpoint did not run (status=$(jval "$WD/checkpoint.json" status))"
grep -aqF "[PERSIST] Environment checkpoint snapshot written" "$LOG" || \
    boot_fail "the checkpoint ran but wrote no environment snapshot line"
ok "L6. the checkpoint wrote the environment region to NVMe (seq $(jval "$WD/checkpoint.json" seq))"

# ─── Record what this boot said and saw, then reboot in place ──────────────
python3 - "$ART/identity.json" "$PID" "$INDEX" "$ENV_ID" "$pname" <<'PY'
import json, sys
p, part, idx, env, name = sys.argv[1:]
json.dump({"partition": int(part), "index": int(idx), "env_id": int(env),
           "partition_name": name}, open(p, "w"), sort_keys=True)
open(p, "a").write("\n")
PY
cp "$WD/create.json" "$ART/create.json"
cp "$WD/pause.json" "$ART/pause.json"
cp "$WD/checkpoint.json" "$ART/checkpoint.json"
cp "$LOG" "$ART/boot1.log"
SPLIT=$(wc -c < "$LOG")
if [ -n "$TOOTH" ]; then printf '%s\n' "$TOOTH" > "$ART/tooth.txt"; fi

planted_before=$(count_line "[E1] control plane planted")
curl -s -X POST "$BASE/api/node/reboot" -H "Authorization: Bearer $TOKEN" \
     -d '{"confirm":"reboot"}' --max-time 10 >/dev/null 2>&1 &
sleep 1
# The reset reboots the machine in place: grub's countdown would auto-boot the
# Phase-5 entry for the second boot too, so select the unified entry again (the
# helper only matches a menu render newer than its own start, so it cannot hit
# the first boot's menu).
bash tests/grub_select_kernel_only.sh "$SER.in" "$LOG" "$QPID" "$ENTRY" || {
    boot_fail "could not select grub entry $ENTRY after the reboot"
}
ok "L7. the node was rebooted in place and grub entry $ENTRY selected again (same NVMe image)"

ready2=0
for i in $(seq 1 $((WINDOW_S * 2))); do
    if [ "$(count_line "[E1] control plane planted")" -gt "$planted_before" ] && \
       grep -aqF "[ENV_CKPT] Restored " "$LOG" && \
       curl -sf --max-time 2 "$BASE/api/health" >/dev/null 2>&1; then
        ready2=1
        break
    fi
    if ! kill -0 "$QPID" 2>/dev/null; then
        boot_fail "QEMU exited during the reboot — the machine did not come back"
    fi
    sleep 0.5
done
[ "$ready2" -eq 1 ] || boot_fail "the rebooted node did not reach its control plane within ${WINDOW_S}s"
ok "L8. the node is up again and the boot line says it restored the snapshot's records"

# ─── L9. the pause came back with the snapshot, before any replay ──────────
api "$WD/pre.json" "$BASE/api/partitions" || boot_fail "GET /api/partitions did not answer after the reboot"
python3 - "$WD/pre.json" "$PID" <<'PY' || boot_fail "partition $PID did not come back PAUSED (the snapshot's pause was not re-applied)"
import json, sys
d = json.load(open(sys.argv[1])); want = str(sys.argv[2])
for p in d.get("partitions", []):
    if str(p.get("id")) == want and str(p.get("paused")) in ("true", "1", "True"):
        sys.exit(0)
sys.exit(1)
PY
ok "L9. partition $PID is paused again after the reboot — the recorded pause came back with the snapshot"

# ─── L10. the replay, unless the no-restore tooth withholds it ─────────────
if [ "$TOOTH" = "no-restore" ]; then
    note "L10. P1A_TOOTH=no-restore: the replay is deliberately withheld — the environment must NOT come back, and B8 must say so"
else
    api "$WD/restore.json" -X POST "$BASE/api/env/restore" || boot_fail "POST /api/env/restore did not answer"
    [ "$(jval "$WD/restore.json" ok)" = "true" ] || boot_fail "the replay pass did not complete (ok=$(jval "$WD/restore.json" ok))"
    ok "L10. the replay ran: replayed $(jval "$WD/restore.json" replayed), refused $(jval "$WD/restore.json" refused), remaining $(jval "$WD/restore.json" remaining), repaused $(jval "$WD/restore.json" repaused)"
fi

# ─── The post-replay snapshots the validator reads ────────────────────────
api "$WD/envlist.json" "$BASE/api/partition/$PID/env" || true
api "$WD/post.json"    "$BASE/api/partitions"        || true
api "$WD/procs.json"   "$BASE/api/processes"          || true
cp "$WD/pre.json"     "$ART/partitions_pre.json"
cp "$WD/envlist.json" "$ART/envlist.json"
cp "$WD/post.json"    "$ART/partitions_post.json"
cp "$WD/procs.json"   "$ART/processes.json"
[ -f "$WD/restore.json" ] && cp "$WD/restore.json" "$ART/restore.json"
tail -c +$((SPLIT + 1)) "$LOG" > "$ART/boot2.log"

kill "$QPID" 2>/dev/null || true
QPID=""
kill "$CATPID" 2>/dev/null || true
CATPID=""

echo
echo "artifacts: $ART (replay them later with: bash tests/env_checkpoint_restore_check.sh --replay \"$ART\")"
echo

if validate "$ART" "$TOOTH"; then
    echo
    echo "env_checkpoint_restore_check: the environment survived a checkpoint and a real reboot — replayed in $PID at index $INDEX, with the recorded pause restored."
    exit 0
fi
echo
echo "env_checkpoint_restore_check: the reboot did NOT hold the restore it should have (artifacts: $ART)."
exit 1
