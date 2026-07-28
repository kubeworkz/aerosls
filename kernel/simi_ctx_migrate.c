/*
 * simi_ctx_migrate.c — live execution-context migration over DSPP
 * (Persistent Execution Contexts, Phase 3). See simi_ctx_migrate.h for
 * the design and the deliberate one-transfer-at-a-time scope.
 *
 * Freestanding: local helpers, no libc, per this codebase's per-file
 * convention (p_memcpy in persist.c, ck_memcpy in simi_ckpt.c).
 */
#include "simi_ctx_migrate.h"
#include "../net/dspp.h"
#include "kernel_io.h"

/* ─── Local helpers ─────────────────────────────────────────────────── */
static void cm_memcpy(void* d, const void* s, uint32_t n) {
    uint8_t* dd = (uint8_t*)d;
    const uint8_t* ss = (const uint8_t*)s;
    while (n--) *dd++ = *ss++;
}
static void cm_memzero(void* d, uint32_t n) {
    uint8_t* p = (uint8_t*)d;
    while (n--) *p++ = 0;
}
static void cm_strncpy(char* d, const char* s, uint32_t n) {
    uint32_t i;
    for (i = 0; i + 1 < n && s && s[i]; i++) d[i] = s[i];
    d[i] = '\0';
}

const char* simi_ctx_mig_status_name(SimiCtxMigStatus s) {
    switch (s) {
        case SIMI_CTXMIG_OK:             return "OK";
        case SIMI_CTXMIG_ERR_BUSY:       return "ERR:transfer-already-in-flight";
        case SIMI_CTXMIG_ERR_TOO_LARGE:  return "ERR:checkpoint-too-large";
        case SIMI_CTXMIG_ERR_NO_TRANSFER:return "ERR:unknown-transfer";
        case SIMI_CTXMIG_ERR_BAD_CHUNK:  return "ERR:bad-chunk";
        case SIMI_CTXMIG_ERR_NO_PROGRAM: return "ERR:no-matching-program-image";
        case SIMI_CTXMIG_ERR_RESTORE:    return "ERR:restore-refused";
        default:                         return "ERR:unknown";
    }
}

/* Worst case: a full-depth context. Frames are 9228 bytes each, so at
 * SIMI_MAX_FRAMES=32 this is ~353 KiB. Sized from the real structs rather
 * than a magic number, so raising SIMI_MAX_FRAMES cannot silently make
 * every deep checkpoint undeliverable. */
#define CM_MAX_CKPT_BYTES  ((uint32_t)sizeof(struct SimiCkptHeader) \
                          + SIMI_MAX_FRAMES * (uint32_t)sizeof(struct SimiFrame) \
                          + SIMI_MEM_SIZE)

/* Static, not stack -- same reason dspp.c's frame_buf and stream.c's
 * dir_buf are static: hundreds of KiB is far past what this freestanding
 * kernel budgets for a single local. */
static uint8_t cm_send_buf[CM_MAX_CKPT_BYTES];

/* ─── Program-image registry ──────────────────────────────────────────
 * Checkpoints carry a program HASH, not the program. Restoring against
 * the wrong image would resume at a valid-looking instruction index in
 * unrelated code, so the receiver must find a local image whose hash
 * matches or refuse outright. */
#define CM_MAX_IMAGES 16
static const SimiObject* cm_images[CM_MAX_IMAGES];
static uint32_t          cm_image_count = 0;

void simi_ctx_migrate_register_image(const SimiObject* obj) {
    if (!obj || cm_image_count >= CM_MAX_IMAGES) return;
    for (uint32_t i = 0; i < cm_image_count; i++)
        if (cm_images[i] == obj) return;          /* idempotent */
    cm_images[cm_image_count++] = obj;
}

void simi_ctx_migrate_clear_images(void) {
    cm_image_count = 0;
    for (uint32_t i = 0; i < CM_MAX_IMAGES; i++) cm_images[i] = 0;
}

