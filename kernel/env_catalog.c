/* env_catalog.c — see env_catalog.h. POSIX-Environments §9's answer.
 *
 * The second kernel-brokered service. It reuses net_socket_service.c's
 * machinery deliberately — the same register/tick/retire cycle, the same
 * 16-byte header, the same "refuse by name" discipline — because the
 * property §9 needs is the one P2 already proved: the boundary is the
 * CHANNEL, not the API. What differs is what the verbs do. P2's verbs run
 * a tenant's traffic against the kernel stack; these verbs answer what
 * objects the tenant's own partition holds, and nothing else.
 */
#include "env_catalog.h"
#include "cap.h"
#include "kernel_io.h"
#include "object_catalog.h"
/* CAT_QUERY (§9.1's SQL path) reads a row-set table through the engine that
 * owns it rather than reimplementing a reader: sql_parse() gates the
 * statement's SHAPE before any name is resolved, sql_execute() runs it under
 * the caller's uid, and the row-set cursor hands back the rows. proc_table
 * is how the channel's (partition, pid) becomes the uid that policy runs
 * against — the wire still states no identity. */
#include "sql_parser.h"
#include "sql_exec.h"
#include "cursor.h"
#include "rowstore.h"
#include "process.h"
#include "../user/permissions.h"   /* PERM_READ — the policy bit CAT_QUERY checks */
#include <stddef.h>

/* The wire, spelled out in C. See env_catalog.h's wire section; the host
 * test pins these constants against the layout the client will speak. */
static const uint8_t cat_magic[CAT_MAGIC_LEN] = { 'A','E','R','O','S','C','T',1 };

#define CAT_LIST_ENTRY 68u   /* {type u8, tier u8, pad u16, name[64]} */
#define CAT_LIST_HDR   26u   /* header(16) + status(2) + {count u16,
                              * total u16, partition u32} */

/* The verbs, named — the refusal renderer's "by name" source, and the same
 * shape as nss_verbs[]. A type outside this table is refused by number. */
static const struct { uint16_t ty; const char* name; } cat_verbs[] = {
    { 1, "CAT_INFO" },
    { 2, "CAT_LIST" },
    { 3, "CAT_STAT" },
    { 4, "CAT_READ" },
    { 5, "CAT_QUERY" },
};

const char* env_catalog_verb_name(uint16_t ty) {
    for (uint32_t i = 0; i < sizeof(cat_verbs) / sizeof(cat_verbs[0]); i++)
        if (cat_verbs[i].ty == ty) return cat_verbs[i].name;
    return 0;
}

/* ─── registry ──────────────────────────────────────────────────────────── */

struct EnvCatalogSvc {
    int      used;
    uint32_t partition;   /* the boundary every verb is computed against */
    uint32_t pid;
    uint16_t k_rd;        /* kernel (pid 0) CHAN_R — requests arrive here */
    uint16_t k_wr;        /* kernel (pid 0) CHAN_W — replies leave here */
    char     name[32];    /* the caller's sidecar name: the refusal names it */
};

static struct EnvCatalogSvc cat_svc[ENV_CATALOG_SERVICE_MAX];

/* One shared drain buffer, single static, for the same reason
 * env_console.c's and net_socket_service.c's are: this tick runs in one
 * non-IRQ kernel context at a time (microkernel_service_poll), and a
 * channel's payload bound is what sizes it. */
static uint8_t  cat_buf[4096];
/* 4 KiB, not 1 KiB: CAT_QUERY's page is the first verb whose body can
 * exceed the old 1024-byte reply (its byte ceiling is CAT_QUERY_BODY_MAX,
 * below CAT_BODY + that still fits the channel's 4 KiB payload bound). */
static uint8_t  cat_reply[4096];

/* ─── CAT_QUERY scratch ────────────────────────────────────────────────────
 * struct SqlStatement embeds a Predicate (32 nodes × a 256-byte literal
 * each) plus 512 bytes of set-operator text — tens of kilobytes, never a
 * stack local. sql_exec.c keeps the same type in static scratch for the
 * same reason, and the re-entrancy argument is identical: this tick runs in
 * ONE non-IRQ kernel context (microkernel_service_poll), which is also what
 * makes cat_buf a single shared static rather than a per-call buffer. A
 * query's statement, engine result and SQL text therefore live here, one
 * query at a time. */
static struct SqlStatement cat_stmt;
static struct SqlResult    cat_result;
static uint8_t             cat_sql[CAT_SQL_MAX + 1];
static uint32_t cat_refusals_total = 0;
static uint32_t cat_dropped_total  = 0;
static uint32_t cat_served_total   = 0;

