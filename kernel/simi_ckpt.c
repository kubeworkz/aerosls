/*
 * simi_ckpt.c — checkpoint/restore for SIMI execution contexts
 * (Persistent Execution Contexts, Phase 2). See simi_ckpt.h for why this
 * is a pure serialiser with no I/O.
 *
 * Freestanding: local helpers, no libc, following this codebase's
 * per-file convention (p_memcpy in persist.c, si_memcpy in simi_interp.c,
 * tn_streq in tenant.c).
 */
#include "simi_ckpt.h"
#include "kernel_io.h"

/* ─── Local helpers ─────────────────────────────────────────────────── */
static void ck_memcpy(void* d, const void* s, uint32_t n) {
    uint8_t* dd = (uint8_t*)d;
    const uint8_t* ss = (const uint8_t*)s;
    while (n--) *dd++ = *ss++;
}
static void ck_memzero(void* d, uint32_t n) {
    uint8_t* p = (uint8_t*)d;
    while (n--) *p++ = 0;
}

/* FNV-1a, 64-bit -- same choice and same reasoning as kernel/persist.c's
 * region checksums: detecting accidental corruption from an interrupted
 * or torn write, not adversarial tampering, so a non-cryptographic hash
 * is the right tool and a shared table-free implementation keeps this
 * file dependency-free. */
#define CK_FNV_OFFSET 1469598103934665603ULL
#define CK_FNV_PRIME  1099511628211ULL

static uint64_t ck_fold(uint64_t h, const void* data, uint32_t n) {
    const uint8_t* p = (const uint8_t*)data;
    while (n--) { h ^= (uint64_t)(*p++); h *= CK_FNV_PRIME; }
    return h;
}

const char* simi_ckpt_status_name(SimiCkptStatus s) {
    switch (s) {
        case SIMI_CKPT_OK:                   return "OK";
        case SIMI_CKPT_ERR_BUFFER_TOO_SMALL: return "ERR:buffer-too-small";
        case SIMI_CKPT_ERR_TRUNCATED:        return "ERR:truncated";
        case SIMI_CKPT_ERR_BAD_MAGIC:        return "ERR:not-a-checkpoint";
        case SIMI_CKPT_ERR_BAD_VERSION:      return "ERR:incompatible-format-version";
        case SIMI_CKPT_ERR_ISA_MISMATCH:     return "ERR:isa-numbering-changed";
        case SIMI_CKPT_ERR_LAYOUT_MISMATCH:  return "ERR:context-layout-changed";
        case SIMI_CKPT_ERR_PROGRAM_MISMATCH: return "ERR:different-program-image";
        case SIMI_CKPT_ERR_CHECKSUM:         return "ERR:payload-checksum";
        case SIMI_CKPT_ERR_BAD_STATE:        return "ERR:bad-context-state";
        default:                             return "ERR:unknown";
    }
}

uint64_t simi_ckpt_program_hash(const SimiObject* obj) {
    if (!obj || !obj->instr) return 0;
    /* Covers the instruction stream and its length. The literal pool and
     * name pool are deliberately included too: RESOLVE takes a name-pool
     * INDEX and LOADI64 a literal INDEX, so two images with identical
     * instructions but different pools are genuinely different programs
     * as far as a resumed pc is concerned. */
    uint64_t h = ck_fold(CK_FNV_OFFSET, &obj->num_instr, sizeof(obj->num_instr));
    h = ck_fold(h, obj->instr, obj->num_instr * (uint32_t)sizeof(uint64_t));
    h = ck_fold(h, &obj->num_literals, sizeof(obj->num_literals));
    if (obj->literals && obj->num_literals)
        h = ck_fold(h, obj->literals, obj->num_literals * (uint32_t)sizeof(uint64_t));
    h = ck_fold(h, &obj->num_names, sizeof(obj->num_names));
    if (obj->names && obj->num_names)
        h = ck_fold(h, obj->names, obj->num_names * (uint32_t)sizeof(SimiName));
    if (h == 0) h = 1;   /* 0 reserved for "no image" */
    return h;
}

/* Live frames only. A context that has not started (frame_top < 0) has
 * none, which is a legal thing to checkpoint. */
static uint32_t ck_num_frames(const struct SimiContext* ctx) {
    return (ctx->frame_top < 0) ? 0u : (uint32_t)(ctx->frame_top + 1);
}

uint32_t simi_ckpt_size(const struct SimiContext* ctx) {
    return (uint32_t)sizeof(struct SimiCkptHeader)
         + ck_num_frames(ctx) * (uint32_t)sizeof(struct SimiFrame)
         + (uint32_t)sizeof(ctx->mem);
}

