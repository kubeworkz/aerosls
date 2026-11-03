/*
 * env_catalog_host_test.c — §9's interop surface, host guard
 * (POSIX-Environments v0.2 §9.1, kernel/env_catalog.c).
 *
 * Links the REAL kernel/cap.c, kernel/env_catalog.c, kernel/kernel_io.c and
 * kernel/console.c. object_catalog[] / object_records[] /
 * object_catalog_count are DEFINED HERE rather than linking
 * kernel/object_catalog.c, for the same reason the socket test stubs
 * net/tcp.c's data path: object_catalog.c pulls in rowstore, index_mgr,
 * lock_mgr and the whole SQL layer, and this test is not testing those — it
 * is testing the BROKER's boundary. The arrays are the real types from
 * object_catalog.h, so a struct change breaks the build rather than silently
 * drifting the fixture.
 *
 * WHAT THIS TEST ESTABLISHES, in one sentence each of the clauses that
 * matter:
 *
 *   - A channel registered in partition 1 CANNOT see a name that exists in
 *     partition 2: absent from CAT_LIST, and CAT_ERR_NOENT on CAT_STAT and
 *     CAT_READ — the SAME answer as a name that never existed. That is the
 *     property §9.1 exists to establish, and the test asserts the two
 *     answers are indistinguishable rather than merely both errors.
 *   - The partition a verb is filtered by is the one the REGISTRATION
 *     carried, which cap.c filled from pd->partition_id. Nothing on the
 *     wire states it: no CAT_* frame carries a partition field at all.
 *   - SYSTEM_METADATA is refused CAT_ERR_PERM — in the namespace, and still
 *     not served — so PERM and NOENT are visibly different answers.
 *   - A rowstore-backed table is refused CAT_ERR_TYPE on CAT_READ alone;
 *     CAT_STAT still answers, so the refusal is explainable. CAT_QUERY is
 *     how such a table IS read, and the gates it runs in order are pinned
 *     here: parse before lookup, lookup inside the channel's own partition
 *     (a foreign table answers NOENT and NEVER reaches the engine), then
 *     uid from proc_table, then catalog_check_access, then execution — plus
 *     the statement shapes the surface does not carry (non-SELECT, JOIN,
 *     set operator, CTE, subquery), each refused BY NAME.
 *   - An unknown verb is refused BY NUMBER with CAT_ERR_UNKNOWN, which is
 *     how a CAT_WRITE asking for a write path is answered: like any other
 *     unknown, not specially. Verb 5 is CAT_QUERY, which is why 6 is the
 *     first unknown — a READ-only surface that grew a query verb did not
 *     grow a write one.
 *   - A non-CAT magic on a catalog channel is dropped, never answered.
 *
 * WHAT IS FAKED HERE, AND WHY THAT IS THE RIGHT THING TO PIN. The SQL
 * engine itself is not linked — sql_exec.c drags in rowstore, mvcc, index,
 * lock, constraint, view and the rest of the relational stack, and its own
 * host tests cover it. What IS linked is the real sql_parse() (kernel/
 * sql_parser.c + predicate.c), so the statement-shape decisions this
 * service makes are made against the REAL parser rather than a stub that
 * agrees with the code under test. sql_execute()/cursor_* are scripted so
 * this test can assert the ORDER of the gates — in particular that a
 * foreign table or a denied uid never reaches execution at all, which is a
 * claim about the broker, not about the engine.
 *
 * Build and run:
 *   gcc -no-pie -Wall -Wextra -std=c11 -I . -I kernel -I net -I arch/x86 \
 *       -include tests/kernel_io_panic_port.h \
 *       -o /tmp/env_catalog_host_test \
 *       tests/env_catalog_host_test.c kernel/cap.c \
 *       kernel/env_catalog.c kernel/kernel_io.c kernel/console.c \
 *       kernel/linux_abi.c kernel/elf64_load.c kernel/entropy.c \
 *       kernel/sha256.c kernel/sql_parser.c kernel/predicate.c
 *   /tmp/env_catalog_host_test
 */
#include "kernel/cap.h"
#include "kernel/env_catalog.h"
#include "kernel/kernel_io.h"
#include "kernel/object_catalog.h"   /* the REAL entry/record types */
#include "kernel/rowstore.h"         /* table_headers + RowValues: what CAT_QUERY reads */
#include "kernel/sql_exec.h"         /* SqlResult — the engine's answer shape */
#include "kernel/sql_parser.h"       /* sql_parse: the REAL parser, linked below */
#include "kernel/cursor.h"           /* RowScanCb: the engine's row hand-off */
#include "../user/permissions.h"     /* PERM_READ — the bit CAT_QUERY asks for */
#include "tests/process_host_stubs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

static int checks_passed = 0;
static int checks_failed = 0;
#define CHECK(cond, msg) do { \
    /* "ok:" at column 0 is not cosmetic -- tests/run_all.sh counts checks \
     * with grep -c '^ok:'. */ \
    if (cond) { checks_passed++; printf("ok:   %s\n", msg); } \
    else      { checks_failed++; printf("FAIL: %s\n", msg); } \
} while (0)

/* kernel/kernel_io.c's read_line() calls the cooperative yield, whose real
 * definition is in kernel/process.c — a file this test deliberately does not
 * link. Same reasoning as net_socket_service_host_test.c. */
void kernel_yield_to_ring3(uint32_t budget_ticks) { (void)budget_ticks; }

/* ─── cap.c link stubs (the same set net_socket_service_host_test.c uses) ── */
char _kernel_image_end[1];

struct ProcessDescriptor proc_table[PROC_MAX];
uint32_t proc_count = 0;

uint32_t alloc_pid(void) {
    uint32_t best = 0;
    for (int i = 0; i < PROC_MAX; i++)
        if (proc_table[i].active && proc_table[i].pid > best)
            best = proc_table[i].pid;
    return best + 1;
}

uint64_t alloc_proc_syscall_stack(uint32_t partition_id) {
    (void)partition_id;
    return 0x400000007000ULL;
}

uint64_t frame_pool_reserve_contiguous(uint64_t nframes, uint64_t align_frames) {
    (void)align_frames;
    static uint8_t* arena;
    if (!arena) {
        uint8_t* raw = malloc((size_t)nframes * 4096u + 4096);
        if (!raw) return 0;
        arena = (uint8_t*)(((uintptr_t)raw + 4095u) & ~(uintptr_t)4095u);
        memset(arena, 0, (size_t)nframes * 4096u);
    }
    return (uint64_t)(uintptr_t)arena;
}

void* allocate_physical_ram_frame_for_partition(uint32_t partition_id) {
    (void)partition_id;
    uint8_t* raw = malloc(8192);
    if (!raw) return 0;
    return (void*)(((uintptr_t)raw + 4095u) & ~(uintptr_t)4095u);
}

uint64_t allocate_contiguous_frames_for_partition(uint32_t partition_id,
                                                  uint64_t nframes,
                                                  uint64_t align_frames) {
    (void)partition_id; (void)nframes; (void)align_frames;
    return 0;
}

int free_contiguous_frames_for_partition(uint64_t base_addr, uint64_t nframes,
                                         uint32_t partition_id) {
    (void)base_addr; (void)nframes; (void)partition_id;
    return 0;
}

/* kernel/env_storage.c — the durable-store module — is NOT linked: it pulls
 * in NVMe and the extent band, and this test exercises neither. cap.c's
 * sys_sls_env_storage_* entry points reference five of its symbols, so they
 * are link-only placeholders returning NOENT ("unattached"), the answer the
 * real module gives a region it has never seen. Nothing in the catalog
 * service reaches them. */
uint64_t env_storage_attach(uint32_t partition_id, uint32_t index,
                            uint64_t region_base, uint64_t region_bytes) {
    (void)partition_id; (void)index; (void)region_base; (void)region_bytes;
    return 1;   /* ENV_STORAGE_NOENT */
}
uint64_t env_storage_restore(uint64_t region_base) {
    (void)region_base; return 1;
}
uint64_t env_storage_write(uint64_t region_base, uint32_t lba,
                           uint32_t bytes) {
    (void)region_base; (void)lba; (void)bytes; return 1;
}
uint64_t env_storage_release(uint64_t region_base) {
    (void)region_base; return 1;
}
uint32_t env_storage_owner(uint64_t region_base) {
    (void)region_base; return 0;
}

uint64_t user_clone_page_table(void) { return 0; }
void user_map_page(uint64_t* pml4, uint64_t vaddr, uint64_t paddr,
                   uint64_t flags) {
    (void)pml4; (void)vaddr; (void)paddr; (void)flags;
}
int user_map_identity(uint64_t* pml4, uint64_t phys, uint32_t npages,
                      uint64_t flags) {
    (void)pml4; (void)phys; (void)npages; (void)flags;
    return 0;
}

/* ─── kernel_io.c stubs ──────────────────────────────────────────────────── */
int  vga_is_ready(void)        { return 0; }
void vga_putchar(char c)       { (void)c; }
void vga_init(void)            { }
void vga_clear(void)           { }
void vga_print(const char *s)  { (void)s; }
int  smp_cpu_id(void)          { return 0; }
int  smp_cpu_count(void)       { return 1; }
void smp_uniprocessor_tick(void) { }