int env_catalog_service_register(uint16_t k_rd, uint16_t k_wr,
                                 uint32_t partition, uint32_t pid,
                                 const char* name) {
    struct EnvCatalogSvc* e = 0;
    /* Same (partition, pid) re-registering (an environment recycled, a pid
     * reused): replace the stale entry rather than leaking it — the same
     * replacement net_socket_service_register() performs. */
    for (uint32_t i = 0; i < ENV_CATALOG_SERVICE_MAX; i++) {
        if (cat_svc[i].used && cat_svc[i].partition == partition &&
            cat_svc[i].pid == pid) { e = &cat_svc[i]; break; }
    }
    if (!e) {
        for (uint32_t i = 0; i < ENV_CATALOG_SERVICE_MAX; i++) {
            if (!cat_svc[i].used) { e = &cat_svc[i]; break; }
        }
    }
    if (!e) {
        kernel_serial_printf(
            "[ENV-CATALOG] partition %u pid %u: registry full (%d) — catalog "
            "service left unregistered\n",
            (unsigned)partition, (unsigned)pid,
            (int)ENV_CATALOG_SERVICE_MAX);
        return 0;
    }
    e->used = 1;
    e->partition = partition;
    e->pid = pid;
    e->k_rd = k_rd;
    e->k_wr = k_wr;
    uint32_t i = 0;
    if (name)
        for (; name[i] && i + 1 < sizeof(e->name); i++) e->name[i] = name[i];
    e->name[i] = '\0';
    kernel_serial_printf(
        "[ENV-CATALOG] %s (partition %u, pid %u) wired: kernel ends rd=%u "
        "wr=%u — read-only catalog view open (§9)\n",
        e->name, (unsigned)partition, (unsigned)pid,
        (unsigned)k_rd, (unsigned)k_wr);
    return 1;
}

int env_catalog_service_kernel_slot(uint16_t slot) {
    for (uint32_t i = 0; i < ENV_CATALOG_SERVICE_MAX; i++)
        if (cat_svc[i].used && cat_svc[i].k_rd == slot) return 1;
    return 0;
}

uint32_t env_catalog_service_count(void) {
    uint32_t n = 0;
    for (uint32_t i = 0; i < ENV_CATALOG_SERVICE_MAX; i++)
        if (cat_svc[i].used) n++;
    return n;
}

uint32_t env_catalog_refusals(void) { return cat_refusals_total; }
uint32_t env_catalog_dropped(void)  { return cat_dropped_total; }

/* ─── the refusal renderer ───────────────────────────────────────────────
 * Hand-rolled because the kernel is freestanding — the same ec_put shape
 * net_socket_refusal_text() uses. Always terminates. */
static void cat_put(char** p, char* end, const char* s) {
    while (*s && *p < end) *(*p)++ = *s++;
}
static void cat_put_u32(char** p, char* end, uint32_t v) {
    char tmp[10];
    int n = 0;
    if (v == 0) tmp[n++] = '0';
    while (v > 0 && n < 10) { tmp[n++] = (char)('0' + (v % 10u)); v /= 10u; }
    while (n > 0 && *p < end) *(*p)++ = tmp[--n];
}

uint32_t env_catalog_refusal_text(uint16_t ty, char* out, uint32_t cap) {
    if (!out || cap == 0) return 0;
    char* p = out;
    char* end = out + (cap - 1);
    const char* verb = env_catalog_verb_name(ty);
    if (verb) {
        cat_put(&p, end, verb);
    } else {
        cat_put(&p, end, "verb ");
        cat_put_u32(&p, end, ty);
    }
    cat_put(&p, end, " refused by kernel.env.catalog"
                     " — not served by the §9 mediated surface");
    *p = '\0';
    return (uint32_t)(p - out);
}

/* ─── channel plumbing ────────────────────────────────────────────────────
 * The same derivation env_console.c's ec_chan_of() and nss_chan_of() make:
 * the tick must notice a peer that closed its end. */

static struct CapChannel* cat_chan_of(uint16_t slot, int* out_kdir) {
    uint64_t w = cap_tables[0].slots[slot].word;
    uint32_t obj_id = (uint32_t)((w >> CAP_OBJ_SHIFT) & CAP_OBJ_MASK);
    if (obj_id >= CAP_OBJECT_MAX) return 0;
    struct CapObject* o = &cap_objects[obj_id];
    if (!o->active || o->kind != CAP_OBJ_KIND_CHAN || o->chan_id >= CAP_CHAN_MAX)
        return 0;
    struct CapChannel* ch = &cap_channels[o->chan_id];
    if (out_kdir) *out_kdir = (0 == ch->end0_pid) ? 0 : 1;
    return ch;
}

static void cat_release(struct EnvCatalogSvc* e) {
    uint16_t rd = e->k_rd;
    /* Revoke the kernel end — the sidecar already closed its half, so this
     * frees the kernel's CHAN_R and destroys the channel, the same
     * close-aware teardown nss_release() performs. There is no per-channel
     * state to unwind here: this service holds no sockets, no connections
     * and no attribution, which is a direct consequence of being read-only.
     */
    if (rd != 0xFFFFu) cap_revoke(0, rd);
    e->used = 0;
    e->pid = 0;
    e->partition = 0;
    e->k_rd = 0xFFFFu;
    e->k_wr = 0xFFFFu;
    e->name[0] = '\0';
}

/* ─── wire helpers ──────────────────────────────────────────────────────── */

static void cat_wire_u16(uint8_t* p, uint16_t v) {
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)(v >> 8);
}
static void cat_wire_u32(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
    p[2] = (uint8_t)((v >> 16) & 0xFFu);
    p[3] = (uint8_t)((v >> 24) & 0xFFu);
}
static uint16_t cat_get_u16(const uint8_t* p) {
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}
static int cat_is_catalog_frame(const uint8_t* b, uint32_t n) {
    if (n < CAT_HDR) return 0;
    for (uint32_t i = 0; i < CAT_MAGIC_LEN; i++)
        if (b[i] != cat_magic[i]) return 0;
    return cat_get_u16(b + 8) == 1;   /* version 1 */
}

