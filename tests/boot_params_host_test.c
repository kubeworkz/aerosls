/*
 * boot_params_host_test.c — N-Node Launcher Plan Phase 1: boot-time cluster
 * identity from the multiboot2 command line. Links the REAL
 * kernel/boot_params.c and net/consensus.c.
 *
 * ─── What this has to get right ───────────────────────────────────────────
 * The failure mode being designed against is not "the parser is wrong", it
 * is "the parser is wrong in a way that still boots". Two nodes that both
 * decide they are node 1 form a cluster that looks healthy and silently
 * misroutes everything, so every ambiguous input here must end STANDALONE
 * rather than at a plausible guess:
 *
 *   node=9  with CLUSTER_NODE_MAX 8   -> standalone, NOT clamped to 8
 *   node=3x                           -> standalone, NOT read as 3
 *   node=                             -> standalone
 *   subnode=3                         -> standalone (whole-token matching)
 *
 * Scenario 5 is the one that matters most: it proves the tag walk is bounded
 * by the tag's own size AND the destination buffer, because a malformed
 * cmdline tag is attacker-adjacent input arriving before any of this
 * kernel's defences exist.
 *
 * Build and run:
 *   gcc -Wall -Wextra -std=c11 -D_GNU_SOURCE -I . -I kernel -I net -I arch/x86 \
 *       -o /tmp/boot_params_host_test \
 *       tests/boot_params_host_test.c kernel/boot_params.c net/consensus.c \
 *       kernel/partition.c
 *   /tmp/boot_params_host_test
 */
#include "kernel/boot_params.h"
#include "kernel/partition.h"
#include "kernel/object_catalog.h"
#include "net/consensus.h"
#include "arch/x86/multiboot2.h"
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include <sys/mman.h>

static int checks_passed = 0;
static int checks_failed = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); checks_failed++; } \
    else         { printf("ok:   %s\n", msg); checks_passed++; } \
} while (0)

/* Captures the log so the "said so loudly" claims can actually be checked --
 * a silent wrong answer and a loud wrong answer are different bugs. */
static char g_log[8192];
static int  g_log_len = 0;
void kernel_serial_print(const char* s) {
    while (*s && g_log_len < (int)sizeof(g_log) - 1) g_log[g_log_len++] = *s++;
    g_log[g_log_len] = '\0';
}
void kernel_serial_printf(const char* fmt, ...) {
    va_list ap; va_start(ap, fmt);
    g_log_len += vsnprintf(g_log + g_log_len, sizeof(g_log) - (size_t)g_log_len, fmt, ap);
    va_end(ap);
    if (g_log_len > (int)sizeof(g_log) - 1) g_log_len = (int)sizeof(g_log) - 1;
}
void failover_note_heartbeat(uint32_t node_id, uint64_t now) { (void)node_id; (void)now; }
static void log_reset(void) { g_log_len = 0; g_log[0] = '\0'; }
static int  logged(const char* needle) { return strstr(g_log, needle) != NULL; }

void dspp_partition_announce(uint32_t partition_id, const char* name, uint32_t owner_node_id) { (void)partition_id; (void)name; (void)owner_node_id; }
void dspp_partition_withdraw(uint32_t partition_id) { (void)partition_id; }
void persist_partitions(void) { }
volatile uint64_t kernel_tick_counter = 0;

/* Faithful stubs for consensus.c's own dependencies, matching
 * tests/consensus_phase1_host_test.c. Nothing here exercises the election
 * or lease paths -- cluster_init() only touches in-memory state -- but the
 * symbols must resolve, and a stub that lies would make a later test that
 * DOES exercise them quietly wrong. */
void update_page_table_permissions_globally(uint32_t force_read_only) {
    (void)force_read_only;
}
void update_page_table_permissions_for_partition(uint32_t partition_id,
                                                 uint32_t force_read_only) {
    (void)partition_id; (void)force_read_only;
}
void dspp_transmit_raw(const void* dspp_payload, uint16_t dspp_len) {
    (void)dspp_payload; (void)dspp_len;
}

/* partition.c's dependencies. Each returns what the real one returns over
 * empty tables -- 0 things reclaimed, 0 things killed -- rather than a
 * convenient constant. Scenario 6 only calls partition_init(), which
 * touches none of them, but a stub that lies is a trap for the next test. */
