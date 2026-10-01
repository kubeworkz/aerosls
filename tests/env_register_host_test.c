/*
 * env_register_host_test.c — POSIX-Environments Roadmap v0.2, Phase P1a: the
 * moment an environment stops being a fact init holds and becomes a record the
 * kernel can persist.
 *
 * ─── What this covers, and what the other P1a guards do not ────────────────
 * tests/env_ckpt_host_test.c proves the record layer: the gate, the table, the
 * all-or-nothing adopt. tests/env_ckpt_check.sh proves the wiring is reachable
 * from a boot. NEITHER proves that anything ever PUTS a record in the table —
 * which is the difference between a checkpoint and a file. This test links the
 * real kernel/env_ckpt.c and drives the one entry point that fills it from a
 * live environment:
 *
 *   env_ckpt_register_from() — init's `struct EnvCkptRegister` (the wire body
 *   of an ENV_REGISTER reply, kernel/env_proto.h) plus the kernel's own task
 *   entries and console binding, turned into one stored record, or nothing.
 *
 * ─── The two halves, and who owns which field ──────────────────────────────
 * The interesting assertions here are about the SEAM. init owns the three
 * region bases and their lengths, the four messenger endpoints and the two
 * sidecar names; the kernel owns the format stamps, the console key, and where
 * it released each named sidecar. This test drives both halves separately — as
 * the kernel sees them — so that a mapping that reads the wrong half (a region
 * taken from the struct's field order instead of the wire's, an entry taken
 * from init instead of the process table) fails here rather than silently
 * producing a record that looks complete.
 *
 * ─── The vacuity control ──────────────────────────────────────────────────
 * Every refusal clause asserts TWICE: the named ENV_CKPT_REFUSE_* code came
 * back, AND the table is still empty. A registration that half-applied — the
 * identity stored and the tasks dropped, say — would be an environment a
 * restore brings back with the wrong sidecars, which is worse than one that
 * cold-starts, and "an error was returned" alone would not catch it.
 *
 * The layout pin for the WIRE body — that kernel/env_proto.h's offsets and
 * user/proto/src/env_proto.rs's agree field for field — is
 * tests/env_register_pin_check.sh (source-only, with teeth in its smoke). The
 * byte offsets asserted below are the C half of that pin, written as literals
 * on purpose: a test that read its expected values out of the same macros the
 * encoder used would agree with any layout, including a wrong one.
 *
 * Build and run:
 *   gcc -Wall -Wextra -std=c11 -I . -I kernel -I drivers \
 *       -o /tmp/env_register_host_test tests/env_register_host_test.c kernel/env_ckpt.c
 *   /tmp/env_register_host_test
 */
#include "kernel/env_proto.h"
#include "kernel/env_ckpt.h"
#include "kernel/checkpoint_delta.h"
#include "tests/partition_host_stubs.h"  /* P1a quiesce: env_ckpt.c's partition_pause/_resume/_is_paused/_exists */
#include <stdio.h>
#include <string.h>
#include <stdint.h>

/* ─── ckpt_mark_dirty: recorded, not ignored ──────────────────────────────
 * env_ckpt.c marks CKPT_REGION_ENV on every mutation; without the mark an
 * incremental checkpoint skips the environment records entirely. Recorded so
 * the clauses below can assert it (the same shape env_ckpt_host_test.c uses). */
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

/* A well-formed registration for (partition, index) with `env_id`: the two
 * sidecars, their four messenger ends, and the three regions in the RECORD's
 * order. Every refusal clause below is this registration with exactly one
 * thing changed, so a failure names what drifted. */
