#ifndef ENV_PROTO_H
#define ENV_PROTO_H

#include <stdint.h>
#include <stddef.h>

/* POSIX-Environments v0.2 P1a: ENV_REGISTER's reply body is the environment
 * manager's half of a checkpoint record, so the name lengths and the region,
 * channel and task counts it must agree on live in kernel/env_ckpt.h -- one
 * definition, so the wire and the record cannot count differently. */
#include "env_ckpt.h"

/* env_proto.h — the ENV_* protocol (Layer 3), POSIX-Environments E4.
 *
 * The wire format the control plane (this kernel's HTTP server, C) speaks to
 * the environment manager (init, Rust) over init's kernel-held request
 * channel. It is the C mirror of user/proto/src/env_proto.rs — the two MUST
 * stay in lockstep (same magic, opcodes, status codes, and little-endian body
 * layouts); this header and that module are the single source of truth for
 * each side.
 *
 * A request is a 16-byte EnvFrame (mirroring the RD_* frame) followed by a
 * per-opcode body; the reply echoes the frame type and carries a uniform
 * { status u16, pad u16, env_id u32, partition u32 } body. All multi-byte
 * fields are little-endian (matching the Rust put_/read_ helpers). */

#define ENV_MAGIC0 'A'
#define ENV_MAGIC1 'E'
#define ENV_MAGIC2 'R'
#define ENV_MAGIC3 'O'
#define ENV_MAGIC4 'S'
#define ENV_MAGIC5 'E'
#define ENV_MAGIC6 'N'
#define ENV_MAGIC7 0x01           /* "AEROSEN\x01" */
#define ENV_VERSION 1

/* Frame types. */
#define ENV_CREATE   1
#define ENV_DESTROY  2
#define ENV_REGISTER 3   /* P1a: describe an environment for the checkpoint record */

/* Frame flag: set on error replies (mirrors RD_FLAG_ERROR). */
#define ENV_FLAG_ERROR 0x0001

/* Reply status codes. */
#define ENV_OK          0
#define ENV_ERR_INVAL   1   /* malformed request / bad body */
#define ENV_ERR_NOMEM   2   /* the frame pool cannot back the environment */
#define ENV_ERR_PART    3   /* absent/paused partition, or a refused create */
#define ENV_ERR_FULL    4   /* the environment table is full */
#define ENV_ERR_UNSUPP  5   /* recognised opcode not yet built */
#define ENV_ERR_NOENT   6   /* no environment with that env_id (E5) */
#define ENV_ERR_QUOTA   7   /* the environment's durable store does not fit the
                             * partition's storage quota (P1b) — the quota's own
                             * error, deliberately not NOMEM's frame-pool one */

#define ENV_FRAME_SIZE       16u
#define ENV_CREATE_BODY_SIZE  8u
/* { env_id u32, partition u32 }. The partition is carried so the control
 * plane's nested destroy route (`POST /api/partition/{id}/env/destroy`)
 * ENFORCES its own {id}: the env id alone identifies the environment globally,
 * so without it a caller could end partition B's environment through a path
 * that names partition A. Mirrors ENV_CREATE's { partition, index }. */
#define ENV_DESTROY_BODY_SIZE 8u
/* P1a: { env_id u32, partition u32 } -- the same pair ENV_DESTROY carries, for
 * the same reason: the registration is ASKED FOR by (env_id, partition) and
 * init answers with the environment those name, so a reply cannot be about a
 * different environment than the request was. */
#define ENV_REGISTER_BODY_SIZE 8u
#define ENV_REPLY_BODY_SIZE  12u

/* ── ENV_REGISTER reply body: init's half of the checkpoint record ──────────
 * Byte-for-byte the `struct EnvCkptRegister` (kernel/env_ckpt.h) that
 * env_ckpt_register_from() consumes, and the exact field order
 * user/proto/src/env_proto.rs encodes -- tests/env_register_pin_check.sh
 * compares the two files rather than trusting them. Offsets are named, not
 * spelled inline, because they ARE the contract. */
