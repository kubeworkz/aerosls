/*
 * boot_params.c — see boot_params.h for why boot-time identity exists and
 * why it comes from the command line rather than the NIC's MAC.
 */
#include "boot_params.h"
#include "../arch/x86/multiboot2.h"
#include "../net/consensus.h"

extern void kernel_serial_print(const char* s);
extern void kernel_serial_printf(const char* fmt, ...);

/* Multiboot2 tag type 1 — the boot command line. Not in multiboot2.h
 * because nothing needed it until now; declared here beside its only user
 * rather than widening that header for one constant. */
#define MB2_TAG_CMDLINE 1

/* Multiboot v1 — the Phase 5 boot path (grub `multiboot`; see the v1 arm
 * in boot_params_scan_mb2()). GRUB passes 0x2BADB002 (0x1BADB002 | 1) and
 * the info struct's cmdline field at +16 is valid when flag bit 2 is set. */
#define MB1_MAGIC        0x2BADB002u
#define MB1_FLAG_CMDLINE (1u << 2)

static char  bp_cmdline[BOOT_CMDLINE_MAX] = { 0 };

/* ─── local helpers ────────────────────────────────────────────────────────
 * Freestanding: no libc. Per-file hand-rolled, matching this tree's
 * convention (p_memcpy, sr_streq, wl_strcpy, mh_streq, wc_memcpy). */

static int bp_is_digit(char c) { return c >= '0' && c <= '9'; }

/* Compares `key` against the start of `s`. Returns the number of characters
 * matched, or 0 if `s` does not begin with exactly `key`. */
static int bp_key_matches(const char* s, const char* key) {
    int i = 0;
    while (key[i] != '\0') {
        if (s[i] != key[i]) return 0;
        i++;
    }
    return i;
}

int boot_params_find_uint(const char* cmdline, const char* key, uint32_t* out) {
    if (!cmdline || !key || !out || key[0] == '\0') return 0;

    for (int i = 0; cmdline[i] != '\0'; i++) {
        /* Whole-token match only: start of string, or just past a space.
         * Without this, key "node" would match inside "subnode=3". */
        if (i != 0 && cmdline[i - 1] != ' ') continue;

        int klen = bp_key_matches(&cmdline[i], key);
        if (klen == 0) continue;
        if (cmdline[i + klen] != '=') continue;   /* "nodeid=3" is not "node" */

        const char* v = &cmdline[i + klen + 1];
        if (!bp_is_digit(*v)) return 0;           /* "node=" or "node=x" */

        uint64_t acc = 0;
        int digits = 0;
        while (bp_is_digit(*v)) {
            acc = acc * 10u + (uint64_t)(*v - '0');
            /* Saturate detection rather than wrapping: a value that does not
             * fit is a typo, and 4294967296 must not quietly become 0. */
            if (acc > 0xFFFFFFFFull) return 0;
            digits++;
            v++;
        }
        if (digits == 0) return 0;

        /* Trailing garbage is a rejection, not something to ignore.
         * "node=3x" is a mistake worth surfacing. */
        if (*v != '\0' && *v != ' ') return 0;

        *out = (uint32_t)acc;
        return 1;
    }
    return 0;
}

int boot_params_find_str(const char* cmdline, const char* key,
                         char* out, int cap) {
    if (!cmdline || !key || !out || cap <= 1 || key[0] == '\0') return 0;

    for (int i = 0; cmdline[i] != '\0'; i++) {
        if (i != 0 && cmdline[i - 1] != ' ') continue;      /* whole token */

        int klen = bp_key_matches(&cmdline[i], key);
        if (klen == 0) continue;
        if (cmdline[i + klen] != '=') continue;

        const char* v = &cmdline[i + klen + 1];
        int n = 0;
        while (v[n] != '\0' && v[n] != ' ') n++;
        if (n == 0) return 0;                                /* "key=" */

        /* Refuse rather than truncate. A clipped role name that still
         * parsed would put traffic on the wrong interface silently. */
        if (n > cap - 1) return 0;

        for (int j = 0; j < n; j++) out[j] = v[j];
        out[n] = '\0';
        return 1;
    }
    return 0;
}

