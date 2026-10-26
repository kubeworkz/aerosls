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
 *     CAT_STAT still answers, so the refusal is explainable.
 *   - An unknown verb is refused BY NUMBER with CAT_ERR_UNKNOWN, which is
 *     how a CAT_WRITE asking for a write path is answered: like any other
 *     unknown, not specially.
 *   - A non-CAT magic on a catalog channel is dropped, never answered.
 *
 * Build and run:
 *   gcc -no-pie -Wall -Wextra -std=c11 -I . -I kernel -I net -I arch/x86 \
 *       -include tests/kernel_io_panic_port.h \
 *       -o /tmp/env_catalog_host_test \
 *       tests/env_catalog_host_test.c kernel/cap.c \
 *       kernel/env_catalog.c kernel/kernel_io.c kernel/console.c \
 *       kernel/linux_abi.c kernel/elf64_load.c kernel/entropy.c kernel/sha256.c
 *   /tmp/env_catalog_host_test
 */
#include "kernel/cap.h"
#include "kernel/env_catalog.h"
#include "kernel/kernel_io.h"
#include "kernel/object_catalog.h"   /* the REAL entry/record types */
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

/* ─── helpers ────────────────────────────────────────────────────────────── */
/* One catalog channel: the kernel's two ends and the sidecar's two.
 * cap_chan_create is the real mint the syscall path uses. */
struct Pair {
    uint16_t krd, kw, frd, fwr;
    uint32_t far_pid;
};

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

int main(void) {
    /* The tables, holder pool, channel ids and slot freelists are all set up
     * here, not by the loader: without it cap_chan_create fails CAP_ENOMEM on
     * the very first mint (the same first line every other cap-hosting test
     * has — env_console_host_test.c, net_socket_service_host_test.c). */
    cap_init();

    /* ── 1. the verb table and the refusal renderer ─────────────────────── */
    {
        const char* expect[4] = { "CAT_INFO", "CAT_LIST", "CAT_STAT", "CAT_READ" };
        for (uint32_t i = 0; i < 4; i++) {
            const char* got = env_catalog_verb_name((uint16_t)(i + 1));
            char msg[72];
            snprintf(msg, sizeof msg, "verb %u is named %s", i + 1,
                     expect[i] ? expect[i] : "(null)");
            CHECK(got && strcmp(got, expect[i]) == 0, msg);
        }
        CHECK(env_catalog_verb_name(0) == 0, "verb 0 is outside the table");
        CHECK(env_catalog_verb_name(5) == 0,
              "verb 5 is outside the table (there is no write verb)");
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
        struct Pair fill[6];
        for (uint32_t i = 0; i < 6; i++) {
            if (mint_pair(&fill[i], (uint32_t)(210 + i)) != 0 ||
                env_catalog_service_register(fill[i].krd, fill[i].kw,
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
    add_field(5, "token", "s3cr3t");

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

    /* ── 14. the service refuses nothing it should not have taken ───────── */
    CHECK(env_catalog_refusals() > 0,
          "the refusal counter counted the refusals above");
    CHECK(env_catalog_dropped() == 1,
          "the drop counter counted exactly the one non-CAT frame");

    printf("\nenv_catalog_host_test: %d passed, %d failed\n",
           checks_passed, checks_failed);
    return checks_failed ? 1 : 0;
}