#define ENV_REG_OFF_PARTITION   0u
#define ENV_REG_OFF_INDEX       4u
#define ENV_REG_OFF_ENV_ID      8u
#define ENV_REG_OFF_N_REGIONS  12u
#define ENV_REG_OFF_REGIONS    16u
#define ENV_REG_REGION_STRIDE  16u   /* { base u64, frames u32, kind u32 } */
#define ENV_REG_OFF_N_CHANS \
    (ENV_REG_OFF_REGIONS + ENV_CKPT_MAX_REGIONS * ENV_REG_REGION_STRIDE)
#define ENV_REG_OFF_CHANS      (ENV_REG_OFF_N_CHANS + 4u)
#define ENV_REG_OFF_N_TASKS    (ENV_REG_OFF_CHANS + ENV_CKPT_MAX_CHANS * 4u)
#define ENV_REG_OFF_TASK_NAMES (ENV_REG_OFF_N_TASKS + 4u)
#define ENV_REG_TASK_NAME_STRIDE ENV_CKPT_NAME_LEN
#define ENV_REG_OFF_TASK_KINDS \
    (ENV_REG_OFF_TASK_NAMES + ENV_CKPT_MAX_TASKS * ENV_REG_TASK_NAME_STRIDE)
#define ENV_REGISTER_REPLY_BODY_SIZE \
    (ENV_REG_OFF_TASK_KINDS + ENV_CKPT_MAX_TASKS * 4u)

/* The body is a padded-free image of the struct, so the two MUST describe the
 * same number of bytes. If a field is added to one and not the other this is a
 * build error, not a boot-time surprise. */
_Static_assert(sizeof(struct EnvCkptRegister) == ENV_REGISTER_REPLY_BODY_SIZE,
               "EnvCkptRegister no longer matches the ENV_REGISTER wire layout");

#define ENV_REQ_MAX (ENV_FRAME_SIZE + ENV_CREATE_BODY_SIZE)   /* largest request */
/* Largest reply: the 12-byte uniform body, or init's full registration. */
#define ENV_REPLY_MAX (ENV_FRAME_SIZE + ENV_REGISTER_REPLY_BODY_SIZE)

/* ── little-endian byte helpers (match user/proto put_/read_) ─────────────── */
static inline void env_put_u16(uint8_t* b, size_t i, uint16_t v) {
    b[i] = (uint8_t)v; b[i + 1] = (uint8_t)(v >> 8);
}
static inline void env_put_u32(uint8_t* b, size_t i, uint32_t v) {
    b[i] = (uint8_t)v; b[i + 1] = (uint8_t)(v >> 8);
    b[i + 2] = (uint8_t)(v >> 16); b[i + 3] = (uint8_t)(v >> 24);
}
static inline uint16_t env_read_u16(const uint8_t* b, size_t i) {
    return (uint16_t)b[i] | ((uint16_t)b[i + 1] << 8);
}
static inline uint32_t env_read_u32(const uint8_t* b, size_t i) {
    return (uint32_t)b[i] | ((uint32_t)b[i + 1] << 8) |
           ((uint32_t)b[i + 2] << 16) | ((uint32_t)b[i + 3] << 24);
}

/* ── EnvFrame (16 bytes): magic[8] version u16, ty u16, flags u16, pad u16 ── */
static inline void env_frame_encode(uint8_t* b, uint16_t ty, int error) {
    b[0] = ENV_MAGIC0; b[1] = ENV_MAGIC1; b[2] = ENV_MAGIC2; b[3] = ENV_MAGIC3;
    b[4] = ENV_MAGIC4; b[5] = ENV_MAGIC5; b[6] = ENV_MAGIC6; b[7] = ENV_MAGIC7;
    env_put_u16(b, 8, ENV_VERSION);
    env_put_u16(b, 10, ty);
    env_put_u16(b, 12, error ? ENV_FLAG_ERROR : 0);
    env_put_u16(b, 14, 0);
}

/* Validate magic + version and read the frame type; returns 1 and sets *ty on
 * a good frame, 0 on a short or foreign one. */
static inline int env_frame_parse(const uint8_t* b, size_t len, uint16_t* ty) {
    if (len < ENV_FRAME_SIZE) return 0;
    if (b[0] != ENV_MAGIC0 || b[1] != ENV_MAGIC1 || b[2] != ENV_MAGIC2 ||
        b[3] != ENV_MAGIC3 || b[4] != ENV_MAGIC4 || b[5] != ENV_MAGIC5 ||
        b[6] != ENV_MAGIC6 || b[7] != ENV_MAGIC7) return 0;
    if (env_read_u16(b, 8) != ENV_VERSION) return 0;
    if (ty) *ty = env_read_u16(b, 10);
    return 1;
}

