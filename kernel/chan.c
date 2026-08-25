/*
 * chan.c — AeroSLS Phase 5 channel transport (the sidecar-facing kabi
 * contract: docs/AeroSLS-Sidecar-Channels-Transport-Spec-v0.1.md §3-§5).
 *
 * This file implements the kernel side of the extern "C" declarations in
 * user/proto/src/kabi.rs — k_chan_wait / k_chan_recv / k_chan_send /
 * k_chan_close / k_cap_info — over the Phase-3 message transport
 * (cap_send_msg / cap_recv_msg in cap.c). The sidecar-side shims (same
 * names, in kabi.rs behind the `target` feature) forward to these via
 * syscalls 311-315, whose request structs mirror the kabi signatures.
 *
 * Contract deltas vs. the Phase-3 syscalls (302/303):
 *   - Returns the transport's POSITIVE CAP_ERR_* codes (0 = CAP_ERR_OK),
 *     NOT the negative CAP_E* codes of the Phase-3 syscalls.
 *   - k_chan_recv reports the head's KIND (CH_KIND_MSG | CH_KIND_CLOSE)
 *     and honors the no-loss rule: an undersized recv (buf or slots)
 *     returns CAP_ERR_BUFSZ with `needed` and does NOT consume the
 *     message (transport spec §3.4).
 *   - k_chan_wait polls a LIST of endpoints and returns the index + kind
 *     of the first ready one.
 *   - k_chan_close is idempotent and delivers a CLOSE event to the peer
 *     after its message queue drains (close_evt[] state on the channel;
 *     cap.c's queue format is untouched).
 *   - k_cap_info introspects one of the caller's own caps.
 *
 * Honest limits (each documented where it bites):
 *   - BLOCKING WAIT, BLOCKING SEND, AND DEADLINES ARE REAL. k_chan_wait
 *     with CH_TIMEOUT_NONE and a queue-full k_chan_send with timeout_ns ==
 *     0 park the caller on the listed channels (cap_wait_chans, process.c
 *     — the same park/resume machinery as the Phase-1.5 blocking recv,
 *     generalized to a channel LIST via park_syscall); a later enqueue /
 *     a slot freed by recv / an explicit close / peer-death wakes them to
 *     re-run the syscall. A FINITE timeout parks WITH a deadline: the
 *     timer ISR (cap_park_deadline_tick, one call per ~10 ms tick) wakes
 *     the park when the deadline passes and the re-run then returns
 *     CAP_ERR_TIMEOUT. Deadline granularity is one tick (KERNEL_TICK_NS,
 *     ~10 ms, calibration-dependent): a sub-tick timeout is rounded UP, so
 *     a 1 ns deadline is met within one tick period, not instantaneously
 *     — the SDK-side retry loop still supplies finer user-visible
 *     timeouts where it needs them.
 *   - NEW_CHANNEL events (path-4 k_chan_create) are not produced; the
 *     kind constant exists so wait/recv can report it once the dynamic
 *     creation path lands.
 *   - The request/reply window (spec §3.3) is not enforced: F_REPLY is
 *     accepted and ignored (the transport has no outstanding_tag
 *     bookkeeping). NO_REPLY + any cap argument IS rejected (CAP_ERR_PROTO)
 *     — the transport's moved MEM caps are transient by nature, and
 *     transient grants require a reply window (§3.4).
 */
#include "cap.h"
#include "kernel_io.h"
#include "timer.h"   /* Phase 5 deadlines: kernel_tick_counter, KERNEL_TICK_NS */
#include <stddef.h>

/* ─── Slot → channel resolution ───────────────────────────────────────────── */
/* Table index for pid, mirroring cap.c's cap_table_index lookup via .pid
 * (cap_table_index itself is static in cap.c). -1 = no table. */
static int chan_table_for_pid(uint32_t pid) {
    for (int i = 0; i < CAP_TABLE_MAX; i++)
        if (cap_tables[i].pid == pid) return i;
    return -1;
}

