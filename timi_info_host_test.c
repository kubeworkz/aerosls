/*
 * timi_info_host_test.c — Gap Remediation Phase G verification: a standalone
 * host-buildable test for loader.c's new loader_timi_info_query() and
 * timi_translate.c's timi_activation_query(), linked against the REAL,
 * unmodified kernel/loader.c, kernel/timi_translate.c, and kernel/timi_x86.c
 * (the portable translator core, byte-identical to the host toolchain's own
 * copy) — not a reimplementation. No prior host test exists for either
 * loader.c or timi_translate.c, so this is the first; the stub set below
 * follows the same "minimal-stub pattern for cross-subsystem dependencies
 * this test has no interest in exercising" precedent as every other host
 * test in this project (e.g. frame_quota_host_test.c, persist_partition_
 * host_test.c).
 *
 * Real dependencies stubbed here, and why each is safe to fake for THIS
 * test's purposes (it never calls loader_load_into_process(), sys_sls_load(),
 * or program_load() — only sys_sls_upload_binary(), loader_timi_info_query(),
 * and timi_translate_and_map()):
 *   - object_catalog[] / object_catalog_count — sys_sls_upload_binary() only
 *     reads these to resolve an object_id for logging; an empty catalog is
 *     fine, obj_id just stays 0.
 *   - persist_programs() — called on the last upload chunk; a real no-op
 *     here since this test never restarts to check persistence.
 *   - process_create() / program_spawn() — never called by anything this
 *     test exercises; present only because process.h is transitively
 *     included and loader.c/timi_translate.c reference them elsewhere in
 *     the same translation unit (unused code paths still need to link).
 *   - user_map_page() — a real no-op; this test never walks a real page
 *     table, it only checks that timi_translate_and_map() reaches the
 *     point of allocating/"mapping" frames and populates the activation
 *     cache, not that paging hardware would accept the result.
 *   - allocate_physical_ram_frame() / _for_partition() — return real,
 *     distinct, writable 4 KiB buffers from a small fake frame pool so
 *     timi_translate_and_map()'s actual frame-fill logic (tt_memcpy of
 *     translated code into each "frame") runs for real, not against NULL.
 *   - timi_rt_resolve/objsize/objtype — the three v0.3 RESOLVE/OBJSIZE/
 *     OBJTYPE runtime bindings (timi_runtime.c, which itself needs
 *     object_catalog.h + process.h + user/permissions.h — a bigger surface
 *     than this test needs). Never actually invoked: the one real TIMI
 *     object translated below (tools/timi/tests/add.timi's assembled
 *     output) uses no RESOLVE/OBJSIZE/OBJTYPE opcodes. Stubbed instead of
 *     linked for real, matching this test's narrow scope.
 *
 * kernel_serial_print()/kernel_serial_printf() are faked no-ops, same as
 * every other host test in this project (kernel_io.h's only two functions,
 * small enough surface to fake outright rather than link kernel_io.c's
 * real serial-port driver).
 *
 * Build and run:
 *   gcc -Wall -Wextra -std=c11 -I kernel -o /tmp/timi_info_host_test \
 *       timi_info_host_test.c kernel/loader.c kernel/timi_translate.c \
 *       kernel/timi_x86.c
 *   /tmp/timi_info_host_test
 */
#include "kernel/loader.h"
#include "kernel/object_catalog.h"
#include "kernel/process.h"
#include "kernel/timi_translate.h"
#include "kernel/frame_pool.h"
#include "arch/x86/user_paging.h"
#include <stdio.h>
#include <stdint.h>
#include <string.h>

// ─── kernel_io.h fakes ──────────────────────────────────────────────────────
void kernel_serial_print(const char* s) { (void)s; }
void kernel_serial_printf(const char* fmt, ...) { (void)fmt; }

// ─── object_catalog.h storage (unused content, just needs to exist) ───────
struct SLSObjectEntry  object_catalog[CATALOG_MAX_OBJECTS];
uint32_t                object_catalog_count = 0;

// ─── persist.h / process.h stubs (unreached code paths) ────────────────────
void persist_programs(void) {}
uint32_t process_create(struct ProcCreateRequest* req) { (void)req; return 0; }
uint32_t program_spawn(const char* object_name, uint32_t owner_uid) {
    (void)object_name; (void)owner_uid; return 0;
}