struct SLSObjectEntry object_catalog[CATALOG_MAX_OBJECTS];
uint32_t              object_catalog_count = 0;
uint32_t catalog_vfree_partition(uint32_t p) { (void)p; return 0; }
uint32_t process_kill_partition(uint32_t p)  { (void)p; return 0; }
uint32_t partition_reclaim_all_frames(uint32_t p) { (void)p; return 0; }
int  stream_relocate_partition(uint32_t p, uint32_t d) { (void)p; (void)d; return 0; }
int  stream_migrate_send_partition(uint32_t p, uint32_t d) { (void)p; (void)d; return 0; }
/* Paired with the relocate/send stubs: a test that stands in "nothing to
 * relocate" must also stand in "nothing to count", or partition_migrate()
 * sees 0 sent against a non-zero expectation and aborts every migration.
 * FAITHFUL -- this test registers no streams, so the real function would
 * also return 0. */
int stream_count_for_partition(uint32_t partition_id) { (void)partition_id; return 0; }
uint32_t simi_ctx_migrate_send_partition(uint32_t p, uint32_t d) { (void)p; (void)d; return 0; }
void dspp_service_announce(const char* n, uint32_t p, uint8_t k, uint32_t e,
                           uint32_t u, uint8_t sv) {
    (void)n;(void)p;(void)k;(void)e;(void)u;(void)sv;
}
void dspp_service_withdraw(const char* n) { (void)n; }
uint32_t service_unregister_partition(uint32_t p) { (void)p; return 0; }
void persist_services(void) { }

/* ─── a synthetic multiboot2 info block ────────────────────────────────────
 * Built by hand rather than mocked, so boot_params_scan_mb2() walks real
 * tag structures with real 8-byte alignment padding.
 *
 * The block has to live BELOW 4 GiB. boot_params_scan_mb2() takes a
 * uint32_t, which is correct -- GRUB hands the info pointer over in ebx, a
 * 32-bit register -- but a static array on an x86-64 host lands well above
 * that, and casting its address down to uint32_t silently truncates it into
 * a wild pointer. (Found the direct way: the first run of this test
 * segfaulted.) MAP_32BIT puts it where a real handoff would be, so the
 * signature stays honest to the target instead of being widened to suit
 * the test. */
#define MB2_BUF_SIZE 1024
static uint8_t* mb2_buf = NULL;

static void mb2_buf_init(void) {
    mb2_buf = mmap(NULL, MB2_BUF_SIZE, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_32BIT, -1, 0);
    if (mb2_buf == MAP_FAILED) {
        printf("FAIL: MAP_32BIT unavailable -- cannot place the mb2 block "
               "below 4 GiB on this host\n");
        checks_failed++;
        mb2_buf = NULL;
        return;
    }
    /* Belt and braces: if this ever lands high, fail loudly rather than
     * segfaulting inside the code under test. */
    if ((uintptr_t)mb2_buf > 0xFFFFFFFFull) {
        printf("FAIL: mb2 block at %p is above 4 GiB\n", (void*)mb2_buf);
        checks_failed++;
    }
}

static uint32_t build_mb2(const char* cmdline, uint32_t forced_tag_size) {
    memset(mb2_buf, 0, MB2_BUF_SIZE);
    struct mb2_info* info = (struct mb2_info*)mb2_buf;
    uint8_t* p = mb2_buf + sizeof(struct mb2_info);

    if (cmdline) {
        struct mb2_tag* t = (struct mb2_tag*)p;
        uint32_t len = (uint32_t)strlen(cmdline) + 1;
        t->type = 1;                                  /* MB2_TAG_CMDLINE */
        t->size = forced_tag_size ? forced_tag_size
                                  : (uint32_t)sizeof(struct mb2_tag) + len;
        memcpy(p + sizeof(struct mb2_tag), cmdline, len);
        uint32_t adv = ((uint32_t)sizeof(struct mb2_tag) + len + 7u) & ~7u;
        p += adv;
    }
    struct mb2_tag* endt = (struct mb2_tag*)p;
    endt->type = 0; endt->size = 8;
    p += 8;

    info->total_size = (uint32_t)(p - mb2_buf);
    info->reserved   = 0;
    return (uint32_t)(uintptr_t)mb2_buf;
}