static int chan_word_valid(uint64_t w) {
    uint32_t ty = (uint32_t)((w >> CAP_TYPE_SHIFT) & CAP_TYPE_MASK);
    uint32_t st = (uint32_t)((w >> CAP_STATE_SHIFT) & CAP_STATE_MASK);
    return ty != CAP_TYPE_NONE && st == CAP_STATE_VALID && !(w & CAP_RSVD_MASK);
}

/* Resolve the caller's `slot` to a live channel endpoint. `want_type` is
 * CAP_TYPE_CHAN_R, CAP_TYPE_CHAN_W, or 0 (either — used by close). Fills
 * *out_chan_id (index into cap_channels[]) and *out_dir (0 = end0, 1 =
 * end1). Returns CAP_ERR_OK or a CAP_ERR_* code. */
static int chan_resolve(uint32_t pid, uint16_t slot, uint32_t want_type,
                        uint32_t* out_chan_id, int* out_dir) {
    int ti = chan_table_for_pid(pid);
    if (ti < 0) return CAP_ERR_NOTFOUND;
    if (slot >= CAP_TABLE_ENTRIES) return CAP_ERR_RANGE;
    uint64_t w = cap_tables[ti].slots[slot].word;
    if (!chan_word_valid(w)) return CAP_ERR_REVOKED;
    uint32_t ty = (uint32_t)((w >> CAP_TYPE_SHIFT) & CAP_TYPE_MASK);
    if (ty != CAP_TYPE_CHAN_R && ty != CAP_TYPE_CHAN_W) return CAP_ERR_TYPE;
    if (want_type != 0 && ty != want_type) return CAP_ERR_TYPE;
    if (want_type == CAP_TYPE_CHAN_R &&
        !((w >> CAP_PERM_SHIFT) & CAP_PERM_RECV)) return CAP_ERR_RIGHTS;
    if (want_type == CAP_TYPE_CHAN_W &&
        !((w >> CAP_PERM_SHIFT) & CAP_PERM_SEND)) return CAP_ERR_RIGHTS;
    uint32_t obj_id = (uint32_t)((w >> CAP_OBJ_SHIFT) & CAP_OBJ_MASK);
    if (obj_id >= CAP_OBJECT_MAX) return CAP_ERR_REVOKED;
    struct CapObject* o = &cap_objects[obj_id];
    if (!o->active || o->kind != CAP_OBJ_KIND_CHAN) return CAP_ERR_REVOKED;
    uint32_t chan_id = o->chan_id;
    if (chan_id >= CAP_CHAN_MAX) return CAP_ERR_REVOKED;
    struct CapChannel* ch = &cap_channels[chan_id];
    if (!ch->active) return CAP_ERR_STATE;
    *out_chan_id = chan_id;
    *out_dir = (pid == ch->end0_pid) ? 0 : 1;
    return CAP_ERR_OK;
}

/* Map a Phase-3 negative CAP_E* code to the transport's CAP_ERR_*. */
static int chan_map_err(int cap_e) {
    switch (cap_e) {
        case 0:                return CAP_ERR_OK;
        case CAP_EBADF:        return CAP_ERR_NOTFOUND;
        case CAP_ECAPREVOKED:  return CAP_ERR_REVOKED;
        case CAP_EINVAL:       return CAP_ERR_TYPE;    /* not a CHAN_W / bad desc */
        case CAP_EAGAIN:       return CAP_ERR_TIMEOUT; /* queue full / empty */
        case CAP_ENOMEM:       return CAP_ERR_NOMEM;
        case CAP_ENOSPC:       return CAP_ERR_NOMEM;   /* payload pool exhausted */
        case CAP_ERANGE:       return CAP_ERR_RANGE;
        case CAP_ETABLEFULL:   return CAP_ERR_SPACE;
        default:               return CAP_ERR_PROTO;
    }
}