static struct EnvCkptRegister good_register(uint32_t partition, uint32_t index,
                                            uint32_t env_id) {
    struct EnvCkptRegister r;
    memset(&r, 0, sizeof r);
    r.partition_id = partition;
    r.index        = index;
    r.env_id       = env_id;
    r.n_regions    = 3;
    r.regions[0].base   = 0x0000000040000000ull; r.regions[0].frames = 1024;
    r.regions[0].kind   = ENV_CKPT_REGION_POSIX_HEAP;
    r.regions[1].base   = 0x0000000050000000ull; r.regions[1].frames = 64;
    r.regions[1].kind   = ENV_CKPT_REGION_RD_HEAP;
    r.regions[2].base   = 0x0000000060000000ull; r.regions[2].frames = 256;
    r.regions[2].kind   = ENV_CKPT_REGION_RD_STORAGE;
    r.n_chans = 4;
    r.chans[0] = 11; r.chans[1] = 12; r.chans[2] = 13; r.chans[3] = 14;
    r.n_tasks = 2;
    memcpy(r.task_name[0], "drv.ramdisk.3", 14);
    r.task_kind[0] = ENV_CKPT_TASK_RAMDISK_SIDECAR;
    memcpy(r.task_name[1], "aerosls.posix.3", 16);
    r.task_kind[1] = ENV_CKPT_TASK_POSIX_SIDECAR;
    return r;
}

/* The kernel's half of the task list: where cap_create_sidecar released each
 * named sidecar. Distinct, nonzero, and NOT what the registration carries — the
 * whole point of the kernel filling these in. */
static const uint64_t k_entries[ENV_CKPT_MAX_TASKS] = {
    0x0000000040001000ull, 0x0000000040002000ull, 0, 0
};

