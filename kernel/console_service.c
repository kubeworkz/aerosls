/* console_service.c — see console_service.h. */
#include "console_service.h"
#include "cap.h"
#include "kernel_io.h"

#define CONSOLE_SVC_BUF 4096

static uint8_t  console_svc_buf[CONSOLE_SVC_BUF];
static uint32_t console_svc_drained = 0;

#define CONSOLE_INPUT_BUF 256
static uint8_t  console_input_buf[CONSOLE_INPUT_BUF];

static int console_is_chan_r(uint64_t w) {
    if (((w >> CAP_TYPE_SHIFT) & CAP_TYPE_MASK) != CAP_TYPE_CHAN_R) return 0;
    if (((w >> CAP_STATE_SHIFT) & CAP_STATE_MASK) != CAP_STATE_VALID) return 0;
    if (w & CAP_RSVD_MASK) return 0;
    return 1;
}

static int console_is_chan_w(uint64_t w) {
    if (((w >> CAP_TYPE_SHIFT) & CAP_TYPE_MASK) != CAP_TYPE_CHAN_W) return 0;
    if (((w >> CAP_STATE_SHIFT) & CAP_STATE_MASK) != CAP_STATE_VALID) return 0;
    if (w & CAP_RSVD_MASK) return 0;
    return 1;
}

void console_service_tick(void) {
    for (uint32_t s = 0; s < CAP_TABLE_ENTRIES; s++) {
        uint64_t w = cap_tables[0].slots[s].word;
        if (!console_is_chan_r(w)) continue;

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

        for (;;) {
            uint32_t plen = 0, tag = 0, flags = 0;
            uint16_t n_caps = 0;
            int r = cap_recv_msg(0, (uint16_t)s,
                                 console_svc_buf, sizeof(console_svc_buf),
                                 &plen, 0, 0, &n_caps, &tag, &flags);
            if (r != 0) break;
            for (uint32_t i = 0; i < plen; i++)
                kernel_serial_putchar((char)console_svc_buf[i]);
            console_svc_drained++;
        }

        if (ch) {
            cap_lock(&ch->lock);
            int peer_gone = ch->close_evt[kdir];
            cap_unlock(&ch->lock);
            if (peer_gone)
                cap_revoke(0, (uint16_t)s);
        }
    }

    {
        int n = serial_console_poll((char*)console_input_buf, CONSOLE_INPUT_BUF);
        if (n > 0) {
            for (uint32_t s = 0; s < CAP_TABLE_ENTRIES; s++) {
                uint64_t w = cap_tables[0].slots[s].word;
                if (!console_is_chan_w(w)) continue;
                cap_send_msg(0, (uint16_t)s,
                             console_input_buf, (uint32_t)n,
                             0, 0, 0, 0);
            }
        }
    }
}

uint32_t console_service_drained(void) {
    return console_svc_drained;
}
