/*
 * vec_schema_export_import_host_test.c -- VectorStore Interface Roadmap
 * follow-on verification: a standalone host-buildable test for
 * vec_schema_export()/vec_schema_import() (kernel/vec_index.c), the
 * collection/index DEFINITION export/import feature scoped as the
 * VectorStore analog of the SQL Feature-Parity Roadmap's own schema
 * export/import (kernel/sql_exec.c).
 *
 * Links the REAL, unmodified kernel/vec_index.c AND kernel/vecstore.c --
 * not a reimplementation of either -- but follows vec_index_host_test.c's
 * own lighter "host-declare object_catalog[]/object_catalog_count directly,
 * stub catalog_check_access() behind a controllable flag" scaffold rather
 * than sql_schema_export_import_host_test.c's heavier "link the real
 * object_catalog.c + real fake-NVMe persist.c" one. That heavier scaffold
 * exists there because CREATE TABLE only has real effect through the
 * genuine sys_sls_valloc()/sys_sls_schema_set() chain; nothing analogous
 * applies here -- vecstore_create_collection()/vec_index_create() both
 * take a plain collection/table name and resolve it directly against
 * object_catalog[], exactly like vec_index_host_test.c's own existing
 * scenarios already exercise without ever linking object_catalog.c itself.
 *
 * A real, named simplification versus sql_schema_export_import_host_
 * test.c's own permission scenario: catalog_check_access() here is a
 * single global on/off stub (g_access_force_deny), not the real per-uid,
 * per-object engine -- so this suite can only test "every collection is
 * readable" vs "nothing is readable," not per-object ownership nuance.
 * Matches vec_index_host_test.c's own pre-existing stub convention for
 * this exact function; not a new gap this test introduces.
 *
 * Build and run:
 *   gcc -Wall -Wextra -std=c11 -I . -I kernel -I drivers -I net \
 *       -o /tmp/vec_schema_export_import_host_test \
 *       tests/vec_schema_export_import_host_test.c \
 *       kernel/vec_index.c kernel/vecstore.c
 *   /tmp/vec_schema_export_import_host_test
 */
#include "kernel/object_catalog.h"
#include "kernel/loader.h"
#include "kernel/partition.h"
#include "kernel/vecstore.h"
#include "kernel/vec_index.h"
#include "user/permissions.h"
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>

struct SLSObjectEntry  object_catalog[CATALOG_MAX_OBJECTS];
uint32_t               object_catalog_count = 0;

void kernel_serial_print(const char* s) { (void)s; }
void kernel_serial_printf(const char* fmt, ...) { (void)fmt; }

static int g_access_force_deny = 0;
int catalog_check_access(uint32_t uid, const char* obj_name, uint32_t needed_perm) {
    (void)uid; (void)obj_name; (void)needed_perm;
    return g_access_force_deny ? 0 : 1;
}
void* allocate_physical_ram_frame(void) { return malloc(4096); }

int ollama_embed(const struct OllamaEmbedRequest* req, struct OllamaEmbedResponse* resp) {
    (void)req; (void)resp;
    return -1;   // unused by this suite -- see vec_index_host_test.c's identical stub/comment
}

void persist_vecstore_headers(void) { }
void persist_vec_index_defs(void) { }

// Forward-declared exactly the way object_catalog.c declares it (a bare
// extern, not a vecstore.h prototype -- see vec_index_host_test.c's own
// identical comment on this same convention). Used directly below to
// simulate a real collection drop for the round-trip scenario.
extern void vecstore_notify_object_freed(const char* collection_name);

#define FAKE_NVME_MAX_FRAMES 256
static struct { uint64_t lba; uint8_t data[4096]; int used; } g_fake_nvme[FAKE_NVME_MAX_FRAMES];
void* io_sq = (void*)1;
void* io_cq = (void*)1;
static int find_or_alloc_frame(uint64_t lba) {
    for (int i = 0; i < FAKE_NVME_MAX_FRAMES; i++)
        if (g_fake_nvme[i].used && g_fake_nvme[i].lba == lba) return i;
    for (int i = 0; i < FAKE_NVME_MAX_FRAMES; i++)
        if (!g_fake_nvme[i].used) { g_fake_nvme[i].used = 1; g_fake_nvme[i].lba = lba; return i; }
    return -1;
}
int nvme_write_sync(uint64_t lba, const void* buf) {
    int idx = find_or_alloc_frame(lba);
    if (idx < 0) return 1;
    memcpy(g_fake_nvme[idx].data, buf, 4096);
    return 0;
}
int nvme_read_sync(uint64_t lba, void* buf) {
    for (int i = 0; i < FAKE_NVME_MAX_FRAMES; i++)
        if (g_fake_nvme[i].used && g_fake_nvme[i].lba == lba) { memcpy(buf, g_fake_nvme[i].data, 4096); return 0; }
    return 1;
}