void boot_params_scan_mb2(uint32_t mb2_magic, uint32_t mb2_phys) {
    bp_cmdline[0] = '\0';

    /* ── Multiboot v1 path ──────────────────────────────────────────────
     * The Phase 5 grub.cfg boots with `multiboot` (v1) — GRUB 2.12's
     * `module` command (the sidecars.cpio initrd) only works with v1, not
     * `multiboot2` (boot_image.c documents this) — so the boot command
     * line arrives in the v1 info struct, not the v2 tag list below:
     * flags at +0, the cmdline pointer at +16, valid when flag bit 2 is
     * set. Without this arm the line is always empty on real boots and
     * every assignment silently falls back (observed: a two-NIC smoke
     * expecting grub `nic0=both nic1=none` got "no nicN= given"). */
    if (mb2_magic == MB1_MAGIC) {
        if (mb2_phys == 0) return;
        const uint32_t* info1 = (const uint32_t*)(uintptr_t)mb2_phys;
        if (!(info1[0] & MB1_FLAG_CMDLINE)) return;
        const char* src = (const char*)(uintptr_t)info1[4];
        if (src == 0) return;
        uint32_t n = 0;
        while (n < (uint32_t)(BOOT_CMDLINE_MAX - 1) && src[n] != '\0') {
            bp_cmdline[n] = src[n];
            n++;
        }
        bp_cmdline[n] = '\0';
        return;
    }

    if (mb2_magic != (uint32_t)MULTIBOOT2_MAGIC) return;
    if (mb2_phys == 0) return;

    const struct mb2_info* info = (const struct mb2_info*)(uintptr_t)mb2_phys;
    const struct mb2_tag*  tag  = (const struct mb2_tag*)(info + 1);
    const struct mb2_tag*  end  = (const struct mb2_tag*)(
                                  (const uint8_t*)(uintptr_t)mb2_phys + info->total_size);

    while (tag < end && tag->type != MB2_TAG_END) {
        if (tag->type == MB2_TAG_CMDLINE) {
            /* Payload is a NUL-terminated string immediately after the
             * 8-byte tag header. Copy bounded by BOTH the tag's own size and
             * our buffer -- a malformed tag claiming a huge size must not
             * walk off the end of the info block, and an over-long line is
             * truncated rather than overflowing. */
            const char* src = (const char*)tag + sizeof(struct mb2_tag);
            uint32_t avail = (tag->size > sizeof(struct mb2_tag))
                           ? (tag->size - (uint32_t)sizeof(struct mb2_tag)) : 0u;
            if (avail > (uint32_t)(BOOT_CMDLINE_MAX - 1))
                avail = (uint32_t)(BOOT_CMDLINE_MAX - 1);

            uint32_t n = 0;
            while (n < avail && src[n] != '\0') { bp_cmdline[n] = src[n]; n++; }
            bp_cmdline[n] = '\0';
            return;
        }
        tag = mb2_tag_next(tag);
    }
}

const char* boot_params_cmdline(void) { return bp_cmdline; }

uint32_t boot_params_apply_node_identity(void) {
    uint32_t node = 0;

    if (bp_cmdline[0] != '\0')
        kernel_serial_printf("[BOOT] command line: \"%s\"\n", bp_cmdline);

    if (!boot_params_find_uint(bp_cmdline, "node", &node)) {
        /* Distinguish "you did not ask for an identity" from "you asked
         * badly". The first is the normal single-node case and deserves no
         * alarm; the second is a typo that would otherwise look identical. */
        for (int i = 0; bp_cmdline[i] != '\0'; i++) {
            if ((i == 0 || bp_cmdline[i - 1] == ' ') &&
                bp_key_matches(&bp_cmdline[i], "node") == 4 &&
                bp_cmdline[i + 4] == '=') {
                kernel_serial_print(
                    "[BOOT] ERROR: 'node=' present but not a plain unsigned "
                    "number -- staying STANDALONE.\n");
                return 0;
            }
        }
        return 0;   /* no node= at all: standalone, the long-standing default */
    }

    if (node == 0 || node > CLUSTER_NODE_MAX) {
        /* Deliberately NOT clamped. Clamping 9 to 8 on a nine-node launch
         * would give two nodes the same id, and a cluster where two members
         * both answer to 8 is broken in a way that still looks healthy.
         * Staying standalone is visibly wrong instead. */
        kernel_serial_printf(
            "[BOOT] ERROR: node=%u is out of range 1..%u -- staying STANDALONE "
            "rather than clamping, which would risk two nodes sharing an id.\n",
            (unsigned)node, (unsigned)CLUSTER_NODE_MAX);
        return 0;
    }

    if (cluster_init(node) != 0) {
        kernel_serial_printf("[BOOT] ERROR: cluster_init(%u) refused -- STANDALONE.\n",
                             (unsigned)node);
        return 0;
    }

    kernel_serial_printf("[BOOT] node identity %u taken from the command line "
                         "(no 'cluster init' needed).\n", (unsigned)node);
    return node;
}