static void cat_send(struct EnvCatalogSvc* e, const uint8_t* payload,
                     uint32_t len, uint32_t tag) {
    if (cap_send_msg(0, e->k_wr, payload, len, 0, 0, tag, 0) != 0) {
        /* The peer is gone between drain and reply: retire now rather than
         * waiting for the next tick's close check to notice. */
        cat_release(e);
    }
}

/* The reply header: echoes the request's magic and verb, sets the error
 * flag when `err`, and writes the STATUS word — at CAT_STATUS on every
 * reply, clean or refused, so a client reads status without first branching
 * on the flag (the discipline nss_reply_ok() gives a NET_* reply). The body
 * starts at CAT_BODY: a verb's own payload on a clean answer, the rendered
 * refusal on an error. Writing the body over the status word — which is what
 * a body at CAT_HDR did — is what env_catalog_host_test.c's INFO clause
 * caught: a CAT_LIST of two objects would have answered status 2 (PERM). */
static void cat_hdr(uint16_t ty, uint16_t status, int err) {
    for (uint32_t i = 0; i < 8; i++) cat_reply[i] = cat_buf[i];
    cat_wire_u16(cat_reply + 8, 1);
    cat_wire_u16(cat_reply + 10, ty);
    cat_wire_u16(cat_reply + 12, err ? CAT_FLAG_ERROR : 0);
    cat_wire_u16(cat_reply + 14, 0);
    cat_wire_u16(cat_reply + 16, status);
}

/* A refusal: the header carries CAT_FLAG_ERROR and the status, and the
 * payload appends the reason rendered BY NAME after the status body. The
 * wire's parse reads the status and ignores the text; the text's audience
 * is the host test, the serial line and the live clause — the same split
 * nss_refuse() draws. */
static void cat_refuse(struct EnvCatalogSvc* e, uint16_t ty, uint32_t tag,
                       uint16_t status, const char* why) {
    char text[CAT_TEXT_CAP];
    uint32_t tlen = env_catalog_refusal_text(ty, text, sizeof text);
    /* The `why` detail rides AFTER the rendered reason, so the caller and the
     * transcript see the same words: "… refused by kernel.env.catalog — not
     * served by the §9 mediated surface: SYSTEM_METADATA is not tenant data".
     * That is what "refused by name" has to mean for the tenant — §9.1 says
     * the refusal is surfaced "so the answer is visible in a transcript
     * instead of silent", and the transcript that matters is the one the
     * CALLER reads, not only the kernel's serial line. The wire's parse
     * reads the status and ignores the text; the text's audience is the host
     * test, the serial line and the live clause — the same split nss_refuse()
     * draws. */
    if (why) {
        static const char sep[] = ": ";
        for (uint32_t i = 0; i + 1 < sizeof sep && tlen + 1 < sizeof text; i++)
            text[tlen++] = sep[i];
        for (uint32_t i = 0; why[i] && tlen + 1 < sizeof text; i++)
            text[tlen++] = why[i];
        text[tlen] = '\0';
    }
    cat_hdr(ty, status, 1);
    for (uint32_t i = 0; i < tlen; i++)
        cat_reply[CAT_BODY + i] = (uint8_t)text[i];
    cat_refusals_total++;
    kernel_serial_printf("[ENV-CATALOG] %s (partition %u, pid %u): %s\n",
                         e->name, (unsigned)e->partition, (unsigned)e->pid,
                         text);
    cat_send(e, cat_reply, CAT_BODY + tlen, tag);
}

/* ─── the namespace ───────────────────────────────────────────────────────
 * THE boundary, in one function. An object is in a channel's namespace if
 * and only if it is active AND its catalog partition_id equals the
 * partition this channel REGISTERED with — the partition the kernel
 * verified in cap_create_sidecar(), never one the wire can state.
 *
 * A foreign-partition entry is skipped by the same `continue` that skips an
 * inactive one: it is not found, and "not found" is the only answer a
 * foreign name ever gets. See env_catalog.h — a distinct "denied" status
 * here would confirm a name exists somewhere the caller cannot see. */

static int cat_name_eq(const char* catalog_name, const uint8_t* want,
                       uint32_t len) {
    if (len == 0 || len >= CAT_MAX_NAME) return 0;
    for (uint32_t i = 0; i < len; i++)
        if (catalog_name[i] != (char)want[i]) return 0;
    return catalog_name[len] == '\0';   /* exact, not a prefix */
}

/* Index of `name` inside `partition`, or -1 — including for a name that
 * exists in a different partition, which is exactly the point. */
static int cat_find(const uint8_t* name, uint32_t len, uint32_t partition) {
    if (!name || len == 0 || len >= CAT_MAX_NAME) return -1;
    uint32_t n = object_catalog_count;
    if (n > CATALOG_MAX_OBJECTS) n = CATALOG_MAX_OBJECTS;
    for (uint32_t i = 0; i < n; i++) {
        const struct SLSObjectEntry* o = &object_catalog[i];
        if (!o->active) continue;
        if (o->partition_id != partition) continue;   /* the boundary */
        if (!cat_name_eq(o->name, name, len)) continue;
        return (int)i;
    }
    return -1;
}

/* How many objects this partition's namespace holds — CAT_LIST's `total`,
 * so a client knows how many pages exist before it asks for the next. */
static uint32_t cat_partition_count(uint32_t partition) {
    uint32_t n = object_catalog_count, c = 0;
    if (n > CATALOG_MAX_OBJECTS) n = CATALOG_MAX_OBJECTS;
    for (uint32_t i = 0; i < n; i++) {
        const struct SLSObjectEntry* o = &object_catalog[i];
        if (o->active && o->partition_id == partition) c++;
    }
    return c;
}

