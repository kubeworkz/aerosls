/* env_console.c — see env_console.h. POSIX-Environments E6. */
#include "env_console.h"
#include "cap.h"
#include "kernel_io.h"

/* One environment's console endpoint. `buf` is that environment's output,
 * never the kernel's: nothing in this file calls kernel_serial_putchar(). */
struct EnvConsole {
    int      used;
    uint32_t partition;
    uint32_t index;      /* identity within the partition (in the names) */
    uint32_t env_id;     /* the manager's id; 0 until env_console_bind_env */
    uint32_t pid;        /* the POSIX sidecar holding the far end */
    uint16_t k_rd;       /* kernel (pid 0) CHAN_R — the environment's output */
    uint16_t k_wr;       /* kernel (pid 0) CHAN_W — the environment's input  */
    uint32_t len;
    uint32_t dropped;
    uint8_t  buf[ENV_CONSOLE_BUF];
};

static struct EnvConsole env_consoles[ENV_CONSOLE_MAX];
/* Shared drain buffer. Single static like the console service's, and for the
 * same reason: this tick runs in one non-IRQ kernel context at a time. Sized
 * to a channel's maximum payload, so a message is never silently truncated. */
static uint8_t env_console_msg[ENV_CONSOLE_BUF];
static uint32_t env_console_drained_total = 0;
static uint32_t env_console_tag = 1;

/* ─── identity ────────────────────────────────────────────────────────────
 * The environment's index is the decimal suffix of the sidecar's own name,
 * `aerosls.posix.<index>`. init builds that name in one place
 * (init/src/env_manager.rs) for both the environment manager and the E3 boot
 * spawn, so both paths register identically and the kernel needs no create
 * window, no extra syscall and no environment table of its own. A name that is
 * not exactly that shape is refused rather than coerced — a console wired to
 * the wrong environment is worse than one that is not wired.
 *
 * The parse is EXPORTED (env_console_name_index) because the BootInfoBlock
 * writer (cap.c) needs the same answer for the same reason: E6 writes the
 * environment's own index into its sidecar's BIB (v3) so the sidecar can
 * announce it on its own console, and the index the sidecar announces must be
 * the index this registry files its console under. One function, two callers,
 * so the two cannot drift. */
#define EC_NAME_PREFIX "aerosls.posix."

int env_console_name_index(const char* name, uint32_t* out_index) {
    if (!name) return 0;
    static const char prefix[] = EC_NAME_PREFIX;
    uint32_t i = 0;
    while (prefix[i]) {
        if (name[i] != prefix[i]) return 0;
        i++;
    }
    if (name[i] < '0' || name[i] > '9') return 0;
    uint32_t v = 0;
    while (name[i] >= '0' && name[i] <= '9') {
        v = v * 10u + (uint32_t)(name[i] - '0');
        i++;
        if (v > 0xFFFFFFu) return 0;   /* absurd index: not ours */
    }
    if (name[i] != '\0') return 0;     /* trailing junk: not exactly our shape */
    *out_index = v;
    return 1;
}

static struct EnvConsole* ec_find(uint32_t partition, uint32_t index) {
    for (uint32_t i = 0; i < ENV_CONSOLE_MAX; i++) {
        struct EnvConsole* e = &env_consoles[i];
        if (e->used && e->partition == partition && e->index == index) return e;
    }
    return 0;
}

/* The control plane addresses an environment by (partition, env_id) — the pair
 * create and destroy speak. An unbound id (0) never matches: "no environment"
 * must not be confusable with "the environment whose id happens to be 0". */
static struct EnvConsole* ec_find_env(uint32_t partition, uint32_t env_id) {
    if (env_id == 0) return 0;
    for (uint32_t i = 0; i < ENV_CONSOLE_MAX; i++) {
        struct EnvConsole* e = &env_consoles[i];
        if (e->used && e->partition == partition && e->env_id == env_id) return e;
    }
    return 0;
}