/* ── bodies ──────────────────────────────────────────────────────────────── */
/* ENV_CREATE request body: { partition u32, index u32 } (8 bytes). */
static inline void env_create_body_encode(uint8_t* b, uint32_t partition, uint32_t index) {
    env_put_u32(b, 0, partition);
    env_put_u32(b, 4, index);
}

/* ENV_DESTROY request body: { env_id u32, partition u32 } (8 bytes). */
static inline void env_destroy_body_encode(uint8_t* b, uint32_t env_id, uint32_t partition) {
    env_put_u32(b, 0, env_id);
    env_put_u32(b, 4, partition);
}

/* ENV_REGISTER request body: { env_id u32, partition u32 } (8 bytes). */
static inline void env_register_body_encode(uint8_t* b, uint32_t env_id, uint32_t partition) {
    env_put_u32(b, 0, env_id);
    env_put_u32(b, 4, partition);
}

/* Validate and read an ENV_REGISTER request body; 1 on success, 0 if short. */
static inline int env_register_body_parse(const uint8_t* b, size_t len,
                                          uint32_t* out_env_id, uint32_t* out_partition) {
    if (len < ENV_REGISTER_BODY_SIZE) return 0;
    if (out_env_id)    *out_env_id    = env_read_u32(b, 0);
    if (out_partition) *out_partition = env_read_u32(b, 4);
    return 1;
}

/* ── ENV_REGISTER reply body ────────────────────────────────────────────────
 * Little-endian, field for field, exactly `struct EnvCkptRegister`. Written by
 * init (user/proto/src/env_proto.rs); read here. The C encoder exists for the
 * same reason env_frame_encode does -- so tests/env_register_host_test.c and
 * tests/env_proto_host_test.c can round-trip the layout without a Rust
 * toolchain, and so a kernel-side producer is possible the day one is needed. */
static inline void env_register_reply_encode(uint8_t* b, const struct EnvCkptRegister* r) {
    size_t n = (size_t)ENV_REGISTER_REPLY_BODY_SIZE;
    for (size_t i = 0; i < n; i++) b[i] = 0;
    env_put_u32(b, ENV_REG_OFF_PARTITION, r->partition_id);
    env_put_u32(b, ENV_REG_OFF_INDEX,     r->index);
    env_put_u32(b, ENV_REG_OFF_ENV_ID,    r->env_id);
    env_put_u32(b, ENV_REG_OFF_N_REGIONS, r->n_regions);
    for (uint32_t i = 0; i < ENV_CKPT_MAX_REGIONS; i++) {
        size_t o = (size_t)ENV_REG_OFF_REGIONS + (size_t)i * ENV_REG_REGION_STRIDE;
        env_put_u32(b, o + 0, (uint32_t)(r->regions[i].base & 0xFFFFFFFFu));
        env_put_u32(b, o + 4, (uint32_t)(r->regions[i].base >> 32));
        env_put_u32(b, o + 8, r->regions[i].frames);
        env_put_u32(b, o + 12, r->regions[i].kind);
    }
    env_put_u32(b, ENV_REG_OFF_N_CHANS, r->n_chans);
    for (uint32_t i = 0; i < ENV_CKPT_MAX_CHANS; i++)
        env_put_u32(b, (size_t)ENV_REG_OFF_CHANS + (size_t)i * 4u, r->chans[i]);
    env_put_u32(b, ENV_REG_OFF_N_TASKS, r->n_tasks);
    for (uint32_t i = 0; i < ENV_CKPT_MAX_TASKS; i++) {
        size_t o = (size_t)ENV_REG_OFF_TASK_NAMES + (size_t)i * ENV_REG_TASK_NAME_STRIDE;
        for (uint32_t c = 0; c < ENV_CKPT_NAME_LEN; c++)
            b[o + c] = (uint8_t)r->task_name[i][c];
    }
    for (uint32_t i = 0; i < ENV_CKPT_MAX_TASKS; i++)
        env_put_u32(b, (size_t)ENV_REG_OFF_TASK_KINDS + (size_t)i * 4u, r->task_kind[i]);
}