SimiCkptStatus simi_ckpt_save(const struct SimiContext* ctx,
                              void* buf, uint32_t cap, uint32_t* out_len) {
    if (!ctx || !buf) return SIMI_CKPT_ERR_BAD_STATE;
    if (ctx->frame_top >= SIMI_MAX_FRAMES) return SIMI_CKPT_ERR_BAD_STATE;

    uint32_t nframes = ck_num_frames(ctx);
    uint32_t need    = simi_ckpt_size(ctx);
    if (cap < need) return SIMI_CKPT_ERR_BUFFER_TOO_SMALL;

    uint8_t* p = (uint8_t*)buf;
    struct SimiCkptHeader h;
    ck_memzero(&h, sizeof(h));

    h.magic           = SIMI_CKPT_MAGIC;
    h.format_version  = SIMI_CKPT_FORMAT_VERSION;
    h.frame_bytes     = (uint32_t)sizeof(struct SimiFrame);
    h.isa_fingerprint = simi_interp_isa_fingerprint();
    h.program_hash    = simi_ckpt_program_hash(ctx->obj);
    h.pc              = ctx->pc;
    h.frame_top       = ctx->frame_top;
    h.num_frames      = nframes;
    h.mem_bytes       = (uint32_t)sizeof(ctx->mem);
    h.steps           = ctx->steps;
    h.result          = ctx->result;
    h.status          = (uint32_t)ctx->status;
    h.trap_pc         = ctx->trap_pc;

    /* Checksum covers the payload (frames + memory) only. The header is
     * excluded because it CONTAINS the checksum; the header's own fields
     * are individually validated on load instead. */
    uint64_t sum = CK_FNV_OFFSET;
    if (nframes) sum = ck_fold(sum, ctx->frames, nframes * (uint32_t)sizeof(struct SimiFrame));
    sum = ck_fold(sum, ctx->mem, (uint32_t)sizeof(ctx->mem));
    h.payload_checksum = sum;

    ck_memcpy(p, &h, sizeof(h));
    p += sizeof(h);
    if (nframes) {
        ck_memcpy(p, ctx->frames, nframes * (uint32_t)sizeof(struct SimiFrame));
        p += nframes * (uint32_t)sizeof(struct SimiFrame);
    }
    ck_memcpy(p, ctx->mem, (uint32_t)sizeof(ctx->mem));

    if (out_len) *out_len = need;
    return SIMI_CKPT_OK;
}

SimiCkptStatus simi_ckpt_load(struct SimiContext* ctx,
                              const void* buf, uint32_t len,
                              const SimiObject* obj) {
    if (!ctx || !buf) return SIMI_CKPT_ERR_BAD_STATE;
    if (len < sizeof(struct SimiCkptHeader)) return SIMI_CKPT_ERR_TRUNCATED;

    struct SimiCkptHeader h;
    ck_memcpy(&h, buf, sizeof(h));

    /* ── Validate everything BEFORE touching ctx ──────────────────────
     * A rejected checkpoint must leave the caller's context exactly as it
     * was, not half-overwritten. Same verify-before-load discipline
     * kernel/persist.c adopted for its regions. */
    if (h.magic != SIMI_CKPT_MAGIC)                       return SIMI_CKPT_ERR_BAD_MAGIC;
    if (h.format_version != SIMI_CKPT_FORMAT_VERSION)     return SIMI_CKPT_ERR_BAD_VERSION;

    /* The ISA fingerprint check is the subtle one and the reason it exists:
     * `pc` is an instruction INDEX and opcodes are numbers, so a build that
     * renumbered either would resume this context executing something
     * entirely different while every other field still looked plausible. */
    if (h.isa_fingerprint != simi_interp_isa_fingerprint()) return SIMI_CKPT_ERR_ISA_MISMATCH;

    if (h.frame_bytes != (uint32_t)sizeof(struct SimiFrame)) return SIMI_CKPT_ERR_LAYOUT_MISMATCH;
    if (h.mem_bytes   != (uint32_t)sizeof(ctx->mem))         return SIMI_CKPT_ERR_LAYOUT_MISMATCH;
    if (h.num_frames > SIMI_MAX_FRAMES)                      return SIMI_CKPT_ERR_LAYOUT_MISMATCH;

    /* frame_top and num_frames must agree, or the payload length and the
     * resumed call depth would describe different things. */
    int32_t expect_top = (h.num_frames == 0) ? -1 : (int32_t)h.num_frames - 1;
    if (h.frame_top != expect_top)                           return SIMI_CKPT_ERR_BAD_STATE;

    uint32_t need = (uint32_t)sizeof(h)
                  + h.num_frames * (uint32_t)sizeof(struct SimiFrame)
                  + h.mem_bytes;
    if (len < need) return SIMI_CKPT_ERR_TRUNCATED;

    /* Restoring against a different program would resume at a valid-looking
     * instruction index in unrelated code -- see simi_ckpt.h. */
    if (h.program_hash != simi_ckpt_program_hash(obj))       return SIMI_CKPT_ERR_PROGRAM_MISMATCH;

    const uint8_t* p = (const uint8_t*)buf + sizeof(h);
    uint64_t sum = CK_FNV_OFFSET;
    if (h.num_frames) sum = ck_fold(sum, p, h.num_frames * (uint32_t)sizeof(struct SimiFrame));
    sum = ck_fold(sum, p + h.num_frames * (uint32_t)sizeof(struct SimiFrame), h.mem_bytes);
    if (sum != h.payload_checksum)                           return SIMI_CKPT_ERR_CHECKSUM;

    /* ── Every check passed: now commit ─────────────────────────────── */
    ck_memzero(ctx, sizeof(*ctx));
    ctx->pc        = h.pc;
    ctx->frame_top = h.frame_top;
    ctx->steps     = h.steps;
    ctx->result    = h.result;
    ctx->status    = (SimiStatus)h.status;
    ctx->trap_pc   = h.trap_pc;
    ctx->obj       = obj;

    if (h.num_frames)
        ck_memcpy(ctx->frames, p, h.num_frames * (uint32_t)sizeof(struct SimiFrame));
    ck_memcpy(ctx->mem, p + h.num_frames * (uint32_t)sizeof(struct SimiFrame), h.mem_bytes);

    return SIMI_CKPT_OK;
}