static uint8_t g_rx[512];
static int     g_rx_n, g_rx_i;
void kio_test_outb(uint16_t port, uint8_t val) { (void)port; (void)val; }
uint8_t kio_test_inb(uint16_t port) {
    switch ((int)(port - SERIAL_COM1_BASE)) {
    case 0: return (g_rx_i < g_rx_n) ? g_rx[g_rx_i++] : 0;
    case 5: return 0x20 | ((g_rx_i < g_rx_n) ? 0x01 : 0x00);
    default: return 0;
    }
}

/* ─── the catalog fixture ──────────────────────────────────────────────────
 * Defined here instead of linking object_catalog.c — see the header comment.
 * The types are the real ones, so this cannot drift from the service's view
 * of the world unnoticed. Partition 1 is the tenant under test; partition 2
 * is the neighbour whose objects must stay invisible. */
struct SLSObjectEntry  object_catalog[CATALOG_MAX_OBJECTS];
struct SLSObjectRecord object_records[CATALOG_MAX_OBJECTS];
struct SLSObjectSchema object_schemas[CATALOG_MAX_OBJECTS];
struct SLSRoleEntry    role_table[ROLE_TABLE_MAX];
uint32_t               object_catalog_count = 0;

static void add_object(const char* name, SLSObjectType type,
                       uint32_t partition, uint32_t size_pages,
                       uint8_t rowstore) {
    uint32_t i = object_catalog_count++;
    struct SLSObjectEntry* o = &object_catalog[i];
    memset(o, 0, sizeof *o);
    o->active = 1;
    o->type = type;
    o->partition_id = partition;
    o->size_pages = size_pages;
    o->owner_uid = 1000 + partition;
    o->storage_tier = STORAGE_TIER_L2_DRAM;
    o->owner_role = ROLE_APP_USER;
    o->perm_mask = 0x4;             /* the read bit */
    o->uses_rowstore = rowstore;
    uint32_t j = 0;
    for (; name[j] && j + 1 < OBJECT_NAME_LEN; j++) o->name[j] = name[j];
    o->name[j] = '\0';
}

static void add_field(uint32_t obj_idx, const char* key, const char* value) {
    struct SLSObjectRecord* r = &object_records[obj_idx];
    r->object_id = object_catalog[obj_idx].object_id;
    uint32_t f = r->field_count++;
    uint32_t j = 0;
    for (; key[j] && j + 1 < RECORD_KEY_LEN; j++) r->fields[f].key[j] = key[j];
    r->fields[f].key[j] = '\0';
    j = 0;
    for (; value[j] && j + 1 < RECORD_VAL_LEN; j++)
        r->fields[f].value[j] = value[j];
    r->fields[f].value[j] = '\0';
    r->fields[f].active = 1;
}

/* ─── the SQL engine, scripted ───────────────────────────────────────────
 * The REAL sql_parse() is linked (kernel/sql_parser.c + predicate.c), so
 * every statement-shape decision the service makes is made against the real
 * parser. sql_execute()/cursor_* are scripted, because what is under test
 * here is the ORDER of the gates: which statements never reach the engine
 * at all, and which uid the engine is told to run as. */
static int      g_exec_calls;
static uint32_t g_exec_uid;
static char     g_exec_sql[600];
static const char* g_exec_fail;      /* non-NULL: sql_execute fails with it */
static int      g_policy = 1;        /* catalog_check_access's verdict */
static uint32_t g_policy_calls;
static uint32_t g_policy_uid;
static char     g_policy_obj[OBJECT_NAME_LEN];
static uint32_t g_policy_perm;

/* The result one scripted execution returns. */
#define Q_MAX_ROWS 8
static uint32_t g_q_ncols;
static char     g_q_cols[ROWSTORE_MAX_COLUMNS][RECORD_KEY_LEN];
static uint32_t g_q_total;           /* SqlResult.row_count: rows in the WHOLE result */
static uint32_t g_q_deliver;         /* rows the fake cursor will hand back */
static struct RowValues g_q_rows[Q_MAX_ROWS];
static uint32_t g_q_fetch_calls;
static uint32_t g_q_close_calls;

static void q_reset(void) {
    g_exec_calls = 0;
    g_exec_uid = 0;
    g_exec_sql[0] = '\0';
    g_exec_fail = 0;
    g_policy_calls = 0;
    g_policy_uid = 0;
    g_policy_obj[0] = '\0';
    g_policy_perm = 0;
    g_q_ncols = 0;
    g_q_total = 0;
    g_q_deliver = 0;
    g_q_fetch_calls = 0;
    g_q_close_calls = 0;
    memset(g_q_rows, 0, sizeof g_q_rows);
}

static void q_cols2(const char* a, const char* b) {
    g_q_ncols = 2;
    memset(g_q_cols, 0, sizeof g_q_cols);
    strncpy(g_q_cols[0], a, RECORD_KEY_LEN - 1);
    strncpy(g_q_cols[1], b, RECORD_KEY_LEN - 1);
}

static void q_row2(const char* a, const char* b) {
    struct RowValues* r = &g_q_rows[g_q_deliver];
    memset(r, 0, sizeof *r);
    r->count = 2;
    strncpy(r->values[0], a, RECORD_VAL_LEN - 1);
    strncpy(r->values[1], b, RECORD_VAL_LEN - 1);
    g_q_deliver++;
}

int sql_execute(uint32_t caller_uid, const char* sql_text, struct SqlResult* out) {
    g_exec_calls++;
    g_exec_uid = caller_uid;
    strncpy(g_exec_sql, sql_text, sizeof g_exec_sql - 1);
    g_exec_sql[sizeof g_exec_sql - 1] = '\0';
    memset(out, 0, sizeof *out);
    out->kind = SQL_STMT_SELECT;
    if (g_exec_fail) {
        out->error = SQL_ERR_INTERNAL;
        strncpy(out->error_msg, g_exec_fail, SQL_ERR_MSG_LEN - 1);
        return 1;
    }
    out->error = SQL_ERR_NONE;
    out->cursor_id = 42;              /* any non-zero id the fake recognises */
    out->row_count = g_q_total;
    out->column_count = g_q_ncols;
    memcpy(out->columns, g_q_cols, sizeof g_q_cols);
    return 0;
}

uint32_t cursor_fetch_rows(uint32_t cursor_id, uint32_t max_rows,
                           RowScanCb cb, void* ctx) {
    (void)cursor_id;
    g_q_fetch_calls++;
    uint32_t n = 0;
    for (uint32_t i = 0; i < g_q_deliver && n < max_rows; i++) {
        struct RowId id = { 0, 0 };   /* row-set cursors are values-only */
        cb(id, &g_q_rows[i], ctx);
        n++;
    }
    return n;
}

int cursor_close(uint32_t cursor_id) {
    (void)cursor_id;
    g_q_close_calls++;
    return 1;
}

/* predicate.c (linked for the REAL parser) reaches rowstore_table_scan()
 * only from predicate_table_scan() — the point where a WHERE clause is
 * EVALUATED against real pages. Nothing here evaluates one: the engine is
 * scripted, so a predicate's truth is decided by this test's rows instead.
 * Link-only, the same category as the env_storage placeholders above. */
uint32_t rowstore_table_scan(uint32_t caller_uid, const char* table_name,
                             RowScanCb cb, void* ctx) {
    (void)caller_uid; (void)table_name; (void)cb; (void)ctx;
    return 0;
}

int catalog_check_access(uint32_t uid, const char* obj_name,
                         uint32_t needed_perm) {
    g_policy_calls++;
    g_policy_uid = uid;
    g_policy_perm = needed_perm;
    strncpy(g_policy_obj, obj_name, sizeof g_policy_obj - 1);
    g_policy_obj[sizeof g_policy_obj - 1] = '\0';
    return g_policy;
}

/* index-aligned with object_catalog[], exactly as rowstore.c's is. */
struct RowTableHeader table_headers[ROWSTORE_MAX_TABLES];

/* ─── helpers ────────────────────────────────────────────────────────────── */
/* One catalog channel: the kernel's two ends and the sidecar's two.
 * cap_chan_create is the real mint the syscall path uses. */
struct Pair {
    uint16_t krd, kw, frd, fwr;
    uint32_t far_pid;
};

/* Hoisted out of section 2's block: CAT_QUERY's uid gates are exercised on
 * these channels later, because their registrations (partitions 3 and 4)
 * are the ones whose namespaces hold a table but whose descriptors are
 * deliberately wrong / absent. */
static struct Pair g_fill[6];

static int mint_pair(struct Pair* p, uint32_t far_pid) {
    p->far_pid = far_pid;
    return cap_chan_create(0, far_pid, &p->krd, &p->kw, &p->frd, &p->fwr);
}

static int far_send(const struct Pair* p, const void* b, uint32_t n,
                    uint32_t tag) {
    return cap_send_msg(p->far_pid, p->fwr, b, n, 0, 0, tag, 0);
}

