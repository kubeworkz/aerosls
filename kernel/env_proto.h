#ifndef ENV_PROTO_H
#define ENV_PROTO_H

#include <stdint.h>
#include <stddef.h>

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
#define ENV_CREATE  1
#define ENV_DESTROY 2

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

#define ENV_FRAME_SIZE       16u
#define ENV_CREATE_BODY_SIZE  8u
/* { env_id u32, partition u32 }. The partition is carried so the control
 * plane's nested destroy route (`POST /api/partition/{id}/env/destroy`)
 * ENFORCES its own {id}: the env id alone identifies the environment globally,
 * so without it a caller could end partition B's environment through a path
 * that names partition A. Mirrors ENV_CREATE's { partition, index }. */
#define ENV_DESTROY_BODY_SIZE 8u
#define ENV_REPLY_BODY_SIZE  12u
#define ENV_REQ_MAX (ENV_FRAME_SIZE + ENV_CREATE_BODY_SIZE)   /* largest request */
#define ENV_REPLY_MAX (ENV_FRAME_SIZE + ENV_REPLY_BODY_SIZE)

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

/* Reply body: { status u16, pad u16, env_id u32, partition u32 } (12 bytes). */
static inline void env_reply_body_parse(const uint8_t* b, uint16_t* status,
                                        uint32_t* env_id, uint32_t* partition) {
    if (status)    *status    = env_read_u16(b, 0);
    if (env_id)    *env_id    = env_read_u32(b, 4);
    if (partition) *partition = env_read_u32(b, 8);
}

#endif /* ENV_PROTO_H */
