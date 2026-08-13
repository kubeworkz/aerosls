/*
 * rowstore_io_host_test.c — what the row store does when a write fails.
 *
 * ─── Why rowstore and not something easier ─────────────────────────────────
 * tests/io_fault_host_test.c established the method against kernel/tls_store.c,
 * which checks every nvme_*_sync() return and therefore reports zero swallowed
 * failures. That is the control. This is the case it was built for.
 *
 * rowstore_flush_page() is three lines and returns void:
 *
 *     static void rowstore_flush_page(uint32_t page_id) {
 *         if (page_id >= ROWSTORE_MAX_PAGES || !row_pages[page_id]) return;
 *         nvme_write_sync(ROWSTORE_LBA_BASE + (uint64_t)page_id * 8, row_pages[page_id]);
 *     }
 *
 * Its callers are rowstore_row_insert(), rowstore_row_update() and
 * rowstore_row_delete() -- all of which return an int, and all of which return
 * SUCCESS whatever the disk did. The row is in RAM, the caller is told it is
 * stored, and it is gone on the next boot with nothing in any log.
 *
 * ─── The read path is NOT the same finding, and this says so ───────────────
 * rowstore_read_page() also discards its return, and that one is deliberate:
 * the frame is zeroed before the read, so a failed read yields an empty page
 * rather than garbage, and the source comment says exactly that. Asserting it
 * behaves as documented is worth as much as catching the write -- a sweep that
 * reported both as defects would be one a reader learns to ignore.
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "fake_nvme.h"
#include "rowstore.h"

void stub_catalog_make_table(const char *name);
extern int g_stub_quiet;

static int pass, fail;
static void ok(const char *m)  { printf("ok:   %s\n", m); pass++; }
static void bad(const char *m) { printf("FAIL: %s\n", m); fail++; }

#define UID 0u
#define TBL "t"

static void fresh_table(void)
{
    fake_nvme_reset();
    stub_catalog_make_table(TBL);
    rowstore_init();
    if (rowstore_create_table(TBL) != 0) {
        printf("FAIL: could not create the table -- the harness is broken, not "
               "the row store\n");
        fail++;
    }
}

/* RowValues carries every column as text -- rowstore.c parses per the
 * column's SLSFieldType internally (rowstore.h:175). */
static int insert_row(uint64_t a, uint64_t b, struct RowId *out)
{
    struct RowValues v;
    struct RowId scratch;
    memset(&v, 0, sizeof v);
    v.count = 2;
    snprintf(v.values[0], RECORD_VAL_LEN, "%llu", (unsigned long long)a);
    snprintf(v.values[1], RECORD_VAL_LEN, "%llu", (unsigned long long)b);
    return rowstore_row_insert(UID, TBL, &v, out ? out : &scratch);
}

int main(void)
{
    g_stub_quiet = 1;

    printf("\n=== a row store on a disk that works ===\n\n");
    fresh_table();
    if (insert_row(1, 100, NULL) == 0) {
        ok("an insert succeeds");
    } else {
        bad("a fault-free insert already fails -- nothing below means anything");
    }
    {
        unsigned long w = fake_nvme_writes();
        if (w > 0) {
            printf("      the insert issued %lu write(s), %lu read(s)\n",
                   w, fake_nvme_reads());
            ok("  and reaches the disk at all");
        } else {
            bad("  but issued no write -- there is nothing here to fail");
        }
    }
    if (fake_nvme_violations() == 0) {
        ok("  with no contract violations (pages are 4 KiB aligned)");
    } else {
        bad("  and violated the driver contract -- see stderr");
    }

    printf("\n=== the same insert, with its write failing ===\n\n");
    {
        unsigned long before;
        int rc;
        unsigned char frame[FAKE_NVME_FRAME];

        fresh_table();
        before = fake_nvme_ops();
        fake_nvme_fail_at(before + 1, 1);   /* persistent: every write from now */
        rc = insert_row(1, 100, NULL);

        if (rc == 0) {
            bad("*** rowstore_row_insert() returned SUCCESS while every write "
                "to the disk failed ***\n"
                "      The row is in RAM only. The caller has been told it is "
                "stored. It is\n"
                "      gone on the next boot and nothing has logged anything. "
                "This is\n"
                "      rowstore_flush_page() discarding nvme_write_sync()'s "
                "status.");
        } else {
            ok("rowstore_row_insert() reports the write failure (rc != 0)");
        }

        /* Independently of what the caller was told: is the page actually
         * absent? This is the half that says the data really is lost, rather
         * than merely unreported. */
        if (!fake_nvme_peek(ROWSTORE_LBA_BASE, frame)) {
            printf("      and the page is absent from the disk, confirming the "
                   "row was not stored\n");
        } else {
            printf("      (the page IS on disk -- the write landed after all)\n");
        }
    }

    printf("\n=== the read path, which is deliberately different ===\n\n");
    {
        /* rowstore_read_page() zeroes the frame and then discards the read's
         * status on purpose, so a failed read presents as an empty page. That
         * is documented in rowstore.c and is a reasonable choice; what matters
         * is that it still behaves that way. */
        struct RowValues out;
        int rc;

        fresh_table();
        if (insert_row(7, 700, NULL) != 0) { bad("setup insert failed"); }

        /* Drop the cached page so the next access must read from disk, then
         * fail every read. */
        rowstore_init();
        stub_catalog_make_table(TBL);
        rowstore_create_table(TBL);
        fake_nvme_fail_at(fake_nvme_ops() + 1, 1);
        memset(&out, 0, sizeof out);
        rc = rowstore_row_get(UID, TBL, (struct RowId){0, 0}, &out);

        if (rc != 0) {
            ok("a failed read presents as 'no such row', not as garbage");
        } else {
            bad("a row was returned from a disk that refused every read");
        }
        if (fake_nvme_violations() == 0) {
            ok("  and no contract violation along the way");
        } else {
            bad("  but the driver contract was violated");
        }
    }

    printf("\n---- checks=%d failed=%d\n", pass + fail, fail);
    return fail == 0 ? 0 : 1;
}
