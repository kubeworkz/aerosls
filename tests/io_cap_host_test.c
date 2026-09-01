/*
 * io_cap_host_test.c — Driver SDK ABI v0.1 §4.2: CAP_TYPE_IO with
 * SYS_IO_IN (307) / SYS_IO_OUT (308), driven against the REAL kernel/cap.c
 * and kernel/chan.c (not a reimplementation).
 *
 * The test mints CAP_TYPE_IO words directly into a fake process's cap
 * table (there is no user-facing "create IO cap" syscall yet — IO caps
 * come from the manifest `devices:` section at create_sidecar, which is
 * M2 work), then drives k_io_in / k_io_out through the REAL cap-word
 * validation, rights checks, and range checks. The privileged in/out
 * instructions are behind cap.c's weak cap_io_read/cap_io_write hooks;
 * here they are overridden with a fake 64 KiB port map (a port-indexed
 * byte array) so the test can prove the exact port and width the kernel
 * chose to access.
 *
 * Scenarios:
 *   1. mint a RO io cap (base 0x60, count 8, PERM_R): in works, out is
 *      CAP_ERR_RIGHTS
 *   2. mint a RW io cap (base 0x3F0, count 4, PERM_R|PERM_W): 8/16/32-bit
 *      in and out at the exact ports (base + index), value round-trips
 *   3. range enforcement: index + size > LEN is CAP_ERR_RANGE (including
 *      the 32-bit straddle at the last port)
 *   4. type enforcement: a MEM cap in the slot is CAP_ERR_TYPE
 *   5. revoked slot (word 0) is CAP_ERR_REVOKED; bad slot is CAP_ERR_RANGE
 *   6. bad size (3) is CAP_ERR_RANGE
 *   7. width precision: 8-bit out leaves neighbor bytes untouched
 *
 * Build and run:
 *   gcc -no-pie -std=c11 -Wall -Wextra -I . -I kernel \
 *       -o /tmp/io_cap_host_test \
 *       tests/io_cap_host_test.c kernel/cap.c kernel/chan.c kernel/frame_pool.c
 *   /tmp/io_cap_host_test
 */
#include "kernel/cap.h"
#include "tests/process_host_stubs.h"   /* stack_bottom/stack_top (frame_pool_init reservation bounds) */

#include <stdio.h>
#include <string.h>
#include <stdint.h>

/* ─── Link stubs ──────────────────────────────────────────────────────────── */
char _kernel_image_end[1];
void kernel_serial_print(const char* s) { (void)s; }
void kernel_serial_printf(const char* fmt, ...) { (void)fmt; }
void kernel_serial_putchar(char c) { (void)c; }

/* kernel/timer.c is not linked; chan.c's deadline logic reads
 * kernel_tick_counter directly (timer.h declares it). */
volatile uint64_t kernel_tick_counter = 0;

/* ─── Strong overrides of cap.c's weak hooks ─────────────────────────────── */
static uint32_t g_cur_pid = 0;
uint32_t cap_current_pid(void) { return g_cur_pid; }

/* ─── Link stubs for cap.c's sidecar-spawn path ──────────────────────────
 * cap.c's cap_create_sidecar references process.c / user_paging.c symbols;
 * this test does not spawn sidecars, but linking cap.c whole requires the
 * symbols. Minimal inert stand-ins (the sidecar host test's full versions
 * live in tests/cap_create_sidecar_host_test.c). */
struct ProcessDescriptor proc_table[PROC_MAX];
uint32_t proc_count = 0;
uint32_t alloc_pid(void) { return 901; }
uint64_t alloc_proc_syscall_stack(uint32_t partition_id) {
    (void)partition_id;
    return 0x400000007000ULL;
}
uint64_t user_clone_page_table(void) { return 0x2000; }
void user_map_page(uint64_t* pml4, uint64_t vaddr, uint64_t paddr, uint64_t flags) {
    (void)pml4; (void)vaddr; (void)paddr; (void)flags;
}
int user_map_identity(uint64_t* pml4, uint64_t phys, uint32_t npages,
                      uint64_t flags) {
    (void)pml4; (void)phys; (void)npages; (void)flags;
    return 0;
}

/* ─── Fake port map ──────────────────────────────────────────────────────── */
/* A 64 KiB port space indexed by port number; the real kernel's strong
 * cap_io_read/cap_io_write (arch/x86/user_paging.c) execute privileged
 * in/out — here the hooks record which port and width was accessed so the
 * test can assert the mediated address arithmetic. */
#define PORT_SPACE 65536
static uint8_t g_port_map[PORT_SPACE];
static int    g_reads  = 0;
static int    g_writes = 0;
static uint16_t g_last_port = 0;
static uint8_t  g_last_size = 0;