// ─── arch/x86/user_paging.h stub ────────────────────────────────────────────
void user_map_page(uint64_t* pml4, uint64_t vaddr, uint64_t paddr, uint64_t flags) {
    (void)pml4; (void)vaddr; (void)paddr; (void)flags;
}

// ─── frame_pool.h stubs — real, distinct, writable fake frames ─────────────
#define FAKE_FRAME_COUNT 64
static uint8_t g_fake_frames[FAKE_FRAME_COUNT][4096];
static int     g_fake_frame_idx = 0;
void* allocate_physical_ram_frame(void) {
    if (g_fake_frame_idx >= FAKE_FRAME_COUNT) return 0;
    return g_fake_frames[g_fake_frame_idx++];
}
void* allocate_physical_ram_frame_for_partition(uint32_t partition_id) {
    (void)partition_id;
    return allocate_physical_ram_frame();
}

// ─── timi_runtime.h stubs — never actually invoked, see top comment ────────
uint64_t timi_rt_resolve(const char* name)      { (void)name; return 0; }
uint64_t timi_rt_objsize(uint64_t base_vaddr)   { (void)base_vaddr; return 0; }
uint64_t timi_rt_objtype(uint64_t base_vaddr)   { (void)base_vaddr; return 0xFFFFFFFFu; }

// ─── Test harness ────────────────────────────────────────────────────────────
static int g_fail = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); g_fail++; } \
    else          { printf("ok:   %s\n", msg); } \
} while (0)

// Real assembled bytes of tools/timi/tests/add.timi (timi-asm, unmodified
// project toolchain): 6 instructions, 0 literals, 1 entry ("main" @ offset
// 0), 0 names — a genuinely valid, real TIMI object, not a synthetic stand-
// in, so the OK-status scenario below exercises the exact same byte shape
// a real upload would contain.
static const uint8_t g_add_tmo[] = {
  0x54, 0x49, 0x4d, 0x31, 0x06, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x40, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x19, 0x21, 0x00, 0x00, 0x00, 0x00, 0x00, 0x20, 0x0e,
  0x31, 0x00, 0x00, 0x00, 0x00, 0x04, 0x20, 0x0e, 0x10, 0x00, 0x00, 0x00,
  0x00, 0x08, 0x20, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x20, 0x0d,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x14, 0x6d, 0x61, 0x69, 0x6e,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};

// Builds a byte-valid (per timi_validate()'s own size arithmetic) TIMI
// object with the given header counts and entry/name tables. Instruction
// and literal words are left zeroed -- fine, since every test using this
// builder only ever calls loader_timi_info_query() (a parse, never an
// execution) against the result, not timi_translate_and_map().
static uint32_t build_timi(uint8_t* buf,
                            uint32_t num_instr, uint32_t num_literals,
                            const char** entry_names, const uint32_t* entry_offsets, uint32_t num_entries,
                            const char** names, uint32_t num_names) {
    uint32_t pos = 0;
    struct TimiObjectHeader hdr;
    hdr.magic = TIMI_MAGIC;
    hdr.num_instr = num_instr;
    hdr.num_literals = num_literals;
    hdr.num_entries = num_entries;
    hdr.num_names = num_names;
    memcpy(buf + pos, &hdr, sizeof(hdr)); pos += sizeof(hdr);
    for (uint32_t i = 0; i < num_instr; i++) { uint64_t z = 0; memcpy(buf + pos, &z, 8); pos += 8; }
    for (uint32_t i = 0; i < num_literals; i++) { uint64_t z = 0; memcpy(buf + pos, &z, 8); pos += 8; }
    for (uint32_t i = 0; i < num_entries; i++) {
        struct TimiEntryRec e; memset(&e, 0, sizeof(e));
        strncpy(e.name, entry_names[i], sizeof(e.name) - 1);
        e.offset = entry_offsets[i];
        memcpy(buf + pos, &e, sizeof(e)); pos += sizeof(e);
    }
    for (uint32_t i = 0; i < num_names; i++) {
        struct TimiNameRec n; memset(&n, 0, sizeof(n));
        strncpy(n.name, names[i], sizeof(n.name) - 1);
        memcpy(buf + pos, &n, sizeof(n)); pos += sizeof(n);
    }
    return pos;
}

