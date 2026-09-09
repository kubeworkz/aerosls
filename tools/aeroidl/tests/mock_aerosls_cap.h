/**
 * mock_aerosls_cap.h — Stub declarations for the AeroSLS sidecar runtime.
 *
 * This header provides enough declarations to compile AeroIDL-generated
 * C headers without linking the actual sidecar runtime. It is used
 * exclusively for interop compilation tests.
 */
#ifndef AEROSLS_CAP_H
#define AEROSLS_CAP_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Capability permissions ─────────────────────────────────────────────── */

#define AEROSLS_CAP_PERM_R        0x01
#define AEROSLS_CAP_PERM_W        0x02
#define AEROSLS_CAP_PERM_X        0x04

/* ── Capability flags ──────────────────────────────────────────────────── */

#define AEROSLS_CAP_FLAG_BORROWED    0x00
#define AEROSLS_CAP_FLAG_ARENA       0x01
#define AEROSLS_CAP_FLAG_ARENA_OWNED 0x02

/* ── Sentinel ──────────────────────────────────────────────────────────── */

#define AEROSLS_CAP_NONE          0xFFFF

/* ── Channel flags ─────────────────────────────────────────────────────── */

#define AEROSLS_CHAN_FLAG_NO_REPLY 0x0001

/* ── Capability descriptor ─────────────────────────────────────────────── */

typedef struct {
    uint16_t slot;      /* capability slot index            */
    uint32_t offset;    /* byte offset into shared arena    */
    uint32_t len;       /* byte length of the region        */
    uint8_t  rights;    /* AEROSLS_CAP_PERM_* bitmask       */
    uint8_t  flags;     /* AEROSLS_CAP_FLAG_*               */
} aerosls_cap_desc_t;

/* ── Runtime functions (stubs) ─────────────────────────────────────────── */

/** Send a message on a channel endpoint. */
static inline int
aerosls_chan_send(uint16_t chan_w,
                  uint32_t opcode,
                  uint32_t req_id,
                  const void *payload,
                  size_t payload_len,
                  const aerosls_cap_desc_t *caps,
                  uint16_t n_caps,
                  uint32_t flags)
{
    (void)chan_w; (void)opcode; (void)req_id;
    (void)payload; (void)payload_len;
    (void)caps; (void)n_caps; (void)flags;
    return 0;
}

/** Receive a message from a channel endpoint. */
static inline int
aerosls_chan_recv(uint16_t chan_r,
                  void *buf,
                  size_t buf_len,
                  uint16_t *cap_slots,
                  uint16_t max_caps,
                  uint32_t *out_kind,
                  uint32_t *out_tag,
                  size_t   *out_len,
                  uint16_t *out_n_caps)
{
    (void)chan_r; (void)buf_len;
    (void)cap_slots; (void)max_caps;
    (void)out_kind; (void)out_tag; (void)out_len; (void)out_n_caps;
    /* A failed/no-op receive must not leave the reply buffer holding
     * garbage: zero it so generated stubs that read reply_buf[0] as the
     * ok byte get a deterministic "error" verdict. */
    if (buf) {
        uint8_t *b = (uint8_t *)buf;
        for (size_t i = 0; i < buf_len; i++) {
            b[i] = 0;
        }
    }
    return 0;
}

/** Allocate a buffer from the shared arena. */
static inline uint16_t
aerosls_arena_alloc(size_t bytes, uint8_t perms)
{
    (void)bytes; (void)perms;
    return AEROSLS_CAP_NONE;
}

/** Free a previously allocated arena buffer. */
static inline void
aerosls_arena_free(uint16_t cap)
{
    (void)cap;
}

/** Generate a unique request ID. */
static uint32_t g_req_id_counter = 0;

static inline uint32_t
aerosls_next_request_id(void)
{
    return ++g_req_id_counter;
}

#ifdef __cplusplus
}
#endif

#endif /* AEROSLS_CAP_H */
