/* rowstore_io_stubs.c — the 26 symbols kernel/rowstore.c needs that are not
 * storage, plus the minimum catalog state that lets a table exist.
 *
 * Deliberately inert. This harness asks what rowstore does when the DISK
 * misbehaves, so the index, the journal, the quota and the catalog persistence
 * all succeed quietly. A stub that did something interesting would make a
 * failure ambiguous, and an ambiguous failure in a fault-injection sweep is
 * worse than no sweep -- it gets explained away. */

#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "object_catalog.h"
#include "rowstore.h"
#include "row_index.h"
#include "row_constraint.h"
#include "row_journal.h"
#include "storage_quota.h"

/* The real arrays, with the real types. rowstore.c reads
 * object_catalog[i].active / .name / .object_id / .uses_rowstore and walks
 * object_schemas[i].fields[] to compute its row layout, so byte arrays would
 * not have done. */
struct SLSObjectEntry  object_catalog[CATALOG_MAX_OBJECTS];
struct SLSObjectSchema object_schemas[CATALOG_MAX_OBJECTS];
uint32_t               object_catalog_count;

/* One table, two UINT64 columns. Small on purpose: a narrow row means many
 * rows per page, so the harness can fill a page and force a second one -- the
 * path where rowstore_flush_page() is called for a page other than the last. */
void stub_catalog_make_table(const char *name)
{
    memset(object_catalog, 0, sizeof object_catalog);
    memset(object_schemas, 0, sizeof object_schemas);
    object_catalog[0].active        = 1;
    object_catalog[0].object_id     = 1;
    object_catalog[0].uses_rowstore = 0;
    snprintf(object_catalog[0].name, sizeof object_catalog[0].name, "%s", name);

    object_schemas[0].object_id   = 1;
    object_schemas[0].field_count = 2;
    object_schemas[0].fields[0].active = 1;
    object_schemas[0].fields[0].type   = FIELD_TYPE_UINT64;
    snprintf(object_schemas[0].fields[0].key,
             sizeof object_schemas[0].fields[0].key, "a");
    object_schemas[0].fields[1].active = 1;
    object_schemas[0].fields[1].type   = FIELD_TYPE_UINT64;
    snprintf(object_schemas[0].fields[1].key,
             sizeof object_schemas[0].fields[1].key, "b");

    object_catalog_count = 1;
}

int  g_stub_quiet = 1;
void kernel_serial_print(const char *s)         { if (!g_stub_quiet) fputs(s, stdout); }
void kernel_serial_printf(const char *fmt, ...) { (void)fmt; }

/* 4 KiB aligned, because the real frame pool is and rowstore hands these
 * straight to nvme_write_sync(). fake_nvme.c enforces it either way; getting
 * it right here means the harness tests rowstore rather than itself. */
void *allocate_physical_ram_frame(void)
{
    void *p = NULL;
    if (posix_memalign(&p, 4096, 4096) != 0) { return NULL; }
    memset(p, 0, 4096);
    return p;
}

/* Signatures taken from the headers, not guessed. A stub whose prototype
 * drifts from the real one is a link that succeeds and a call that corrupts
 * its own arguments -- which in a fault-injection harness would surface as a
 * "finding" in the module under test. */
uint64_t sys_sls_vfree(const char *name) { (void)name; return 0; }
int catalog_check_access(uint32_t uid, const char *obj_name, uint32_t needed_perm)
{ (void)uid; (void)obj_name; (void)needed_perm; return 1; }

void persist_catalog(void)          { }
void persist_rowstore_headers(void) { }
void persist_row_index_defs(void)   { }
void persist_row_constraints(void)  { }
void persist_row_journal(void)      { }

struct RowConstraintDef    row_constraints[ROW_CONSTRAINT_MAX];
struct RowIndex            row_indexes[ROW_INDEX_MAX];
struct RowJournalAttachment row_journal_attachments[ROW_JOURNAL_MAX_ATTACHMENTS];

int row_index_create(uint32_t caller_uid, const char *index_name,
                     const char *table_name, const char *column_name)
{ (void)caller_uid; (void)index_name; (void)table_name; (void)column_name; return 0; }
int row_index_drop(uint32_t caller_uid, const char *index_name)
{ (void)caller_uid; (void)index_name; return 0; }
void row_index_notify_insert(uint64_t oid, struct RowId id,
                             const struct RowValues *v,
                             const struct RowTableLayout *l)
{ (void)oid; (void)id; (void)v; (void)l; }
void row_index_notify_update(uint64_t oid, struct RowId id,
                             const struct RowValues *ov,
                             const struct RowValues *nv,
                             const struct RowTableLayout *l)
{ (void)oid; (void)id; (void)ov; (void)nv; (void)l; }
void row_index_notify_delete(uint64_t oid, struct RowId id,
                             const struct RowValues *v,
                             const struct RowTableLayout *l)
{ (void)oid; (void)id; (void)v; (void)l; }

/* ZERO is success for both. rowstore_alloc_page() reads it as
 *     if (storage_page_reserve(partition_id)) return ROWSTORE_INVALID_PAGE;
 * so a stub returning 1 denies every page and every insert fails with rc=6 --
 * which is what this harness did on its first run, and which looked exactly
 * like a rowstore bug until the return was read instead of assumed. */
int  storage_page_reserve(uint32_t partition_id) { (void)partition_id; return 0; }
int  storage_page_release(uint32_t partition_id, uint64_t count)
{ (void)partition_id; (void)count; return 0; }