int main(void) {
    printf("=== Phase 1: boot-time node identity (CLUSTER_NODE_MAX=%u) ===\n\n",
           (unsigned)CLUSTER_NODE_MAX);
    mb2_buf_init();
    if (!mb2_buf) { printf("\n=== %d passed, %d failed ===\n", checks_passed, checks_failed); return 1; }

    /* ═══ 1: the parser, in isolation ═════════════════════════════════════ */
    printf("-- 1: boot_params_find_uint --\n");
    {
        uint32_t v = 0xDEAD;
        CHECK(boot_params_find_uint("node=3", "node", &v) && v == 3,
              "a bare key=value parses");
        v = 0xDEAD;
        CHECK(boot_params_find_uint("quiet node=7 debug", "node", &v) && v == 7,
              "a value in the middle of a line parses");
        v = 0xDEAD;
        CHECK(boot_params_find_uint("debug node=12", "node", &v) && v == 12,
              "multi-digit values parse");
        v = 0xDEAD;
        CHECK(boot_params_find_uint("node=0", "node", &v) && v == 0,
              "zero parses (range is the caller's business, not the parser's)");

        /* Whole-token matching. Without it a future "node_role=" parameter
         * would be misread as a malformed "node". */
        v = 0xDEAD;
        CHECK(!boot_params_find_uint("subnode=3", "node", &v),
              "*** 'subnode=3' does NOT match key 'node' ***");
        CHECK(v == 0xDEAD, "...and *out is untouched on failure");
        CHECK(!boot_params_find_uint("nodeid=3", "node", &v),
              "*** 'nodeid=3' does NOT match either -- the '=' must follow ***");
        /* A space where the '=' should be. Mutation testing found that
         * dropping the '=' check survived every other case here, because
         * the digit check happened to reject them anyway -- but not this
         * one: without the '=' requirement, "node 3" parses as 3. It is
         * also the likeliest way to actually mistype the parameter. */
        CHECK(!boot_params_find_uint("node 3", "node", &v),
              "*** 'node 3' is NOT read as node=3 -- a space is not an '=' ***");
        CHECK(!boot_params_find_uint("nodeX3", "node", &v),
              "...nor is any other separator");

        /* Malformed values are rejected, never salvaged. */
        CHECK(!boot_params_find_uint("node=3x", "node", &v),
              "*** 'node=3x' is REJECTED, not read as 3 ***");
        CHECK(!boot_params_find_uint("node=", "node", &v), "'node=' is rejected");
        CHECK(!boot_params_find_uint("node=x", "node", &v), "'node=x' is rejected");
        CHECK(!boot_params_find_uint("node", "node", &v), "'node' with no '=' is rejected");
        CHECK(!boot_params_find_uint("", "node", &v), "an empty line is rejected");
        CHECK(!boot_params_find_uint("other=1", "node", &v), "an absent key is rejected");

        /* Overflow must not wrap: 4294967296 becoming 0 would be a node id
         * of 0, the reserved sentinel. */
        CHECK(!boot_params_find_uint("node=4294967296", "node", &v),
              "*** a value past UINT32_MAX is rejected, not wrapped to 0 ***");
        v = 0xDEAD;
        CHECK(boot_params_find_uint("node=4294967295", "node", &v) && v == 4294967295u,
              "...while UINT32_MAX itself still parses");

        CHECK(!boot_params_find_uint(NULL, "node", &v), "a NULL cmdline is safe");
        CHECK(!boot_params_find_uint("node=1", NULL, &v), "a NULL key is safe");
        CHECK(!boot_params_find_uint("node=1", "node", NULL), "a NULL out is safe");
        CHECK(!boot_params_find_uint("node=1", "", &v), "an empty key matches nothing");
    }

    /* ═══ 1b: boot_params_find_str ════════════════════════════════════════
     * Added for `nicN=mgmt|cluster` (Multi-NIC Phase 4). Same rules as the
     * uint parser, and one that matters more here: an over-long value is
     * REFUSED, never truncated. A clipped role name that still parses as a
     * valid role would put DSPP on the management wire silently. */
    printf("\n-- 1b: boot_params_find_str --\n");
    {
        char v[16];
        CHECK(boot_params_find_str("nic0=cluster", "nic0", v, sizeof(v)) &&
              !strcmp(v, "cluster"), "a bare key=token parses");
        CHECK(boot_params_find_str("node=1 nic1=mgmt quiet", "nic1", v, sizeof(v)) &&
              !strcmp(v, "mgmt"), "a token mid-line parses, stopping at the space");
        CHECK(!boot_params_find_str("nic0=cluster", "nic1", v, sizeof(v)),
              "an absent key is rejected");
        CHECK(!boot_params_find_str("nic0=", "nic0", v, sizeof(v)), "an empty value is rejected");
        CHECK(!boot_params_find_str("xnic0=mgmt", "nic0", v, sizeof(v)),
              "*** whole-token matching: 'xnic0' is not 'nic0' ***");
        CHECK(!boot_params_find_str("nic0mgmt", "nic0", v, sizeof(v)),
              "...and the '=' is required");

        /* THE one. cap 8 leaves room for 7 characters, and "cluster" is
         * exactly 7 -- so a truncating parser would turn "clusterX" into a
         * perfectly valid role name and put traffic on the wrong wire. */
        char small[8];
        CHECK(!boot_params_find_str("nic0=clusterX", "nic0", small, sizeof(small)),
              "*** 'clusterX' is REFUSED, not truncated to the valid 'cluster' ***");
        CHECK(boot_params_find_str("nic0=cluster", "nic0", small, sizeof(small)) &&
              !strcmp(small, "cluster"),
              "...while a value that exactly fits still parses");

        CHECK(!boot_params_find_str(NULL, "nic0", v, sizeof(v)), "a NULL cmdline is safe");
        CHECK(!boot_params_find_str("nic0=mgmt", NULL, v, sizeof(v)), "a NULL key is safe");
        CHECK(!boot_params_find_str("nic0=mgmt", "nic0", NULL, sizeof(v)), "a NULL out is safe");
        CHECK(!boot_params_find_str("nic0=mgmt", "nic0", v, 1), "a cap of 1 holds no token");
    }

    /* ═══ 2: the multiboot2 tag walk ══════════════════════════════════════ */
    printf("\n-- 2: reading the command line out of the tag list --\n");
    {
        uint32_t phys = build_mb2("node=5 quiet", 0);
        boot_params_scan_mb2((uint32_t)MULTIBOOT2_MAGIC, phys);
        CHECK(!strcmp(boot_params_cmdline(), "node=5 quiet"),
              "the cmdline tag is found and copied");

        boot_params_scan_mb2(0xBADBAD, phys);
        CHECK(boot_params_cmdline()[0] == '\0',
              "*** a bad magic yields NO command line, not a stale one ***");

        boot_params_scan_mb2((uint32_t)MULTIBOOT2_MAGIC, 0);
        CHECK(boot_params_cmdline()[0] == '\0', "a null info pointer is safe");

        phys = build_mb2(NULL, 0);
        boot_params_scan_mb2((uint32_t)MULTIBOOT2_MAGIC, phys);
        CHECK(boot_params_cmdline()[0] == '\0',
              "a tag list with no cmdline tag yields an empty string, never NULL");
    }

    /* ═══ 3: identity is applied, and only in range ═══════════════════════ */
    printf("\n-- 3: applying the identity --\n");
    {
        boot_params_scan_mb2((uint32_t)MULTIBOOT2_MAGIC, build_mb2("node=3", 0));
        log_reset();
        CHECK(boot_params_apply_node_identity() == 3, "node=3 is accepted");
        CHECK(cluster_local_node_id() == 3,
              "*** cluster_init() really ran -- the node knows who it is ***");

        /* The whole point: no shell was involved. */
        CHECK(logged("no 'cluster init' needed"),
              "...and says so, since this replaces an interactive step");
    }

    /* ═══ 4: every ambiguous input ends STANDALONE ════════════════════════ */
    printf("\n-- 4: refusing to guess --\n");
    {
        struct { const char* line; const char* what; } bad[] = {
            { "node=9",       "out of range (CLUSTER_NODE_MAX is 8)" },
            { "node=0",       "the reserved uninitialised sentinel" },
            { "node=3x",      "trailing garbage" },
            { "node=",        "no value" },
            { "node=999999",  "far out of range" },
        };
        for (unsigned i = 0; i < sizeof(bad)/sizeof(bad[0]); i++) {
            boot_params_scan_mb2((uint32_t)MULTIBOOT2_MAGIC, build_mb2(bad[i].line, 0));
            log_reset();
            uint32_t got = boot_params_apply_node_identity();
            char msg[160];
            snprintf(msg, sizeof(msg), "'%s' (%s) -> standalone", bad[i].line, bad[i].what);
            CHECK(got == 0, msg);
            CHECK(logged("ERROR"), "...loudly, not silently");
        }

        /* The clamp that must not happen. */
        boot_params_scan_mb2((uint32_t)MULTIBOOT2_MAGIC, build_mb2("node=9", 0));
        log_reset();
        boot_params_apply_node_identity();
        CHECK(logged("rather than clamping"),
              "*** node=9 is NOT clamped to 8 -- two nodes sharing an id is worse ***");

        /* No node= at all is the normal single-instance case and must stay
         * quiet; an operator booting one machine should see no alarm. */
        boot_params_scan_mb2((uint32_t)MULTIBOOT2_MAGIC, build_mb2("quiet debug", 0));
        log_reset();
        CHECK(boot_params_apply_node_identity() == 0, "no node= at all -> standalone");
        CHECK(!logged("ERROR"),
              "*** ...and NO error is logged -- absence is not a mistake ***");

        boot_params_scan_mb2((uint32_t)MULTIBOOT2_MAGIC, build_mb2("node=bogus", 0));
        log_reset();
        boot_params_apply_node_identity();
        CHECK(logged("ERROR"),
              "*** but a malformed node= IS an error -- distinguished from absence ***");
    }

    /* ═══ 5: a malformed tag must not walk off the end ════════════════════ */
    printf("\n-- 5: bounds --\n");
    {
        /* A tag claiming far more payload than the info block contains. The
         * copy must stop at the buffer, not at the tag's claim. */
        uint32_t phys = build_mb2("node=2", 100000);
        boot_params_scan_mb2((uint32_t)MULTIBOOT2_MAGIC, phys);
        CHECK(!strcmp(boot_params_cmdline(), "node=2"),
              "*** an over-long tag size does not run past the NUL ***");

        /* A command line longer than the buffer is truncated, not overflowed.
         * 400 chars into a 256-byte buffer. */
        char big[512];
        memset(big, 'a', sizeof(big)); big[400] = '\0';
        memcpy(big, "node=4 ", 7);
        phys = build_mb2(big, 0);
        boot_params_scan_mb2((uint32_t)MULTIBOOT2_MAGIC, phys);
        CHECK(strlen(boot_params_cmdline()) == BOOT_CMDLINE_MAX - 1,
              "*** an over-long command line is truncated to the buffer ***");
        uint32_t v = 0;
        CHECK(boot_params_find_uint(boot_params_cmdline(), "node", &v) && v == 4,
              "...and a parameter at the front still parses after truncation");
    }

    /* ═══ 6: the ordering contract ════════════════════════════════════════
     * The whole reason identity is resolved where it is. partition_init()
     * stamps PARTITION_SYSTEM's owner from cluster_local_node_id(), so if
     * the call is ever moved below it, partition 0 records owner node 0
     * while the node believes it is node 6 -- and every ownership check
     * downstream reads that split wrongly. A comment cannot catch that
     * being reordered; this can. */
    printf("\n-- 6: identity must be settled BEFORE partition_init() --\n");
    {
        boot_params_scan_mb2((uint32_t)MULTIBOOT2_MAGIC, build_mb2("node=6", 0));
        boot_params_apply_node_identity();      /* the real boot order */
        partition_init();

        CHECK(partition_get_owner_node(PARTITION_SYSTEM) == 6,
              "*** PARTITION_SYSTEM is owned by node 6, the id from the cmdline ***");
        CHECK(partition_is_local(PARTITION_SYSTEM),
              "...and the node correctly sees it as local");

        /* Now demonstrate the bug the ordering prevents, by doing it the
         * wrong way round. This is the regression being guarded against. */
        cluster_init(1);                        /* pretend we are node 1 ... */
        partition_init();                       /* ... stamp happens here ... */
        cluster_init(7);                        /* ... identity changes after */
        CHECK(partition_get_owner_node(PARTITION_SYSTEM) == 1,
              "*** stamping BEFORE identity leaves partition 0 owned by the OLD id ***");
        CHECK(!partition_is_local(PARTITION_SYSTEM),
              "*** ...and the node no longer recognises its own system partition ***");
    }

    printf("\n=== %d passed, %d failed ===\n", checks_passed, checks_failed);
    return checks_failed ? 1 : 0;
}
