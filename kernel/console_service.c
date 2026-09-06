/* console_service.c — see console_service.h. */
#include "console_service.h"
#include "cap.h"
#include "kernel_io.h"

#define CONSOLE_SVC_BUF 4096

static uint8_t  console_svc_buf[CONSOLE_SVC_BUF];
static uint32_t console_svc_drained = 0;

#define CONSOLE_INPUT_BUF 256
static uint8_t  console_input_buf[CONSOLE_INPUT_BUF];
/* Single-flight flag for the RX poll (see console_service_tick). */
static volatile int console_rx_busy = 0;

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

    /* ── Serial RX → sidecar console channels ────────────────────────────
     * Poll the UART and forward completed lines to every kernel-held
     * CHAN_W console cap. console_feed() strips the line terminator, so
     * we re-add '\n' — the sidecar's sh applet completes a line only on
     * '\n' (the shell is what echoes, not the kernel, once input is
     * delivered).
     *
     * Single-flight: this tick runs on the AP core's service poll AND on
     * the BSP timer (uniprocessor fallback), and console_feed()'s line
     * editor is one shared static instance. Two concurrent feeders
     * corrupt it (caught live: typed input echoed back as "ecoh" with
     * h/o swapped, and one of two typed lines lost entirely). The
     * compare-and-swap admits exactly one poller per round; the loser
     * returns — bytes stay in the UART FIFO for the next tick. */
    if (__sync_bool_compare_and_swap(&console_rx_busy, 0, 1)) {
        int n = serial_console_poll((char*)console_input_buf, CONSOLE_INPUT_BUF);
        if (n > 0) {
            /* Line ≤ 255 chars; '\n' at index 255 is the last byte. */
            if (n < (int)CONSOLE_INPUT_BUF)
                console_input_buf[n++] = '\n';
            for (uint32_t s = 0; s < CAP_TABLE_ENTRIES; s++) {
                uint64_t w = cap_tables[0].slots[s].word;
                if (!console_is_chan_w(w)) continue;
                /* Skip consoles whose peer has NOT drained the previous
                 * line: the non-interactive sidecars (init/dm/ramdisk/
                 * network) never recv their console input, so every
                 * forwarded line pins a 4 KiB payload-pool frame until
                 * the 32-frame pool exhausts (~7 lines), starving the
                 * interactive shell's console with ENOSPC (caught live:
                 * typed line 7 of a 20-line soak never executed). The
                 * interactive consumer drains within a pump iteration,
                 * so its queue is empty between lines. */
                uint32_t obj_id =
                    (uint32_t)((w >> CAP_OBJ_SHIFT) & CAP_OBJ_MASK);
                int undrained = 0;
                if (obj_id < CAP_OBJECT_MAX) {
                    struct CapObject* o = &cap_objects[obj_id];
                    if (o->active && o->kind == CAP_OBJ_KIND_CHAN &&
                        o->chan_id < CAP_CHAN_MAX) {
                        struct CapChannel* ch = &cap_channels[o->chan_id];
                        int dest = (0 == ch->end0_pid) ? 1 : 0;
                        cap_lock(&ch->lock);
                        undrained = (ch->qdepth[dest] > 0);
                        cap_unlock(&ch->lock);
                    }
                }
                if (undrained) continue;
                cap_send_msg(0, (uint16_t)s,
                             console_input_buf, (uint32_t)n,
                             0, 0, 0, 0);
            }
        }
        console_rx_busy = 0;
    }
}


uint32_t console_service_drained(void) {
    return console_svc_drained;
}

/* ─── IRQ deferral (see console_service.h) ────────────────────────────────
 * timer_irq_handler latches this instead of running the drain in interrupt
 * context; smp_uniprocessor_tick consumes it in process context. On SMP
 * boots the AP's microkernel_service_poll already drains every 10 ticks, so
 * a latched-but-unconsumed tick is harmless (one extra drain at most). */
static volatile int console_tick_pending;

void console_service_irq_defer(void) {
    console_tick_pending = 1;
}

void console_service_deferred_tick(void) {
    if (!__atomic_exchange_n(&console_tick_pending, 0, __ATOMIC_ACQUIRE))
        return;
    console_service_tick();
}