static int far_recv(const struct Pair* p, char* out, size_t cap,
                    uint32_t* out_tag) {
    /* Run the service first: nothing answers until the tick drains the
     * channel, which is exactly how the kernel uses it
     * (microkernel_service_poll -> env_catalog_service_tick). */
    env_catalog_service_tick();
    uint32_t plen = 0, tag = 0, flags = 0;
    uint16_t nc = 0;
    int r = cap_recv_msg(p->far_pid, p->frd, (uint8_t*)out, (uint32_t)cap,
                         &plen, 0, 0, &nc, &tag, &flags);
    if (r != 0) return -1;
    out[plen < cap ? plen : cap - 1] = '\0';
    if (out_tag) *out_tag = tag;
    return (int)plen;
}

/* The one refusal text every refusal carries — pinned here because the
 * guard greps for this exact string on the serial transcript, and because
 * a renderer that drifts must fail a test rather than a live clause. */
#define REFUSAL_SUFFIX \
    " refused by kernel.env.catalog — not served by the §9 mediated surface"

static uint8_t g_frame[512];

/* A CAT_* request: the 16-byte header with CAT_MAGIC and version 1. */
static void cat_frame(uint8_t* f, uint16_t ty) {
    static const uint8_t magic[8] = { 'A','E','R','O','S','C','T',1 };
    memset(f, 0, 64);
    for (int i = 0; i < 8; i++) f[i] = magic[i];
    f[8] = 1; f[9] = 0;                  /* version */
    f[10] = (uint8_t)(ty & 0xFFu);
    f[11] = (uint8_t)(ty >> 8);
    f[12] = 0; f[13] = 0;                /* flags */
    f[14] = 0; f[15] = 0;                /* pad */
}

static void put_u16(uint8_t* p, uint16_t v) {
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)(v >> 8);
}

static uint16_t reply_u16(const char* r, uint32_t off) {
    return (uint16_t)((uint8_t)r[off] | ((uint16_t)(uint8_t)r[off + 1] << 8));
}
static uint32_t reply_u32(const char* r, uint32_t off) {
    return (uint32_t)(uint8_t)r[off] | ((uint32_t)(uint8_t)r[off + 1] << 8)
         | ((uint32_t)(uint8_t)r[off + 2] << 16)
         | ((uint32_t)(uint8_t)r[off + 3] << 24);
}

/* A CAT_LIST request body: {page u16, page_size u16}. */
static void list_req(uint8_t* f, uint16_t page, uint16_t page_sz) {
    cat_frame(f, 2);
    put_u16(f + 16, page);
    put_u16(f + 18, page_sz);
}

/* A CAT_STAT or CAT_READ request: {name_len u8, name[, key_len u8, key]}. */
static uint32_t named_req(uint8_t* f, uint16_t ty, const char* name,
                          const char* key) {
    cat_frame(f, ty);
    uint32_t n = 0;
    uint32_t nl = (uint32_t)strlen(name);
    f[16] = (uint8_t)nl;
    for (uint32_t i = 0; i < nl; i++) f[17 + i] = (uint8_t)name[i];
    n = 17 + nl;
    if (key) {
        uint32_t kl = (uint32_t)strlen(key);
        f[n] = (uint8_t)kl;
        for (uint32_t i = 0; i < kl; i++) f[n + 1 + i] = (uint8_t)key[i];
        n += 1 + kl;
    }
    return n;
}

/* A CAT_QUERY request: {sql_len u16, page u16, limit u8, flags u8, sql[]}. */
static uint32_t query_req(uint8_t* f, const char* sql, uint16_t page,
                          uint8_t limit) {
    cat_frame(f, 5);
    uint32_t sl = (uint32_t)strlen(sql);
    put_u16(f + 16, (uint16_t)sl);
    put_u16(f + 18, page);
    f[20] = limit;
    f[21] = 0;
    for (uint32_t i = 0; i < sl; i++) f[22 + i] = (uint8_t)sql[i];
    return 22 + sl;
}

/* Read one cell out of a CAT_QUERY reply: {head 12, names, rows}. Returns 0
 * on success, -1 if the reply does not hold what it claims — a caller CHECKs
 * that rather than reading past the buffer. */
static int q_cell(const char* r, int n, uint32_t row, uint32_t col,
                  uint8_t* flags, char* out, uint32_t cap) {
    if (n < 30) return -1;
    uint32_t ncols = (uint8_t)r[18];
    uint32_t nrows = (uint8_t)r[19];
    if (row >= nrows || col >= ncols) return -1;
    uint32_t off = 30;
    for (uint32_t c = 0; c < ncols; c++) {
        if (off + 1 > (uint32_t)n) return -1;
        off += 1u + (uint8_t)r[off];
    }
    for (uint32_t rw = 0; rw <= row; rw++) {
        for (uint32_t c = 0; c < ncols; c++) {
            if (off + 2 > (uint32_t)n) return -1;
            uint8_t fl = (uint8_t)r[off];
            uint32_t ln = (uint8_t)r[off + 1];
            if (off + 2 + ln > (uint32_t)n) return -1;
            if (rw == row && c == col) {
                if (flags) *flags = fl;
                uint32_t k = ln < cap - 1 ? ln : cap - 1;
                memcpy(out, r + off + 2, k);
                out[k] = '\0';
                return 0;
            }
            off += 2 + ln;
        }
    }
    return -1;
}

/* One column's name out of the reply's name block. */
static int q_colname(const char* r, int n, uint32_t col, char* out,
                     uint32_t cap) {
    if (n < 30) return -1;
    uint32_t ncols = (uint8_t)r[18];
    if (col >= ncols) return -1;
    uint32_t off = 30;
    for (uint32_t c = 0; c <= col; c++) {
        if (off + 1 > (uint32_t)n) return -1;
        uint32_t ln = (uint8_t)r[off];
        if (off + 1 + ln > (uint32_t)n) return -1;
        if (c == col) {
            uint32_t k = ln < cap - 1 ? ln : cap - 1;
            memcpy(out, r + off + 1, k);
            out[k] = '\0';
            return 0;
        }
        off += 1 + ln;
    }
    return -1;
}