uint32_t cap_io_read(uint16_t port, uint8_t size) {
    g_reads++; g_last_port = port; g_last_size = size;
    switch (size) {
    case 1: return g_port_map[port];
    case 2: return (uint32_t)g_port_map[port] |
                   ((uint32_t)g_port_map[port + 1] << 8);
    default: return (uint32_t)g_port_map[port] |
                   ((uint32_t)g_port_map[port + 1] << 8) |
                   ((uint32_t)g_port_map[port + 2] << 16) |
                   ((uint32_t)g_port_map[port + 3] << 24);
    }
}

void cap_io_write(uint16_t port, uint8_t size, uint32_t val) {
    g_writes++; g_last_port = port; g_last_size = size;
    switch (size) {
    case 1: g_port_map[port] = (uint8_t)val; break;
    case 2: g_port_map[port] = (uint8_t)val;
            g_port_map[port + 1] = (uint8_t)(val >> 8); break;
    default: g_port_map[port] = (uint8_t)val;
             g_port_map[port + 1] = (uint8_t)(val >> 8);
             g_port_map[port + 2] = (uint8_t)(val >> 16);
             g_port_map[port + 3] = (uint8_t)(val >> 24); break;
    }
}

/* cap_table_index (kernel/cap.c) binds a pid to a cap table on first use;
 * it is not declared in cap.h (only used internally), so declare it here. */
int cap_table_index(uint32_t pid);

/* ─── Cap word construction (mirrors kernel cap word layout) ─────────────── */
static uint64_t io_word(uint16_t base, uint16_t count, uint8_t perm) {
    return ((uint64_t)CAP_TYPE_IO << CAP_TYPE_SHIFT) |
           ((uint64_t)base   << CAP_OBJ_SHIFT) |
           ((uint64_t)perm   << CAP_PERM_SHIFT) |
           ((uint64_t)count  << CAP_LEN_SHIFT);
}

#define CHECK(cond, msg) do { \
    if (!(cond)) { fprintf(stderr, "FAIL: %s\n", msg); return 1; } \
    printf("ok: %s\n", msg); \
} while (0)

/* Mint a cap word into pid's table at `slot` (no user-facing mint path for
 * IO caps yet; write the word directly like cap.c's own slot population). */
static void mint_io(uint32_t pid, uint16_t slot, uint64_t word) {
    int ti = cap_table_index(pid);
    if (ti < 0) { fprintf(stderr, "FATAL: no cap table for pid %u\n", pid); }
    cap_tables[ti].slots[slot].word = word;
}