static const SimiObject* cm_find_image(uint64_t program_hash) {
    if (program_hash == 0) return 0;              /* 0 is "no image" -- never a match */
    for (uint32_t i = 0; i < cm_image_count; i++) {
        if (cm_images[i] && simi_ckpt_program_hash(cm_images[i]) == program_hash)
            return cm_images[i];
    }
    return 0;
}

/* ─── Inbound reassembly ──────────────────────────────────────────────── */
static struct {
    int      active;
    uint64_t transfer_id;
    uint64_t total_bytes;
    uint32_t total_chunks;
    uint32_t chunks_seen;
    uint64_t program_hash;
    char     ctx_name[DSPP_CTX_NAME_LEN];
    /* Which chunks have arrived. Counting alone would accept the same
     * chunk N times and call the transfer complete with holes in it --
     * and a hole is not detectable afterwards, because the checkpoint
     * checksum would simply fail with no indication of why. */
    uint8_t  chunk_present[(CM_MAX_CKPT_BYTES / DSPP_CTX_CHUNK_BYTES) + 2];
    uint8_t  buf[CM_MAX_CKPT_BYTES];
} cm_rx;

static struct SimiContext cm_landed_ctx;
static int                cm_landed_valid = 0;

struct SimiContext* simi_ctx_migrate_landed(void) { return &cm_landed_ctx; }
int                 simi_ctx_migrate_arrived(void) { return cm_landed_valid; }

void simi_ctx_migrate_reset(void) {
    cm_memzero(&cm_rx, sizeof(cm_rx));
    cm_memzero(&cm_landed_ctx, sizeof(cm_landed_ctx));
    cm_landed_valid = 0;
}

/* ─── Sender ──────────────────────────────────────────────────────────── */
uint32_t simi_ctx_migrate_send(const struct SimiContext* ctx,
                               uint64_t transfer_id, uint32_t dest_node,
                               uint32_t partition_id, const char* ctx_name) {
    if (!ctx) return 0;

    uint32_t len = 0;
    SimiCkptStatus cs = simi_ckpt_save(ctx, cm_send_buf, sizeof(cm_send_buf), &len);
    if (cs != SIMI_CKPT_OK || len == 0) {
        kernel_serial_printf("[CTXMIG] send: checkpoint failed (%s)\n",
                             simi_ckpt_status_name(cs));
        return 0;
    }

    uint32_t total_chunks = (len + DSPP_CTX_CHUNK_BYTES - 1) / DSPP_CTX_CHUNK_BYTES;
    uint64_t phash        = simi_ckpt_program_hash(ctx->obj);

    dspp_ctx_migrate_send_begin(transfer_id, dest_node, partition_id,
                                ctx_name, (uint64_t)len, total_chunks, phash);

    for (uint32_t i = 0; i < total_chunks; i++) {
        uint32_t off  = i * DSPP_CTX_CHUNK_BYTES;
        uint32_t take = len - off;
        if (take > DSPP_CTX_CHUNK_BYTES) take = DSPP_CTX_CHUNK_BYTES;
        dspp_ctx_migrate_send_chunk(transfer_id, dest_node, partition_id,
                                    i, cm_send_buf + off, take);
    }

    kernel_serial_printf("[CTXMIG] sent ctx '%s' pc=%u -> node %u (%u bytes, %u chunks)\n",
                         ctx_name ? ctx_name : "", (unsigned)ctx->pc,
                         (unsigned)dest_node, (unsigned)len, (unsigned)total_chunks);
    return total_chunks;
}

/* ─── Live-context registry ───────────────────────────────────────────
 * See simi_ctx_migrate.h for the honest status: real, iterated for real
 * by partition_migrate(), and empty on every current boot because no
 * subsystem creates long-lived contexts yet. */