/* Convert a RELATIVE deadline (nanoseconds) to an ABSOLUTE deadline in
 * kernel ticks, rounded UP to whole ~10 ms ticks (KERNEL_TICK_NS) and
 * saturating at UINT64_MAX (a huge-but-finite timeout becomes a deadline
 * ~5.8e9 years out — effectively never, but never a wrapped small value;
 * the wrapping case is a real hazard for the SDK's TIMEOUT_NONE = u64::MAX
 * constant, see below). CH_TIMEOUT_NONE is handled by the CALLERS before
 * this (it means block-forever, deadline 0), because adding KERNEL_TICK_NS
 * to u64::MAX would wrap to ~0 ticks and make a "block forever" send
 * expire instantly. */
static uint64_t chan_deadline_from_ns(uint64_t timeout_ns) {
    uint64_t add = KERNEL_TICK_NS - 1;
    uint64_t ticks;
    if (timeout_ns > UINT64_MAX - add)
        ticks = UINT64_MAX / KERNEL_TICK_NS;   /* saturate */
    else
        ticks = (timeout_ns + add) / KERNEL_TICK_NS;
    uint64_t now = kernel_tick_counter;
    if (now > UINT64_MAX - ticks) return UINT64_MAX;
    return now + ticks;
}

/* ─── k_chan_wait ────────────────────────────────────────────────────────────
 * Poll `chans` (caller's CHAN_R slots) in order; return the index and the
 * head entry's kind of the first ready endpoint. Nothing ready → BLOCK:
 * park the caller on the resolved channels (cap_wait_chans) and re-run the
 * wait on wake. CH_TIMEOUT_NONE blocks forever; a finite timeout parks
 * WITH a deadline — the timer ISR wakes the park when the deadline passes
 * and this re-run then returns CAP_ERR_TIMEOUT. `req` is the
 * SLSChanWaitRequest the park's resume re-runs. */
int k_chan_wait(uint32_t pid, const uint16_t* chans, uint32_t n,
                uint64_t timeout_ns, uint32_t* out_idx, uint16_t* out_kind,
                void* req) {
    if (!chans || n == 0 || n > CHAN_WAIT_MAX_CHANS) return CAP_ERR_PROTO;
    if (!out_idx || !out_kind) return CAP_ERR_PROTO;
    *out_idx = 0;
    *out_kind = CH_KIND_NONE;

    /* Deadline bookkeeping. A RE-RUN (woken by an event or the deadline
     * tick) takes the ORIGINAL absolute deadline from the parked state
     * (cap_park_deadline_take), so re-parks after spurious wakes never
     * extend a finite timeout. A fresh call (nothing stored) computes
     * now + timeout_ns, rounded UP to whole ~10 ms ticks. 0 = block
     * forever (CH_TIMEOUT_NONE). */
    uint64_t deadline = cap_park_deadline_take();
    if (deadline == 0 && timeout_ns != CH_TIMEOUT_NONE)
        deadline = chan_deadline_from_ns(timeout_ns);

    uint32_t chan_ids[CHAN_WAIT_MAX_CHANS];
    int dirs[CHAN_WAIT_MAX_CHANS];
    for (uint32_t i = 0; i < n; i++) {
        uint32_t chan_id;
        int dir;
        int r = chan_resolve(pid, chans[i], CAP_TYPE_CHAN_R, &chan_id, &dir);
        if (r != CAP_ERR_OK) return r;   /* every listed handle must be valid */
        chan_ids[i] = chan_id;
        dirs[i] = dir;
        struct CapChannel* ch = &cap_channels[chan_id];
        cap_lock(&ch->lock);
        int ready = (ch->qdepth[dir] > 0);
        uint16_t kind = CH_KIND_MSG;
        if (!ready && ch->close_evt[dir]) { ready = 1; kind = CH_KIND_CLOSE; }
        cap_unlock(&ch->lock);
        if (ready) {
            *out_idx = i;
            *out_kind = kind;
            return CAP_ERR_OK;
        }
    }
    /* Nothing ready. Park the caller on the resolved channel ids
     * (cap_wait_chans — process.c's strong override): a later enqueue
     * (cap_send_msg's wake), explicit close, peer-death (teardown's
     * wake), or the deadline tick (cap_park_deadline_tick) resumes the
     * process, which re-runs THIS syscall and re-polls the same request —
     * the wake cannot be lost and the wait returns the data/event that
     * woke it (or CAP_ERR_TIMEOUT when the deadline elapsed with nothing
     * ready). A deadline that has already passed returns CAP_ERR_TIMEOUT
     * without re-parking. Single-CPU atomicity makes check-then-park
     * safe: this syscall does not yield until the park's switch, so
     * nothing can enqueue between the last poll and the waiting-set
     * registration. The weak default (host tests) returns 0 →
     * CAP_ERR_TIMEOUT, preserving scan-once semantics. */
    if (deadline != 0 && kernel_tick_counter >= deadline)
        return CAP_ERR_TIMEOUT;   /* deadline elapsed; nothing arrived */
    cap_wait_chans(chan_ids, n, req, SYS_SLS_CHAN_WAIT, deadline);
    /* noreturn when it parks. If it returned (couldn't park / hlt woke),
     * re-check the queues — the message may have arrived during the hlt.
     * Use the per-channel dir[] resolved in the first poll loop. */
    for (uint32_t i = 0; i < n; i++) {
        struct CapChannel* ch = &cap_channels[chan_ids[i]];
        int d = dirs[i];
        cap_lock(&ch->lock);
        int ready = (ch->qdepth[d] > 0);
        uint16_t kind = CH_KIND_MSG;
        if (!ready && ch->close_evt[d]) { ready = 1; kind = CH_KIND_CLOSE; }
        cap_unlock(&ch->lock);
        if (ready) {
            *out_idx = i;
            *out_kind = kind;
            return CAP_ERR_OK;
        }
    }
    return CAP_ERR_TIMEOUT;   /* nothing arrived: retry */
}

