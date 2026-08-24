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
        if (!console_is_chan_r(cap_tables[0].slots[s].word)) continue;
        /* Drain everything queued on this endpoint. cap_recv_msg is
         * non-blocking: CAP_EAGAIN means empty, and a message with caps
         * is fully drained even with max_caps = 0 (cap_recv_msg drops the
         * queue holders). */
        for (;;) {
            uint32_t plen = 0, n_caps = 0, tag = 0, flags = 0;
            int r = cap_recv_msg(0, (uint16_t)s,
                                 console_svc_buf, sizeof(console_svc_buf),
                                 &plen, 0, 0, &n_caps, &tag, &flags);
            if (r != 0) break;   /* empty or error: move to the next slot */
            for (uint32_t i = 0; i < plen; i++)
                kernel_serial_putchar((char)console_svc_buf[i]);
            console_svc_drained++;
        }
    }
}

uint32_t console_service_drained(void) {
    return console_svc_drained;
}
