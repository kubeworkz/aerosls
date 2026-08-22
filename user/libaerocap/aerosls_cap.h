/* user/libaerocap/aerosls_cap.h — AeroSLS sidecar channel runtime (Phase 3)
 *
 * The REAL transport behind the AeroIDL-generated C stubs. Generated code
 * (tools/aeroidl --target c) includes <aerosls_cap.h> and calls the five
 * aerosls_* functions below; until now they only existed as the no-op mock
 * (tools/aeroidl/tests/mock_aerosls_cap.h) that proved the stubs COMPILE.
 * This header is the real thing: each function issues the Polyglot Nexus
 * message syscalls (SYS_SLS_CAP_SEND_MSG 302 / SYS_SLS_CAP_RECV_MSG 303 /
 * SYS_SLS_CAP_ARENA_FREE 304) on top of libsls's _sls_syscall(), with the
 * request structs byte-for-byte identical to kernel/cap.h (pinned by
 * tests/aerocap_abi_host_test.c, same hand-sync convention as aerocap.h).
 *
 * Wire framing (matches what the generated dispatcher parses):
 *   - Requests (opcode != 0): the library prepends the IDL header
 *         [opcode: u16 LE][payload_len: u32 LE]
 *     to the args payload the stub built, and sends THAT as the kernel
 *     message payload. The Rust dispatcher's chan_recv buffer then reads
 *     exactly [opcode][payload_len][args] — see gen_dispatcher.rs's
 *     "Parse header" block.
 *   - Replies (opcode == 0, the generated convention "0 = response"): the
 *     payload is sent raw, so the client's reply_buf starts at the `ok`
 *     byte (reply layout [ok u8][pad 3][result...], per §2.3).
 *
 * The kernel envelope itself is payload-opaque: it stages the bytes, moves
 * the caps, and echoes the tag; it never interprets the opcode header.
 *
 * Cap descriptors (aerosls_cap_desc_t) name MEM caps in the CALLER's table.
 * The kernel moves each into the queue; on recv they are installed into
 * fresh slots and the descriptor's `slot` is replaced with the receiver's
 * new index — exactly the SEND_CAP / RECV_CAP sequence of §2.2.
 *
 * Build: include from ring-3 C; links against libsls (for _sls_syscall).
 *   gcc -Iuser/libsls -Iuser/libaerocap ...
 */
#ifndef AEROSLS_CAP_H
#define AEROSLS_CAP_H

#include <stdint.h>
#include <stddef.h>
#include <sls.h>   /* _sls_syscall */
#include <aerocap.h>  /* sls_cap_arena_alloc_req, cap_arena_alloc (syscall 290) */

#ifdef __cplusplus
extern "C" {
#endif

/* ── Capability permissions / flags (MUST match the kernel + mock) ─────── */
#define AEROSLS_CAP_PERM_R        0x01
#define AEROSLS_CAP_PERM_W        0x02
#define AEROSLS_CAP_PERM_X        0x04

#define AEROSLS_CAP_FLAG_BORROWED    0x00
#define AEROSLS_CAP_FLAG_ARENA       0x01
#define AEROSLS_CAP_FLAG_ARENA_OWNED 0x02

#define AEROSLS_CAP_NONE          0xFFFF
#define AEROSLS_CHAN_FLAG_NO_REPLY 0x0001

/* ── Syscall numbers (MUST match kernel/cap.h SYS_SLS_CAP_*) ───────────── */
#define SLS_SYS_CAP_SEND_MSG    302
#define SLS_SYS_CAP_RECV_MSG    303
#define SLS_SYS_CAP_ARENA_FREE  304

/* ── Capability descriptor (16 bytes; MUST match kernel SLSCapDesc) ────── */
typedef struct {
    uint16_t slot;      /* sender's cap-table slot; [out] receiver's on recv */
    uint32_t offset;    /* byte offset into the arena region */
    uint32_t len;       /* bytes granted */
    uint8_t  rights;    /* AEROSLS_CAP_PERM_* */
    uint8_t  flags;     /* AEROSLS_CAP_FLAG_* */
} aerosls_cap_desc_t;

/* ── Request structs — layout MUST be identical to kernel/cap.h ────────── */
#define AEROSLS_MSG_MAX_CAPS  4
#define AEROSLS_MSG_MAX_PAYLOAD 4096

struct sls_cap_send_msg_req {
    uint16_t ch_w_idx;
    uint16_t n_caps;
    uint8_t  _pad[4];
    uint32_t tag;
    uint32_t flags;
    uint32_t payload_len;
    uint8_t  _pad2[4];
    void*    payload;
    aerosls_cap_desc_t caps[AEROSLS_MSG_MAX_CAPS];
};

struct sls_cap_recv_msg_req {
    uint16_t ch_r_idx;
    uint8_t  block;
    uint8_t  _pad[1];
    uint16_t max_caps;
    uint8_t  _pad2[2];
    void*    buf;
    uint32_t buf_len;
    uint8_t  _pad3[4];
    uint32_t out_tag;
    uint32_t out_flags;
    uint32_t out_payload_len;
    uint16_t out_n_caps;
    uint8_t  _pad4[2];
    aerosls_cap_desc_t out_caps[AEROSLS_MSG_MAX_CAPS];
};

struct sls_cap_arena_free_req {
    uint16_t cap_idx;
    uint8_t  _pad[6];
};

/* ── Request-id counter (matches the mock's contract) ──────────────────── */
static uint32_t g_aerosls_req_id_counter = 0;

static inline uint32_t
aerosls_next_request_id(void)
{
    return ++g_aerosls_req_id_counter;
}

/* ── Send a message (SEND_CAP) ──────────────────────────────────────────────
 * opcode 0 = reply (payload sent raw); opcode != 0 = request (the library
 * prepends the [opcode u16][payload_len u32] header the dispatcher parses).
 * caps[] name MEM caps in the caller's table; the kernel MOVES them. Returns
 * 0 on success, negative CAP_E* on failure (nothing queued on failure). */
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
    /* IDL frame: [opcode u16 LE][payload_len u32 LE][args...] for requests. */
    uint8_t frame[AEROSLS_MSG_MAX_PAYLOAD];
    size_t  wire_len = payload_len;
    if (opcode != 0) {
        if (payload_len + 6 > sizeof(frame)) return -9; /* CAP_ERANGE */
        frame[0] = (uint8_t)(opcode & 0xFF);
        frame[1] = (uint8_t)((opcode >> 8) & 0xFF);
        frame[2] = (uint8_t)(payload_len & 0xFF);
        frame[3] = (uint8_t)((payload_len >> 8) & 0xFF);
        frame[4] = (uint8_t)((payload_len >> 16) & 0xFF);
        frame[5] = (uint8_t)((payload_len >> 24) & 0xFF);
        for (size_t i = 0; i < payload_len; i++)
            frame[6 + i] = ((const uint8_t*)payload)[i];
        payload  = frame;
        wire_len = payload_len + 6;
    }

    struct sls_cap_send_msg_req req;
    sls_memset(&req, 0, sizeof(req));
    req.ch_w_idx    = chan_w;
    req.n_caps      = (n_caps > AEROSLS_MSG_MAX_CAPS) ? AEROSLS_MSG_MAX_CAPS : n_caps;
    req.tag         = req_id;
    req.flags       = flags;
    req.payload     = (void*)(uintptr_t)payload;
    req.payload_len = (uint32_t)wire_len;
    for (int i = 0; i < req.n_caps; i++) {
        req.caps[i].slot   = caps[i].slot;
        req.caps[i].offset = caps[i].offset;
        req.caps[i].len    = caps[i].len;
        req.caps[i].rights = caps[i].rights;
        req.caps[i].flags  = caps[i].flags;
    }
    return (int)(int64_t)_sls_syscall(SLS_SYS_CAP_SEND_MSG, &req);
}

