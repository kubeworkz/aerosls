/*
 * vec_data_export_import_host_test.c -- VectorStore Interface Roadmap
 * follow-on verification: a standalone host-buildable test for
 * vec_data_export()/vec_data_import() (kernel/vecstore.c), the bulk vector
 * DATA export/import feature that complements vec_schema_export_import_
 * host_test.c's own definitions-only (COLLECTION/INDEX) coverage -- see
 * vecstore.h's own header comment for the full "why this format, why
 * per-collection, why 8192 is genuinely tight, auto-indexing-for-free, and
 * the inherited external_id-dedup gap" writeup this test exercises.
 *
 * Links the REAL, unmodified kernel/vecstore.c AND kernel/vec_index.c --
 * not a reimplementation of either -- reusing vec_schema_export_import_
 * host_test.c's own lighter scaffold (host-declare object_catalog[]/
 * object_catalog_count directly, stub catalog_check_access() behind a
 * controllable flag) rather than the SQL schema test's heavier "link the
 * real object_catalog.c" one, for the identical reason that file's own top
 * comment already gives: vecstore_create_collection()/vecstore_insert()
 * both resolve a plain name directly against object_catalog[] with no
 * sys_sls_valloc()-equivalent multi-step chain to prove out.
 *
 * Build and run:
 *   gcc -Wall -Wextra -std=c11 -I . -I kernel -I drivers -I net \
 *       -o /tmp/vec_data_export_import_host_test \
 *       tests/vec_data_export_import_host_test.c \
 *       kernel/vecstore.c kernel/vec_index.c
 *   /tmp/vec_data_export_import_host_test
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
// extern, matching vec_schema_export_import_host_test.c's own identical
// convention for this same function).
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

// Collects every (external_id, values) a scan visits, for round-trip
// verification -- mirrors vecstore.c's own vs_search_ctx/vs_search_cb
// shape (one static-ish struct, one callback), just gathering everything
// instead of doing a distance comparison.
struct scan_ctx {
    uint64_t ext_ids[32];
    float    vals[32][8];
    uint32_t dims[32];
    uint32_t count;
};
static void scan_cb(struct VecId id, uint64_t external_id, const struct VecValues* v, void* ctxp) {
    (void)id;
    struct scan_ctx* ctx = (struct scan_ctx*)ctxp;
    if (ctx->count >= 32) return;
    ctx->ext_ids[ctx->count] = external_id;
    ctx->dims[ctx->count] = v->count;
    for (uint32_t i = 0; i < v->count && i < 8; i++) ctx->vals[ctx->count][i] = v->values[i];
    ctx->count++;
}
static int scan_count_ext_id(struct scan_ctx* ctx, uint64_t ext_id) {
    int n = 0;
    for (uint32_t i = 0; i < ctx->count; i++) if (ctx->ext_ids[i] == ext_id) n++;
    return n;
}

int main(void) {
    vecstore_init();
    vec_index_init();

    static char out[VEC_DATA_EXPORT_MAX_LEN];
    struct VecDataExportResult er;
    struct VecDataImportResult ir;

    /* ── Fixture: one collection "docs" (dim=4), three inserted vectors. ── */
    add_catalog_entry("docs", 0xD001);
    CHECK(vecstore_create_collection(1, "docs", 4) == 0, "fixture: create 'docs' collection (dim 4)");

    struct VecValues v1 = { 4, { 1.5f, -2.25f, 3.0f, 0.0f } };
    struct VecValues v2 = { 4, { 10.0f, 20.5f, -30.0f, 0.25f } };
    struct VecValues v3 = { 4, { -1.0f, -1.0f, -1.0f, -1.0f } };
    struct VecId id1, id2, id3;
    CHECK(vecstore_insert(1, "docs", 1001, &v1, &id1) == 0, "fixture: insert vector 1001");
    CHECK(vecstore_insert(1, "docs", 1002, &v2, &id2) == 0, "fixture: insert vector 1002");
    CHECK(vecstore_insert(1, "docs", 1003, &v3, &id3) == 0, "fixture: insert vector 1003");

    /* ── Scenario 1: export reconstructs correct VECTOR lines. ─────────── */
    memset(&er, 0, sizeof(er));
    uint32_t n = vec_data_export(1, "docs", 0, out, sizeof(out), &er);
    CHECK(n > 0, "scenario 1: vec_data_export() returns a nonzero byte count");
    CHECK(er.bytes_written == n, "scenario 1b: result->bytes_written matches the return value");
    CHECK(er.vectors_written == 3 && er.vectors_total == 3 && er.truncated == 0,
          "scenario 1c: all 3 vectors written, none truncated");
    CHECK(er.entries_remaining == 0,
          "scenario 1h: entries_remaining is 0 -- this one call already covered the whole collection");
    CHECK(strstr(out, "VECTOR docs 1001 1.500000 -2.250000 3.000000 0.000000") != NULL,
          "scenario 1d: export includes the exact 'VECTOR docs 1001 ...' line for vector 1001");
    CHECK(strstr(out, "VECTOR docs 1002 10.000000 20.500000 -30.000000 0.250000") != NULL,
          "scenario 1e: export includes the exact 'VECTOR docs 1002 ...' line for vector 1002");
    CHECK(strstr(out, "VECTOR docs 1003 -1.000000 -1.000000 -1.000000 -1.000000") != NULL,
          "scenario 1f: export includes the exact 'VECTOR docs 1003 ...' line for vector 1003");
    CHECK(strlen(out) == er.bytes_written, "scenario 1g: out[] is a clean NUL-terminated prefix "
                                           "matching bytes_written exactly");

    /* Capture this export text before dropping the collection below. */
    static char captured[VEC_DATA_EXPORT_MAX_LEN];
    strcpy(captured, out);

    /* ── Scenario 2: real round trip -- drop 'docs' entirely (vecstore_
     * notify_object_freed(), the real cascade sys_sls_vfree() itself
     * calls), recreate it fresh (same dimension), reimport the captured
     * text, and verify all 3 vectors reappear with their exact original
     * values. ─────────────────────────────────────────────────────────── */
    vecstore_notify_object_freed("docs");
    CHECK(vector_collections[0].active == 0, "scenario 2: 'docs' is genuinely dropped (inactive)");

    CHECK(vecstore_create_collection(1, "docs", 4) == 0, "scenario 2b: 'docs' recreated fresh (dim 4, empty)");
    CHECK(vector_collections[0].entry_count == 0, "scenario 2c: freshly recreated 'docs' has zero entries");

    memset(&ir, 0, sizeof(ir));
    vec_data_import(1, captured, &ir);
    CHECK(ir.total == 3 && ir.succeeded == 3 && ir.failed == 0,
          "scenario 2d: reimporting the captured text succeeds for all 3 lines");
    CHECK(vector_collections[0].entry_count == 3, "scenario 2e: 'docs' has exactly 3 entries again");

    struct scan_ctx sc; memset(&sc, 0, sizeof(sc));
    vecstore_collection_scan(1, "docs", scan_cb, &sc);
    CHECK(sc.count == 3, "scenario 2f: scan visits exactly 3 restored entries");
    int found_1001 = 0, found_1002 = 0, found_1003 = 0;
    for (uint32_t i = 0; i < sc.count; i++) {
        if (sc.ext_ids[i] == 1001 && sc.dims[i] == 4 &&
            sc.vals[i][0] == 1.5f && sc.vals[i][1] == -2.25f && sc.vals[i][2] == 3.0f && sc.vals[i][3] == 0.0f)
            found_1001 = 1;
        if (sc.ext_ids[i] == 1002 && sc.dims[i] == 4 &&
            sc.vals[i][0] == 10.0f && sc.vals[i][1] == 20.5f && sc.vals[i][2] == -30.0f && sc.vals[i][3] == 0.25f)
            found_1002 = 1;
        if (sc.ext_ids[i] == 1003 && sc.dims[i] == 4 &&
            sc.vals[i][0] == -1.0f && sc.vals[i][1] == -1.0f && sc.vals[i][2] == -1.0f && sc.vals[i][3] == -1.0f)
            found_1003 = 1;
    }
    CHECK(found_1001, "scenario 2g: restored vector 1001's exact float values round-tripped correctly");
    CHECK(found_1002, "scenario 2h: restored vector 1002's exact float values round-tripped correctly");
    CHECK(found_1003, "scenario 2i: restored vector 1003's exact float values round-tripped correctly");

    /* ── Scenario 3: auto-indexing for free -- create an HNSW index on the
     * now-repopulated 'docs' collection (no retroactive indexing of the 3
     * existing vectors -- that's vec_index_rebuild()'s job, a separate
     * feature), then import ONE MORE vector via vec_data_import() and
     * confirm vec_index_search() finds it, proving vecstore_insert()'s
     * pre-existing vec_index_notify_insert() hook fired automatically on
     * this brand-new import path with zero extra code. ────────────────── */
    CHECK(vec_index_create(1, "idx_docs", "docs", VEC_METRIC_COSINE) == 0,
          "scenario 3: create HNSW index 'idx_docs' on 'docs' (cosine)");
    memset(&ir, 0, sizeof(ir));
    vec_data_import(1, "VECTOR docs 2001 5.000000 6.000000 7.000000 8.000000\n", &ir);
    CHECK(ir.total == 1 && ir.succeeded == 1, "scenario 3b: importing one new vector (2001) succeeds");

    struct VecValues query = { 4, { 5.0f, 6.0f, 7.0f, 8.0f } };
    struct VecMatch matches[4];
    uint32_t found = vec_index_search(1, "idx_docs", &query, 1, 8, matches);
    CHECK(found == 1 && matches[0].external_id == 2001,
          "scenario 3c: vec_index_search() finds the freshly-imported vector 2001 -- "
          "auto-indexing fired with zero extra code in vec_data_import()");

    /* ── Scenario 4: multi-line import with one malformed component
     * (non-numeric token) -- the other lines still run, counts stay
     * accurate. ──────────────────────────────────────────────────────────── */
    memset(&ir, 0, sizeof(ir));
    vec_data_import(1,
        "VECTOR docs 3001 1.000000 2.000000 3.000000 4.000000\n"
        "VECTOR docs 3002 1.000000 NOTANUMBER 3.000000 4.000000\n"
        "VECTOR docs 3003 1.000000 2.000000 3.000000 4.000000\n",
        &ir);
    CHECK(ir.total == 3, "scenario 4: import reports exactly 3 lines found");
    CHECK(ir.succeeded == 2 && ir.failed == 1,
          "scenario 4b: 2 succeed and 1 fails, matching the deliberately-malformed middle line");
    CHECK(ir.lines[0].ok == 1 && ir.lines[2].ok == 1, "scenario 4c: lines 0 and 2 (3001, 3003) are reported ok");
    CHECK(ir.lines[1].ok == 0 && ir.lines[1].error_msg[0] != '\0',
          "scenario 4d: line 1 (3002, bad component) is reported failed with a real error message");

    /* ── Scenario 5: dimension mismatch is reported distinctly from a
     * malformed-token failure. ──────────────────────────────────────────── */
    memset(&ir, 0, sizeof(ir));
    vec_data_import(1, "VECTOR docs 4001 1.000000 2.000000\n", &ir);   // only 2 components, collection dim is 4
    CHECK(ir.total == 1 && ir.failed == 1, "scenario 5: a 2-component line into a dim-4 collection fails");
    CHECK(strstr(ir.lines[0].error_msg, "dimension mismatch") != NULL,
          "scenario 5b: the failure is specifically reported as a dimension mismatch");

    /* ── Scenario 6: a VECTOR line naming a nonexistent collection fails
     * cleanly (vecstore_insert()'s own "collection not found" path), not
     * a crash. ────────────────────────────────────────────────────────────── */
    memset(&ir, 0, sizeof(ir));
    vec_data_import(1, "VECTOR nonexistent_collection 1 1.0 2.0\n", &ir);
    CHECK(ir.total == 1 && ir.failed == 1, "scenario 6: a line naming a nonexistent collection fails cleanly");
    CHECK(strstr(ir.lines[0].error_msg, "collection not found") != NULL,
          "scenario 6b: the failure is specifically reported as 'collection not found'");

    /* ── Scenario 7: comment/blank-line handling -- neither is counted as
     * a real VECTOR line. ────────────────────────────────────────────────── */
    memset(&ir, 0, sizeof(ir));
    vec_data_import(1,
        "# a leading comment, no trailing content expected to matter\n"
        "\n"
        "   \n"
        "VECTOR docs 5001 9.000000 9.000000 9.000000 9.000000\n"
        "# a trailing comment\n",
        &ir);
    CHECK(ir.total == 1, "scenario 7: only the one real VECTOR line is counted "
                         "(comment and blank lines are skipped, not miscounted)");
    CHECK(ir.succeeded == 1 && ir.failed == 0, "scenario 7b: the one real line succeeds");

    /* ── Scenario 8: truncation -- a deliberately small output buffer can't
     * fit the whole collection's data. vectors_written must be strictly
     * less than vectors_total, truncated must be set, and the output
     * buffer must still be a clean, complete (never half-written-line)
     * NUL-terminated prefix. ──────────────────────────────────────────────── */
    static char small_out[80];
    struct VecDataExportResult er2;
    memset(&er2, 0, sizeof(er2));
    uint32_t n8 = vec_data_export(1, "docs", 0, small_out, sizeof(small_out), &er2);
    CHECK(er2.truncated == 1, "scenario 8: a too-small buffer is honestly reported as truncated");
    CHECK(er2.vectors_written < er2.vectors_total,
          "scenario 8b: fewer vectors were written than the collection actually has");
    CHECK(strlen(small_out) == n8 && n8 == er2.bytes_written,
          "scenario 8c: the truncated output is still a clean, complete NUL-terminated prefix "
          "(no half-written trailing line)");
    CHECK(er2.entries_remaining == er2.vectors_total - er2.vectors_written,
          "scenario 8d: entries_remaining accounts for exactly what this call didn't cover "
          "(skip_count was 0, so it's simply vectors_total - vectors_written here)");

    /* ── Scenario 8e (VectorStore Gap Analysis §1.4, closed): pagination --
     * walking the SAME too-small buffer repeatedly, advancing skip_count by
     * each call's own vectors_written, must eventually reach entries_
     * remaining == 0 and, across every call combined, visit every external_
     * id in the collection exactly once (no duplicates, no omissions) --
     * proving the resumption contract vec_data_export()'s own header
     * comment describes, not just that truncation is reported honestly. ─── */
    // A dedicated, differently-sized buffer from small_out[80] above:
    // 150 bytes fits the ~47-byte header comment plus exactly one ~54-56-
    // byte VECTOR line per call (not two), which both guarantees real
    // progress every call (unlike small_out[80], which fits header-only
    // and legitimately writes 0 vectors on its own -- a valid, separately-
    // covered degenerate case, not this scenario's concern) and still
    // forces several pages across 'docs'' several entries by this point.
    uint32_t total_before_pagination = vector_collections[0].entry_count;
    uint32_t skip = 0, pages = 0, seen_total = 0;
    uint64_t seen_ids[64]; uint32_t seen_count = 0;
    for (;;) {
        struct VecDataExportResult per;
        memset(&per, 0, sizeof(per));
        static char page_out[150];
        vec_data_export(1, "docs", skip, page_out, sizeof(page_out), &per);
        CHECK(per.vectors_written > 0 || per.vectors_total == 0,
              "scenario 8e: every page (while entries remain) writes at least one vector "
              "-- an infinite no-progress loop would mean the resumption contract is broken");
        // Parse out each VECTOR line's external_id from this page's text,
        // recording it so the whole walk's coverage can be checked below.
        const char* p = page_out;
        while ((p = strstr(p, "VECTOR docs ")) != NULL) {
            p += 12;
            uint64_t eid = 0;
            while (*p >= '0' && *p <= '9') { eid = eid * 10 + (uint64_t)(*p - '0'); p++; }
            if (seen_count < 64) seen_ids[seen_count++] = eid;
        }
        seen_total += per.vectors_written;
        skip += per.vectors_written;
        pages++;
        if (per.entries_remaining == 0 || per.vectors_written == 0 || pages > 20) break;
    }
    CHECK(seen_total == total_before_pagination,
          "scenario 8f: paginating across enough calls visits every entry in the collection exactly once "
          "(seen_total matches the collection's own entry_count)");
    CHECK(pages > 1, "scenario 8g: the too-small buffer genuinely forced more than one page "
                     "(not a degenerate single-call pass)");
    int dup_found = 0;
    for (uint32_t a = 0; a < seen_count && !dup_found; a++)
        for (uint32_t b = a + 1; b < seen_count; b++)
            if (seen_ids[a] == seen_ids[b]) { dup_found = 1; break; }
    CHECK(!dup_found, "scenario 8h: no external_id was seen twice across the whole paginated walk");

    /* ── Scenario 9: re-importing the same VECTOR line twice duplicates the
     * vector rather than overwriting it -- vecstore_insert() has never
     * deduplicated on external_id (see vecstore.h's own header comment);
     * vec_data_import() calls it exactly as-is, and this is that inherited
     * behavior, named rather than silently assumed. ─────────────────────── */
    uint32_t before_dupe = vector_collections[0].entry_count;
    memset(&ir, 0, sizeof(ir));
    vec_data_import(1, "VECTOR docs 9999 1.000000 1.000000 1.000000 1.000000\n", &ir);
    CHECK(ir.succeeded == 1, "scenario 9: first import of external_id 9999 succeeds");
    memset(&ir, 0, sizeof(ir));
    vec_data_import(1, "VECTOR docs 9999 1.000000 1.000000 1.000000 1.000000\n", &ir);
    CHECK(ir.succeeded == 1, "scenario 9b: reimporting the identical line again also 'succeeds' "
                             "(vecstore_insert() has no uniqueness check to fail against)");
    CHECK(vector_collections[0].entry_count == before_dupe + 2,
          "scenario 9c: entry_count grew by 2, not 1 -- the second import duplicated "
          "the vector instead of overwriting it, confirming the inherited non-dedup gap");
    struct scan_ctx sc9; memset(&sc9, 0, sizeof(sc9));
    vecstore_collection_scan(1, "docs", scan_cb, &sc9);
    CHECK(scan_count_ext_id(&sc9, 9999) == 2,
          "scenario 9d: a scan confirms exactly two distinct entries now carry external_id 9999");

    /* ── Scenario 10: permission-gated export -- when nothing is readable,
     * the export produces zero bytes/vectors, matching vecstore_search()'s
     * own already-documented "0 is ambiguous" posture (not distinguished
     * from "genuinely empty" at this layer). ─────────────────────────────── */
    g_access_force_deny = 1;
    struct VecDataExportResult er10;
    memset(&er10, 0, sizeof(er10));
    uint32_t n10 = vec_data_export(1, "docs", 0, out, sizeof(out), &er10);
    CHECK(n10 == 0 && er10.vectors_written == 0,
          "scenario 10: with access denied, export produces zero bytes and zero vectors");
    g_access_force_deny = 0;
    struct VecDataExportResult er10b;
    memset(&er10b, 0, sizeof(er10b));
    uint32_t n10b = vec_data_export(1, "docs", 0, out, sizeof(out), &er10b);
    CHECK(n10b > 0 && er10b.vectors_written > 0,
          "scenario 10b: with access restored, export is non-trivial again");

    printf("\n%s\n", g_fail == 0 ? "ALL CHECKS PASSED" : "SOME CHECKS FAILED");
    return g_fail == 0 ? 0 : 1;
}