/* ─── the verbs ─────────────────────────────────────────────────────────── */

static uint32_t cat_strnlen(const char* s, uint32_t max) {
    uint32_t n = 0;
    while (n < max && s[n]) n++;
    return n;
}

static int cat_streq(const char* a, const char* b) {
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

/* ─── CAT_QUERY: one page of rows ──────────────────────────────────────────
 * The cursor hands rows to this callback one at a time; the page is built
 * here so the two bounds (row count and bytes) are enforced in one place.
 * `map` decides a row's SHAPE: NULL means "this row already carries exactly
 * the reported columns" (SELECT *, and every aggregate/group result, whose
 * rows are built from the projection); non-NULL is the projection case,
 * where the engine hands back the FULL row and `map[c]` says which of its
 * cells output column c came from — see the arm below for why that split is
 * the engine's documented behaviour rather than a guess. */
struct cat_qpage {
    uint8_t* out;        /* reply base (cat_reply) */
    uint32_t cap;        /* first byte past the body budget */
    uint32_t off;        /* next byte to write */
    uint32_t skip;       /* rows this page does not carry (page * limit) */
    uint32_t want;       /* rows this page may carry */
    uint32_t seen;       /* rows the cursor has delivered so far */
    uint32_t ncols;      /* output columns */
    uint32_t written;
    const uint8_t* map;  /* projection: output col -> cell index; 0 = identity */
    uint8_t  more;
    uint8_t  over_budget;
    uint8_t  mismatch;
};

static void cat_query_row(struct RowId id, const struct RowValues* v, void* p) {
    (void)id;
    struct cat_qpage* g = (struct cat_qpage*)p;
    if (g->mismatch) return;
    if (g->seen < g->skip) { g->seen++; return; }
    if (g->written >= g->want) { g->more = 1; return; }
    g->seen++;

    uint32_t need = 0;
    for (uint32_t c = 0; c < g->ncols; c++) {
        uint32_t src = g->map ? (uint32_t)g->map[c] : c;
        if (src >= v->count) { g->mismatch = 1; return; }
        uint32_t len = cat_strnlen(v->values[src], RECORD_VAL_LEN);
        need += 2u + len;
    }
    if (g->off + need > g->cap) {
        /* The byte bound stopped the page — `more` says so, so a client
         * pages on rather than believing the result ended here. */
        g->over_budget = 1;
        g->more = 1;
        return;
    }
    for (uint32_t c = 0; c < g->ncols; c++) {
        uint32_t src = g->map ? (uint32_t)g->map[c] : c;
        uint32_t len = cat_strnlen(v->values[src], RECORD_VAL_LEN);
        int is_null = (v->null_mask & (uint16_t)(1u << src)) != 0;
        g->out[g->off++] = (uint8_t)(is_null ? 1u : 0u);   /* cell flags */
        g->out[g->off++] = (uint8_t)len;
        for (uint32_t i = 0; i < len; i++)
            g->out[g->off++] = (uint8_t)v->values[src][i];
    }
    g->written++;
}

/* Answer one drained frame. */
static void cat_handle(struct EnvCatalogSvc* e, uint32_t plen, uint32_t tag) {
    if (!cat_is_catalog_frame(cat_buf, plen)) {
        /* Not a CAT_* frame. Nothing that speaks to this channel should
         * produce one — the only sender is the tenant's catalog client — so
         * it is counted rather than answered: a reply would be parsed as an
         * error by a client that never sent this. Same reasoning as
         * nss_is_net_frame()'s drop. */
        cat_dropped_total++;
        return;
    }
    uint16_t ty = cat_get_u16(cat_buf + 10);

    switch (ty) {

    case CAT_VERB_INFO: {
        /* The handshake, answered honestly — and it answers the partition
         * the KERNEL believes this caller is in, so a tenant can verify the
         * identity the boundary is computed from instead of trusting that
         * it was attached correctly. */
        cat_hdr(ty, CAT_OK, 0);
        cat_wire_u32(cat_reply + CAT_BODY + 0,  CATALOG_MAX_OBJECTS);
        cat_wire_u32(cat_reply + CAT_BODY + 4,  CAT_MAX_NAME);
        cat_wire_u32(cat_reply + CAT_BODY + 8,  CAT_MAX_VALUE);
        cat_wire_u32(cat_reply + CAT_BODY + 12, e->partition);
        cat_served_total++;
        cat_send(e, cat_reply, CAT_BODY + 16, tag);
        return;
    }

    case CAT_VERB_LIST: {
        /* {page u16, page_size u16}. Fixed-slot entries, so a reply is
         * never truncated and never needs a length-negotiation dance: the
         * catalog's names are already bounded by OBJECT_NAME_LEN. */
        if (plen < CAT_HDR + 4) { cat_refuse(e, ty, tag, CAT_ERR_SHAPE, 0); return; }
        uint16_t page     = cat_get_u16(cat_buf + CAT_HDR);
        uint16_t page_sz  = cat_get_u16(cat_buf + CAT_HDR + 2);
        if (page_sz == 0 || page_sz > CAT_LIST_PAGE_MAX) {
            cat_refuse(e, ty, tag, CAT_ERR_SHAPE, 0);
            return;
        }
        uint32_t total = cat_partition_count(e->partition);
        uint32_t skip  = (uint32_t)page * (uint32_t)page_sz;
        uint32_t count = 0;
        uint32_t seen  = 0;

        cat_hdr(ty, CAT_OK, 0);
        uint8_t* out = cat_reply + CAT_LIST_HDR;

        uint32_t n = object_catalog_count;
        if (n > CATALOG_MAX_OBJECTS) n = CATALOG_MAX_OBJECTS;
        for (uint32_t i = 0; i < n && count < page_sz; i++) {
            const struct SLSObjectEntry* o = &object_catalog[i];
            if (!o->active) continue;
            if (o->partition_id != e->partition) continue;  /* the boundary */
            if (seen++ < skip) continue;
            out[0] = (uint8_t)o->type;
            out[1] = (uint8_t)o->storage_tier;
            out[2] = 0;
            out[3] = 0;
            uint32_t j = 0;
            for (; o->name[j] && j + 1 < CAT_MAX_NAME; j++)
                out[4 + j] = (uint8_t)o->name[j];
            out[4 + j] = '\0';
            out += CAT_LIST_ENTRY;
            count++;
        }
        cat_wire_u16(cat_reply + CAT_BODY + 0, (uint16_t)count);
        cat_wire_u16(cat_reply + CAT_BODY + 2, (uint16_t)total);
        cat_wire_u32(cat_reply + CAT_BODY + 4, e->partition);
        cat_served_total++;
        cat_send(e, cat_reply, CAT_LIST_HDR + count * CAT_LIST_ENTRY, tag);
        return;
    }

    case CAT_VERB_STAT: {
        /* {name_len u8, name[name_len]} -> {type u8, tier u8, pad u16,
         * size_pages u32, owner_uid u32, database_id u32}. */
        if (plen < CAT_HDR + 1) { cat_refuse(e, ty, tag, CAT_ERR_SHAPE, 0); return; }
        uint8_t nlen = cat_buf[CAT_HDR];
        if (nlen == 0 || nlen >= CAT_MAX_NAME || plen < CAT_HDR + 1u + nlen) {
            cat_refuse(e, ty, tag, CAT_ERR_SHAPE, 0);
            return;
        }
        int idx = cat_find(cat_buf + CAT_HDR + 1, nlen, e->partition);
        if (idx < 0) { cat_refuse(e, ty, tag, CAT_ERR_NOENT, 0); return; }
        const struct SLSObjectEntry* o = &object_catalog[idx];

        /* In the namespace, and refused anyway. object_catalog.h's role
         * table gives APP_USER no read on SYSTEM_METADATA; the catalog does
         * not re-derive that policy, it surfaces the refusal BY NAME so the
         * answer is visible in a transcript instead of silent. */
        if (o->type == OBJ_TYPE_SYSTEM_METADATA) {
            cat_refuse(e, ty, tag, CAT_ERR_PERM, "SYSTEM_METADATA is not tenant data");
            return;
        }
        cat_hdr(ty, CAT_OK, 0);
        cat_reply[CAT_BODY + 0] = (uint8_t)o->type;
        cat_reply[CAT_BODY + 1] = (uint8_t)o->storage_tier;
        cat_reply[CAT_BODY + 2] = 0;
        cat_reply[CAT_BODY + 3] = 0;
        cat_wire_u32(cat_reply + CAT_BODY + 4,  o->size_pages);
        cat_wire_u32(cat_reply + CAT_BODY + 8,  o->owner_uid);
        cat_wire_u32(cat_reply + CAT_BODY + 12, o->database_id);
        cat_served_total++;
        cat_send(e, cat_reply, CAT_BODY + 16, tag);
        return;
    }

    case CAT_VERB_READ: {
        /* {name_len u8, name[nl], key_len u8, key[kl]} ->
         * {key_len u8, pad u8, value_len u16, key[CAT_MAX_KEY],
         * value[CAT_MAX_VALUE]}. Fixed fields, for the same reason CAT_LIST
         * uses fixed slots: no truncation logic, no negotiation, and the
         * offsets a guard pins are unambiguous. */
        if (plen < CAT_HDR + 2) { cat_refuse(e, ty, tag, CAT_ERR_SHAPE, 0); return; }
        uint8_t nlen = cat_buf[CAT_HDR];
        if (nlen == 0 || nlen >= CAT_MAX_NAME ||
            plen < CAT_HDR + 1u + nlen + 1u) {
            cat_refuse(e, ty, tag, CAT_ERR_SHAPE, 0);
            return;
        }
        uint8_t klen = cat_buf[CAT_HDR + 1u + nlen];
        if (klen >= CAT_MAX_KEY ||
            plen < CAT_HDR + 2u + nlen + klen) {
            cat_refuse(e, ty, tag, CAT_ERR_SHAPE, 0);
            return;
        }
        const uint8_t* kname = cat_buf + CAT_HDR + 1;
        const uint8_t* kkey  = cat_buf + CAT_HDR + 2u + nlen;

        int idx = cat_find(kname, nlen, e->partition);
        if (idx < 0) { cat_refuse(e, ty, tag, CAT_ERR_NOENT, 0); return; }
        const struct SLSObjectEntry* o = &object_catalog[idx];

        if (o->type == OBJ_TYPE_SYSTEM_METADATA) {
            cat_refuse(e, ty, tag, CAT_ERR_PERM, "SYSTEM_METADATA is not tenant data");
            return;
        }
        /* Phase 24's own guard, inherited verbatim: sys_sls_select() names
         * the SQL path for a row-set table, and CAT_READ IS the legacy
         * record path. Routing SQL through a mediated byte channel is a
         * larger design than §9's first cut — refusing BY NAME says so,
         * where a legacy read would silently return a view of a table that
         * no longer keeps its rows there. */
        if (o->uses_rowstore) {
            cat_refuse(e, ty, tag, CAT_ERR_TYPE,
                       "rowstore-backed table — use the SQL path");
            return;
        }

        /* The legacy KV record path, read directly: the catalog owns
         * object_records[], index-aligned with object_catalog[] by
         * construction (rowstore.h's own note). No locking is taken here
         * for the same reason the legacy sys_sls_select() path does not
         * take lock_mgr's lock for its READ arm beyond what it already
         * does — this service adds no new writer, so it cannot add a new
         * race; it is another reader of a table the kernel already reads
         * from a single non-IRQ context. */
        const struct SLSObjectRecord* rec = &object_records[idx];
        const char* value = 0;
        for (uint32_t f = 0; f < RECORD_MAX_FIELDS && f < rec->field_count; f++) {
            if (!rec->fields[f].active) continue;
            if (!cat_name_eq(rec->fields[f].key, kkey, klen)) continue;
            value = rec->fields[f].value;
            break;
        }
        if (!value) { cat_refuse(e, ty, tag, CAT_ERR_NOENT, 0); return; }

        cat_hdr(ty, CAT_OK, 0);
        cat_reply[CAT_BODY + 0] = klen;
        cat_reply[CAT_BODY + 1] = 0;
        uint32_t vlen = 0;
        while (value[vlen] && vlen < CAT_MAX_VALUE) vlen++;
        cat_wire_u16(cat_reply + CAT_BODY + 2, (uint16_t)vlen);
        for (uint32_t i = 0; i < klen; i++)
            cat_reply[CAT_BODY + 4 + i] = kkey[i];
        for (uint32_t i = 0; i < vlen; i++)
            cat_reply[CAT_BODY + 4 + CAT_MAX_KEY + i] = (uint8_t)value[i];
        cat_served_total++;
        cat_send(e, cat_reply, CAT_BODY + 4 + CAT_MAX_KEY + vlen, tag);
        return;
    }

    case CAT_VERB_QUERY: {
        /* {sql_len u16, page u16, limit u8, flags u8, sql[sql_len]}.
         * The ORDER of the gates below is the design — env_catalog.h's
         * CAT_QUERY section numbers them 1..9 — and the order is what makes
         * the boundary hold: nothing resolves a name until the statement
         * has parsed, and nothing executes until the name has resolved
         * INSIDE this channel's partition. */

        /* 1. SHAPE. */
        if (plen < CAT_HDR + 6) { cat_refuse(e, ty, tag, CAT_ERR_SHAPE, 0); return; }
        uint16_t slen = cat_get_u16(cat_buf + CAT_HDR);
        uint16_t page = cat_get_u16(cat_buf + CAT_HDR + 2);
        uint8_t  lim  = cat_buf[CAT_HDR + 4];
        int bad = (cat_buf[CAT_HDR + 5] != 0) || slen == 0 || slen > CAT_SQL_MAX ||
                  lim == 0 || lim > CAT_QUERY_ROWS_MAX ||
                  plen < CAT_HDR + 6u + slen;
        /* An embedded NUL would make the engine parse a PREFIX of what the
         * caller believes it sent — a statement the caller never wrote. */
        for (uint32_t i = 0; !bad && i < (uint32_t)slen; i++)
            if (cat_buf[CAT_HDR + 6u + i] == 0) bad = 1;
        if (bad) { cat_refuse(e, ty, tag, CAT_ERR_SHAPE, 0); return; }
        for (uint32_t i = 0; i < (uint32_t)slen; i++)
            cat_sql[i] = cat_buf[CAT_HDR + 6u + i];
        cat_sql[slen] = '\0';

        /* 2. PARSE first — before any name is looked up, so a statement the
         *    kernel has not understood never reaches the catalog at all. */
        char perr[SQL_ERR_MSG_LEN];
        perr[0] = '\0';
        if (sql_parse((const char*)cat_sql, &cat_stmt, perr,
                      (uint32_t)sizeof perr) != 0) {
            cat_refuse(e, ty, tag, CAT_ERR_QUERY,
                       perr[0] ? perr : "the statement did not parse");
            return;
        }

        /* 3. Shape by meaning. Only a plain, single-table SELECT is served:
         *    every other form can name a SECOND table, and a second table
         *    would have to be resolved outside the partition check below.
         *    Each is refused BY NAME so a tenant learns which construct the
         *    surface does not carry rather than a bare number. */
        if (cat_stmt.kind != SQL_STMT_SELECT) {
            cat_refuse(e, ty, tag, CAT_ERR_TYPE,
                       "only SELECT is served on this surface");
            return;
        }
        const struct SqlSelectStmt* s = &cat_stmt.u.select;
        if (s->has_join) {
            cat_refuse(e, ty, tag, CAT_ERR_TYPE,
                       "a JOIN is not served — one table per query");
            return;
        }
        if (s->has_set_op) {
            cat_refuse(e, ty, tag, CAT_ERR_TYPE,
                       "a set operator is not served — one table per query");
            return;
        }
        if (s->has_cte) {
            cat_refuse(e, ty, tag, CAT_ERR_TYPE,
                       "a CTE is not served — one table per query");
            return;
        }
        if (s->where.subquery_count || s->having.subquery_count) {
            cat_refuse(e, ty, tag, CAT_ERR_TYPE,
                       "a subquery is not served — one table per query");
            return;
        }

        /* 4. THE BOUNDARY: one name, resolved inside THIS channel's
         *    partition. A view is not a catalog object, so `FROM v` lands
         *    here too and answers NOENT — views are not served by name. */
        uint32_t tlen = cat_strnlen(s->table_name, OBJECT_NAME_LEN);
        int idx = cat_find((const uint8_t*)s->table_name, tlen, e->partition);
        if (idx < 0) { cat_refuse(e, ty, tag, CAT_ERR_NOENT, 0); return; }
        const struct SLSObjectEntry* o = &object_catalog[idx];

        /* 5./6. The same two in-namespace refusals every other verb makes. */
        if (o->type == OBJ_TYPE_SYSTEM_METADATA) {
            cat_refuse(e, ty, tag, CAT_ERR_PERM, "SYSTEM_METADATA is not tenant data");
            return;
        }
        if (!o->uses_rowstore) {
            cat_refuse(e, ty, tag, CAT_ERR_TYPE,
                       "not a row-set table — use CAT_READ");
            return;
        }

        /* 7. THE UID: read off the process this channel was registered for,
         *    never off the wire. The descriptor's own partition must be the
         *    channel's — defence in depth against a registration that outlived
         *    the process it was minted for. */
        uint32_t uid = 0;
        int found = 0, same_partition = 0;
        for (uint32_t i = 0; i < (uint32_t)PROC_MAX; i++) {
            if (proc_table[i].active && proc_table[i].pid == e->pid) {
                uid = proc_table[i].owner_uid;
                found = 1;
                same_partition = (proc_table[i].partition_id == e->partition);
                break;
            }
        }
        if (!found) {
            cat_refuse(e, ty, tag, CAT_ERR_PERM,
                       "the caller's pid no longer stands");
            return;
        }
        if (!same_partition) {
            cat_refuse(e, ty, tag, CAT_ERR_PERM,
                       "the caller's process is not in this channel's partition");
            return;
        }

        /* 8. POLICY: the SQL path's OWN row-level check, surfaced by name.
         *    The engine would deny it too (mvcc.c's scan gate), but a denial
         *    there returns an EMPTY result — indistinguishable from a table
         *    with no matching rows. Refusing before executing is what keeps
         *    "denied" and "empty" different answers, which is the same
         *    distinction §9.1 draws between PERM and NOENT. */
        if (!catalog_check_access(uid, s->table_name, PERM_READ)) {
            cat_refuse(e, ty, tag, CAT_ERR_PERM,
                       "row-level policy denied this uid");
            return;
        }

        /* 9. EXECUTE — read-only, because step 3 admitted SELECT alone. */
        if (sql_execute(uid, (const char*)cat_sql, &cat_result) != 0 ||
            cat_result.error != SQL_ERR_NONE) {
            cat_refuse(e, ty, tag, CAT_ERR_QUERY,
                       cat_result.error_msg[0]
                           ? cat_result.error_msg
                           : "the SQL engine refused the statement");
            return;
        }
        if (cat_result.kind != SQL_STMT_SELECT) {
            /* Belt: step 3 admitted SELECT only, so this cannot fire — kept
             * because "cannot fire" is a claim a reader should be able to
             * check at the call site rather than in a comment. */
            cat_refuse(e, ty, tag, CAT_ERR_TYPE,
                       "only SELECT is served on this surface");
            return;
        }
        if (cat_result.column_count == 0 ||
            cat_result.column_count > ROWSTORE_MAX_COLUMNS) {
            cat_refuse(e, ty, tag, CAT_ERR_QUERY,
                       "the result carries no representable columns");
            return;
        }
        uint32_t ncols = cat_result.column_count;

        /* The projection question, answered from the engine's own contract:
         * for `SELECT *` it fills columns[] from the layout, and for a
         * projection it fills them from the SELECT list while the row still
         * carries the FULL underlying row (sql_exec.h's documented "rows
         * always carry the full row"). So a projection needs a map from
         * output column -> cell index, and the layout is what supplies it.
         * Aggregate/group results are the opposite: their rows are built
         * FROM the projection, so they are already in step. */
        uint8_t map[ROWSTORE_MAX_COLUMNS];
        const uint8_t* mapp = 0;
        int aggregate = (s->has_aggregates || s->has_group_by) ? 1 : 0;
        if (!aggregate && !s->select_all) {
            if (!table_headers[idx].active) {
                cat_refuse(e, ty, tag, CAT_ERR_QUERY,
                           "the table has no row-set layout");
                return;
            }
            const struct RowTableLayout* lay = &table_headers[idx].layout;
            for (uint32_t c = 0; c < ncols; c++) {
                uint32_t hit = 0xFFFFFFFFu;
                for (uint32_t k = 0; k < lay->column_count; k++) {
                    if (cat_streq(lay->column_names[k], cat_result.columns[c])) {
                        hit = k;
                        break;
                    }
                }
                if (hit == 0xFFFFFFFFu || hit > 0xFFu) {
                    cat_refuse(e, ty, tag, CAT_ERR_QUERY,
                               "a projected column is not in the table's layout");
                    return;
                }
                map[c] = (uint8_t)hit;
            }
            mapp = map;
        }

        /* Build the page. The head is written first because its offsets are
         * known, and PATCHED after the cursor has delivered the rows — nrows
         * and `more` only exist once the page does. */
        cat_hdr(ty, CAT_OK, 0);
        uint8_t* body = cat_reply + CAT_BODY;
        body[0] = (uint8_t)ncols;
        body[1] = 0;                                   /* nrows: patched */
        body[2] = 0;                                   /* more: patched */
        body[3] = cat_result.truncated ? 1u : 0u;
        cat_wire_u16(body + 4, page);
        cat_wire_u16(body + 6, 0);
        cat_wire_u32(body + 8, cat_result.row_count);  /* rows in the whole result */

        uint32_t off = CAT_BODY + 12;
        for (uint32_t c = 0; c < ncols; c++) {
            uint32_t nlen = cat_strnlen(cat_result.columns[c], RECORD_KEY_LEN);
            if (nlen > 0xFFu) nlen = 0xFFu;
            if (off + 1u + nlen >= CAT_BODY + CAT_QUERY_BODY_MAX) {
                if (cat_result.cursor_id) cursor_close(cat_result.cursor_id);
                cat_refuse(e, ty, tag, CAT_ERR_QUERY,
                           "the column names exceed the reply budget");
                return;
            }
            cat_reply[off++] = (uint8_t)nlen;
            for (uint32_t i = 0; i < nlen; i++)
                cat_reply[off++] = (uint8_t)cat_result.columns[c][i];
        }

        struct cat_qpage g;
        g.out = cat_reply;
        g.cap = CAT_BODY + CAT_QUERY_BODY_MAX;
        g.off = off;
        g.skip = (uint32_t)page * (uint32_t)lim;
        g.want = lim;
        g.seen = 0;
        g.ncols = ncols;
        g.written = 0;
        g.map = mapp;
        g.more = 0;
        g.over_budget = 0;
        g.mismatch = 0;

        if (cat_result.cursor_id) {
            uint32_t fetch = g.skip + g.want + 1;   /* +1: see the row past the page */
            if (fetch > CURSOR_MAX_ROWSET_ROWS) fetch = CURSOR_MAX_ROWSET_ROWS;
            cursor_fetch_rows(cat_result.cursor_id, fetch, cat_query_row, &g);
            cursor_close(cat_result.cursor_id);
        } else if (cat_result.row_count != 0) {
            cat_refuse(e, ty, tag, CAT_ERR_QUERY,
                       "the statement reported rows but opened no result set");
            return;
        }

        if (g.mismatch) {
            cat_refuse(e, ty, tag, CAT_ERR_QUERY,
                       "a result row does not match the reported columns");
            return;
        }
        if (g.written == 0 && g.over_budget) {
            cat_refuse(e, ty, tag, CAT_ERR_QUERY,
                       "one row exceeds the reply budget");
            return;
        }

        body[1] = (uint8_t)g.written;
        body[2] = (g.more || g.skip + g.written < cat_result.row_count) ? 1u : 0u;
        cat_served_total++;
        /* g.off, not the pre-fetch `off`: the cursor wrote the rows past the
         * name block, and sending the head-and-names length would hand a
         * client a page whose nrows promises rows the bytes do not carry. */
        cat_send(e, cat_reply, g.off, tag);
        return;
    }

    default:
        /* Any verb outside the table: refused by number — the same
         * contract P2's increment 1 keeps for the types no increment
         * defines. There is no write verb in this table at all, which is
         * the point of §9's first cut; a CAT_WRITE asking here is refused
         * like any other unknown, not specially. */
        cat_refuse(e, ty, tag, CAT_ERR_UNKNOWN, 0);
        return;
    }
}

void env_catalog_service_tick(void) {
    for (uint32_t i = 0; i < ENV_CATALOG_SERVICE_MAX; i++) {
        struct EnvCatalogSvc* e = &cat_svc[i];
        if (!e->used) continue;

        for (;;) {
            uint32_t plen = 0, tag = 0, flags = 0;
            static struct SLSCapDesc cat_out_caps[CAP_MSG_MAX_CAPS];
            static uint16_t cat_out_n_caps;
            cat_out_n_caps = 0;
            int r = cap_recv_msg(0, e->k_rd, cat_buf,
                                 (uint32_t)sizeof cat_buf,
                                 &plen, CAP_MSG_MAX_CAPS, cat_out_caps,
                                 &cat_out_n_caps, &tag, &flags);
            if (r != 0) break;
            if (plen > sizeof cat_buf) plen = (uint32_t)sizeof cat_buf;
            cat_handle(e, plen, tag);
            if (!e->used) break;   /* cat_send retired it mid-drain */
        }
        if (!e->used) continue;

        /* A closed peer ends the channel — the same close-awareness
         * console_service_tick(), env_console_tick() and
         * net_socket_service_tick() apply, so a destroyed environment
         * leaves no dead registration behind. */
        int kdir = 0;
        struct CapChannel* ch = cat_chan_of(e->k_rd, &kdir);
        int ended = 0;
        if (!ch) {
            ended = 1;
        } else {
            cap_lock(&ch->lock);
            ended = ch->close_evt[kdir];
            cap_unlock(&ch->lock);
        }
        if (ended) {
            if (!ch) e->k_rd = 0xFFFFu;  /* nothing left to revoke */
            cat_release(e);
        }
    }
}