int main(void) {
    cap_init();
    const uint32_t P = 900;
    g_cur_pid = P;
    memset(g_port_map, 0, sizeof(g_port_map));

    /* ── 1. RO cap: in allowed, out refused ────────────────────────────── */
    uint16_t ro = 5;                    /* slot 5 */
    mint_io(P, ro, io_word(0x60, 8, CAP_PERM_R));
    uint32_t v = 0;
    CHECK(k_io_in(P, ro, 0, 1, &v) == CAP_ERR_OK, "RO in at base+0");
    CHECK(g_last_port == 0x60 && g_last_size == 1, "RO in used port 0x60, width 1");
    CHECK(v == 0, "RO in read the (zeroed) port map");
    CHECK(k_io_out(P, ro, 0, 1, 0xAA) == CAP_ERR_RIGHTS,
          "RO cap refuses out (CAP_ERR_RIGHTS)");

    /* ── 2. RW cap: 8/16/32-bit round-trips at base + index ───────────── */
    uint16_t rw = 7;
    mint_io(P, rw, io_word(0x3F0, 4, CAP_PERM_R | CAP_PERM_W));

    CHECK(k_io_out(P, rw, 0, 1, 0x42) == CAP_ERR_OK, "RW out 8-bit at base+0");
    CHECK(g_last_port == 0x3F0 && g_last_size == 1, "RW out used port 0x3F0, width 1");
    CHECK(g_port_map[0x3F0] == 0x42, "port map byte written");
    v = 0;
    CHECK(k_io_in(P, rw, 0, 1, &v) == CAP_ERR_OK, "RW in 8-bit at base+0");
    CHECK(v == 0x42, "8-bit value round-trips");

    /* 16-bit at index 2 → port 0x3F2 (base+2) */
    CHECK(k_io_out(P, rw, 2, 2, 0xBEEF) == CAP_ERR_OK, "RW out 16-bit at base+2");
    CHECK(g_last_port == 0x3F2 && g_last_size == 2, "16-bit out used port 0x3F2");
    CHECK(g_port_map[0x3F2] == 0xEF && g_port_map[0x3F3] == 0xBE,
          "16-bit written little-endian");
    v = 0;
    CHECK(k_io_in(P, rw, 2, 2, &v) == CAP_ERR_OK, "RW in 16-bit at base+2");
    CHECK(v == 0xBEEF, "16-bit value round-trips");

    /* 32-bit at index 0 → ports 0x3F0..0x3F3 (count 4 covers exactly) */
    CHECK(k_io_out(P, rw, 0, 4, 0xDEADBEEFu) == CAP_ERR_OK,
          "RW out 32-bit at base+0 (exactly fits count 4)");
    v = 0;
    CHECK(k_io_in(P, rw, 0, 4, &v) == CAP_ERR_OK, "RW in 32-bit at base+0");
    CHECK(v == 0xDEADBEEFu, "32-bit value round-trips");

    /* ── 3. Range enforcement ──────────────────────────────────────────── */
    CHECK(k_io_in(P, rw, 4, 1, &v) == CAP_ERR_RANGE, "index == LEN is out of range");
    CHECK(k_io_in(P, rw, 3, 2, &v) == CAP_ERR_RANGE, "16-bit straddle past LEN");
    CHECK(k_io_out(P, rw, 3, 4, 0) == CAP_ERR_RANGE, "32-bit at last port out of range");
    CHECK(k_io_in(P, rw, 100, 1, &v) == CAP_ERR_RANGE, "far index out of range");
    CHECK(k_io_in(P, rw, 0, 3, &v) == CAP_ERR_RANGE, "unsupported width 3 rejected");

    /* ── 4. Type enforcement: a MEM cap in the slot ────────────────────── */
    uint16_t mem_slot = 9;
    mint_io(P, mem_slot, ((uint64_t)CAP_TYPE_MEM << CAP_TYPE_SHIFT) |
                         ((uint64_t)1 << CAP_OBJ_SHIFT) |
                         ((uint64_t)(CAP_PERM_R | CAP_PERM_W) << CAP_PERM_SHIFT));
    CHECK(k_io_in(P, mem_slot, 0, 1, &v) == CAP_ERR_TYPE,
          "MEM cap in the slot is CAP_ERR_TYPE");

    /* ── 5. Revoked / bad slot ─────────────────────────────────────────── */
    uint16_t dead = 11;
    cap_tables[cap_table_index(P)].slots[dead].word = 0;   /* free slot */
    CHECK(k_io_in(P, dead, 0, 1, &v) == CAP_ERR_REVOKED,
          "empty (free) slot is CAP_ERR_REVOKED");
    CHECK(k_io_in(P, 3000, 0, 1, &v) == CAP_ERR_RANGE,
          "slot beyond CAP_TABLE_ENTRIES is CAP_ERR_RANGE");
    CHECK(k_io_in(P, rw, 0, 1, NULL) == CAP_ERR_PROTO,
          "NULL out pointer is CAP_ERR_PROTO");

    /* ── 6. Width precision: 8-bit write leaves neighbors untouched ────── */
    memset(g_port_map, 0, sizeof(g_port_map));
    g_port_map[0x3F0] = 0xFF; g_port_map[0x3F1] = 0xFF;
    CHECK(k_io_out(P, rw, 0, 1, 0x00) == CAP_ERR_OK, "8-bit write at base+0");
    CHECK(g_port_map[0x3F0] == 0x00 && g_port_map[0x3F1] == 0xFF,
          "8-bit write does not clobber the neighbor byte");

    /* ── 7. syscall wrappers (sys_sls_io_in/out) ───────────────────────── */
    struct SLSIoOutRequest oreq;
    memset(&oreq, 0, sizeof(oreq));
    oreq.slot = rw; oreq.index = 1; oreq.size = 1; oreq.value = 0x77;
    CHECK(sys_sls_io_out(&oreq) == CAP_ERR_OK, "sys_sls_io_out wrapper ok");
    struct SLSIoInRequest ireq;
    memset(&ireq, 0, sizeof(ireq));
    ireq.slot = rw; ireq.index = 1; ireq.size = 1;
    CHECK(sys_sls_io_in(&ireq) == CAP_ERR_OK, "sys_sls_io_in wrapper ok");
    CHECK(ireq.value == 0x77, "syscall wrapper returns the value read");
    CHECK(sys_sls_io_in(NULL) == CAP_ERR_PROTO, "NULL request is CAP_ERR_PROTO");
    CHECK(sys_sls_io_out(NULL) == CAP_ERR_PROTO, "NULL request is CAP_ERR_PROTO");

    printf("all io-cap checks passed\n");
    return 0;
}
