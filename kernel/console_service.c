/* console_service.c — see console_service.h. */
#include "console_service.h"
#include "cap.h"
#include "kernel_io.h"

/* One message's worth of console text. Channel payloads are bounded by
 * CAP_MSG_MAX_PAYLOAD (4096), so a full-size message always fits in one
 * recv and the drain can never stall on an oversized payload. */
#define CONSOLE_SVC_BUF 4096

static uint8_t  console_svc_buf[CONSOLE_SVC_BUF];
static uint32_t console_svc_drained = 0;

/* Is `w` a usable CHAN_R cap in the kernel context table? Mirrors
 * cap_word_valid() (static in kernel/cap.c) restricted to CHAN_R: real
 * type, VALID state, reserved bits zero (the forgery tag check). */
static int console_is_chan_r(uint64_t w) {
    if (((w >> CAP_TYPE_SHIFT) & CAP_TYPE_MASK) != CAP_TYPE_CHAN_R) return 0;
    if (((w >> CAP_STATE_SHIFT) & CAP_STATE_MASK) != CAP_STATE_VALID) return 0;
    if (w & CAP_RSVD_MASK) return 0;
    return 1;
}

void console_service_tick(void) {
    /* Every CHAN_R cap in the kernel context table is the far end of a
     * "kernel.*" console channel — cap_create_sidecar's kernel-peer
     * wiring is the only thing that mints into table 0 today. If a later
     * kernel service adds its own kernel-owned endpoints, this loop must
     * learn to tell them apart (e.g. by channel metadata). */
    for (uint32_t s = 0; s < CAP_TABLE_ENTRIES; s++) {
        uint64_t w = cap_tables[0].slots[s].word;
        if (!console_is_chan_r(w)) continue;

        /* Resolve to the channel so we can tell whether the child's end is
         * gone. The child's explicit k_chan_close and its death (via
         * cap_table_teardown's peer-death scan) both set close_evt[0] on
         * the kernel's dir — that is the signal to stop serving. */
        struct CapChannel* ch = 0;
        int kdir = 0;
        uint32_t obj_id = (uint32_t)((w >> CAP_OBJ_SHIFT) & CAP_OBJ_MASK);
        if (obj_id < CAP_OBJECT_MAX) {
            struct CapObject* o = &cap_objects[obj_id];
            if (o->active && o->kind == CAP_OBJ_KIND_CHAN &&
                o->chan_id < CAP_CHAN_MAX) {
                ch = &cap_channels[o->chan_id];
                kdir = (0 == ch->end0_pid) ? 0 : 1;
            }
        }

        /* Drain everything queued on this endpoint. cap_recv_msg is
         * non-blocking: CAP_EAGAIN means empty, and a message with caps
         * is fully drained even with max_caps = 0 (cap_recv_msg drops the
         * queue holders). */
        for (;;) {
            uint32_t plen = 0, tag = 0, flags = 0;
            uint16_t n_caps = 0;
            int r = cap_recv_msg(0, (uint16_t)s,
                                 console_svc_buf, sizeof(console_svc_buf),
                                 &plen, 0, 0, &n_caps, &tag, &flags);
            if (r != 0) break;   /* empty or error: move to the next slot */
            for (uint32_t i = 0; i < plen; i++)
                kernel_serial_putchar((char)console_svc_buf[i]);
            console_svc_drained++;
        }

        /* ...then, if the child's end is gone, drop the kernel end instead
         * of leaving the channel open forever. cap_revoke is total: it
         * frees THIS slot and the kernel's CHAN_W for the same object and
         * destroys the channel at refcount 0 (a dead child's console
         * channel must not linger in the kernel context table). The drain
         * above already consumed the child's last messages, so nothing is
         * lost. */
        if (ch) {
            cap_lock(&ch->lock);
            int peer_gone = ch->close_evt[kdir];
            cap_unlock(&ch->lock);
            if (peer_gone)
                cap_revoke(0, (uint16_t)s);
        }
    }
}

uint32_t console_service_drained(void) {
    return console_svc_drained;
}