static struct {
    int                 active;
    uint32_t            partition_id;
    char                name[DSPP_CTX_NAME_LEN];
    struct SimiContext* ctx;
} cm_reg[SIMI_CTX_REGISTRY_MAX];

int simi_ctx_register(uint32_t partition_id, const char* name, struct SimiContext* ctx) {
    if (!ctx) return 1;
    for (uint32_t i = 0; i < SIMI_CTX_REGISTRY_MAX; i++)
        if (cm_reg[i].active && cm_reg[i].ctx == ctx) return 0;   /* idempotent */
    for (uint32_t i = 0; i < SIMI_CTX_REGISTRY_MAX; i++) {
        if (cm_reg[i].active) continue;
        cm_reg[i].active       = 1;
        cm_reg[i].partition_id = partition_id;
        cm_reg[i].ctx          = ctx;
        cm_strncpy(cm_reg[i].name, name, sizeof(cm_reg[i].name));
        return 0;
    }
    kernel_serial_printf("[CTXMIG] registry full (%u) -- context not registered\n",
                         (unsigned)SIMI_CTX_REGISTRY_MAX);
    return 1;
}

void simi_ctx_unregister(const struct SimiContext* ctx) {
    for (uint32_t i = 0; i < SIMI_CTX_REGISTRY_MAX; i++)
        if (cm_reg[i].active && cm_reg[i].ctx == ctx) { cm_reg[i].active = 0; cm_reg[i].ctx = 0; }
}

uint32_t simi_ctx_registered_count(uint32_t partition_id) {
    uint32_t n = 0;
    for (uint32_t i = 0; i < SIMI_CTX_REGISTRY_MAX; i++)
        if (cm_reg[i].active && cm_reg[i].partition_id == partition_id) n++;
    return n;
}

uint32_t simi_ctx_migrate_send_partition(uint32_t partition_id, uint32_t dest_node) {
    uint32_t sent = 0;
    for (uint32_t i = 0; i < SIMI_CTX_REGISTRY_MAX; i++) {
        if (!cm_reg[i].active || cm_reg[i].partition_id != partition_id) continue;
        /* transfer_id must be unique per transfer. Partition, slot and the
         * destination together are unique for a given migration, and the
         * receiver only uses it to tie chunks to their BEGIN. */
        uint64_t tid = ((uint64_t)partition_id << 40) | ((uint64_t)dest_node << 8) | i;
        if (simi_ctx_migrate_send(cm_reg[i].ctx, tid, dest_node,
                                  partition_id, cm_reg[i].name) > 0)
            sent++;
    }
    return sent;
}

/* ─── Receiver ────────────────────────────────────────────────────────── */
SimiCtxMigStatus simi_ctx_migrate_recv_begin(uint64_t transfer_id,
                                             const char* ctx_name,
                                             uint64_t total_bytes,
                                             uint32_t total_chunks,
                                             uint64_t program_hash) {
    /* A second BEGIN while one is in flight is refused rather than
     * clobbering the first. Re-sending the SAME transfer_id is treated as
     * a restart, not a conflict -- that is the retry case, not a
     * collision. */
    if (cm_rx.active && cm_rx.transfer_id != transfer_id)
        return SIMI_CTXMIG_ERR_BUSY;

    if (total_bytes == 0 || total_bytes > CM_MAX_CKPT_BYTES)
        return SIMI_CTXMIG_ERR_TOO_LARGE;

    /* total_chunks must be exactly what total_bytes implies. A mismatch
     * means the sender and receiver disagree about the chunking, and
     * trusting either number alone lets a bad one index the bitmap. */
    uint32_t expect = (uint32_t)((total_bytes + DSPP_CTX_CHUNK_BYTES - 1) / DSPP_CTX_CHUNK_BYTES);
    if (total_chunks != expect) return SIMI_CTXMIG_ERR_BAD_CHUNK;

    /* Refuse EARLY if no local image matches. Reassembling ~353 KiB only
     * to discover at the last chunk that the program is missing wastes
     * the whole transfer; the sender learns from the BEGIN_ACK instead. */
    if (!cm_find_image(program_hash)) return SIMI_CTXMIG_ERR_NO_PROGRAM;

    cm_memzero(&cm_rx, sizeof(cm_rx));
    cm_rx.active       = 1;
    cm_rx.transfer_id  = transfer_id;
    cm_rx.total_bytes  = total_bytes;
    cm_rx.total_chunks = total_chunks;
    cm_rx.program_hash = program_hash;
    cm_strncpy(cm_rx.ctx_name, ctx_name, sizeof(cm_rx.ctx_name));
    cm_landed_valid    = 0;

    return SIMI_CTXMIG_OK;
}

