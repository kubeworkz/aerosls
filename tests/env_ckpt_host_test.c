/*
 * env_ckpt_host_test.c — POSIX-Environments Roadmap v0.2, Phase P1a: the
 * environment checkpoint record's own guard.
 *
 * ─── What this covers, and what it deliberately leaves to other guards ──────
 * This test links the REAL, unmodified kernel/env_ckpt.c — not a
 * reimplementation — and drives its two halves:
 *
 *   1. The record gate (env_ckpt_valid()), the table
 *      (env_ckpt_record()/_drop()/_find()), and dirty marking.
 *   2. The snapshot replay (env_ckpt_adopt()), which is where P1a's
 *      "refusal over partial application" property either holds or does not.
 *
 * ─── Why the refusal clauses are the point ─────────────────────────────────
 * A restore that quietly applies half of a snapshot is worse than one that
 * fails: the operator gets an environment that exists, answers, and is
 * missing files, with nothing anywhere saying so. Every refusal clause below
 * is therefore asserted TWICE — the refusal is named, AND the live table is
 * empty afterwards. The second half is the vacuity control: a guard that only
 * checked "an error code came back" would pass against an implementation that
 * returned the code and applied the records anyway.
 *
 * The persistence half of P1a — the NVMe region, its checksum span, the
 * header fields the restore side reads, and the block in
 * persist_restore_all() that calls env_ckpt_adopt() — is asserted by
 * tests/env_ckpt_check.sh (source-level wiring, with teeth in
 * tests/env_ckpt_check_smoke.sh) and by tests/persist_lba_layout_host_test.c
 * (the region's placement inside the on-disk envelope). Deliberately not
 * re-driven here: linking kernel/persist.c for real would drag in fifteen
 * other subsystems' globals, and that stub set is exactly the duplication
 * tests/process_host_stubs.h was created to stop.
 *
 * Build and run:
 *   gcc -Wall -Wextra -std=c11 -I . -I kernel -I drivers \
 *       -o /tmp/env_ckpt_host_test tests/env_ckpt_host_test.c kernel/env_ckpt.c
 *   /tmp/env_ckpt_host_test
 */
#include "kernel/env_ckpt.h"
#include "kernel/checkpoint_delta.h"
#include "tests/partition_host_stubs.h"  /* P1a quiesce: env_ckpt.c's partition_pause/_resume/_is_paused/_exists */
#include <stdio.h>
#include <string.h>
#include <stdint.h>

/* ─── ckpt_mark_dirty: a RECORDING stand-in, not a no-op ─────────────────
 * env_ckpt.c calls this on every mutation, and "the region bit is marked" is
 * a real invariant (without it an incremental checkpoint silently skips the
 * environment records and a clean shutdown loses them). Recorded rather than
 * ignored so the clauses below can assert it; the bit itself is the same
 * expression kernel/checkpoint_delta.c uses. */
static uint32_t g_dirty_mask = 0;
static int      g_mark_calls = 0;
void ckpt_mark_dirty(uint32_t region) {
    g_mark_calls++;
    if (region < 32) g_dirty_mask |= (1u << region);
}

static int checks_passed = 0;
static int checks_failed = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); checks_failed++; } \
    else         { printf("ok:   %s\n", msg); checks_passed++; } \
} while (0)

/* A well-formed record for (partition, index). Every refusal clause below is
 * this record with exactly one field spoiled, so a failure names the field. */
static struct EnvCkptRecord good_record(uint32_t partition, uint32_t index) {
    struct EnvCkptRecord r;
    memset(&r, 0, sizeof r);
    r.magic        = ENV_CKPT_REC_MAGIC;
    r.version      = ENV_CKPT_REC_VERSION;
    r.size         = (uint32_t)sizeof(struct EnvCkptRecord);
    r.sequence     = 7;
    r.partition_id = partition;
    r.index        = index;
    r.env_id       = 100u + index;
    r.console_id   = 3;
    r.flags        = ENV_CKPT_FLAG_QUIESCED;
    r.n_tasks      = 2;
    r.n_regions    = 3;
    r.n_chans      = 4;
    r.chans[0] = 0x11; r.chans[1] = 0x12; r.chans[2] = 0x13; r.chans[3] = 0x14;

    r.regions[0].base = 0x100000ull; r.regions[0].frames = 1024;
    r.regions[0].kind = ENV_CKPT_REGION_POSIX_HEAP;
    r.regions[1].base = 0x200000ull; r.regions[1].frames = 64;
    r.regions[1].kind = ENV_CKPT_REGION_RD_HEAP;
    r.regions[2].base = 0x300000ull; r.regions[2].frames = 256;
    r.regions[2].kind = ENV_CKPT_REGION_RD_STORAGE;

    strcpy(r.tasks[0].name, "aerosls.posix.0");
    r.tasks[0].kind      = ENV_CKPT_TASK_POSIX_SIDECAR;
    r.tasks[0].cap_count = 5;
    r.tasks[0].entry     = 0x401000ull;
    r.tasks[0].stack_bottom = 0x700000ull;
    r.tasks[0].stack_top    = 0x701000ull;