int main(void) {
    /* ── 1. The wire body and the struct are the same 200 bytes ─────────────
     * The struct is a padding-free image of the wire layout, which is what
     * makes the Rust encoder's output parseable as this struct at all. */
    CHECK(ENV_REGISTER_REPLY_BODY_SIZE == 200u,
          "the ENV_REGISTER reply body is 200 bytes");
    CHECK(sizeof(struct EnvCkptRegister) == ENV_REGISTER_REPLY_BODY_SIZE,
          "sizeof(struct EnvCkptRegister) equals the wire body size (no padding hole)");
    CHECK((uint32_t)ENV_REG_OFF_TASK_KINDS == 184u &&
          (uint32_t)ENV_REG_REGION_STRIDE == 16u &&
          (uint32_t)ENV_REG_TASK_NAME_STRIDE == 24u,
          "the offset arithmetic lands where the literals below assert");
    CHECK(ENV_CKPT_MAX_REGIONS == 3 && ENV_CKPT_MAX_CHANS == 4 &&
          ENV_CKPT_MAX_TASKS == 4 && ENV_CKPT_NAME_LEN == 24,
          "the counts and name length the body is built from are the record's");

    /* ── 2. The byte offsets, as literals ───────────────────────────────────
     * These are the numbers user/proto/src/env_proto.rs writes to. Asserted
     * against the encoder's own output so a field added, removed or reordered
     * on either side shows up here. */
    {
        struct EnvCkptRegister r = good_register(0x0A0B0C0Du, 0x01020304u, 0x11223344u);
        uint8_t b[ENV_REGISTER_REPLY_BODY_SIZE];
        env_register_reply_encode(b, &r);
        CHECK(env_read_u32(b, 0)  == 0x0A0B0C0Du, "offset 0: partition");
        CHECK(env_read_u32(b, 4)  == 0x01020304u, "offset 4: index");
        CHECK(env_read_u32(b, 8)  == 0x11223344u, "offset 8: env_id");
        CHECK(env_read_u32(b, 12) == 3u,          "offset 12: n_regions");
        /* Region 0 at 16, 16 bytes apart, base low word first. */
        CHECK(env_read_u32(b, 16) == 0x40000000u && env_read_u32(b, 20) == 0u,
              "offset 16/20: region[0].base is little-endian, low word first");
        CHECK(env_read_u32(b, 24) == 1024u, "offset 24: region[0].frames");
        CHECK(env_read_u32(b, 28) == ENV_CKPT_REGION_POSIX_HEAP,
              "offset 28: region[0].kind");
        CHECK(env_read_u32(b, 32) == 0x50000000u && env_read_u32(b, 48) == 0x60000000u,
              "offsets 32/48: regions 1 and 2 start one stride (16) apart");
        CHECK(env_read_u32(b, 64) == 4u, "offset 64: n_chans");
        CHECK(env_read_u32(b, 68) == 11u && env_read_u32(b, 80) == 14u,
              "offsets 68..80: the four channel handles, in order");
        CHECK(env_read_u32(b, 84) == 2u, "offset 84: n_tasks");
        CHECK(memcmp(b + 88, "drv.ramdisk.3", 13) == 0 && b[101] == 0,
              "offset 88: task 0's name, NUL-padded to 24 bytes");
        CHECK(memcmp(b + 112, "aerosls.posix.3", 15) == 0 && b[127] == 0,
              "offset 112: task 1's name, one name stride (24) later");
        CHECK(env_read_u32(b, 184) == ENV_CKPT_TASK_RAMDISK_SIDECAR &&
              env_read_u32(b, 188) == ENV_CKPT_TASK_POSIX_SIDECAR,
              "offsets 184/188: the task kinds sit after every name");
    }

    /* ── 3. Encode → parse round-trips field for field ───────────────────── */
    {
        struct EnvCkptRegister in = good_register(6, 3, 42);
        uint8_t b[ENV_REGISTER_REPLY_BODY_SIZE];
        struct EnvCkptRegister out;
        memset(&out, 0xAA, sizeof out);
        env_register_reply_encode(b, &in);
        CHECK(env_register_reply_parse(b, sizeof b, &out) == 1,
              "the reply body parses");
        CHECK(memcmp(&in, &out, sizeof in) == 0,
              "encode → parse round-trips the registration byte for byte");
        /* A short body is refused rather than half-read: a partial
         * registration is exactly the half-applied record P1a refuses. */
        CHECK(env_register_reply_parse(b, sizeof b - 1, &out) == 0,
              "a body one byte short is refused, not half-read");
        CHECK(env_register_reply_parse(b, ENV_FRAME_SIZE, &out) == 0,
              "a frame's worth of bytes is not a registration");
    }

    /* ── 4. Registration stores one complete record ──────────────────────── */
    env_ckpt_reset();
    g_dirty_mask = 0;
    g_mark_calls = 0;
    {
        struct EnvCkptRegister r = good_register(6, 3, 42);
        int rc = env_ckpt_register_from(&r, k_entries, /*console_id*/ 3, /*sequence*/ 9);
        CHECK(rc == 0, "a well-formed registration is accepted");
        CHECK(env_ckpt_count_live() == 1, "and it leaves exactly one live record");
        CHECK((g_dirty_mask & (1u << CKPT_REGION_ENV)) != 0 && g_mark_calls >= 1,
              "registering marks CKPT_REGION_ENV dirty (an unmarked region is skipped "
              "by an incremental checkpoint)");

        const struct EnvCkptRecord* rec = env_ckpt_find(6, 3);
        CHECK(rec != 0, "the record is findable by the (partition, index) it names");
        if (rec) {
            CHECK(env_ckpt_valid(rec) == ENV_CKPT_REFUSE_NONE,
                  "and it passes the same gate every other path goes through");
            CHECK(rec->magic == ENV_CKPT_REC_MAGIC && rec->version == ENV_CKPT_REC_VERSION,
                  "the kernel stamped its own magic and version, not init's");
            CHECK(rec->size == (uint32_t)sizeof(struct EnvCkptRecord),
                  "the record carries this build's size (the field that catches a "
                  "same-version rebuild with a different layout)");
            CHECK(rec->sequence == 9 && rec->console_id == 3,
                  "the sequence and the console binding come from the kernel");
            CHECK(rec->partition_id == 6 && rec->index == 3 && rec->env_id == 42,
                  "the identity comes from init's registration");
            CHECK(rec->n_regions == 3 && rec->n_chans == 4 && rec->n_tasks == 2,
                  "the counts survive the crossing");
            CHECK(rec->regions[0].base == 0x40000000ull &&
                  rec->regions[0].kind == ENV_CKPT_REGION_POSIX_HEAP &&
                  rec->regions[1].kind == ENV_CKPT_REGION_RD_HEAP &&
                  rec->regions[2].kind == ENV_CKPT_REGION_RD_STORAGE,
                  "each region kept its kind AND its base (the swap the kinds exist to catch)");
            CHECK(rec->chans[0] == 11 && rec->chans[3] == 14,
                  "all four messenger endpoints crossed intact");
            CHECK(strcmp(rec->tasks[0].name, "drv.ramdisk.3") == 0 &&
                  strcmp(rec->tasks[1].name, "aerosls.posix.3") == 0,
                  "both sidecar names crossed intact");
            CHECK(rec->tasks[0].kind == ENV_CKPT_TASK_RAMDISK_SIDECAR &&
                  rec->tasks[1].kind == ENV_CKPT_TASK_POSIX_SIDECAR,
                  "each task kept its kind");
            /* THE SEAM: the entry is the KERNEL's, from the process table, not
             * anything the registration carried. */
            CHECK(rec->tasks[0].entry == k_entries[0] && rec->tasks[1].entry == k_entries[1],
                  "each task's entry is the kernel's — where it released the sidecar");
            CHECK(rec->tasks[0].entry != rec->regions[0].base,
                  "and is demonstrably not just the region base copied across");
        }
    }

    /* ── 5. Re-registering an environment upserts, never duplicates ───────── */
    {
        struct EnvCkptRegister r = good_register(6, 3, 77);
        CHECK(env_ckpt_register_from(&r, k_entries, 3, 10) == 0,
              "a second registration for the same (partition, index) is accepted");
        CHECK(env_ckpt_count_live() == 1,
              "it UPDATES the environment's record rather than adding a second");
        const struct EnvCkptRecord* rec = env_ckpt_find(6, 3);
        CHECK(rec && rec->env_id == 77 && rec->sequence == 10,
              "and the newer identity and sequence are what the record holds");
    }

    /* ── 6. Every refusal names its reason AND stores nothing ─────────────
     * The vacuity control: after each of these the table must be exactly as
     * empty as before, because a half-registered environment is the failure
     * P1a's refusal-over-partial-application exists for. */
#define REFUSAL_CASE(mutate, expect, label) do {                              \
        env_ckpt_reset();                                                     \
        struct EnvCkptRegister bad = good_register(6, 8, 42);                 \
        mutate;                                                               \
        int rc = env_ckpt_register_from(&bad, k_entries, 3, 11);              \
        CHECK(rc == (expect), label " is refused (" #expect ")");              \
        CHECK(env_ckpt_count_live() == 0 && env_ckpt_find(6, 8) == 0,          \
              label " leaves NO environment behind (refusal, not partial apply)"); \
    } while (0)

    REFUSAL_CASE(memset(bad.task_name[0], 'x', ENV_CKPT_NAME_LEN),
                 ENV_CKPT_REFUSE_BAD_TASK,
                 "a task name with no terminator in its field");
    REFUSAL_CASE(bad.regions[2].kind = ENV_CKPT_REGION_POSIX_HEAP,
                 ENV_CKPT_REFUSE_BAD_REGIONS,
                 "two regions claiming the same kind (a swap that moved bases only)");
    REFUSAL_CASE(bad.n_regions = 2, ENV_CKPT_REFUSE_BAD_REGIONS,
                 "a registration missing a region");
    REFUSAL_CASE(bad.regions[1].frames = 0, ENV_CKPT_REFUSE_BAD_REGIONS,
                 "a region of zero frames");
    REFUSAL_CASE(bad.partition_id = 0xFFFFFFFFu, ENV_CKPT_REFUSE_BAD_IDENTITY,
                 "a registration naming no partition");
    REFUSAL_CASE(bad.task_kind[1] = 7, ENV_CKPT_REFUSE_BAD_TASK,
                 "a task of a kind this build does not know");
#undef REFUSAL_CASE

    /* A registration asking for more of something than a record holds is
     * refused by COUNT, before any copy — not truncated to fit. */
    {
        env_ckpt_reset();
        struct EnvCkptRegister bad = good_register(6, 8, 42);
        bad.n_chans = ENV_CKPT_MAX_CHANS + 1;
        CHECK(env_ckpt_register_from(&bad, k_entries, 3, 11) == ENV_CKPT_REFUSE_BAD_TASKS,
              "n_chans past what a record holds is refused by count");
        bad = good_register(6, 8, 42);
        bad.n_regions = ENV_CKPT_MAX_REGIONS + 1;
        CHECK(env_ckpt_register_from(&bad, k_entries, 3, 11) == ENV_CKPT_REFUSE_BAD_REGIONS,
              "n_regions past what a record holds is refused by count");
        CHECK(env_ckpt_count_live() == 0,
              "neither over-counting registration stored anything");
    }

    /* ── 7. An unplaceable sidecar refuses the WHOLE registration ──────────
     * `task_entry` 0 means the kernel could not resolve the name it was asked
     * about — a sidecar that died between the create and the registration, or a
     * name that never registered. Storing the environment anyway would put a
     * record on disk that names a restore target nobody can find, so it is a
     * refusal (and the environment simply is not checkpointed). */
    {
        env_ckpt_reset();
        uint64_t half[ENV_CKPT_MAX_TASKS] = { k_entries[0], 0, 0, 0 };
        struct EnvCkptRegister r = good_register(6, 9, 42);
        CHECK(env_ckpt_register_from(&r, half, 3, 12) == ENV_CKPT_REFUSE_BAD_TASK,
              "a sidecar the kernel cannot place refuses the registration");
        CHECK(env_ckpt_count_live() == 0,
              "and the environment is left OUT of the checkpoint rather than half in it");
        /* The refusal is a queryable VALUE, not a log line: the kernel's own
         * serial report renders it from this code. */
        struct EnvCkptRefusal ref = env_ckpt_last_refusal();
        char text[96];
        memset(text, 0x7F, sizeof text);
        env_ckpt_refusal_text(&ref, text, (uint32_t)sizeof text);
        CHECK(ref.code == ENV_CKPT_REFUSE_BAD_TASK && text[0] != '\0' &&
              memchr(text, '\0', sizeof text) != 0 &&
              (uint32_t)strlen(text) < (uint32_t)sizeof text,
              "the refusal is the same code, and it renders as a terminated reason");
    }

    /* ── 8. The table's capacity is a refusal, not an overwrite ──────────── */
    {
        env_ckpt_reset();
        for (uint32_t i = 0; i < ENV_CKPT_MAX; i++) {
            struct EnvCkptRegister r = good_register(6, i, 100u + i);
            CHECK(env_ckpt_register_from(&r, k_entries, i, 1) == 0,
                  "filling the table: registration accepted");
        }
        struct EnvCkptRegister over = good_register(6, ENV_CKPT_MAX, 999);
        CHECK(env_ckpt_register_from(&over, k_entries, 0, 1) == ENV_CKPT_REFUSE_FULL,
              "one registration past ENV_CKPT_MAX is refused (FULL)");
        CHECK(env_ckpt_count_live() == ENV_CKPT_MAX,
              "and the refused one displaced nothing");
    }

    printf("\n%d checks passed, %d failed\n", checks_passed, checks_failed);
    if (checks_failed == 0) {
        printf("ALL PASS: the environment manager's registration becomes a record\n");
        return 0;
    }
    printf("%d FAILURE(S): the ENV_REGISTER → record mapping is wrong\n", checks_failed);
    return 1;
}