SimiCtxMigStatus simi_ctx_migrate_recv_chunk(uint64_t transfer_id,
                                             uint32_t chunk_index,
                                             const uint8_t* data,
                                             uint32_t chunk_bytes) {
    if (!cm_rx.active || cm_rx.transfer_id != transfer_id)
        return SIMI_CTXMIG_ERR_NO_TRANSFER;
    if (!data || chunk_index >= cm_rx.total_chunks)
        return SIMI_CTXMIG_ERR_BAD_CHUNK;
    if (chunk_bytes == 0 || chunk_bytes > DSPP_CTX_CHUNK_BYTES)
        return SIMI_CTXMIG_ERR_BAD_CHUNK;

    uint32_t off = chunk_index * DSPP_CTX_CHUNK_BYTES;
    if ((uint64_t)off + chunk_bytes > cm_rx.total_bytes)
        return SIMI_CTXMIG_ERR_BAD_CHUNK;

    /* Every chunk but the last must be full. Otherwise a short interior
     * chunk would leave a gap of zeroes that the length arithmetic still
     * considers covered. */
    int is_last = (chunk_index + 1 == cm_rx.total_chunks);
    if (!is_last && chunk_bytes != DSPP_CTX_CHUNK_BYTES)
        return SIMI_CTXMIG_ERR_BAD_CHUNK;
    if (is_last && (uint64_t)off + chunk_bytes != cm_rx.total_bytes)
        return SIMI_CTXMIG_ERR_BAD_CHUNK;

    cm_memcpy(cm_rx.buf + off, data, chunk_bytes);
    if (!cm_rx.chunk_present[chunk_index]) {
        cm_rx.chunk_present[chunk_index] = 1;
        cm_rx.chunks_seen++;
    }

    if (cm_rx.chunks_seen < cm_rx.total_chunks) return SIMI_CTXMIG_OK;

    /* ── Complete: rebind the program and restore ──────────────────────
     * The image was checked at BEGIN, but look it up again rather than
     * caching the pointer: images can be unregistered between BEGIN and
     * the final chunk, and simi_ckpt_load() stores this pointer into the
     * restored context. */
    const SimiObject* obj = cm_find_image(cm_rx.program_hash);
    if (!obj) { cm_rx.active = 0; return SIMI_CTXMIG_ERR_NO_PROGRAM; }

    SimiCkptStatus cs = simi_ckpt_load(&cm_landed_ctx, cm_rx.buf,
                                       (uint32_t)cm_rx.total_bytes, obj);
    cm_rx.active = 0;
    if (cs != SIMI_CKPT_OK) {
        kernel_serial_printf("[CTXMIG] restore refused: %s\n", simi_ckpt_status_name(cs));
        return SIMI_CTXMIG_ERR_RESTORE;
    }

    cm_landed_valid = 1;
    kernel_serial_printf("[CTXMIG] ctx '%s' landed: resuming at pc=%u (%llu steps in)\n",
                         cm_rx.ctx_name, (unsigned)cm_landed_ctx.pc,
                         (unsigned long long)cm_landed_ctx.steps);
    return SIMI_CTXMIG_OK;
}