int env_console_register(uint16_t k_rd, uint16_t k_wr, uint32_t partition,
                         uint32_t pid, const char* name) {
    uint32_t index = 0;
    if (!env_console_name_index(name, &index)) {
        kernel_serial_printf(
            "[ENV-CONSOLE] '%s' (pid %u, partition %u) is not named "
            "'%s<index>' — console left unwired\n",
            name ? name : "(null)", (unsigned)pid, (unsigned)partition,
            EC_NAME_PREFIX);
        return 0;
    }

    /* Re-creating an environment (E5 recycle) re-registers its index: replace
     * the stale entry rather than leaking it, so the registry never fills with
     * consoles of environments that no longer exist. */
    struct EnvConsole* e = ec_find(partition, index);
    if (!e) {
        for (uint32_t i = 0; i < ENV_CONSOLE_MAX; i++) {
            if (!env_consoles[i].used) { e = &env_consoles[i]; break; }
        }
    }
    if (!e) {
        kernel_serial_printf(
            "[ENV-CONSOLE] partition %u index %u: registry full (%d) — "
            "console left unwired\n",
            (unsigned)partition, (unsigned)index, (int)ENV_CONSOLE_MAX);
        return 0;
    }

    e->used = 1;
    e->partition = partition;
    e->index = index;
    e->env_id = 0;   /* bound when the ENV_CREATE reply arrives */
    e->pid = pid;
    e->k_rd = k_rd;
    e->k_wr = k_wr;
    e->len = 0;
    e->dropped = 0;
    kernel_serial_printf(
        "[ENV-CONSOLE] environment (partition %u, index %u) console wired: "
        "posix pid %u, kernel ends rd=%u wr=%u\n",
        (unsigned)partition, (unsigned)index, (unsigned)pid,
        (unsigned)k_rd, (unsigned)k_wr);
    return 1;
}

int env_console_bind_env(uint32_t partition, uint32_t index, uint32_t env_id) {
    struct EnvConsole* e = ec_find(partition, index);
    if (!e || env_id == 0) return 0;
    e->env_id = env_id;
    return 1;
}

int env_console_kernel_slot(uint16_t slot) {
    for (uint32_t i = 0; i < ENV_CONSOLE_MAX; i++) {
        if (env_consoles[i].used && env_consoles[i].k_rd == slot) return 1;
    }
    return 0;
}

/* Resolve a kernel-held slot to its channel and whether pid 0 is its end0 —
 * the same derivation console_service_tick() uses to test for a closed peer. */
static struct CapChannel* ec_chan_of(uint16_t slot, int* out_kdir) {
    uint64_t w = cap_tables[0].slots[slot].word;
    uint32_t obj_id = (uint32_t)((w >> CAP_OBJ_SHIFT) & CAP_OBJ_MASK);
    if (obj_id >= CAP_OBJECT_MAX) return 0;
    struct CapObject* o = &cap_objects[obj_id];
    if (!o->active || o->kind != CAP_OBJ_KIND_CHAN || o->chan_id >= CAP_CHAN_MAX)
        return 0;
    struct CapChannel* ch = &cap_channels[o->chan_id];
    if (out_kdir) *out_kdir = (0 == ch->end0_pid) ? 0 : 1;
    return ch;
}

/* Append output, keeping the NEWEST bytes when the buffer overflows (an
 * operator wants the end of the stream, not its beginning) and counting what
 * fell off the front so a reader can tell "quiet" from "outran the buffer". */
static void ec_push(struct EnvConsole* e, const uint8_t* data, uint32_t n) {
    if (n >= ENV_CONSOLE_BUF) {
        uint32_t skip = n - ENV_CONSOLE_BUF;
        e->dropped += e->len + skip;
        for (uint32_t i = 0; i < ENV_CONSOLE_BUF; i++)
            e->buf[i] = data[skip + i];
        e->len = ENV_CONSOLE_BUF;
        return;
    }
    uint32_t space = ENV_CONSOLE_BUF - e->len;
    if (n > space) {
        uint32_t drop = n - space;
        uint32_t keep = e->len - drop;
        for (uint32_t i = 0; i < keep; i++) e->buf[i] = e->buf[i + drop];
        e->len = keep;
        e->dropped += drop;
    }
    for (uint32_t i = 0; i < n; i++) e->buf[e->len + i] = data[i];
    e->len += n;
}

static void ec_release(struct EnvConsole* e) {
    uint16_t rd = e->k_rd;
    /* Revoke the kernel end of the output channel. The channel is shared with
     * the POSIX sidecar, so this frees the kernel's CHAN_R (and destroys the
     * channel once the far end is gone too) rather than leaking it — the same
     * close-aware teardown console_service_tick() performs. */
    if (rd != 0xFFFFu) cap_revoke(0, rd);
    e->used = 0;
    e->len = 0;
    e->dropped = 0;
    e->env_id = 0;
    e->pid = 0;
    e->k_rd = 0xFFFFu;
    e->k_wr = 0xFFFFu;
}