/* ─── k_chan_recv ────────────────────────────────────────────────────────────
 * Receive one message/event from `chan` (caller's CHAN_R slot). Delivers a
 * queued CLOSE event once the message queue is drained; otherwise the head
 * MSG. Undersize (buf_len < payload or n_slots < cap count) → CAP_ERR_BUFSZ
 * with `needed` = (caps_needed << 16) | payload_needed, message NOT
 * consumed. Empty queue → CAP_ERR_STATE (the fake kernel's convention). */
int k_chan_recv(uint32_t pid, uint16_t chan, void* buf, uint32_t buf_len,
                struct SLSChanCapRef* slots, uint32_t n_slots,
                struct SLSChanRecvOut* out) {
    if (!out) return CAP_ERR_PROTO;
    out->kind = CH_KIND_NONE;
    out->flags = 0;
    out->tag = 0;
    out->len = 0;
    out->n_caps = 0;
    out->needed = 0;

    uint32_t chan_id;
    int dir;
    int r = chan_resolve(pid, chan, CAP_TYPE_CHAN_R, &chan_id, &dir);
    if (r != CAP_ERR_OK) return r;
    struct CapChannel* ch = &cap_channels[chan_id];

    cap_lock(&ch->lock);
    if (ch->qdepth[dir] == 0) {
        /* Empty: deliver a pending CLOSE event (after all in-flight
         * messages), else report the empty state. */
        if (ch->close_evt[dir]) {
            uint16_t reason = ch->close_reason[dir];
            uint32_t detail = ch->close_detail[dir];
            if (buf_len < 8) {
                out->needed = 8;              /* close body: reason u16 + detail u32 + pad */
                cap_unlock(&ch->lock);
                return CAP_ERR_BUFSZ;         /* not consumed */
            }
            ch->close_evt[dir] = 0;           /* delivered exactly once */
            cap_unlock(&ch->lock);
            uint8_t* p = (uint8_t*)buf;
            p[0] = (uint8_t)(reason & 0xFF);
            p[1] = (uint8_t)(reason >> 8);
            p[2] = (uint8_t)(detail & 0xFF);
            p[3] = (uint8_t)((detail >> 8) & 0xFF);
            p[4] = (uint8_t)((detail >> 16) & 0xFF);
            p[5] = (uint8_t)((detail >> 24) & 0xFF);
            p[6] = 0;
            p[7] = 0;
            out->kind = CH_KIND_CLOSE;
            out->flags = 0;
            out->tag = 0;
            out->len = 8;
            out->n_caps = 0;
            return CAP_ERR_OK;
        }
        cap_unlock(&ch->lock);
        return CAP_ERR_STATE;
    }
    /* Peek the head for the no-loss size check. The queue can only grow
     * between here and cap_recv_msg (send appends), so the head is stable. */
    uint32_t entry = ch->qhead[dir] % CHAN_QUEUE_DEPTH;
    struct ChanMsg* m = &ch->q[dir][entry];
    uint32_t need = m->payload_len;
    uint32_t needc = m->n_caps;
    cap_unlock(&ch->lock);

    if (buf_len < need || n_slots < needc) {
        out->needed = ((needc & 0xFFFFu) << 16) | (need & 0xFFFFu);
        return CAP_ERR_BUFSZ;   /* message stays queued */
    }

    uint32_t plen = 0, tag = 0, tflags = 0;
    uint16_t n_inst = 0;
    struct SLSCapDesc descs[CAP_MSG_MAX_CAPS];
    int rr = cap_recv_msg(pid, chan, buf, buf_len, &plen,
                          (uint16_t)n_slots, descs, &n_inst, &tag, &tflags);
    if (rr < 0) return chan_map_err(rr);

    out->kind = CH_KIND_MSG;
    out->flags = (tflags & 0x1u) ? CH_F_NO_REPLY : 0;   /* transport bit0 → kabi bit1 */
    out->tag = tag;
    out->len = plen;
    out->n_caps = n_inst;
    if (slots) {
        for (uint16_t i = 0; i < n_inst && i < n_slots; i++) {
            uint32_t oid = cap_debug_objid(pid, descs[i].slot);
            uint64_t pbase = (oid != 0xFFFFFFFFu) ? cap_debug_obj_phys(oid) : 0;
            slots[i].handle = descs[i].slot;   /* the receiver's NEW slot */
            slots[i].rights = descs[i].rights;
            slots[i].flags  = descs[i].flags;
            slots[i].pad    = 0;
            slots[i].base   = pbase + descs[i].offset;   /* sender base + offset */
            slots[i].len    = descs[i].len;
        }
    }
    return CAP_ERR_OK;
}