    strcpy(r.tasks[1].name, "drv.ramdisk.0");
    r.tasks[1].kind      = ENV_CKPT_TASK_RAMDISK_SIDECAR;
    r.tasks[1].cap_count = 4;
    r.tasks[1].entry     = 0x402000ull;
    r.tasks[1].stack_bottom = 0x702000ull;
    r.tasks[1].stack_top    = 0x703000ull;
    return r;
}

/* C has no "address of a struct returned by value", so the clauses that just
 * want a good record in the table go through this. One scratch slot is
 * deliberate: it keeps every call site identical to the real kernel's shape
 * (store a record built somewhere, from a caller-owned buffer). */
static struct EnvCkptRecord g_scratch;
static int record_at(uint32_t partition, uint32_t index) {
    g_scratch = good_record(partition, index);
    return env_ckpt_record(&g_scratch);
}

static int table_is_empty(void) {
    if (env_ckpt_count_live() != 0) return 0;
    for (uint32_t i = 0; i < ENV_CKPT_MAX; i++) {
        struct EnvCkptRecord zero;
        memset(&zero, 0, sizeof zero);
        if (memcmp(&env_ckpt_table[i], &zero, sizeof zero) != 0) return 0;
    }
    return 1;
}

int main(void) {
    printf("=== env_ckpt: P1a environment checkpoint record ===\n\n");

    /* ── 0. The frame-fit invariant, as a runtime check too ───────────────
     * env_ckpt.c already makes this a _Static_assert; repeating it here means
     * the constraint is visible in this test's own output rather than only in
     * a build log nobody reads. Growing ENV_CKPT_MAX past the frame is what
     * would silently truncate the on-disk array. */
    CHECK(sizeof(struct EnvCkptRecord) * ENV_CKPT_MAX <= 4096,
          "the whole record array fits one 4 KiB NVMe frame (persist_environments() writes it as one span)");
    printf("      (%u records x %u B = %u B, one frame is 4096 B)\n",
           (unsigned)ENV_CKPT_MAX, (unsigned)sizeof(struct EnvCkptRecord),
           (unsigned)(ENV_CKPT_MAX * sizeof(struct EnvCkptRecord)));

    /* ── 1. The gate accepts a well-formed record ────────────────────────── */
    env_ckpt_reset();
    struct EnvCkptRecord r = good_record(3, 0);
    CHECK(env_ckpt_valid(&r) == ENV_CKPT_REFUSE_NONE,
          "a well-formed record validates");

    g_dirty_mask = 0; g_mark_calls = 0;
    CHECK(env_ckpt_record(&r) == 0 && env_ckpt_count_live() == 1,
          "the live handler stores it (no longer a claim: count is 1)");
    CHECK((g_dirty_mask & (1u << CKPT_REGION_ENV)) != 0,
          "storing a record marks CKPT_REGION_ENV dirty (an unmarked region is skipped by an incremental checkpoint)");

    /* ── 2. Upsert on (partition, index), not on sequence ────────────────── */
    struct EnvCkptRecord r2 = good_record(3, 0);
    r2.sequence = 8;
    r2.env_id   = 999;
    CHECK(env_ckpt_record(&r2) == 0 && env_ckpt_count_live() == 1,
          "a second record for the same (partition, index) replaces rather than appends");
    const struct EnvCkptRecord* got = env_ckpt_find(3, 0);
    CHECK(got != NULL && got->env_id == 999 && got->sequence == 8,
          "the replacement's fields are what is stored (env_id 999, sequence 8)");
    CHECK(env_ckpt_find(3, 1) == NULL,
          "a different index is a different environment (no cross-index match)");

    /* ── 3. The table fills, then refuses instead of overrunning ───────────
     * The refusal is not cosmetic: without it this write is a buffer overrun
     * past env_ckpt_table[ENV_CKPT_MAX]. */
    for (uint32_t i = 1; i < ENV_CKPT_MAX; i++)
        record_at(3, i);
    CHECK(env_ckpt_count_live() == ENV_CKPT_MAX,
          "the table holds exactly ENV_CKPT_MAX records");
    int rc_full = record_at(3, ENV_CKPT_MAX);
    CHECK(rc_full == ENV_CKPT_REFUSE_FULL && env_ckpt_count_live() == ENV_CKPT_MAX,
          "one record past the table is REFUSED (FULL) and the table is unchanged");

    /* ── 4. Every refusal names its own reason ───────────────────────────── */
    struct EnvCkptRecord bad;

    bad = good_record(3, 0); bad.magic = 0x1122334455667788ull;
    CHECK(env_ckpt_valid(&bad) == ENV_CKPT_REFUSE_BAD_MAGIC,
          "a record with the wrong magic is refused (this is the slot-past-the-live-prefix case too)");

    bad = good_record(3, 0); bad.version = ENV_CKPT_REC_VERSION + 1;
    CHECK(env_ckpt_valid(&bad) == ENV_CKPT_REFUSE_VERSION,
          "a record from a newer format version is refused, not guessed at");

    bad = good_record(3, 0); bad.size = (uint32_t)sizeof(struct EnvCkptRecord) - 8;
    CHECK(env_ckpt_valid(&bad) == ENV_CKPT_REFUSE_REC_SIZE,
          "a record whose own recorded size disagrees with this build is refused (the foreign-build case)");

    bad = good_record(3, 0); bad.n_tasks = ENV_CKPT_MAX_TASKS + 1;
    CHECK(env_ckpt_valid(&bad) == ENV_CKPT_REFUSE_BAD_TASKS,
          "a task count past the array is refused rather than walked off the end");

    bad = good_record(3, 0); bad.tasks[0].kind = 0x7F;
    CHECK(env_ckpt_valid(&bad) == ENV_CKPT_REFUSE_BAD_TASK,
          "an unknown task kind is refused (the restore cannot re-create what it cannot name)");

    bad = good_record(3, 0);
    memset(bad.tasks[0].name, 'x', ENV_CKPT_NAME_LEN);   /* no terminator */
    CHECK(env_ckpt_valid(&bad) == ENV_CKPT_REFUSE_BAD_TASK,
          "an unterminated task name is refused (a restore re-registers by this name, so it is read, not trusted)");

    bad = good_record(3, 0); bad.tasks[1].entry = 0;
    CHECK(env_ckpt_valid(&bad) == ENV_CKPT_REFUSE_BAD_TASK,
          "a task with no entry point is refused");

    bad = good_record(3, 0); bad.n_regions = 2;
    CHECK(env_ckpt_valid(&bad) == ENV_CKPT_REFUSE_BAD_REGIONS,
          "a record missing one of its three regions is refused (a partial environment is a different object)");

    bad = good_record(3, 0); bad.regions[1].frames = 0;
    CHECK(env_ckpt_valid(&bad) == ENV_CKPT_REFUSE_BAD_REGIONS,
          "a zero-frame region is refused");

    bad = good_record(3, 0); bad.regions[2].kind = 9;
    CHECK(env_ckpt_valid(&bad) == ENV_CKPT_REFUSE_BAD_REGIONS,
          "an unknown region kind is refused (reattaching the heap where the storage belongs is the corruption this prevents)");

    /* Each of the three kinds exactly once. Three KNOWN kinds with one
     * repeated is the swap the kinds are named for: every field is
     * self-consistent, both bases look plausible, and the POSIX heap would be
     * restored where the ramdisk storage belongs. P1a's register path makes
     * this reachable from a live environment manager, whose `struct
     * Environment` declares its regions in a different order from the record's. */
    bad = good_record(3, 0); bad.regions[2].kind = ENV_CKPT_REGION_POSIX_HEAP;
    CHECK(env_ckpt_valid(&bad) == ENV_CKPT_REFUSE_BAD_REGIONS,
          "two regions claiming the same kind are refused (a swap that moved the base without the kind)");

    bad = good_record(0xFFFFFFFFu, 0);
    CHECK(env_ckpt_valid(&bad) == ENV_CKPT_REFUSE_BAD_IDENTITY,
          "a record naming no partition (0xFFFFFFFF, the gate's own sentinel) is refused");

    /* The live handler refuses an invalid record too -- and stores nothing. */
    uint32_t before = env_ckpt_count_live();
    bad = good_record(3, 99); bad.magic = 0;
    CHECK(env_ckpt_record(&bad) == ENV_CKPT_REFUSE_BAD_MAGIC &&
          env_ckpt_count_live() == before,
          "the live handler refuses an invalid record and stores nothing");

    /* ── 5. Drop keeps the table dense ───────────────────────────────────── */
    env_ckpt_reset();
    record_at(5, 0);
    record_at(5, 1);
    record_at(5, 2);
    CHECK(env_ckpt_drop(5, 1) == 0 && env_ckpt_count_live() == 2,
          "drop removes exactly one record");
    CHECK(env_ckpt_find(5, 1) == NULL && env_ckpt_find(5, 2) != NULL,
          "the dropped record is gone and its successor survived (the table stayed dense)");
    CHECK(env_ckpt_find(5, 2)->index == 2,
          "the shifted record is still itself, not a stale copy of its neighbour");

    /* ── 6. Adopt: the good snapshot ─────────────────────────────────────── */
    env_ckpt_reset();
    struct EnvCkptRecord staging[ENV_CKPT_MAX];
    memset(staging, 0, sizeof staging);
    staging[0] = good_record(2, 0);
    staging[1] = good_record(2, 1);
    int rc = env_ckpt_adopt(staging, 2, (uint32_t)sizeof(struct EnvCkptRecord),
                            ENV_CKPT_REC_VERSION);
    CHECK(rc == ENV_CKPT_REFUSE_NONE && env_ckpt_count_live() == 2,
          "adopting a two-record snapshot restores both");
    CHECK(env_ckpt_find(2, 1) != NULL && env_ckpt_find(2, 1)->env_id == 101,
          "the second record came back with its own fields, not the first's");
    CHECK(env_ckpt_last_refusal().code == ENV_CKPT_REFUSE_NONE,
          "a successful adopt records no refusal");

    /* ── 7. Adopt REFUSES IN FULL -- the vacuity control ───────────────────
     * The snapshot is good for record 0 and bad at record 1. Every clause
     * below asserts BOTH the refusal AND an empty table: an implementation
     * that refused the snapshot but had already written record 0 would pass a
     * code-only check and fail this one. */
    staging[1] = good_record(2, 1);
    staging[1].regions[0].frames = 0;          /* one field spoiled */
    rc = env_ckpt_adopt(staging, 2, (uint32_t)sizeof(struct EnvCkptRecord),
                        ENV_CKPT_REC_VERSION);
    CHECK(rc == ENV_CKPT_REFUSE_BAD_REGIONS,
          "a snapshot with one bad record is refused (BAD_REGIONS)");
    CHECK(table_is_empty(),
          "and NOTHING was applied -- no environment is left half-restored (the property P1a is built around)");
    struct EnvCkptRefusal ref = env_ckpt_last_refusal();
    CHECK(ref.code == ENV_CKPT_REFUSE_BAD_REGIONS && ref.a == 1,
          "the refusal names the offending record index (1), not just the code");

    /* ── 8. The header's fields are checked before the records ───────────── */
    staging[0] = good_record(2, 0);
    staging[1] = good_record(2, 1);
    rc = env_ckpt_adopt(staging, 2, (uint32_t)sizeof(struct EnvCkptRecord),
                        ENV_CKPT_REC_VERSION + 1);
    CHECK(rc == ENV_CKPT_REFUSE_VERSION && table_is_empty(),
          "a snapshot whose header names another record version is refused with an empty table");
    CHECK(env_ckpt_last_refusal().a == ENV_CKPT_REC_VERSION + 1 &&
          env_ckpt_last_refusal().b == ENV_CKPT_REC_VERSION,
          "the refusal carries both the found and the expected version");

    rc = env_ckpt_adopt(staging, 2, (uint32_t)sizeof(struct EnvCkptRecord) + 16,
                        ENV_CKPT_REC_VERSION);
    CHECK(rc == ENV_CKPT_REFUSE_REC_SIZE && table_is_empty(),
          "a snapshot written by a build with a different record size is refused with an empty table");

    /* ── 9. A count that cannot be describing this build's array ─────────── */
    rc = env_ckpt_adopt(staging, ENV_CKPT_MAX + 1,
                        (uint32_t)sizeof(struct EnvCkptRecord), ENV_CKPT_REC_VERSION);
    CHECK(rc == ENV_CKPT_REFUSE_COUNT && table_is_empty(),
          "a header count past the table is refused before it can steer a read");

    /* ── 10. Two records for one environment: whatever wins would be luck ── */
    staging[0] = good_record(2, 0);
    staging[1] = good_record(2, 0);
    staging[1].env_id = 555;
    rc = env_ckpt_adopt(staging, 2, (uint32_t)sizeof(struct EnvCkptRecord),
                        ENV_CKPT_REC_VERSION);
    CHECK(rc == ENV_CKPT_REFUSE_DUPLICATE && table_is_empty(),
          "a snapshot whose records collide on (partition, index) is refused as corrupt");
    CHECK(env_ckpt_last_refusal().a == 1 && env_ckpt_last_refusal().b == 0,
          "the refusal names both colliding records (1 and 0)");

    /* ── 11. A committed-but-empty snapshot is legitimate, not a refusal ───
     * This is what the writer records once the last environment is gone, and
     * it is the state that must NOT resurrect a stale environment. Accepting
     * it silently is the correct answer; refusing it would make destroying
     * every environment a permanent boot error. */
    record_at(4, 0);                          /* put something live first */
    rc = env_ckpt_adopt(staging, 0, (uint32_t)sizeof(struct EnvCkptRecord),
                        ENV_CKPT_REC_VERSION);
    CHECK(rc == ENV_CKPT_REFUSE_NONE && table_is_empty(),
          "an empty snapshot is accepted and clears the live set (destroying every environment is not a boot error)");

    /* ── 12. after_restore() is a real compaction, not a no-op ───────────── */
    env_ckpt_reset();
    record_at(6, 0);
    record_at(6, 1);
    env_ckpt_table[0].magic = 0;              /* simulate damage the restore missed */
    env_ckpt_after_restore();
    CHECK(env_ckpt_count_live() == 1 && env_ckpt_find(6, 1) != NULL,
          "after_restore() compacts away an unusable record instead of leaving it counted as live");

    /* ── 13. The refusal renders, and always terminates ──────────────────── */
    char text[96];
    struct EnvCkptRefusal v = { ENV_CKPT_REFUSE_VERSION, 4, 1 };
    env_ckpt_refusal_text(&v, text, sizeof text);
    CHECK(text[0] != '\0' && strstr(text, "4") != NULL && strstr(text, "1") != NULL,
          "the rendered refusal carries both operands (the transcript says which and why)");
    CHECK(strlen(text) < sizeof text,
          "the renderer terminates within its bound (an unterminated reason would corrupt the serial transcript)");
    char tiny[8];
    memset(tiny, 'Z', sizeof tiny);
    env_ckpt_refusal_text(&v, tiny, sizeof tiny);
    CHECK(tiny[sizeof tiny - 1] == '\0',
          "a truncated render is still NUL-terminated at the end of its buffer");
    env_ckpt_refusal_text(&(struct EnvCkptRefusal){ ENV_CKPT_REFUSE_FULL, 8, 8 },
                          text, sizeof text);
    CHECK(strstr(text, "full") != NULL,
          "the FULL refusal reads as the table being full");

    /* ── 14. A flag this build does not know is refused, not skipped ────── */
    bad = good_record(3, 0); bad.flags = 0x0004u;   /* not QUIESCED, not PAUSED */
    CHECK(env_ckpt_valid(&bad) == ENV_CKPT_REFUSE_BAD_FLAGS,
          "an unknown flag bit is refused (a record whose meaning is partly unknown cannot be vouched for)");

    /* ── 15. Quiescing: the capture freezes what it captures ───────────────
     * ENV_CKPT_FLAG_QUIESCED is only worth persisting if something writes it.
     * This is that writer: a capture pauses each record's partition, stamps the
     * records it froze, and remembers the partitions so a release can thaw
     * exactly its own. Two records in ONE partition is deliberate -- the pause
     * happens once and both records carry the flag, because the partition is
     * the unit that freezes. */
    host_partition_stubs_reset();
    host_partition_define(1);
    env_ckpt_reset();
    record_at(1, 0);
    record_at(1, 1);
    struct EnvCkptQuiesce q;
    env_ckpt_quiesce_for_capture(&q);
    CHECK(partition_is_paused(1),
          "the capture pauses the environment's partition before writing it down (rule: freeze before capture)");
    CHECK(q.n_quiesced == 2 && q.n_paused_by_capture == 1 && q.paused_by_capture[0] == 1,
          "two records in one partition: both are counted frozen and the partition is remembered once");
    CHECK((env_ckpt_find(1, 0)->flags & ENV_CKPT_FLAG_QUIESCED) != 0 &&
          (env_ckpt_find(1, 1)->flags & ENV_CKPT_FLAG_QUIESCED) != 0,
          "ENV_CKPT_FLAG_QUIESCED finally has a writer: the frozen records carry it");
    CHECK((env_ckpt_find(1, 0)->flags & ENV_CKPT_FLAG_PARTITION_PAUSED) == 0,
          "a partition the capture itself paused is NOT marked operator-paused (the two flags mean different things)");
    env_ckpt_release_capture(&q);
    CHECK(!partition_is_paused(1),
          "the release resumes exactly the partition this capture froze");

    /* ── 16. Rule 1: a partition an operator paused stays paused ─────────── */
    host_partition_stubs_reset();
    host_partition_define(2);
    host_partition_set_paused(2, 1);              /* operator intent, before the capture */
    env_ckpt_reset();
    record_at(2, 0);
    env_ckpt_quiesce_for_capture(&q);
    CHECK(partition_is_paused(2), "an already-paused partition is still paused after the capture");
    CHECK((env_ckpt_find(2, 0)->flags & ENV_CKPT_FLAG_QUIESCED) != 0 &&
          (env_ckpt_find(2, 0)->flags & ENV_CKPT_FLAG_PARTITION_PAUSED) != 0,
          "its record carries BOTH flags: frozen for the capture AND paused before it (what a restore replays)");
    CHECK(q.n_already_paused == 1 && q.n_paused_by_capture == 0,
          "it is counted as already-paused, not as something this capture paused");
    env_ckpt_release_capture(&q);
    CHECK(partition_is_paused(2),
          "the release does NOT resume a partition this capture did not pause (no silent state change wearing a checkpoint's name)");

    /* ── 17. Rule 2: the control plane's own partition is never frozen ───── */
    host_partition_stubs_reset();                 /* 0 (PARTITION_SYSTEM) is active */
    env_ckpt_reset();
    record_at(0, 0);
    CHECK(partition_pause(PARTITION_SYSTEM) == 0 && partition_resume(PARTITION_SYSTEM) == 0,
          "the stub can pause partition 0, so the capture's refusal to is a DECISION, not an inability");
    env_ckpt_quiesce_for_capture(&q);
    CHECK(!partition_is_paused(PARTITION_SYSTEM),
          "freezing the machinery performing the capture is never done (rule 2)");
    CHECK((env_ckpt_find(0, 0)->flags & ENV_CKPT_FLAG_QUIESCED) == 0,
          "the record says so honestly: QUIESCED is clear on a PARTITION_SYSTEM record");
    CHECK(q.n_unfrozen == 1 && q.n_quiesced == 0,
          "and the unfrozen record is counted rather than quietly passed over");

    /* ── 18. Rule 3: a record whose partition is gone is dropped ─────────── */
    host_partition_stubs_reset();
    host_partition_define(3);
    env_ckpt_reset();
    record_at(3, 0);
    host_partition_destroy(3);                    /* the partition no longer exists */
    env_ckpt_quiesce_for_capture(&q);
    CHECK(env_ckpt_count_live() == 0 && q.n_dropped == 1,
          "a record naming a partition that no longer exists is DROPPED and counted, not persisted (nothing could restore it)");

    /* ── 19. The drop, and the destroy's use of it ───────────────────────── */
    host_partition_stubs_reset();
    env_ckpt_reset();
    record_at(7, 0);                              /* good_record(): env_id = 100 */
    CHECK(env_ckpt_drop_env(7, 100) == 0 && env_ckpt_count_live() == 0,
          "env_ckpt_drop_env finds the record by (partition, env_id) and drops it");
    CHECK(env_ckpt_drop_env(7, 100) == 1,
          "dropping an environment with no record reports there was none (idempotent, not an error)");

    /* ── 20. Restore side: the pause state the snapshot recorded comes back ─
     * The whole point of persisting PARTITION_PAUSED: a snapshot taken while
     * an operator had frozen a tenant restores that tenant FROZEN, not
     * running. Driven through the real capture so the flags are the ones the
     * writer would have written. */
    host_partition_stubs_reset();
    host_partition_define(4);
    host_partition_set_paused(4, 1);
    env_ckpt_reset();
    record_at(4, 0);
    record_at(4, 1);                              /* two environments, one partition */
    env_ckpt_quiesce_for_capture(&q);
    env_ckpt_release_capture(&q);
    CHECK(partition_is_paused(4),
          "(setup) the operator's pause outlives both the capture and its release");
    host_partition_set_paused(4, 0);              /* a fresh boot: nothing is paused yet */
    uint32_t repaused = env_ckpt_apply_restored_pauses();
    CHECK(repaused == 1 && partition_is_paused(4),
          "env_ckpt_apply_restored_pauses() re-pauses the partition a restored record names (a paused partition is restored paused)");
    host_partition_set_paused(4, 0);
    CHECK(env_ckpt_apply_restored_pauses() == 1,
          "two records naming one partition re-pause it once, not twice");

    /* A PARTITION_SYSTEM record that says "was paused" is NOT acted on: a boot
     * that comes up with the control plane frozen is worse than losing the
     * flag. The record still says what was true; only the action is declined. */
    host_partition_stubs_reset();
    host_partition_set_paused(PARTITION_SYSTEM, 0);
    env_ckpt_reset();
    g_scratch = good_record(0, 0);
    g_scratch.flags = ENV_CKPT_FLAG_PARTITION_PAUSED;
    CHECK(env_ckpt_record(&g_scratch) == 0,
          "(setup) a PARTITION_SYSTEM record can carry PARTITION_PAUSED");
    CHECK(env_ckpt_apply_restored_pauses() == 0 && !partition_is_paused(PARTITION_SYSTEM),
          "restoring never freezes the control plane's own partition, even when a record says it was paused");

    /* ── 21. A record whose partition is gone is not re-paused either ────── */
    host_partition_stubs_reset();
    host_partition_define(5);
    host_partition_set_paused(5, 1);
    env_ckpt_reset();
    record_at(5, 0);
    env_ckpt_quiesce_for_capture(&q);
    env_ckpt_release_capture(&q);
    host_partition_destroy(5);
    CHECK(env_ckpt_apply_restored_pauses() == 0,
          "a record naming a partition that no longer exists is skipped by the restore, not acted on");

    /* ── 22. Restore-through-create: the pending set is armed, and settles ──
     * A boot adopts a snapshot and then has to turn each record back into a
     * running environment. env_ckpt_after_restore() arms exactly what was
     * adopted (not what the disk held, and not what a refused snapshot left),
     * and each record is handed out ONCE -- settle or refuse takes it out of
     * the set, so a pass cannot replay the same record twice. */
    host_partition_stubs_reset();
    host_partition_define(6);
    env_ckpt_reset();
    CHECK(env_ckpt_restore_pending() == 0,
          "a fresh table has nothing pending (no snapshot, no replays to make)");
    record_at(6, 0);
    record_at(6, 1);
    record_at(6, 2);
    CHECK(env_ckpt_restore_pending() == 0,
          "storing a record does NOT put it up for replay (only a restore arms the set)");
    env_ckpt_restore_begin();
    CHECK(env_ckpt_restore_pending() == 3,
          "env_ckpt_restore_begin() arms every live record — the arming after_restore() performs on a real boot");
    env_ckpt_after_restore();
    CHECK(env_ckpt_restore_pending() == 3,
          "after_restore() arms every adopted record for replay (a snapshot is a to-do list, not a souvenir)");

    uint32_t rp = 0, ri = 0, re = 0;
    CHECK(env_ckpt_restore_next(&rp, &ri, &re) == 1 && rp == 6 && ri == 0,
          "restore_next() hands out the first record with its identity");
    CHECK(env_ckpt_restore_pending() == 3,
          "a record handed out but not settled is still pending (a caller that gives up leaves it honestly pending)");
    env_ckpt_restore_settle(rp, ri);
    CHECK(env_ckpt_restore_pending() == 2,
          "settling a replayed record takes it out of the set (no record is replayed twice)");
    CHECK(env_ckpt_restore_next(&rp, &ri, &re) == 1 && ri == 1,
          "the next call hands out the next record, not the settled one");
    env_ckpt_restore_refuse(rp, ri, ENV_CKPT_REFUSE_CREATE, 7, 3);
    CHECK(env_ckpt_restore_pending() == 1 && env_ckpt_restore_refused() == 1,
          "refusing a replay takes it out of the set AND counts it (a refused record is not a pending one, and not a replayed one either)");
    CHECK(env_ckpt_last_refusal().code == ENV_CKPT_REFUSE_CREATE &&
          env_ckpt_last_refusal().a == 7 && env_ckpt_last_refusal().b == 3,
          "the refusal is queryable with its operands, not only printed");

    /* A record dropped while pending -- a destroy -- takes its pending bit with
     * it, and the bits of the records behind it shift with the table, or the
     * next hand-out would be about a different environment than the bit says. */
    env_ckpt_reset();
    record_at(6, 0);
    record_at(6, 1);
    record_at(6, 2);
    env_ckpt_after_restore();
    CHECK(env_ckpt_drop(6, 0) == 0 && env_ckpt_restore_pending() == 2,
          "dropping a pending record removes it from the pending set with it");
    CHECK(env_ckpt_restore_next(&rp, &ri, &re) == 1 && ri == 1,
          "and the remaining records are still the ones pending (the set shifted with the table)");
    env_ckpt_restore_settle(rp, ri);
    CHECK(env_ckpt_restore_next(&rp, &ri, &re) == 1 && ri == 2,
          "and then the last one, under the index it kept across the drop");
    env_ckpt_restore_settle(rp, ri);
    CHECK(env_ckpt_restore_pending() == 0 && env_ckpt_restore_next(&rp, &ri, &re) == 0,
          "the set drains exactly once: two drops into one, then nothing");

    /* ── 23. The dirty-capture gate: a record captured while it RAN is not
     *         replayed ──────────────────────────────────────────────────────
     * A create that reproduced a moving target would not be a restore. The
     * record that a capture could not freeze (QUIESCED clear) is refused with
     * its own reason -- except a PARTITION_SYSTEM record, which is the one the
     * capture is documented never to freeze (rule 2), so its QUIESCED-less
     * record is exactly as trustworthy as any other. */
    host_partition_stubs_reset();
    host_partition_define(6);
    env_ckpt_reset();
    g_scratch = good_record(6, 0);
    g_scratch.flags = 0;                       /* captured running: no quiesce */
    CHECK(env_ckpt_record(&g_scratch) == 0,
          "(setup) a record with QUIESCED clear is still a valid record -- it is only unrestorable");
    env_ckpt_after_restore();
    CHECK(env_ckpt_restore_admissible(env_ckpt_find(6, 0)) == ENV_CKPT_REFUSE_UNQUIESCED,
          "a tenant record the capture never froze is refused by the gate, with its own reason");
    CHECK(env_ckpt_restore_next(&rp, &ri, &re) == 0 && env_ckpt_restore_pending() == 0,
          "restore_next() applies that gate itself, so no caller can forget it: nothing is handed out");
    CHECK(env_ckpt_restore_refused() == 1 && env_ckpt_last_refusal().code == ENV_CKPT_REFUSE_UNQUIESCED,
          "and the refused replay is counted and queryable (it is not silently skipped)");

    env_ckpt_reset();
    g_scratch = good_record(PARTITION_SYSTEM, 0);
    g_scratch.flags = 0;                       /* rule 2: never frozen, by design */
    CHECK(env_ckpt_record(&g_scratch) == 0 &&
          env_ckpt_restore_admissible(env_ckpt_find(PARTITION_SYSTEM, 0)) == ENV_CKPT_REFUSE_NONE,
          "a PARTITION_SYSTEM record is admissible without QUIESCED -- the capture never freezes the control plane (rule 2)");
    env_ckpt_after_restore();
    CHECK(env_ckpt_restore_next(&rp, &ri, &re) == 1 && rp == PARTITION_SYSTEM && ri == 0,
          "so it IS replayed: the exemption is a decision carried to the restore side, not a hole");

    host_partition_stubs_reset();
    host_partition_define(6);
    env_ckpt_reset();
    record_at(6, 0);
    host_partition_destroy(6);                 /* gone between capture and restore */
    env_ckpt_after_restore();
    CHECK(env_ckpt_restore_admissible(env_ckpt_find(6, 0)) == ENV_CKPT_REFUSE_NO_PARTITION,
          "a record whose partition is gone is refused to be replayed (there is nowhere to put the environment)");
    CHECK(env_ckpt_restore_next(&rp, &ri, &re) == 0 &&
          env_ckpt_last_refusal().code == ENV_CKPT_REFUSE_NO_PARTITION &&
          env_ckpt_last_refusal().b == 6,
          "restore_next() refuses it too, naming the partition that is missing");

    /* ── 24. Identity, not placement ───────────────────────────────────────
     * The record the create's own ENV_REGISTER produces is compared with the
     * one that was adopted. What must match is the environment: its
     * (partition, index), the tasks it is made of, and the shape of its three
     * regions. What legitimately changes across a create is placement and
     * per-boot machinery -- and the pids, which are not in the record at all. */
    struct EnvCkptRecord w = good_record(6, 0);
    struct EnvCkptRecord g = good_record(6, 0);
    g.env_id = 991;                     /* the manager's handle is fresh each boot */
    g.chans[0] = 0x99; g.chans[3] = 0x98;   /* freshly minted endpoints */
    g.console_id = 0;                   /* bound (or not) by this boot's create */
    g.sequence = 424242;                /* this checkpoint's sequence, not the old one */
    g.regions[0].base = 0xDEAD000ull;   /* the allocator places it where it lands */
    g.regions[1].base = 0xBEEF000ull;
    g.regions[2].base = 0xF00D000ull;
    g.tasks[0].entry = 0x9A000ull;      /* where THIS boot loaded the sidecar */
    g.tasks[0].stack_bottom = 0x1; g.tasks[0].stack_top = 0x2;
    CHECK(env_ckpt_restore_same_identity(&w, &g) == 1,
          "identity survives a real create: new env_id, channels, console key, sequence, bases and entries are placement, not identity");

    struct EnvCkptRecord s;
    s = g; strcpy(s.tasks[1].name, "drv.ramdisk.9");
    CHECK(env_ckpt_restore_same_identity(&w, &s) == 0,
          "a create that came back with a different sidecar name is not the environment that was written down");
    s = g; s.tasks[0].kind = ENV_CKPT_TASK_RAMDISK_SIDECAR;
    CHECK(env_ckpt_restore_same_identity(&w, &s) == 0,
          "nor is one whose task list swapped kinds (the names and kinds are the identity of the sidecars)");
    s = g; s.regions[0].kind = ENV_CKPT_REGION_RD_STORAGE; s.regions[2].kind = ENV_CKPT_REGION_POSIX_HEAP;
    CHECK(env_ckpt_restore_same_identity(&w, &s) == 0,
          "nor one whose regions came back as a different set of kinds (the heap is not the storage)");
    s = g; s.regions[2].frames = 128;
    CHECK(env_ckpt_restore_same_identity(&w, &s) == 0,
          "nor one whose region SIZES differ: the frame counts are the environment's shape, not placement");
    s = g; s.index = 1;
    CHECK(env_ckpt_restore_same_identity(&w, &s) == 0,
          "nor one that came back under a different index (the index is the name the registry knows it by)");
    s = g; s.n_tasks = 1;
    CHECK(env_ckpt_restore_same_identity(&w, &s) == 0,
          "nor one missing a task: a one-sidecar environment is a different environment");
    CHECK(env_ckpt_restore_same_identity(&w, NULL) == 0 &&
          env_ckpt_restore_same_identity(NULL, &g) == 0,
          "and a missing side of the comparison is a mismatch, never a pass");

    /* ── 25. The pause, stepped out of for one create and put straight back ──
     * The snapshot's pause is restored at boot, and the create path then
     * refuses a paused target (E4's placement gate). So the replay resumes it,
     * and MUST put it back -- the ledger is what it puts back, never "every
     * partition that has a record". This is the capture's quiesce mirrored;
     * the failure it prevents is a partition an operator froze coming out of a
     * failed restore RUNNING. */
    host_partition_stubs_reset();
    host_partition_define(6);
    host_partition_define(7);
    env_ckpt_reset();
    record_at(6, 0);
    record_at(7, 0);
    env_ckpt_after_restore();
    CHECK(env_ckpt_restore_resume_for_create(6) == 0 && env_ckpt_restore_resumed() == 0,
          "a partition that was NOT paused is left alone (nothing to step out of, nothing owed back)");
    host_partition_set_paused(6, 1);           /* the snapshot restored this pause */
    host_partition_set_paused(7, 1);
    CHECK(env_ckpt_restore_resume_for_create(6) == 1 && !partition_is_paused(6),
          "a partition the snapshot left paused IS resumed for the create (the create path refuses a paused target)");
    CHECK(env_ckpt_restore_resume_for_create(6) == 0 && env_ckpt_restore_resumed() == 1,
          "asking twice resumes once and owes it once (the ledger holds what actually happened)");
    CHECK(env_ckpt_restore_resume_for_create(7) == 1 && env_ckpt_restore_resumed() == 2,
          "a second paused partition joins the ledger");
    CHECK(env_ckpt_restore_repause() == 2 && partition_is_paused(6) && partition_is_paused(7),
          "repause puts back every pause this pass stepped out of -- and only those");
    CHECK(env_ckpt_restore_resumed() == 0 && env_ckpt_restore_repause() == 0,
          "the ledger is cleared by the repause, so a second call is a no-op (idempotent cleanup)");

    /* A partition that dies between the resume and the repause is skipped by
     * the cleanup, not acted on: a release path must never be the thing that
     * faults, and a partition that is gone cannot be put back paused. */
    host_partition_define(8);
    host_partition_set_paused(8, 1);
    CHECK(env_ckpt_restore_resume_for_create(8) == 1 && env_ckpt_restore_resumed() == 1,
          "(setup) a third paused partition is stepped out of for its create");
    host_partition_destroy(8);
    CHECK(env_ckpt_restore_repause() == 0 && env_ckpt_restore_resumed() == 0,
          "...and if it is gone by then, the repause skips it and still clears the ledger");
    CHECK(env_ckpt_restore_resume_for_create(8) == -1 && env_ckpt_restore_resumed() == 0,
          "a partition that is gone is never resumed, and is NOT entered in the ledger (refused, not attempted)");

    printf("\n%d passed, %d failed\n", checks_passed, checks_failed);
    return checks_failed == 0 ? 0 : 1;
}
