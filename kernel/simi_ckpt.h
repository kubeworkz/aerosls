#ifndef SIMI_CKPT_H
#define SIMI_CKPT_H

#include <stdint.h>
#include "simi_interp.h"

/*
 * simi_ckpt.h — checkpoint and restore a running SIMI execution context
 * (Persistent Execution Contexts, Phase 2).
 *
 * ─── Why this is a pure serialiser, not a storage layer ───────────────
 * This module turns a `struct SimiContext` into a flat byte buffer and
 * back. It performs no I/O and knows nothing about NVMe, streams or the
 * object catalog. That separation is deliberate, for three reasons:
 *
 *   1. The dangerous part of checkpointing is OMISSION -- forgetting to
 *      save a field means a context that resumes subtly wrong, or loses
 *      work silently. Keeping serialisation pure makes it exhaustively
 *      testable without a disk (see tests/simi_ckpt_host_test.c, which
 *      checkpoints at EVERY instruction of a program and requires all
 *      restores to converge).
 *   2. Storage choice stays open. A checkpoint is a byte range; a stream
 *      object is the natural home (streams already have NVMe backing, a
 *      reboot-surviving directory, partition ownership, and -- via
 *      stream_migrate_send_partition() -- cross-node transport), but
 *      nothing here forces that.
 *   3. Phase 3 (live cross-node migration) moves exactly these bytes. If
 *      serialisation were entangled with local storage, migration would
 *      need a second, parallel implementation. It does not.
 *
 * This mirrors the approach taken with nvme_build_prp(): isolate the part
 * where a mistake corrupts something silently, and test that part hard.
 *
 * ─── What is deliberately NOT in the checkpoint ───────────────────────
 * The program image (instructions, literals, name pool). It is immutable
 * and potentially large, and re-serialising it on every checkpoint would
 * dominate the payload. The caller re-binds it on restore.
 *
 * That creates a real hazard -- `pc` is an INSTRUCTION INDEX, so restoring
 * a checkpoint against a different program would resume at a valid-looking
 * index in unrelated code. The format therefore stores a hash of the
 * instruction stream and refuses to restore against an image that does not
 * match. See SIMI_CKPT_ERR_PROGRAM_MISMATCH.
 */

/* Bump on ANY change to the on-disk layout below. A checkpoint carrying a
 * different version is refused rather than reinterpreted -- the same
 * one-way-format-change discipline kernel/persist.h uses for its
 * PERSIST_MAGIC_* regions. */
#define SIMI_CKPT_FORMAT_VERSION 1
#define SIMI_CKPT_MAGIC 0x53494D49434B5054ULL   /* "SIMICKPT" */

typedef enum {
    SIMI_CKPT_OK = 0,
    SIMI_CKPT_ERR_BUFFER_TOO_SMALL,   /* save: caller's buffer cannot hold the checkpoint */
    SIMI_CKPT_ERR_TRUNCATED,          /* load: buffer shorter than the header claims */
    SIMI_CKPT_ERR_BAD_MAGIC,          /* load: not a checkpoint at all */
    SIMI_CKPT_ERR_BAD_VERSION,        /* load: written by an incompatible build */
    SIMI_CKPT_ERR_ISA_MISMATCH,       /* load: opcode numbering changed -- pc would mean something else */
    SIMI_CKPT_ERR_LAYOUT_MISMATCH,    /* load: SimiFrame/mem sizes changed (e.g. NREGS) */
    SIMI_CKPT_ERR_PROGRAM_MISMATCH,   /* load: restoring against a different program image */
    SIMI_CKPT_ERR_CHECKSUM,           /* load: payload corrupt or torn */
    SIMI_CKPT_ERR_BAD_STATE,          /* save/load: nonsensical frame_top, etc. */
} SimiCkptStatus;

const char* simi_ckpt_status_name(SimiCkptStatus s);

/* On-disk header. Fixed layout, little-endian by construction (this kernel
 * is x86-64 / RV64 only, both LE); every field is explicit rather than
 * relying on struct packing across builds, and the sizes of the variable
 * parts are recorded so a layout change is detected instead of silently
 * misparsed. */
struct SimiCkptHeader {
    uint64_t magic;
    uint32_t format_version;
    uint32_t frame_bytes;       /* sizeof(struct SimiFrame) at save time */
    uint64_t isa_fingerprint;   /* simi_interp_isa_fingerprint() at save time */
    uint64_t program_hash;      /* over the instruction stream -- see header comment */

    uint32_t pc;                /* instruction index */
    int32_t  frame_top;
    uint32_t num_frames;        /* frame_top + 1: LIVE frames only */
    uint32_t mem_bytes;
    uint64_t steps;
    uint64_t result;
    uint32_t status;            /* SimiStatus at save time */
    uint32_t trap_pc;

    uint64_t payload_checksum;  /* FNV-1a over frames + mem */
} __attribute__((packed));

/* Exact bytes simi_ckpt_save() will write for this context. Depends on
 * LIVE call depth, not on SIMI_MAX_FRAMES -- a depth-1 context costs ~73
 * KiB even though sizeof(struct SimiContext) is far larger. */
uint32_t simi_ckpt_size(const struct SimiContext* ctx);

/* Serialises `ctx` into `buf`. Writes *out_len on success. */
SimiCkptStatus simi_ckpt_save(const struct SimiContext* ctx,
                              void* buf, uint32_t cap, uint32_t* out_len);

/* Restores into `ctx` from `buf`, re-binding the program image `obj`.
 *
 * Validates, in order: magic, format version, buffer length, ISA
 * fingerprint, struct layout, program-image hash, payload checksum. `ctx`
 * is only written once every check has passed, so a rejected checkpoint
 * leaves the caller's context untouched rather than half-overwritten --
 * the same verify-before-load discipline kernel/persist.c adopted after
 * torn-write detection landed. */
SimiCkptStatus simi_ckpt_load(struct SimiContext* ctx,
                              const void* buf, uint32_t len,
                              const SimiObject* obj);

/* Hash of a program image's instruction stream, as stored in the header.
 * Exposed so a caller can pre-check compatibility (e.g. pick the right
 * image out of a catalog) before attempting a restore. */
uint64_t simi_ckpt_program_hash(const SimiObject* obj);

#endif /* SIMI_CKPT_H */