/* ─── k_chan_send ────────────────────────────────────────────────────────────
 * Send one MSG on `chan` (caller's CHAN_W slot). NO_REPLY + cap arguments
 * is rejected atomically (CAP_ERR_PROTO — moved caps are transient and
 * need a reply window). Queue full → BLOCK: park the sender on the
 * channel (cap_wait_chans, park_syscall = SYS_SLS_CHAN_SEND); a recv
 * freeing a slot (cap_recv_msg's wake) resumes it to re-run THIS send,
 * which now enqueues. timeout_ns == 0 blocks forever; a finite timeout
 * parks WITH a deadline — the timer ISR wakes the park when the deadline
 * passes and the re-run then returns CAP_ERR_TIMEOUT. A park that could
 * not happen (kernel context / nothing runnable) → CAP_ERR_TIMEOUT,
 * nothing enqueued. `req` is the SLSChanSendRequest the park's resume
 * re-runs. */
int k_chan_send(uint32_t pid, uint16_t chan, uint32_t tag, uint16_t flags,
                const void* payload, uint32_t payload_len,
                const struct SLSCapDesc* caps, uint16_t n_caps,
                uint64_t timeout_ns, void* req) {
    if (payload_len > CAP_MSG_MAX_PAYLOAD) return CAP_ERR_RANGE;
    if (n_caps > CAP_MSG_MAX_CAPS) return CAP_ERR_RANGE;
    if (payload_len > 0 && !payload) return CAP_ERR_PROTO;
    if (n_caps > 0 && !caps) return CAP_ERR_PROTO;
    if ((flags & CH_F_NO_REPLY) && n_caps > 0) return CAP_ERR_PROTO;

    /* Deadline bookkeeping (same shape as k_chan_wait): timeout_ns == 0
     * OR CH_TIMEOUT_NONE blocks until enqueued (deadline 0 = forever — the
     * SDK passes TIMEOUT_NONE for every blocking send, so u64::MAX must
     * mean forever here, never a wrapped deadline); any other finite
     * timeout blocks with a deadline. A re-run takes the ORIGINAL absolute
     * deadline (never extended); a fresh call computes now + timeout_ns,
     * rounded UP to whole ~10 ms ticks. */
    uint64_t deadline = cap_park_deadline_take();
    if (deadline == 0 && timeout_ns != 0 && timeout_ns != CH_TIMEOUT_NONE)
        deadline = chan_deadline_from_ns(timeout_ns);

    uint32_t chan_id;
    int dir;
    /* Accept both CHAN_W (normal send) and CHAN_R (reply from server on
     * its receive handle — the kernel routes to the peer's receive queue). */
    int r = chan_resolve(pid, chan, CAP_TYPE_CHAN_W, &chan_id, &dir);
    if (r == CAP_ERR_TYPE)
        r = chan_resolve(pid, chan, CAP_TYPE_CHAN_R, &chan_id, &dir);
    if (r != CAP_ERR_OK) return r;
    struct CapChannel* ch = &cap_channels[chan_id];
    cap_lock(&ch->lock);
    /* STATE if EITHER side is closed — the fake kernel checks both
     * (driver_closed || client_closed): an endpoint whose peer closed or
     * died cannot receive (the close event is the signal to stop). */
    int closed_side = ch->closed[dir] || ch->closed[1 - dir];
    cap_unlock(&ch->lock);
    if (closed_side) return CAP_ERR_STATE;

    /* kabi flags: bit0 REPLY, bit1 NO_REPLY. Transport flags: bit0 = NO_REPLY. */
    uint32_t tflags = (flags & CH_F_NO_REPLY) ? 0x1u : 0u;
    int rr = cap_send_msg(pid, chan, payload, payload_len, caps, n_caps,
                          tag, tflags);
    if (rr == 0) return CAP_ERR_OK;
    if (rr == CAP_EAGAIN) {
        /* Queue full → BLOCK: park the caller on this channel (process.c's
         * strong cap_wait_chans — the resume re-runs SYS_SLS_CHAN_SEND
         * with the same request, which now finds space; a re-run after the
         * peer closed/died fails CAP_ERR_STATE instead, so a blocked
         * sender never hangs on a dead peer; a re-run after the deadline
         * tick returns CAP_ERR_TIMEOUT here). A deadline that has already
         * passed returns CAP_ERR_TIMEOUT without re-parking. Noreturn when
         * it parks; the weak default / no-runnable case falls through to
         * chan_map_err(CAP_EAGAIN) = CAP_ERR_TIMEOUT below (nothing
         * enqueued — cap_send_msg failed before mutating anything). */
        if (deadline != 0 && kernel_tick_counter >= deadline)
            return CAP_ERR_TIMEOUT;   /* deadline elapsed; still full */
        cap_wait_chans(&chan_id, 1, req, SYS_SLS_CHAN_SEND, deadline);
    }
    return chan_map_err(rr);
}