static int g_fail = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); g_fail++; } \
    else          { printf("ok:   %s\n", msg); } \
} while (0)

static int add_catalog_entry(const char* name, uint64_t object_id) {
    uint32_t idx = object_catalog_count++;
    memset(&object_catalog[idx], 0, sizeof(object_catalog[idx]));
    strcpy(object_catalog[idx].name, name);
    object_catalog[idx].type = OBJ_TYPE_DB_TABLE;
    object_catalog[idx].object_id = object_id;
    object_catalog[idx].active = 1;
    return (int)idx;
}

static int index_active_by_name(const char* name) {
    for (int i = 0; i < VEC_INDEX_MAX; i++)
        if (vec_indexes[i].active && strcmp(vec_indexes[i].index_name, name) == 0) return 1;
    return 0;
}

int main(void) {
    vecstore_init();
    vec_index_init();

    static char out[VEC_SCHEMA_EXPORT_MAX_LEN];
    struct VecSchemaImportResult ir;

    /* ── Fixture: two collections ("docs" dim=128 with an index, "images"
     * dim=512 with NO index -- collections have no metric of their own,
     * so "images" should export with no metric information at all). ────── */
    add_catalog_entry("docs", 0xD001);
    add_catalog_entry("images", 0xD002);
    CHECK(vecstore_create_collection("docs", 128) == 0, "fixture: create 'docs' collection (dim 128)");
    CHECK(vecstore_create_collection("images", 512) == 0, "fixture: create 'images' collection (dim 512, no index)");
    CHECK(vec_index_create(1, "idx_docs", "docs", VEC_METRIC_COSINE) == 0,
          "fixture: create HNSW index 'idx_docs' on 'docs' (cosine)");

    /* ── Scenario 1: export reconstructs correct COLLECTION/INDEX text. ──── */
    uint32_t n = vec_schema_export(1, out, sizeof(out));
    CHECK(n > 0, "scenario 1: vec_schema_export() returns a nonzero byte count");
    CHECK(strstr(out, "COLLECTION docs DIM 128") != NULL,
          "scenario 1b: export includes 'COLLECTION docs DIM 128'");
    CHECK(strstr(out, "COLLECTION images DIM 512") != NULL,
          "scenario 1c: export includes 'COLLECTION images DIM 512'");
    CHECK(strstr(out, "INDEX idx_docs ON docs METRIC cosine") != NULL,
          "scenario 1d: export includes 'INDEX idx_docs ON docs METRIC cosine'");
    {
        /* images has no index -- no INDEX line should reference it. Checking
         * for "ON images" is a false-positive trap: "COLLECTION images"
         * itself ends in "...ON images" (COLLECTI-ON + " images"), so that
         * substring is present even with zero INDEX lines. "images METRIC"
         * only ever appears in a real "INDEX <name> ON images METRIC <m>"
         * line, so that's the substring actually worth checking for. */
        const char* p = strstr(out, "images METRIC");
        CHECK(p == NULL, "scenario 1e: no INDEX line references 'images' (it has none)");
    }

    /* ── Scenario 2: real round trip -- export, drop both collections
     * (vecstore_notify_object_freed(), the real cascade sys_sls_vfree()
     * itself calls), import the exported text, verify both collections
     * AND the index exist again. ────────────────────────────────────────── */
    uint32_t n2 = vec_schema_export(1, out, sizeof(out));
    CHECK(n2 > 0, "scenario 2: captured export text before dropping the fixture collections");
    vecstore_notify_object_freed("docs");
    vecstore_notify_object_freed("images");
    CHECK(vector_collections[0].active == 0 && vector_collections[1].active == 0,
          "scenario 2b: both collections are genuinely dropped (inactive)");
    CHECK(index_active_by_name("idx_docs") == 0,
          "scenario 2c: idx_docs was cascade-deactivated when its collection was dropped");

    memset(&ir, 0, sizeof(ir));
    vec_schema_import(1, out, &ir);
    CHECK(ir.total == 3, "scenario 2d: import reports exactly 3 definition lines found "
                         "(COLLECTION docs, INDEX idx_docs, COLLECTION images -- images itself "
                         "has no index, so it contributes only its one COLLECTION line)");
    CHECK(ir.failed == 0, "scenario 2e: every line in the round-trip import succeeds");
    CHECK(vector_collections[0].active == 1 && vector_collections[0].dimension == 128,
          "scenario 2f: 'docs' is a real collection again with dimension 128");
    CHECK(vector_collections[1].active == 1 && vector_collections[1].dimension == 512,
          "scenario 2g: 'images' is a real collection again with dimension 512");
    CHECK(index_active_by_name("idx_docs") == 1,
          "scenario 2h: idx_docs exists again on the freshly re-created 'docs' collection");

    /* ── Scenario 3: multi-line import with one deliberately malformed
     * line -- the others still run, and counts are accurate. ───────────────── */
    add_catalog_entry("t1", 0xE001);
    add_catalog_entry("t2", 0xE002);
    memset(&ir, 0, sizeof(ir));
    vec_schema_import(1,
        "COLLECTION t1 DIM 4\n"
        "BOGUS LINE HERE\n"
        "COLLECTION t2 DIM 8\n",
        &ir);
    CHECK(ir.total == 3, "scenario 3: import reports exactly 3 lines found");
    CHECK(ir.succeeded == 2 && ir.failed == 1,
          "scenario 3b: 2 succeed and 1 fails, matching the deliberately-malformed middle line");
    CHECK(ir.lines[0].ok == 1, "scenario 3c: line 0 (t1) is reported ok");
    CHECK(ir.lines[1].ok == 0, "scenario 3d: line 1 (bogus) is reported failed");
    CHECK(ir.lines[2].ok == 1, "scenario 3e: line 2 (t2) still ran and is reported ok "
                               "(the earlier failure did not block it)");
    CHECK(vector_collections[2].active == 1 && vector_collections[2].dimension == 4,
          "scenario 3f: t1 genuinely exists post-import");
    CHECK(vector_collections[3].active == 1 && vector_collections[3].dimension == 8,
          "scenario 3g: t2 genuinely exists post-import");

    /* ── Scenario 4: comment/blank-line handling -- neither is counted as
     * a real definition line. ──────────────────────────────────────────────── */
    add_catalog_entry("t3", 0xE003);
    memset(&ir, 0, sizeof(ir));
    vec_schema_import(1,
        "# a leading comment, no trailing content expected to matter\n"
        "\n"
        "   \n"
        "COLLECTION t3 DIM 16\n"
        "# a trailing comment\n",
        &ir);
    CHECK(ir.total == 1, "scenario 4: only the one real COLLECTION line is counted "
                         "(comment and blank lines are skipped, not miscounted)");
    CHECK(ir.succeeded == 1 && ir.failed == 0, "scenario 4b: the one real line succeeds");
    CHECK(vector_collections[4].active == 1 && vector_collections[4].dimension == 16,
          "scenario 4c: t3 genuinely exists post-import");

    /* ── Scenario 5: an INDEX line referencing a collection that doesn't
     * exist fails cleanly (vec_index_create()'s own "collection not
     * found" path), not a crash. ────────────────────────────────────────────── */
    memset(&ir, 0, sizeof(ir));
    vec_schema_import(1, "INDEX idx_ghost ON nonexistent_collection METRIC l2\n", &ir);
    CHECK(ir.total == 1 && ir.failed == 1 && ir.succeeded == 0,
          "scenario 5: an INDEX line naming a nonexistent collection fails cleanly, not a crash");
    CHECK(ir.lines[0].error_msg[0] != '\0', "scenario 5b: the failure carries a real error message");

    /* ── Scenario 6: permission-gated export -- when nothing is readable,
     * the export contains no COLLECTION lines at all (only the header
     * comment). See this file's own top comment for why this is a
     * binary on/off check, not a per-object one. ────────────────────────────── */
    g_access_force_deny = 1;
    uint32_t n6 = vec_schema_export(1, out, sizeof(out));
    CHECK(strstr(out, "COLLECTION") == NULL,
          "scenario 6: with access denied, the export contains no COLLECTION lines at all");
    g_access_force_deny = 0;
    uint32_t n6b = vec_schema_export(1, out, sizeof(out));
    CHECK(n6b > n6 && strstr(out, "COLLECTION") != NULL,
          "scenario 6b: with access restored, the export is non-trivial again");

    printf("\n%s\n", g_fail == 0 ? "ALL CHECKS PASSED" : "SOME CHECKS FAILED");
    return g_fail == 0 ? 0 : 1;
}