void env_console_tick(void) {
    for (uint32_t i = 0; i < ENV_CONSOLE_MAX; i++) {
        struct EnvConsole* e = &env_consoles[i];
        if (!e->used) continue;

        /* Buffer this environment's output. Unlike the console service this
         * does NOT print it — that is E6's whole property. */
        for (;;) {
            uint32_t plen = 0, tag = 0, flags = 0;
            uint16_t n_caps = 0;
            int r = cap_recv_msg(0, e->k_rd, env_console_msg,
                                 (uint32_t)sizeof(env_console_msg),
                                 &plen, 0, 0, &n_caps, &tag, &flags);
            if (r != 0) break;
            if (plen > sizeof(env_console_msg)) plen = sizeof(env_console_msg);
            ec_push(e, env_console_msg, plen);
            env_console_drained_total++;
        }

        /* A closed peer ends the console: print the tail it never read (so an
         * environment that dies mid-command is not silent in the kernel log),
         * then release the entry. */
        int kdir = 0;
        struct CapChannel* ch = ec_chan_of(e->k_rd, &kdir);
        int ended = 0;
        if (!ch) {
            /* The kernel end no longer resolves to a channel: the console is
             * already dead (its slot was revoked or the channel destroyed), so
             * there is nothing left to revoke and the entry is retired. */
            ended = 1;
        } else {
            cap_lock(&ch->lock);
            ended = ch->close_evt[kdir];
            cap_unlock(&ch->lock);
        }
        if (ended) {
            if (e->len) {
                kernel_serial_printf(
                    "[ENV-CONSOLE] environment (partition %u, index %u) "
                    "ended with %u buffered byte(s) never attached to\n",
                    (unsigned)e->partition, (unsigned)e->index,
                    (unsigned)e->len);
            }
            if (!ch) e->k_rd = 0xFFFFu;  /* gone: nothing left to revoke */
            ec_release(e);
        }
    }
}

int env_console_write(uint32_t partition, uint32_t env_id,
                      const uint8_t* bytes, uint32_t len) {
    struct EnvConsole* e = ec_find_env(partition, env_id);
    if (!e) return -1;
    if (!bytes || len == 0) return 0;

    /* A line needs its terminator: the POSIX sh applet completes a command only
     * on '\n' (the sidecar echoes, not the kernel). */
    uint8_t line[512];
    uint32_t n = 0;
    for (uint32_t i = 0; i < len && n + 1 < sizeof(line); i++) line[n++] = bytes[i];
    if (n == 0 || line[n - 1] != '\n') {
        if (n + 1 < sizeof(line)) line[n++] = '\n';
    }

    /* Refuse to queue onto a peer that has not drained the previous line: the
     * payload pool is 32 frames (the same starvation console_service avoids),
     * and an environment that never reads its console must not be able to
     * exhaust it on behalf of the others. */
    struct CapChannel* ch = ec_chan_of(e->k_wr, 0);
    if (ch) {
        int dest = (0 == ch->end0_pid) ? 1 : 0;
        cap_lock(&ch->lock);
        int undrained = (ch->qdepth[dest] > 0);
        cap_unlock(&ch->lock);
        if (undrained) return 0;
    }

    uint32_t tag = env_console_tag++;
    if (cap_send_msg(0, e->k_wr, line, n, 0, 0, tag, 0) != 0) {
        /* The peer is gone: retire the console now rather than waiting for the
         * next tick to notice, so the control plane's answer is immediate. */
        ec_release(e);
        return -1;
    }
    return (int)n;
}

int env_console_read(uint32_t partition, uint32_t env_id,
                     uint8_t* out, uint32_t cap, uint32_t* out_len) {
    if (out_len) *out_len = 0;
    struct EnvConsole* e = ec_find_env(partition, env_id);
    if (!e) return 0;

    /* Drain what has arrived since the last read, under no lock held across the
     * copy: the tick is the only writer of buf/len, and it runs in this same
     * non-IRQ kernel context on one core at a time. */
    uint32_t n = e->len < cap ? e->len : cap;
    for (uint32_t i = 0; i < n; i++) out[i] = e->buf[i];
    uint32_t rest = e->len - n;
    for (uint32_t i = 0; i < rest; i++) e->buf[i] = e->buf[n + i];
    e->len = rest;
    if (out_len) *out_len = n;
    return 1;
}

uint32_t env_console_dropped(uint32_t partition, uint32_t env_id) {
    struct EnvConsole* e = ec_find_env(partition, env_id);
    return e ? e->dropped : 0;
}

uint32_t env_console_count(void) {
    uint32_t n = 0;
    for (uint32_t i = 0; i < ENV_CONSOLE_MAX; i++)
        if (env_consoles[i].used) n++;
    return n;
}

int env_console_entry(uint32_t i, uint32_t* out_partition, uint32_t* out_env_id,
                      uint32_t* out_index, uint32_t* out_pid) {
    uint32_t seen = 0;
    for (uint32_t s = 0; s < ENV_CONSOLE_MAX; s++) {
        if (!env_consoles[s].used) continue;
        if (seen == i) {
            if (out_partition) *out_partition = env_consoles[s].partition;
            if (out_env_id) *out_env_id = env_consoles[s].env_id;
            if (out_index) *out_index = env_consoles[s].index;
            if (out_pid) *out_pid = env_consoles[s].pid;
            return 1;
        }
        seen++;
    }
    return 0;
}

uint32_t env_console_drained(void) {
    return env_console_drained_total;
}