/* ─── k_chan_close ───────────────────────────────────────────────────────────
 * Close the caller's endpoint (by its CHAN_R or CHAN_W slot). Idempotent
 * (spec §6.1). Marks the endpoint closed (further sends/recvs fail with
 * CAP_ERR_STATE) and queues a CLOSE event for the peer, delivered once its
 * message queue drains. */
int k_chan_close(uint32_t pid, uint16_t chan, uint16_t reason, uint32_t detail) {
    uint32_t chan_id;
    int dir;
    int r = chan_resolve(pid, chan, 0, &chan_id, &dir);   /* either endpoint */
    if (r != CAP_ERR_OK) return r;
    struct CapChannel* ch = &cap_channels[chan_id];
    cap_lock(&ch->lock);
    if (ch->closed[dir]) {
        cap_unlock(&ch->lock);
        return CAP_ERR_OK;   /* idempotent */
    }
    ch->closed[dir] = 1;
    ch->close_evt[1 - dir] = 1;        /* the peer observes the close */
    ch->close_reason[1 - dir] = reason;
    ch->close_detail[1 - dir] = detail;
    cap_unlock(&ch->lock);
    /* Wake any process parked in k_chan_wait (TIMEOUT_NONE) on this
     * channel: the CLOSE event just queued for the peer is exactly what it
     * is waiting for — same wake-then-handoff pattern as cap_send_msg
     * after an enqueue (the peer observes the close before the closer's
     * syscall even returns to ring-3). No-op when nobody is parked; the
     * idempotent early-return above means the wake fires once per close. */
    cap_wake_chan(chan_id);
    cap_maybe_handoff();
    return CAP_ERR_OK;
}