static void upload(const char* name, const uint8_t* data, uint32_t len) {
    struct SLSUploadRequest req;
    memset(&req, 0, sizeof(req));
    strncpy(req.object_name, name, PROC_NAME_LEN - 1);
    memcpy(req.chunk, data, len);
    req.chunk_len   = len;
    req.byte_offset = 0;
    req.is_last     = 1;
    uint64_t rc = sys_sls_upload_binary(&req);
    if (rc != 0) { printf("FAIL: upload('%s') returned %llu, expected 0\n", name, (unsigned long long)rc); g_fail++; }
}

int main(void) {
    loader_init();

    // ── Scenario 1: unknown object name -> NOT_FOUND, *out fully zeroed ──────
    struct TimiInfoResult r1;
    memset(&r1, 0xAA, sizeof(r1));   // poison first, so a real fill is provable
    int ok1 = loader_timi_info_query("nosuchobject", &r1);
    CHECK(ok1 == 0, "scenario 1: query for an unknown object returns 0");
    CHECK(r1.status == TIMI_INFO_STATUS_NOT_FOUND, "scenario 1: status == NOT_FOUND");
    CHECK(r1.num_instr == 0 && r1.entries_returned == 0 && r1.names_returned == 0,
          "scenario 1: counts are zeroed, not left poisoned -- confirms the "
          "'always fully initialize before any early return' contract");
    CHECK(r1.activation.cached == 0, "scenario 1: activation.cached == 0 on a not-found object");

    // ── Scenario 2: a real, already-existing flat binary -> NOT_TIMI ─────────
    upload("demoprog", aerosls_demo_bin, aerosls_demo_bin_size);
    struct TimiInfoResult r2;
    int ok2 = loader_timi_info_query("demoprog", &r2);
    CHECK(ok2 == 0, "scenario 2: query on a non-TIMI object returns 0");
    CHECK(r2.status == TIMI_INFO_STATUS_NOT_TIMI, "scenario 2: status == NOT_TIMI");
    CHECK(strcmp(r2.format_name, "flat") == 0, "scenario 2: format_name == 'flat' (aerosls_demo_bin is a flat binary, not ELF or TIMI)");

    // ── Scenario 3: TIMI magic present, but truncated -> CORRUPT ─────────────
    // Build a genuinely valid object (num_instr=1) but only upload its
    // 20-byte header -- timi_validate()'s own size arithmetic (header +
    // num_instr*8 + ...) then expects 28 bytes against an actual 20,
    // correctly refusing it rather than reading past the buffer.
    uint8_t corrupt_buf[128];
    uint32_t corrupt_full_len = build_timi(corrupt_buf, 1, 0, 0, 0, 0, 0, 0);
    (void)corrupt_full_len;
    upload("corruptobj", corrupt_buf, sizeof(struct TimiObjectHeader));  // truncated on purpose
    struct TimiInfoResult r3;
    int ok3 = loader_timi_info_query("corruptobj", &r3);
    CHECK(ok3 == 0, "scenario 3: query on a truncated/corrupt TIMI object returns 0");
    CHECK(r3.status == TIMI_INFO_STATUS_CORRUPT, "scenario 3: status == CORRUPT");

    // ── Scenario 4: a real, valid TIMI object (assembled add.timi) -> OK ─────
    upload("addobj", g_add_tmo, (uint32_t)sizeof(g_add_tmo));
    struct TimiInfoResult r4;
    int ok4 = loader_timi_info_query("addobj", &r4);
    CHECK(ok4 == 1, "scenario 4: query on a real, valid TIMI object returns 1");
    CHECK(r4.status == TIMI_INFO_STATUS_OK, "scenario 4: status == OK");
    CHECK(r4.num_instr == 6, "scenario 4: num_instr == 6 (matches add.timi's real assembled header)");
    CHECK(r4.num_literals == 0, "scenario 4: num_literals == 0");
    CHECK(r4.num_entries == 1 && r4.entries_returned == 1, "scenario 4: exactly 1 entry, none truncated");
    CHECK(strcmp(r4.entries[0].name, "main") == 0, "scenario 4: entry[0].name == 'main'");
    CHECK(r4.entries_truncated == 0, "scenario 4: entries_truncated == 0 (1 <= cap of 16)");
    CHECK(r4.names_returned == 0 && r4.names_truncated == 0, "scenario 4: no names (v0.3 field), none truncated");
    CHECK(r4.activation.cached == 0, "scenario 4: activation.cached == 0 -- never translated/spawned yet");

    // ── Scenario 5: truncation -- more entries/names than the 16-cap ─────────
    char namebuf[20][8];
    const char* entry_names[20];
    uint32_t entry_offsets[20];
    const char* name_names[20];
    for (int i = 0; i < 20; i++) {
        snprintf(namebuf[i], sizeof(namebuf[i]), "e%d", i);
        entry_names[i]   = namebuf[i];
        entry_offsets[i] = (uint32_t)i;
        name_names[i]    = namebuf[i];
    }
    uint8_t trunc_buf[2048];
    uint32_t trunc_len = build_timi(trunc_buf, 0, 0, entry_names, entry_offsets, 20, name_names, 20);
    upload("truncobj", trunc_buf, trunc_len);
    struct TimiInfoResult r5;
    int ok5 = loader_timi_info_query("truncobj", &r5);
    CHECK(ok5 == 1, "scenario 5: query on a valid-but-oversized TIMI object still returns 1 (status OK)");
    CHECK(r5.status == TIMI_INFO_STATUS_OK, "scenario 5: status == OK (truncation is not an error)");
    CHECK(r5.num_entries == 20 && r5.entries_returned == 16, "scenario 5: num_entries reports the true count (20); entries_returned caps at 16");
    CHECK(r5.entries_truncated == 1, "scenario 5: entries_truncated == 1 -- the signal a naive reader of entries_returned alone would miss");
    CHECK(strcmp(r5.entries[0].name, "e0") == 0 && strcmp(r5.entries[15].name, "e15") == 0,
          "scenario 5: the 16 entries actually returned are the first 16 (e0..e15), not garbage or a random subset");
    CHECK(r5.num_names == 20 && r5.names_returned == 16, "scenario 5: num_names reports 20; names_returned caps at 16");
    CHECK(r5.names_truncated == 1, "scenario 5: names_truncated == 1");

    // ── Scenario 6: activation cache reflected end-to-end through a real ─────
    // translation. Calls timi_translate_and_map() directly (bypassing the
    // process-spawn syscall layer this test doesn't exercise) against the
    // same real "addobj" uploaded in scenario 4, then re-queries and
    // confirms loader_timi_info_query() now reports activation.cached == 1
    // with plausible fields -- proving the Phase G refactor's activation
    // sub-object is wired to the REAL Phase 4 cache, not a stub.
    uint64_t fake_pml4[512];
    memset(fake_pml4, 0, sizeof(fake_pml4));
    uint64_t entry_rip = timi_translate_and_map("addobj", 0x400000ULL, fake_pml4);
    CHECK(entry_rip != 0, "scenario 6: timi_translate_and_map() succeeds against the real translator (add.timi has no RESOLVE/OBJSIZE/OBJTYPE calls, so the stubbed runtime bindings are never invoked)");

    struct TimiInfoResult r6;
    int ok6 = loader_timi_info_query("addobj", &r6);
    CHECK(ok6 == 1, "scenario 6: re-query after translation still returns 1 (status OK)");
    CHECK(r6.activation.cached == 1, "scenario 6: activation.cached == 1 after a real translation -- the exact bit Phase G's own item 1 was about exposing structurally");
    CHECK(r6.activation.code_pages >= 1, "scenario 6: activation.code_pages >= 1 (add.timi's translated output is at least one page)");
    CHECK(r6.activation.content_hash != 0, "scenario 6: activation.content_hash is non-zero (a real FNV-1a hash of add.timi's real bytes, not a stub sentinel)");

    // A second translate call against the SAME unchanged object must hit the
    // cache (same content hash/size) rather than re-translate -- confirmed
    // indirectly: the entry RIP must be identical both times, since a cache
    // hit reuses base_vaddr + act->entry_off exactly, while a fresh
    // (re-)translation could in principle produce a different stub offset
    // if anything about the pipeline were non-deterministic.
    uint64_t entry_rip2 = timi_translate_and_map("addobj", 0x400000ULL, fake_pml4);
    CHECK(entry_rip2 == entry_rip, "scenario 6: a second translate_and_map() call against the same unchanged object returns the identical entry RIP (cache hit path)");

    printf("\n%s\n", g_fail == 0 ? "ALL CHECKS PASSED" : "SOME CHECKS FAILED");
    return g_fail == 0 ? 0 : 1;
}