int main(void) {
    /* The tables, holder pool, channel ids and slot freelists are all set up
     * here, not by the loader: without it cap_chan_create fails CAP_ENOMEM on
     * the very first mint (the same first line every other cap-hosting test
     * has — env_console_host_test.c, net_socket_service_host_test.c). */
    cap_init();

    /* ── 1. the verb table and the refusal renderer ─────────────────────── */
    {
        const char* expect[5] = { "CAT_INFO", "CAT_LIST", "CAT_STAT",
                                  "CAT_READ", "CAT_QUERY" };
        for (uint32_t i = 0; i < 5; i++) {
            const char* got = env_catalog_verb_name((uint16_t)(i + 1));
            char msg[72];
            snprintf(msg, sizeof msg, "verb %u is named %s", i + 1,
                     expect[i] ? expect[i] : "(null)");
            CHECK(got && strcmp(got, expect[i]) == 0, msg);
        }
        CHECK(env_catalog_verb_name(0) == 0, "verb 0 is outside the table");
        CHECK(env_catalog_verb_name(6) == 0,
              "verb 6 is outside the table (a query verb is not a write verb)");
        CHECK(env_catalog_verb_name(0xFFFFu) == 0,
              "an absurd verb resolves to no name");

        char t[192];
        /* Verb 4 is CAT_READ — the check below asserts the NAME of the verb
         * it asks for, so the number and the literal have to agree. */
        uint32_t n = env_catalog_refusal_text(4, t, sizeof t);
        CHECK(strcmp(t, "CAT_READ" REFUSAL_SUFFIX) == 0,
              "a known verb renders its name plus the one refusal text");
        CHECK(n == strlen(t), "the renderer returns the length it wrote");
        n = env_catalog_refusal_text(99, t, sizeof t);
        CHECK(strcmp(t, "verb 99" REFUSAL_SUFFIX) == 0,
              "an unknown verb renders with its number, not nothing");

        char small[11];
        memset(small, 'X', sizeof small);
        n = env_catalog_refusal_text(3, small, sizeof small);
        CHECK(n == 10 && small[10] == '\0' && strlen(small) == 10,
              "a small cap truncates and STILL terminates");
        char one[1] = { 'X' };
        n = env_catalog_refusal_text(3, one, sizeof one);
        CHECK(n == 0 && one[0] == '\0',
              "cap 1 yields the empty string, still terminated");
        n = env_catalog_refusal_text(3, NULL, 16);
        CHECK(n == 0, "a null buffer writes nothing");
    }

    /* ── 2. the registry: claim, replace, fill, refuse ──────────────────── */
    CHECK(env_catalog_service_count() == 0,
          "no channel is registered before cap.c wires one");

    struct Pair a, b;
    int mrc = mint_pair(&a, 200);
    if (mrc != 0) { printf("FAIL: mint a rc=%d\n", mrc); return 1; }
    CHECK(env_catalog_service_register(a.krd, a.kw, 1, 200,
                                       "aerosls.posix.0") == 1,
          "the kernel end of the tenant's catalog cap registers");
    CHECK(env_catalog_service_kernel_slot(a.krd) == 1,
          "its kernel CHAN_R is claimed (console_service must skip it)");
    CHECK(env_catalog_service_kernel_slot(a.kw) == 0,
          "the kernel CHAN_W is not a drain slot and is not claimed");
    CHECK(env_catalog_service_kernel_slot(0xFFFFu) == 0,
          "an unminted slot is not claimed");

    if (mint_pair(&b, 201) != 0) { printf("FAIL: mint b\n"); return 1; }
    CHECK(env_catalog_service_register(b.krd, b.kw, 2, 201,
                                       "aerosls.posix.1") == 1,
          "a second sidecar registers — in a DIFFERENT partition");
    CHECK(env_catalog_service_count() == 2, "both are live");

    /* Re-registration of the same (partition, pid) REPLACES the entry. */
    struct Pair a2;
    if (mint_pair(&a2, 200) != 0) { printf("FAIL: remint\n"); return 1; }
    CHECK(env_catalog_service_register(a2.krd, a2.kw, 1, 200,
                                       "aerosls.posix.0") == 1,
          "the same (partition, pid) re-registers (environment re-created)");
    CHECK(env_catalog_service_count() == 2,
          "re-registering replaces instead of adding a stale entry");
    CHECK(env_catalog_service_kernel_slot(a.krd) == 0,
          "the replaced entry's old kernel end is no longer claimed");
    CHECK(env_catalog_service_kernel_slot(a2.krd) == 1,
          "the new kernel end is the claimed one");

    /* Fill the registry and refuse the overflow. */
    {
        int all = 1;
        for (uint32_t i = 0; i < 6; i++) {
            if (mint_pair(&g_fill[i], (uint32_t)(210 + i)) != 0 ||
                env_catalog_service_register(g_fill[i].krd, g_fill[i].kw,
                                             (uint32_t)(3 + i),
                                             (uint32_t)(210 + i),
                                             "aerosls.posix.f") != 1)
                all = 0;
        }
        CHECK(all, "six more sidecars register (registry at capacity)");
        CHECK(env_catalog_service_count() == ENV_CATALOG_SERVICE_MAX,
              "the registry holds exactly ENV_CATALOG_SERVICE_MAX channels");
        struct Pair extra;
        if (mint_pair(&extra, 220) != 0) { printf("FAIL: mint x\n"); return 1; }
        CHECK(env_catalog_service_register(extra.krd, extra.kw, 9, 220,
                                           "aerosls.posix.x") == 0,
              "a ninth channel is refused — the registry is full");
    }

    /* ── 3. seed the catalog ────────────────────────────────────────────── */
    /* Partition 1 (the tenant under test): a KV table, a blob, a rowstore
     * table, and a SYSTEM_METADATA object. Partition 2 (the neighbour): one
     * table whose name must NEVER be visible from partition 1. */
    add_object("app.users",   OBJ_TYPE_DB_TABLE,        1, 4, 0);
    add_field(0, "name",  "ada");
    add_field(0, "email", "ada@example.com");
    add_field(0, "role",  "admin");
    add_object("app.blob",    OBJ_TYPE_HEAP_BLOB,       1, 1, 0);
    add_object("app.warehse", OBJ_TYPE_DB_TABLE,        1, 9, 1);  /* rowstore */
    add_object("app.sysmeta", OBJ_TYPE_SYSTEM_METADATA, 1, 1, 0);
    add_object("other.secret", OBJ_TYPE_DB_TABLE,       2, 2, 0);
    /* Slot 4 is other.secret (objects are index-ascending above), so the
     * neighbour's row belongs there. It was written to slot 5 — an INACTIVE
     * slot, skipped by every lookup — which made section 8's "the value
     * never appears in the reply" clause true for the wrong reason: there
     * was no value attached to the foreign object to leak. */
    add_field(4, "token", "s3cr3t");
    CHECK(strcmp(object_records[4].fields[0].value, "s3cr3t") == 0,
          "the neighbour's row is attached to the neighbour's object (the "
          "leak clause below now has something to leak)");
    CHECK(object_records[5].field_count == 0,
          "and nothing is parked in an inactive slot");

    /* Two row-set tables (and two neighbours that are NOT) in the partitions
     * the FILLED registry slots were registered under — so every CAT_QUERY
     * gate below can be reached by a channel whose own namespace holds a
     * table. The boundary check runs BEFORE the uid is resolved (by design:
     * a name is resolved inside the partition first), so a channel with an
     * empty namespace would answer NOENT and never exercise either uid gate.
     *
     * The names are deliberately undotted: this tree's SQL parser reads a
     * `.` in FROM as the start of a qualified COLUMN, so a dotted catalog
     * name (`app.warehse`) cannot be expressed in a statement at all. That
     * is the engine's own rule, pre-existing, and section 14 pins it as an
     * honest refusal rather than working around it silently. */
    add_object("stock",    OBJ_TYPE_DB_TABLE,         3, 1, 1);  /* rowstore */
    add_object("kv3",      OBJ_TYPE_DB_TABLE,         3, 1, 0);  /* legacy KV */
    add_object("sys3meta", OBJ_TYPE_SYSTEM_METADATA,  3, 1, 0);
    add_object("inv",      OBJ_TYPE_DB_TABLE,         4, 1, 1);  /* rowstore */
    add_object("ledger",   OBJ_TYPE_DB_TABLE,         5, 1, 1);  /* rowstore */

    /* proc_table: the channel was registered for pid 200 in partition 1, so
     * that descriptor is where CAT_QUERY's uid must come from — the wire
     * states none, and this is the only place the service can learn one. */
    proc_table[0].active = 1;
    proc_table[0].pid = 200;
    proc_table[0].owner_uid = 1001;
    proc_table[0].partition_id = 1;
    proc_table[1].active = 1;
    proc_table[1].pid = 201;
    proc_table[1].owner_uid = 1002;
    proc_table[1].partition_id = 2;
    /* pid 210: the CORRECT descriptor for partition 3's channel — the one
     * served query below runs as this uid. */
    proc_table[2].active = 1;
    proc_table[2].pid = 210;
    proc_table[2].owner_uid = 1003;
    proc_table[2].partition_id = 3;
    /* pid 212's descriptor is in partition 9 while its channel was
     * registered in partition 5 — the defence-in-depth clause: a
     * registration that outlived the partition it was minted in. */
    proc_table[3].active = 1;
    proc_table[3].pid = 212;
    proc_table[3].owner_uid = 1004;
    proc_table[3].partition_id = 9;
    /* pid 211 deliberately has NO descriptor: the missing-pid clause. */

    /* The row-set layouts: what a CAT_QUERY projection maps its output
     * columns through, because the engine hands back the FULL row. */
    table_headers[5].active = 1;                    /* stock */
    table_headers[5].layout.column_count = 3;
    strncpy(table_headers[5].layout.column_names[0], "sku",  RECORD_KEY_LEN - 1);
    strncpy(table_headers[5].layout.column_names[1], "qty",  RECORD_KEY_LEN - 1);
    strncpy(table_headers[5].layout.column_names[2], "site", RECORD_KEY_LEN - 1);
    table_headers[8].active = 1;                    /* inv */
    table_headers[8].layout.column_count = 1;
    strncpy(table_headers[8].layout.column_names[0], "sku", RECORD_KEY_LEN - 1);
    table_headers[9].active = 1;                    /* ledger */
    table_headers[9].layout.column_count = 1;
    strncpy(table_headers[9].layout.column_names[0], "amt", RECORD_KEY_LEN - 1);

    /* ── 4. CAT_INFO answers honestly, including the partition ──────────── */
    {
        cat_frame(g_frame, 1);
        CHECK(far_send(&a2, g_frame, 16, 300) == 0, "the sidecar sends CAT_INFO");
        char reply[512];
        int n = far_recv(&a2, reply, sizeof reply, NULL);
        CHECK(n == 34, "CAT_INFO is answered with its full body");
        CHECK(reply_u16(reply, 10) == 1, "the reply echoes the CAT_INFO verb");
        CHECK(reply_u16(reply, 12) == 0, "the handshake is not an error");
        CHECK(reply_u16(reply, 16) == CAT_OK, "status CAT_OK");
        CHECK(reply_u32(reply, 18) == CATALOG_MAX_OBJECTS,
              "max_objects is the real CATALOG_MAX_OBJECTS");
        CHECK(reply_u32(reply, 22) == CAT_MAX_NAME, "max_name is the real cap");
        CHECK(reply_u32(reply, 26) == CAT_MAX_VALUE,
              "max_value is the real cap");
        CHECK(reply_u32(reply, 30) == 1,
              "the handshake answers partition 1 — the identity the boundary "
              "is computed from, so a tenant can verify it");
    }

    /* ── 5. CAT_LIST: partition 1 sees four names, and NOT other.secret ── */
    {
        list_req(g_frame, 0, 8);
        CHECK(far_send(&a2, g_frame, 20, 301) == 0, "the sidecar sends CAT_LIST");
        char reply[1024];
        int n = far_recv(&a2, reply, sizeof reply, NULL);
        CHECK(n > 0, "CAT_LIST is answered");
        uint16_t count = reply_u16(reply, 18);
        uint16_t total = reply_u16(reply, 20);
        CHECK(count == 4, "partition 1's namespace holds four objects");
        CHECK(total == 4, "the total matches the count on a single page");
        CHECK(reply_u32(reply, 22) == 1, "the list reports its own partition");

        /* THE boundary clause: walk the entries and assert the neighbour's
         * name is not among them. */
        int saw_users = 0, saw_blob = 0, saw_warehse = 0, saw_sysmeta = 0;
        int saw_secret = 0;
        uint32_t off = 26;
        for (uint16_t i = 0; i < count && off + 68 <= (uint32_t)n; i++) {
            const char* nm = reply + off + 4;
            if (strcmp(nm, "app.users") == 0)   saw_users = 1;
            if (strcmp(nm, "app.blob") == 0)    saw_blob = 1;
            if (strcmp(nm, "app.warehse") == 0) saw_warehse = 1;
            if (strcmp(nm, "app.sysmeta") == 0) saw_sysmeta = 1;
            if (strcmp(nm, "other.secret") == 0) saw_secret = 1;
            off += 68;
        }
        CHECK(saw_users && saw_blob && saw_warehse && saw_sysmeta,
              "every object in partition 1 is listed");
        CHECK(!saw_secret,
              "THE BOUNDARY: partition 2's object is absent from partition "
              "1's list entirely — not listed-with-a-denial, ABSENT");

        /* The entry carries the object's type and tier, so a client can
         * tell a table from a blob before it asks for a record. */
        CHECK((uint8_t)reply[26] == (uint8_t)OBJ_TYPE_DB_TABLE,
              "the first entry carries its object type");
    }

    /* ── 6. CAT_LIST paging ─────────────────────────────────────────────── */
    {
        list_req(g_frame, 0, 2);
        CHECK(far_send(&a2, g_frame, 20, 302) == 0, "page 0 of size 2");
        char reply[1024];
        int n = far_recv(&a2, reply, sizeof reply, NULL);
        CHECK(n > 0 && reply_u16(reply, 18) == 2,
              "a page of size 2 returns exactly 2");
        CHECK(reply_u16(reply, 20) == 4, "the total is still 4 across pages");

        list_req(g_frame, 1, 2);
        CHECK(far_send(&a2, g_frame, 20, 303) == 0, "page 1 of size 2");
        n = far_recv(&a2, reply, sizeof reply, NULL);
        CHECK(n > 0 && reply_u16(reply, 18) == 2,
              "page 1 returns the remaining 2");

        list_req(g_frame, 2, 2);
        CHECK(far_send(&a2, g_frame, 20, 304) == 0, "page 2 (past the end)");
        n = far_recv(&a2, reply, sizeof reply, NULL);
        CHECK(n > 0 && reply_u16(reply, 18) == 0,
              "a page past the end answers an empty page, not an error");

        /* page_size 0 and an over-large page_size are malformed by name. */
        list_req(g_frame, 0, 0);
        CHECK(far_send(&a2, g_frame, 20, 305) == 0, "page_size 0");
        n = far_recv(&a2, reply, sizeof reply, NULL);
        CHECK(n > 0 && reply_u16(reply, 12) == CAT_FLAG_ERROR &&
              reply_u16(reply, 16) == CAT_ERR_SHAPE,
              "page_size 0 is refused CAT_ERR_SHAPE by name");

        list_req(g_frame, 0, 4095);
        CHECK(far_send(&a2, g_frame, 20, 306) == 0, "page_size 4095");
        n = far_recv(&a2, reply, sizeof reply, NULL);
        CHECK(n > 0 && reply_u16(reply, 12) == CAT_FLAG_ERROR &&
              reply_u16(reply, 16) == CAT_ERR_SHAPE,
              "an over-large page_size is refused CAT_ERR_SHAPE");
    }

    /* ── 7. CAT_STAT, inside the partition ──────────────────────────────── */
    {
        uint32_t len = named_req(g_frame, 3, "app.users", NULL);
        CHECK(far_send(&a2, g_frame, len, 307) == 0, "the sidecar sends CAT_STAT");
        char reply[512];
        int n = far_recv(&a2, reply, sizeof reply, NULL);
        CHECK(n == 34, "CAT_STAT is answered with its full body");
        CHECK(reply_u16(reply, 12) == 0, "CAT_STAT is not an error");
        CHECK((uint8_t)reply[18] == (uint8_t)OBJ_TYPE_DB_TABLE,
              "the type is the object's own");
        CHECK((uint8_t)reply[19] == (uint8_t)STORAGE_TIER_L2_DRAM,
              "the storage tier is the object's own");
        CHECK(reply_u32(reply, 22) == 4, "the page count is the object's own");
        CHECK(reply_u32(reply, 26) == 1001,
              "the owner uid is the object's own (partition 1 + 1000)");
    }

    /* ── 8. THE BOUNDARY: other.secret answers NOENT from partition 1 ─────
     * And — the clause that makes this a security property rather than a
     * bug — it answers the SAME status as a name that never existed
     * anywhere. A distinct PERM here would confirm the name exists. */
    {
        char reply[512];

        uint32_t len = named_req(g_frame, 3, "other.secret", NULL);
        CHECK(far_send(&a2, g_frame, len, 308) == 0,
              "the sidecar sends CAT_STAT on the NEIGHBOUR's object");
        int n = far_recv(&a2, reply, sizeof reply, NULL);
        uint16_t foreign_status = (n > 0) ? reply_u16(reply, 16) : 0xFFFFu;
        uint16_t foreign_flags  = (n > 0) ? reply_u16(reply, 12) : 0xFFFFu;
        CHECK(n > 0 && foreign_flags == CAT_FLAG_ERROR &&
              foreign_status == CAT_ERR_NOENT,
              "THE BOUNDARY: a partition-2 object answers CAT_ERR_NOENT to a "
              "partition-1 channel — not CAT_ERR_PERM, which would confirm "
              "the name exists somewhere");

        len = named_req(g_frame, 3, "never.existed", NULL);
        CHECK(far_send(&a2, g_frame, len, 309) == 0,
              "the sidecar sends CAT_STAT on a name that never existed");
        n = far_recv(&a2, reply, sizeof reply, NULL);
        uint16_t absent_status = (n > 0) ? reply_u16(reply, 16) : 0xFFFFu;
        CHECK(n > 0 && absent_status == CAT_ERR_NOENT,
              "a name that never existed answers CAT_ERR_NOENT too");
        CHECK(foreign_status == absent_status,
              "THE TWO ANSWERS ARE INDISTINGUISHABLE — the property §9.1 "
              "rests on, asserted rather than assumed");

        /* And the same for CAT_READ, on both sides. */
        len = named_req(g_frame, 4, "other.secret", "token");
        CHECK(far_send(&a2, g_frame, len, 310) == 0,
              "the sidecar sends CAT_READ on the neighbour's row");
        n = far_recv(&a2, reply, sizeof reply, NULL);
        CHECK(n > 0 && reply_u16(reply, 12) == CAT_FLAG_ERROR &&
              reply_u16(reply, 16) == CAT_ERR_NOENT,
              "THE BOUNDARY: reading a partition-2 row through partition 1 "
              "answers NOENT, not the value");
        /* The value must not appear anywhere in the reply. */
        int leaked = 0;
        for (int i = 0; i < n; i++)
            if (strncmp(reply + i, "s3cr3t", 6) == 0) leaked = 1;
        CHECK(!leaked, "the neighbour's value never appears in the reply");
    }

    /* ── 9. CAT_READ inside the partition ───────────────────────────────── */
    {
        uint32_t len = named_req(g_frame, 4, "app.users", "email");
        CHECK(far_send(&a2, g_frame, len, 311) == 0, "the sidecar sends CAT_READ");
        char reply[1024];
        int n = far_recv(&a2, reply, sizeof reply, NULL);
        CHECK(n > 0 && reply_u16(reply, 12) == 0, "CAT_READ is not an error");
        CHECK((uint8_t)reply[18] == 5, "the reply echoes the key's length");
        CHECK(strncmp(reply + 22, "email", 5) == 0, "the reply echoes the key");
        uint16_t vlen = reply_u16(reply, 20);
        char value[256];
        uint32_t i = 0;
        for (; i < vlen && i + 1 < sizeof value; i++)
            value[i] = reply[22 + CAT_MAX_KEY + i];
        value[i] = '\0';
        CHECK(vlen == strlen("ada@example.com") &&
              strcmp(value, "ada@example.com") == 0,
              "the value comes back byte-exact from the legacy record path");

        /* A key that is a PREFIX of a real key must not match: cat_name_eq
         * is exact, not a prefix compare. */
        len = named_req(g_frame, 4, "app.users", "em");
        CHECK(far_send(&a2, g_frame, len, 312) == 0, "a prefix key");
        int n2 = far_recv(&a2, reply, sizeof reply, NULL);
        CHECK(n2 > 0 && reply_u16(reply, 16) == CAT_ERR_NOENT,
              "a key that is only a prefix of a real key answers NOENT");

        /* A missing key on a real object answers NOENT. */
        len = named_req(g_frame, 4, "app.users", "nosuchfield");
        CHECK(far_send(&a2, g_frame, len, 313) == 0, "a missing key");
        n2 = far_recv(&a2, reply, sizeof reply, NULL);
        CHECK(n2 > 0 && reply_u16(reply, 16) == CAT_ERR_NOENT,
              "a missing key on a real object answers NOENT");

        /* An object with no records at all answers NOENT on READ, and it
         * is the OBJECT that is missing the key, not the namespace. */
        len = named_req(g_frame, 4, "app.blob", "anything");
        CHECK(far_send(&a2, g_frame, len, 314) == 0, "a key on a record-less object");
        n2 = far_recv(&a2, reply, sizeof reply, NULL);
        CHECK(n2 > 0 && reply_u16(reply, 16) == CAT_ERR_NOENT,
              "a record-less object answers NOENT for any key");
    }

    /* ── 10. the two in-partition refusals, by name ─────────────────────── */
    {
        char reply[512];

        /* SYSTEM_METADATA: PERM — in the namespace, still not served. */
        uint32_t len = named_req(g_frame, 3, "app.sysmeta", NULL);
        CHECK(far_send(&a2, g_frame, len, 315) == 0,
              "the sidecar sends CAT_STAT on SYSTEM_METADATA");
        int n = far_recv(&a2, reply, sizeof reply, NULL);
        CHECK(n > 0 && reply_u16(reply, 16) == CAT_ERR_PERM,
              "SYSTEM_METADATA answers CAT_ERR_PERM — visibly different from "
              "the NOENT a foreign name gets");
        CHECK(strstr(reply + 18, "SYSTEM_METADATA") != NULL,
              "the refusal is rendered BY NAME");

        len = named_req(g_frame, 4, "app.sysmeta", "anything");
        CHECK(far_send(&a2, g_frame, len, 316) == 0,
              "the sidecar sends CAT_READ on SYSTEM_METADATA");
        n = far_recv(&a2, reply, sizeof reply, NULL);
        CHECK(n > 0 && reply_u16(reply, 16) == CAT_ERR_PERM,
              "CAT_READ on SYSTEM_METADATA is CAT_ERR_PERM too");

        /* The rowstore table: CAT_STAT answers (its metadata is real), and
         * CAT_READ alone refuses with CAT_ERR_TYPE — Phase 24's guard,
         * inherited. */
        len = named_req(g_frame, 3, "app.warehse", NULL);
        CHECK(far_send(&a2, g_frame, len, 317) == 0,
              "the sidecar sends CAT_STAT on the rowstore table");
        n = far_recv(&a2, reply, sizeof reply, NULL);
        CHECK(n > 0 && reply_u16(reply, 12) == 0 &&
              reply_u32(reply, 22) == 9,
              "CAT_STAT still answers for a rowstore table — hiding its "
              "metadata would make the READ refusal unexplainable");

        len = named_req(g_frame, 4, "app.warehse", "col");
        CHECK(far_send(&a2, g_frame, len, 318) == 0,
              "the sidecar sends CAT_READ on the rowstore table");
        n = far_recv(&a2, reply, sizeof reply, NULL);
        CHECK(n > 0 && reply_u16(reply, 16) == CAT_ERR_TYPE,
              "CAT_READ on a rowstore table answers CAT_ERR_TYPE — Phase 24's "
              "own refusal, inherited by name");
        CHECK(strstr(reply + 18, "CAT_READ") != NULL,
              "the TYPE refusal is rendered by the verb's name too");
    }

    /* ── 11. unknown verbs are refused BY NUMBER, including a write ─────── */
    {
        cat_frame(g_frame, 9);            /* a CAT_WRITE that does not exist */
        CHECK(far_send(&a2, g_frame, 16, 319) == 0,
              "the sidecar sends verb 9 (a write verb that does not exist)");
        char reply[512];
        int n = far_recv(&a2, reply, sizeof reply, NULL);
        CHECK(n > 0 && reply_u16(reply, 12) == CAT_FLAG_ERROR &&
              reply_u16(reply, 16) == CAT_ERR_UNKNOWN,
              "an unknown verb is refused CAT_ERR_UNKNOWN — a write path is "
              "answered like any other unknown, not specially");
        CHECK(strstr(reply + 18, "verb 9") != NULL,
              "the unknown verb is rendered WITH ITS NUMBER");

        /* A NET_* verb arriving on a catalog channel is not a CAT_* frame
         * at all: magic differs, so it is dropped rather than answered. */
        static const uint8_t net_magic[8] = { 'A','E','R','O','S','N','T',1 };
        memset(g_frame, 0, 32);
        for (int i = 0; i < 8; i++) g_frame[i] = net_magic[i];
        g_frame[8] = 1;
        g_frame[10] = 1;                  /* NET_INFO */
        CHECK(far_send(&a2, g_frame, 16, 320) == 0,
              "the sidecar sends a NET_* frame on a catalog channel");
        n = far_recv(&a2, reply, sizeof reply, NULL);
        CHECK(n < 0, "a NET_* frame is DROPPED, never answered as a verb");
    }

    /* ── 12. malformed frames by name ───────────────────────────────────── */
    {
        char reply[512];

        cat_frame(g_frame, 2);                       /* CAT_LIST, no body */
        CHECK(far_send(&a2, g_frame, 16, 321) == 0, "a bodyless CAT_LIST");
        int n = far_recv(&a2, reply, sizeof reply, NULL);
        CHECK(n > 0 && reply_u16(reply, 16) == CAT_ERR_SHAPE,
              "a bodyless CAT_LIST is CAT_ERR_SHAPE");

        cat_frame(g_frame, 3);                       /* CAT_STAT, no body */
        CHECK(far_send(&a2, g_frame, 16, 322) == 0, "a bodyless CAT_STAT");
        n = far_recv(&a2, reply, sizeof reply, NULL);
        CHECK(n > 0 && reply_u16(reply, 16) == CAT_ERR_SHAPE,
              "a bodyless CAT_STAT is CAT_ERR_SHAPE");

        cat_frame(g_frame, 4);                       /* CAT_READ, no body */
        CHECK(far_send(&a2, g_frame, 16, 323) == 0, "a bodyless CAT_READ");
        n = far_recv(&a2, reply, sizeof reply, NULL);
        CHECK(n > 0 && reply_u16(reply, 16) == CAT_ERR_SHAPE,
              "a bodyless CAT_READ is CAT_ERR_SHAPE");

        /* A name length that runs off the end of the frame. */
        cat_frame(g_frame, 3);
        g_frame[16] = 60;                            /* claims 60 bytes */
        CHECK(far_send(&a2, g_frame, 20, 324) == 0,
              "a CAT_STAT whose name length exceeds the frame");
        n = far_recv(&a2, reply, sizeof reply, NULL);
        CHECK(n > 0 && reply_u16(reply, 16) == CAT_ERR_SHAPE,
              "an over-long name length is CAT_ERR_SHAPE, not a buffer read");

        /* A zero-length name. */
        cat_frame(g_frame, 3);
        g_frame[16] = 0;
        CHECK(far_send(&a2, g_frame, 17, 325) == 0, "a zero-length name");
        n = far_recv(&a2, reply, sizeof reply, NULL);
        CHECK(n > 0 && reply_u16(reply, 16) == CAT_ERR_SHAPE,
              "a zero-length name is CAT_ERR_SHAPE");
    }

    /* ── 13. the symmetric clause: partition 2 sees ITS objects, not ours ── */
    {
        list_req(g_frame, 0, 8);
        CHECK(far_send(&b, g_frame, 20, 326) == 0,
              "the NEIGHBOUR sends CAT_LIST");
        char reply[1024];
        int n = far_recv(&b, reply, sizeof reply, NULL);
        CHECK(n > 0 && reply_u16(reply, 18) == 1,
              "partition 2's namespace holds exactly one object");
        CHECK(reply_u32(reply, 22) == 2, "and it reports partition 2");
        CHECK(strncmp(reply + 26 + 4, "other.secret", 12) == 0,
              "partition 2's own object is the one it sees");
        int leaked = 0;
        for (int i = 0; i < n; i++)
            if (strncmp(reply + i, "app.users", 9) == 0) leaked = 1;
        CHECK(!leaked, "partition 1's names never appear in partition 2's list");

        /* And partition 2 reading partition 1's table gets NOENT. */
        uint32_t len = named_req(g_frame, 4, "app.users", "email");
        CHECK(far_send(&b, g_frame, len, 327) == 0,
              "the neighbour reads partition 1's row");
        n = far_recv(&b, reply, sizeof reply, NULL);
        CHECK(n > 0 && reply_u16(reply, 16) == CAT_ERR_NOENT,
              "the boundary holds in BOTH directions");
    }

    /* ── 14. CAT_QUERY: the SQL path, gate by gate ─────────────────────────
     * The order asserted here is the design: nothing resolves a name until
     * the statement parsed, and nothing EXECUTES until the name resolved
     * inside this channel's own partition. Every refusal clause also checks
     * that g_exec_calls stayed where it was — a refusal that still ran the
     * engine would be a boundary failure wearing a refusal's clothes. */
    {
        char reply[4096];
        char cell[256], col[RECORD_KEY_LEN];
        uint8_t flags;

        /* (a) Served: SELECT * on the channel's OWN row-set table. */
        q_reset();
        g_policy = 1;
        q_cols2("sku", "qty");
        g_q_total = 2;
        q_row2("A1", "7");
        q_row2("B2", "9");
        uint32_t len = query_req(g_frame, "SELECT * FROM stock", 0, 4);
        CHECK(far_send(&g_fill[0], g_frame, len, 330) == 0,
              "the sidecar sends CAT_QUERY");
        int n = far_recv(&g_fill[0], reply, sizeof reply, NULL);
        CHECK(n > 0 && reply_u16(reply, 12) == 0 &&
              reply_u16(reply, 16) == CAT_OK,
              "a SELECT on this partition's own row-set table is SERVED");
        CHECK((uint8_t)reply[18] == 2 && (uint8_t)reply[19] == 2,
              "the head reports 2 columns and 2 rows");
        CHECK(reply_u32(reply, 26) == 2,
              "and the WHOLE result's row count, so a client can page");
        CHECK(q_colname(reply, n, 0, col, sizeof col) == 0 &&
              strcmp(col, "sku") == 0,
              "the column names come from the result");
        CHECK(q_cell(reply, n, 0, 0, &flags, cell, sizeof cell) == 0 &&
              strcmp(cell, "A1") == 0 && !(flags & 1),
              "the first cell is the row's own value");
        CHECK(q_cell(reply, n, 1, 1, &flags, cell, sizeof cell) == 0 &&
              strcmp(cell, "9") == 0,
              "and the second row's second cell too");
        CHECK(g_exec_calls == 1 && g_exec_uid == 1003,
              "the engine ran ONCE, as the uid proc_table[pid] holds — the "
              "wire states no identity, and this is where one came from");
        CHECK(g_policy_calls == 1 && g_policy_uid == 1003 &&
              strcmp(g_policy_obj, "stock") == 0 &&
              g_policy_perm == PERM_READ,
              "catalog_check_access was asked PERM_READ for THIS table and "
              "THIS uid — the SQL path's own policy check, surfaced");
        CHECK(g_q_close_calls == 1, "the result set's cursor is closed");

        /* (b) Pagination: page 1 of a 2-row result with limit 4 skips both. */
        q_reset();
        g_policy = 1;
        q_cols2("sku", "qty");
        g_q_total = 2;
        q_row2("A1", "7");
        q_row2("B2", "9");
        len = query_req(g_frame, "SELECT * FROM stock", 1, 4);
        CHECK(far_send(&g_fill[0], g_frame, len, 331) == 0, "page 1 of the same query");
        n = far_recv(&g_fill[0], reply, sizeof reply, NULL);
        CHECK(n > 0 && reply_u16(reply, 16) == CAT_OK &&
              (uint8_t)reply[19] == 0 && reply[20] == 0,
              "a page past the end is an EMPTY page with more=0, not an error");

        /* (c) `more`: 3 rows and a limit of 2 stops at the bound, not at the
         *     end — the client is told to ask again. */
        q_reset();
        g_policy = 1;
        q_cols2("sku", "qty");
        g_q_total = 3;
        q_row2("A1", "7");
        q_row2("B2", "9");
        q_row2("C3", "1");
        len = query_req(g_frame, "SELECT * FROM stock", 0, 2);
        CHECK(far_send(&g_fill[0], g_frame, len, 332) == 0, "limit 2 of 3 rows");
        n = far_recv(&g_fill[0], reply, sizeof reply, NULL);
        CHECK(n > 0 && (uint8_t)reply[19] == 2 && reply[20] == 1,
              "more=1 when the row bound stopped the page");

        /* (d) A NULL cell is NULL on the wire, not an empty string. */
        q_reset();
        g_policy = 1;
        q_cols2("sku", "qty");
        g_q_total = 1;
        q_row2("A1", "");
        g_q_rows[0].null_mask = 0x2;
        len = query_req(g_frame, "SELECT * FROM stock", 0, 4);
        CHECK(far_send(&g_fill[0], g_frame, len, 333) == 0, "a row with a NULL cell");
        n = far_recv(&g_fill[0], reply, sizeof reply, NULL);
        CHECK(q_cell(reply, n, 0, 1, &flags, cell, sizeof cell) == 0 &&
              (flags & 1) && cell[0] == '\0',
              "NULL carries its own flag with no bytes — the distinction "
              "rowstore's null_mask exists to preserve");

        /* (e) Projection: the engine hands back the FULL row, so the service
         *     maps the requested column through the table's layout. */
        q_reset();
        g_policy = 1;
        q_cols2("sku", "qty");           /* one requested column */
        g_q_ncols = 1;
        g_q_total = 1;
        q_row2("A1", "7");
        /* the engine's row still carries all three cells */
        g_q_rows[0].count = 3;
        strncpy(g_q_rows[0].values[2], "ams", RECORD_VAL_LEN - 1);
        len = query_req(g_frame, "SELECT sku FROM stock", 0, 4);
        CHECK(far_send(&g_fill[0], g_frame, len, 334) == 0, "a projected column");
        n = far_recv(&g_fill[0], reply, sizeof reply, NULL);
        CHECK(n > 0 && reply_u16(reply, 16) == CAT_OK &&
              (uint8_t)reply[18] == 1,
              "a projection is served with one column");
        CHECK(q_cell(reply, n, 0, 0, &flags, cell, sizeof cell) == 0 &&
              strcmp(cell, "A1") == 0,
              "the cell is the REQUESTED column's, mapped through the layout");

        /* (f) THE BOUNDARY: a foreign table answers NOENT and the engine is
         *     never invoked — the name is resolved inside the partition
         *     first, which is what makes one lookup the whole boundary. */
        q_reset();
        g_policy = 1;
        len = query_req(g_frame, "SELECT * FROM inv", 0, 4);
        CHECK(far_send(&g_fill[0], g_frame, len, 335) == 0,
              "partition 3's channel SELECTs partition 4's table");
        n = far_recv(&g_fill[0], reply, sizeof reply, NULL);
        CHECK(n > 0 && reply_u16(reply, 12) == CAT_FLAG_ERROR &&
              reply_u16(reply, 16) == CAT_ERR_NOENT,
              "THE BOUNDARY: a foreign table answers CAT_ERR_NOENT");
        CHECK(g_exec_calls == 0,
              "and the engine never ran — no cross-partition row was read, "
              "not even transiently");
        int leaked = 0;
        for (int i = 0; i < n; i++)
            if (strncmp(reply + i, "s3cr3t", 6) == 0 ||
                strncmp(reply + i, "amt", 3) == 0) leaked = 1;
        CHECK(!leaked, "no foreign value of any kind reaches the reply");

        len = query_req(g_frame, "SELECT * FROM nosuch", 0, 4);
        CHECK(far_send(&g_fill[0], g_frame, len, 336) == 0, "a table that never existed");
        n = far_recv(&g_fill[0], reply, sizeof reply, NULL);
        CHECK(n > 0 && reply_u16(reply, 16) == CAT_ERR_NOENT &&
              g_exec_calls == 0,
              "and a name that never existed answers the SAME NOENT — the "
              "two are indistinguishable here too");

        /* (g) Statement shapes the surface does not carry: each refused BY
         *     NAME, before any lookup and before any execution. */
        static const struct { const char* sql; const char* why; } shapes[] = {
            { "DELETE FROM stock",                          "only SELECT" },
            { "UPDATE stock SET qty = 1",                   "only SELECT" },
            { "CREATE TABLE x (a STRING)",                  "only SELECT" },
            { "SELECT * FROM stock JOIN kv3 ON stock.sku = kv3.sku", "a JOIN" },
            { "SELECT * FROM stock UNION SELECT * FROM stock", "a set operator" },
            { "WITH x AS (SELECT * FROM stock) SELECT * FROM x", "a CTE" },
            { "SELECT * FROM stock WHERE qty = (SELECT qty FROM stock)", "a subquery" },
        };
        for (uint32_t i = 0; i < sizeof shapes / sizeof shapes[0]; i++) {
            q_reset();
            g_policy = 1;
            len = query_req(g_frame, shapes[i].sql, 0, 4);
            CHECK(far_send(&g_fill[0], g_frame, len, (uint32_t)(340 + i)) == 0,
                  shapes[i].sql);
            n = far_recv(&g_fill[0], reply, sizeof reply, NULL);
            CHECK(n > 0 && reply_u16(reply, 16) == CAT_ERR_TYPE &&
                  strstr(reply + 18, shapes[i].why) != NULL,
                  "refused BY NAME with the construct it refuses");
            CHECK(g_exec_calls == 0,
                  "a refused shape never reaches the engine");
        }

        /* (h) Inside the namespace, still refused: the two in-namespace
         *     answers CAT_QUERY makes before it will execute. */
        q_reset();
        g_policy = 1;
        len = query_req(g_frame, "SELECT * FROM kv3", 0, 4);
        CHECK(far_send(&g_fill[0], g_frame, len, 350) == 0, "a legacy KV table");
        n = far_recv(&g_fill[0], reply, sizeof reply, NULL);
        CHECK(n > 0 && reply_u16(reply, 16) == CAT_ERR_TYPE &&
              strstr(reply + 18, "not a row-set table") != NULL &&
              strstr(reply + 18, "CAT_READ") != NULL &&
              g_exec_calls == 0,
              "a non-rowstore table is refused BY NAME, pointing at the "
              "verb that does serve it");

        q_reset();
        g_policy = 1;
        len = query_req(g_frame, "SELECT * FROM sys3meta", 0, 4);
        CHECK(far_send(&g_fill[0], g_frame, len, 351) == 0, "SYSTEM_METADATA");
        n = far_recv(&g_fill[0], reply, sizeof reply, NULL);
        CHECK(n > 0 && reply_u16(reply, 16) == CAT_ERR_PERM && g_exec_calls == 0,
              "SYSTEM_METADATA answers PERM here too, before execution");

        /* (i) The policy gate: a denial is a REFUSAL, not an empty result —
         *     which is the whole reason it runs before the engine. */
        q_reset();
        g_policy = 0;
        len = query_req(g_frame, "SELECT * FROM stock", 0, 4);
        CHECK(far_send(&g_fill[0], g_frame, len, 352) == 0, "a query policy denies");
        n = far_recv(&g_fill[0], reply, sizeof reply, NULL);
        CHECK(n > 0 && reply_u16(reply, 16) == CAT_ERR_PERM &&
              strstr(reply + 18, "row-level policy") != NULL,
              "a policy denial is CAT_ERR_PERM BY NAME, not 0 rows");
        CHECK(g_exec_calls == 0,
              "and the engine never ran — denied and empty stay different "
              "answers");
        g_policy = 1;

        /* (j) The uid gates: a descriptor in the wrong partition, and a pid
         *     with no descriptor at all. Both are PERM, both pre-execution. */
        q_reset();
        g_policy = 1;
        len = query_req(g_frame, "SELECT * FROM ledger", 0, 4);
        CHECK(far_send(&g_fill[2], g_frame, len, 353) == 0,
              "partition 5's channel SELECTs its own table");
        n = far_recv(&g_fill[2], reply, sizeof reply, NULL);
        CHECK(n > 0 && reply_u16(reply, 16) == CAT_ERR_PERM &&
              strstr(reply + 18, "not in this channel's partition") != NULL &&
              g_exec_calls == 0,
              "a descriptor whose partition != the channel's is PERM — the "
              "registration outlived the partition it was minted in");

        q_reset();
        g_policy = 1;
        len = query_req(g_frame, "SELECT * FROM inv", 0, 4);
        CHECK(far_send(&g_fill[1], g_frame, len, 354) == 0,
              "partition 4's channel SELECTs its own table");
        n = far_recv(&g_fill[1], reply, sizeof reply, NULL);
        CHECK(n > 0 && reply_u16(reply, 16) == CAT_ERR_PERM &&
              strstr(reply + 18, "pid no longer stands") != NULL &&
              g_exec_calls == 0,
              "a pid with no live descriptor is PERM — no identity, no read");

        /* (k) The engine's own refusals arrive as CAT_ERR_QUERY with its
         *     message, so a caller sees WHY rather than a number. */
        q_reset();
        g_policy = 1;
        g_exec_fail = "SELECT column not found in table";
        len = query_req(g_frame, "SELECT nosuch FROM stock", 0, 4);
        CHECK(far_send(&g_fill[0], g_frame, len, 355) == 0, "a statement the engine refuses");
        n = far_recv(&g_fill[0], reply, sizeof reply, NULL);
        CHECK(n > 0 && reply_u16(reply, 16) == CAT_ERR_QUERY &&
              strstr(reply + 18, "SELECT column not found") != NULL,
              "CAT_ERR_QUERY carries the ENGINE's own reason");
        CHECK(strstr(reply + 18, "CAT_QUERY") != NULL,
              "and the refusal still names its verb");
        g_exec_fail = 0;

        q_reset();
        g_policy = 1;
        len = query_req(g_frame, "SELECTD * FROM stock", 0, 4);
        CHECK(far_send(&g_fill[0], g_frame, len, 356) == 0, "a statement that will not parse");
        n = far_recv(&g_fill[0], reply, sizeof reply, NULL);
        CHECK(n > 0 && reply_u16(reply, 16) == CAT_ERR_QUERY &&
              g_exec_calls == 0 && g_policy_calls == 0,
              "a PARSE failure is CAT_ERR_QUERY, and it happened before any "
              "name was resolved or any policy asked");

        /* (k2) The one honest refusal a reader will actually hit: a DOTTED
         *      catalog name cannot be expressed in SQL at all, because this
         *      parser reads `.` in FROM as the start of a qualified column
         *      ("unexpected trailing tokens"). That is the engine's own
         *      pre-existing rule, and the mediated surface reports it as the
         *      engine worded it — with the engine never run — rather than
         *      pretending the table does not exist. CAT_STAT/CAT_READ still
         *      serve such a name; only SQL cannot reach it. */
        q_reset();
        g_policy = 1;
        len = query_req(g_frame, "SELECT * FROM app.warehse", 0, 4);
        CHECK(far_send(&a2, g_frame, len, 361) == 0, "a dotted catalog name in FROM");
        n = far_recv(&a2, reply, sizeof reply, NULL);
        CHECK(n > 0 && reply_u16(reply, 16) == CAT_ERR_QUERY &&
              strstr(reply + 18, "trailing tokens") != NULL &&
              g_exec_calls == 0 && g_policy_calls == 0,
              "a dotted name answers the PARSER's own refusal, not NOENT — "
              "the object exists and is still visible to CAT_STAT");

        /* (l) Shape, by name: the bounds the request itself carries. */
        q_reset();
        g_policy = 1;
        len = query_req(g_frame, "SELECT * FROM app.warehse", 0, 5);
        CHECK(far_send(&a2, g_frame, len, 357) == 0, "limit 5");
        n = far_recv(&a2, reply, sizeof reply, NULL);
        CHECK(n > 0 && reply_u16(reply, 16) == CAT_ERR_SHAPE && g_exec_calls == 0,
              "a limit past CAT_QUERY_ROWS_MAX is CAT_ERR_SHAPE");

        cat_frame(g_frame, 5);
        put_u16(g_frame + 16, 600);          /* sql_len 600 > CAT_SQL_MAX */
        put_u16(g_frame + 18, 0);
        g_frame[20] = 4;
        CHECK(far_send(&a2, g_frame, 22, 358) == 0, "a statement past the cap");
        n = far_recv(&a2, reply, sizeof reply, NULL);
        CHECK(n > 0 && reply_u16(reply, 16) == CAT_ERR_SHAPE,
              "a statement past CAT_SQL_MAX is CAT_ERR_SHAPE");

        /* An embedded NUL would make the engine parse a PREFIX of what the
         * caller believes it sent — refused instead of half-executed. */
        cat_frame(g_frame, 5);
        put_u16(g_frame + 16, 12);
        put_u16(g_frame + 18, 0);
        g_frame[20] = 4;
        memcpy(g_frame + 22, "SELECT 1\0xxx", 12);
        CHECK(far_send(&a2, g_frame, 34, 359) == 0, "a statement with a NUL");
        n = far_recv(&a2, reply, sizeof reply, NULL);
        CHECK(n > 0 && reply_u16(reply, 16) == CAT_ERR_SHAPE &&
              g_exec_calls == 0,
              "an embedded NUL is CAT_ERR_SHAPE — no prefix is ever parsed");

        cat_frame(g_frame, 5);               /* no body at all */
        CHECK(far_send(&a2, g_frame, 16, 360) == 0, "a bodyless CAT_QUERY");
        n = far_recv(&a2, reply, sizeof reply, NULL);
        CHECK(n > 0 && reply_u16(reply, 16) == CAT_ERR_SHAPE,
              "a bodyless CAT_QUERY is CAT_ERR_SHAPE");

        q_reset();
        g_policy = 1;
    }

    /* ── 15. the service refuses nothing it should not have taken ───────── */
    CHECK(env_catalog_refusals() > 0,
          "the refusal counter counted the refusals above");
    CHECK(env_catalog_dropped() == 1,
          "the drop counter counted exactly the one non-CAT frame");

    printf("\nenv_catalog_host_test: %d passed, %d failed\n",
           checks_passed, checks_failed);
    return checks_failed ? 1 : 0;
}