/* ─── k_cap_info ─────────────────────────────────────────────────────────────
 * Introspect one of the caller's own caps (capability-layer spec §3.5).
 * MEM: base = object's physical base, len = npages × 4096. CHAN: base/len
 * are 0 (a channel is not a region). */
int k_cap_info(uint32_t pid, uint16_t handle, struct SLSCapInfoOut* out) {
    if (!out) return CAP_ERR_PROTO;
    out->ty = 0;
    out->rights = 0;
    out->flags = 0;
    out->base = 0;
    out->len = 0;
    int ti = chan_table_for_pid(pid);
    if (ti < 0) return CAP_ERR_NOTFOUND;
    if (handle >= CAP_TABLE_ENTRIES) return CAP_ERR_RANGE;
    uint64_t w = cap_tables[ti].slots[handle].word;
    if (!chan_word_valid(w)) return CAP_ERR_REVOKED;
    out->ty = (uint16_t)((w >> CAP_TYPE_SHIFT) & CAP_TYPE_MASK);
    out->rights = (uint16_t)((w >> CAP_PERM_SHIFT) & CAP_PERM_MASK);
    uint32_t obj_id = (uint32_t)((w >> CAP_OBJ_SHIFT) & CAP_OBJ_MASK);
    if (obj_id < CAP_OBJECT_MAX) {
        struct CapObject* o = &cap_objects[obj_id];
        if (o->active && out->ty == CAP_TYPE_MEM) {
            out->base = o->phys_base;
            out->len = (uint64_t)o->npages * 4096u;
        }
    }
    return CAP_ERR_OK;
}

/* ─── Syscall wrappers (311-315) ─────────────────────────────────────────────
 * Each unpacks the single request pointer (the sidecar's shim builds it)
 * and calls the kabi function above with cap_current_pid() as the caller.
 * Return values are the POSITIVE CAP_ERR_* codes, passed through verbatim. */
uint64_t sys_sls_chan_wait(struct SLSChanWaitRequest* req) {
    if (!req) return CAP_ERR_PROTO;
    req->out_idx = 0;
    req->out_kind = CH_KIND_NONE;
    return (uint64_t)k_chan_wait(cap_current_pid(), req->chans, req->n_chans,
                                 req->timeout_ns, &req->out_idx, &req->out_kind,
                                 (void*)req);
}

uint64_t sys_sls_chan_recv(struct SLSChanRecvRequest* req) {
    if (!req) return CAP_ERR_PROTO;
    return (uint64_t)k_chan_recv(cap_current_pid(), req->chan, req->buf,
                                 req->buf_len, req->slots, req->n_slots,
                                 &req->out);
}

uint64_t sys_sls_chan_send(struct SLSChanSendRequest* req) {
    if (!req) return CAP_ERR_PROTO;
    return (uint64_t)k_chan_send(cap_current_pid(), req->chan, req->tag,
                                 req->flags, req->payload, req->payload_len,
                                 req->caps, req->n_caps, req->timeout_ns,
                                 (void*)req);
}

uint64_t sys_sls_chan_close(struct SLSChanCloseRequest* req) {
    if (!req) return CAP_ERR_PROTO;
    return (uint64_t)k_chan_close(cap_current_pid(), req->chan, req->reason,
                                  req->detail);
}

uint64_t sys_sls_cap_info(struct SLSCapInfoRequest* req) {
    if (!req) return CAP_ERR_PROTO;
    return (uint64_t)k_cap_info(cap_current_pid(), req->handle, &req->out);
}