/* Parse an ENV_REGISTER reply body into `out`; 1 on success, 0 if `len` is
 * short of the full body (a partial registration must not be half-applied).
 * The count fields are read, not trusted: env_ckpt_register_from() refuses a
 * count past what a record holds, so the loops here are bounded by the arrays
 * and the counts carry the honest value through. */
static inline int env_register_reply_parse(const uint8_t* b, size_t len,
                                           struct EnvCkptRegister* out) {
    if (len < ENV_REGISTER_REPLY_BODY_SIZE) return 0;
    out->partition_id = env_read_u32(b, ENV_REG_OFF_PARTITION);
    out->index        = env_read_u32(b, ENV_REG_OFF_INDEX);
    out->env_id       = env_read_u32(b, ENV_REG_OFF_ENV_ID);
    out->n_regions    = env_read_u32(b, ENV_REG_OFF_N_REGIONS);
    for (uint32_t i = 0; i < ENV_CKPT_MAX_REGIONS; i++) {
        size_t o = (size_t)ENV_REG_OFF_REGIONS + (size_t)i * ENV_REG_REGION_STRIDE;
        out->regions[i].base   = (uint64_t)env_read_u32(b, o + 0)
                               | ((uint64_t)env_read_u32(b, o + 4) << 32);
        out->regions[i].frames = env_read_u32(b, o + 8);
        out->regions[i].kind   = env_read_u32(b, o + 12);
    }
    out->n_chans = env_read_u32(b, ENV_REG_OFF_N_CHANS);
    for (uint32_t i = 0; i < ENV_CKPT_MAX_CHANS; i++)
        out->chans[i] = env_read_u32(b, (size_t)ENV_REG_OFF_CHANS + (size_t)i * 4u);
    out->n_tasks = env_read_u32(b, ENV_REG_OFF_N_TASKS);
    for (uint32_t i = 0; i < ENV_CKPT_MAX_TASKS; i++) {
        size_t o = (size_t)ENV_REG_OFF_TASK_NAMES + (size_t)i * ENV_REG_TASK_NAME_STRIDE;
        for (uint32_t c = 0; c < ENV_CKPT_NAME_LEN; c++)
            out->task_name[i][c] = (char)b[o + c];
        out->task_kind[i] = env_read_u32(b, (size_t)ENV_REG_OFF_TASK_KINDS + (size_t)i * 4u);
    }
    return 1;
}

/* The human-readable name of an ENV_* reply status.
 *
 * ONE definition, two askers: the control plane's create/destroy routes relay a
 * refusal to an HTTP client (net/http.c), and the shell's `env create`/
 * `env destroy` print the same refusal on the serial console
 * (user/shell.c). They were about to be two switches over the same seven
 * codes — the drift this file's own header warns about — so the mapping lives
 * beside the codes it names. */
static inline const char* env_status_name(uint16_t s) {
    switch (s) {
        case ENV_OK:         return "ok";
        case ENV_ERR_INVAL:  return "invalid request";
        case ENV_ERR_NOMEM:  return "frame pool exhausted";
        case ENV_ERR_PART:   return "partition absent, paused, or placement refused";
        case ENV_ERR_FULL:   return "environment table full";
        case ENV_ERR_UNSUPP: return "unsupported";
        /* E5: an ENV_DESTROY for an env_id the manager does not hold. Its own
         * code rather than a generic failure, so a caller can tell "destroyed"
         * from "there was nothing there". */
        case ENV_ERR_NOENT:  return "no such environment";
        /* P1b: the create's durable store does not fit the tenant's storage
         * quota. Its own code and its own words — "exhausted" here is the
         * quota speaking, not the frame pool. */
        case ENV_ERR_QUOTA:  return "storage quota exhausted";
        default:             return "unknown";
    }
}

/* Reply body: { status u16, pad u16, env_id u32, partition u32 } (12 bytes). */
static inline void env_reply_body_parse(const uint8_t* b, uint16_t* status,
                                        uint32_t* env_id, uint32_t* partition) {
    if (status)    *status    = env_read_u16(b, 0);
    if (env_id)    *env_id    = env_read_u32(b, 4);
    if (partition) *partition = env_read_u32(b, 8);
}

#endif /* ENV_PROTO_H */