/* ── Receive a message (RECV_CAP) ───────────────────────────────────────────
 * Copies the kernel payload into buf (min(buf_len, payload_len)), installs
 * up to max_caps moved caps into the caller's table and writes their NEW
 * slot indices + descriptors into cap_slots/out_caps. Empty queue returns
 * negative CAP_EAGAIN — callers block by retrying (the kernel-side park is
 * Phase-1.5 territory, single-cap path only). Returns 0 on success. */
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
    struct sls_cap_recv_msg_req req;
    sls_memset(&req, 0, sizeof(req));
    req.ch_r_idx = chan_r;
    req.buf      = buf;
    req.buf_len  = (uint32_t)buf_len;
    req.max_caps = (max_caps > AEROSLS_MSG_MAX_CAPS) ? AEROSLS_MSG_MAX_CAPS
                                                     : max_caps;

    int64_t r = (int64_t)_sls_syscall(SLS_SYS_CAP_RECV_MSG, &req);
    if (r < 0) return (int)r;

    if (out_kind)     *out_kind     = 0;   /* MSG */
    if (out_tag)      *out_tag      = req.out_tag;
    if (out_len)      *out_len      = req.out_payload_len;
    if (out_n_caps)   *out_n_caps   = req.out_n_caps;
    for (int i = 0; i < req.out_n_caps && cap_slots; i++)
        cap_slots[i] = req.out_caps[i].slot;
    return 0;
}

/* ── Arena allocation / free ─────────────────────────────────────────────────
 * arena_alloc wraps the existing SYS_SLS_CAP_ARENA_ALLOC (290), rounding the
 * byte request up to pages; arena_free drops the cap via 304 (arena frames
 * return to the pool at refcount 0 — the callee's half of the ownership
 * handoff the IDL's @arena/@owned annotations describe). */
static inline uint16_t
aerosls_arena_alloc(size_t bytes, uint8_t perms)
{
    uint32_t npages = (uint32_t)((bytes + 4095u) / 4096u);
    if (npages == 0) npages = 1;
    cap_t idx = CAP_NONE;
    int r = cap_arena_alloc(npages, perms, &idx);
    if (r < 0) return AEROSLS_CAP_NONE;
    return (uint16_t)idx;
}

static inline void
aerosls_arena_free(uint16_t cap)
{
    if (cap == AEROSLS_CAP_NONE) return;
    struct sls_cap_arena_free_req req;
    sls_memset(&req, 0, sizeof(req));
    req.cap_idx = cap;
    _sls_syscall(SLS_SYS_CAP_ARENA_FREE, &req);
}

#ifdef __cplusplus
}
#endif

#endif /* AEROSLS_CAP_H */
