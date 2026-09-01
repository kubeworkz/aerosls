/*
 * cap_create_sidecar_host_test.c — Phase 5 verification: the kernel-side
 * sidecar spawner (kernel/cap.c's cap_create_sidecar(), SYS_SLS_CREATE_SIDECAR
 * = 310) and the kernel console service (kernel/console_service.c), driven
 * against the REAL kernel files (not a reimplementation).
 *
 * The test builds a minimal packed sidecar manifest blob in memory — the
 * exact wire format cap_create_sidecar() parses (24-byte header with an
 * IEEE CRC-32 over the record body, then IMAGE/BUDGET/CAP_MEM TLV records,
 * then the 8-byte image_kaddr footer) — calls cap_create_sidecar(), and
 * verifies end to end that the kernel:
 *
 *   1. parses and validates the manifest (magic/version/CRC/record bounds),
 *   2. creates a process: fresh PID, parent linkage, partition/uid
 *      inheritance, HELD→SUSPENDED release, correct entry/RSP/cr3,
 *   3. maps the sidecar image and stack into the child's page table with
 *      the right PTE permissions (code exec-no-write, stack write-noexec,
 *      guard page between them), copying the image bytes into the frames,
 *   4. pre-binds the child's capability table and mints a messenger channel
 *      whose far-end CHAN_R/CHAN_W land in the child's table (same object
 *      as the parent's returned CHAN_R),
 *   5. mints the manifest's CAP_MEM records as MEM caps and wires its
 *      CAP_CHAN records into real channels (each peer resolved against the
 *      sidecar registry; "kernel.*" peers connect to the kernel context),
 *      and
 *   6. writes a BootInfoBlock at the child's stack top whose header and cap
 *      table (messenger caps first, then the named MEM caps and the wired
 *      CHAN_R/CHAN_W pairs with their real slots) are exactly what a
 *      sidecar's _start would parse (user/proto/src/bootinfo.rs), and
 *   7. (section 8) the spawn-to-spawn name→pid link: a sidecar spawned
 *      through cap_create_sidecar registers under its NAME record, and a
 *      LATER manifest's CAP_CHAN peer_name ("drv.ramdisk.0") resolves to
 *      that registry entry — minting R/W endpoints in BOTH real processes'
 *      tables, with nothing planted. Sections 8b/8c then prove the wired
 *      channel CARRIES DATA in BOTH directions: the consumer sends on its
 *      console CHAN_W and the spawned peer receives the payload verbatim
 *      on its CHAN_R, then the peer replies on its CHAN_W and the
 *      consumer receives the reply on its CHAN_R — both queues live, and
 *   8. (section 9) the Phase 5 channel transport syscalls 311-315
 *      (kernel/chan.c, the kabi.rs k_chan_* contract) driven through
 *      their syscall wrappers on that same channel: wait/send/recv round
 *      trip with tag echo, the no-loss undersize rule (BUFSZ + needed,
 *      message NOT consumed), wait timeout on an empty queue, close → the
 *      peer observes a CLOSE event exactly once (then STATE), the
 *      validation/error mapping (TYPE/PROTO/RANGE/STATE), and cap_info
 *      on both a MEM and a CHAN cap, and
 *   9. (section 10) peer-death close events: cap_table_teardown marks every
 *      channel the dying sidecar was an end of with CLOSE_PEER_DEAD
 *      (detail = the dead pid) and drops its registry entry, and the
 *      surviving peer's k_chan_wait/recv observe the death exactly once —
 *      then a send after the peer died fails STATE, and
 *  10. (section 11) the kernel console service is close-aware: a live
 *      child's console channel is drained without revoke, but after the
 *      child's death the next console_service_tick revokes the kernel end
 *      (freeing the kernel's CHAN_R/CHAN_W) instead of leaving the
 *      channel open in the kernel context table forever, and
 *  11. (section 12) the wait-aware park: k_chan_wait with TIMEOUT_NONE on
 *      an empty queue requests the park (cap_wait_chans) with the
 *      RESOLVED channel ids and the request pointer — the real kernel
 *      (process.c) parks the process there and re-runs the wait on wake.
 *      The transport half proven here: an enqueue (cap_send_msg's wake),
 *      an explicit k_chan_close, and a peer-death teardown each fire
 *      cap_wake_chan with the channel id, and the re-poll after each wake
 *      returns the MSG / CLOSE event (CLOSE_PEER_DEAD with the dead pid),
 *      and
 *  12. (section 13) the blocking k_chan_send: a queue-full send with
 *      timeout_ns == 0 requests the park (cap_wait_chans) with the
 *      channel id, the request pointer, and park_syscall =
 *      SYS_SLS_CHAN_SEND — the real kernel parks the sender there and a
 *      recv freeing a slot (cap_recv_msg's wake) re-runs it to enqueue.
 *      The transport half proven here: the park request, that a finite
 *      timeout skips it, that a dequeue fires cap_wake_chan with the
 *      channel id and the re-run enqueues, and that a close wakes the
 *      parked sender whose re-run then fails CAP_ERR_STATE, and
 *  13. (section 14) finite deadlines: a park carries an ABSOLUTE deadline
 *      in ticks (rounded up to one ~10 ms KERNEL_TICK_NS tick); a re-run
 *      takes the ORIGINAL deadline (cap_park_deadline_take) so re-parks
 *      never extend it, an elapsed deadline returns CAP_ERR_TIMEOUT
 *      without re-parking, and a ready queue beats an expired deadline.
 *      The test advances a test-owned kernel_tick_counter to drive the
 *      real expiry/re-park logic in chan.c (the timer ISR wake that
 *      triggers the re-run lives in process.c, out of this link set), and
 *  14. (section 15) SYS_SLS_CREATE_SIDECAR (310) through its FULL syscall
 *      wrapper — the path the init sidecar actually calls (kabi.rs
 *      k_create_sidecar → RealKernel::create_sidecar): the wrapper fills
 *      the request's out_ch_r / out_ch_w with the parent's messenger
 *      CHAN_R and CHAN_W (both valid caps wrapping the same channel
 *      object), the spawned sidecar registers under its manifest name,
 *      and the messenger CARRIES DATA — the parent sends over out_ch_w
 *      and the child receives the payload verbatim on its CHAN_R.
 *
 * The transport returns the POSITIVE CAP_ERR_* codes (0 = CAP_ERR_OK)
 * from kernel/cap.h, distinct from the Phase-3 negative CAP_E* codes.
 *
 * What is faked, and why (same "fake the address-space-shaped primitive,
 * keep the subsystem under test real" precedent as the stream/frame-quota
 * host tests): cap_create_sidecar DEREFERENCES the frames it allocates
 * (copies image bytes into them, writes the BIB into the stack frame) and
 * walks the child's page table as raw host pointers, so
 * allocate_physical_ram_frame_for_partition() and
 * user_clone_page_table()/user_map_page() are stubbed with real host memory
 * instead of frame_pool.c's physical-address-shaped frames (0x1000-shaped
 * pointers are not valid addresses in a host process). kernel/frame_pool.c
 * is therefore NOT linked; cap.c's two frame-pool dependencies are stubbed
 * (frame_pool_reserve_contiguous for cap_init's arena carve, and the frame
 * allocator). Everything else — the cap words, tables, holders, channels,
 * CRC, manifest walk, BIB construction, page-mapping loop, process release —
 * is the real kernel code.
 *
 * -no-pie is load-bearing, exactly as in cap_lifecycle_host_test.c:
 * cap_create_mem() rejects ranges overlapping the kernel image
 * [0x100000, _kernel_image_end). In a PIE host binary that symbol sits at a
 * 47-bit address, so the manifest's 512 MiB MEM base would look like it
 * overlaps the image and every MEM cap would be silently skipped; -no-pie
 * puts the symbol at ~0x40xxxx, below the 512 MiB base, so the caps mint.
 *
 * Build and run:
 *   gcc -no-pie -std=c11 -Wall -Wextra -I . -I kernel \
 *       -o /tmp/cap_create_sidecar_host_test \
 *       tests/cap_create_sidecar_host_test.c kernel/cap.c kernel/chan.c \
 *           kernel/console_service.c
 *   /tmp/cap_create_sidecar_host_test
 */
#include "kernel/cap.h"
#include "tests/process_host_stubs.h"   /* per_cpu_data (weak), etc. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* ─── Link stubs ──────────────────────────────────────────────────────────── */
char _kernel_image_end[1];
void kernel_serial_print(const char* s) { (void)s; }
void kernel_serial_printf(const char* fmt, ...) { (void)fmt; }

/* console_service.c polls serial input via serial_console_poll()
 * (kernel_io.c, not linked here). Stub it to "nothing ready" so the
 * service links; the test drives the console via console_service_tick()
 * with an empty input buffer anyway. */
int serial_console_poll(char* out, size_t cap) { (void)out; (void)cap; return 0; }

/* kernel/timer.c is not linked; the Phase 5 deadline logic in chan.c reads
 * kernel_tick_counter directly (timer.h declares it). Test-owned so the
 * deadline tests can advance time deterministically. */
volatile uint64_t kernel_tick_counter = 0;

/* Serial capture for the console-service assertions: the kernel console
 * service (kernel/console_service.c) writes sidecar console payloads via
 * kernel_serial_putchar(). Capture those bytes here instead of touching
 * a UART. */
static char   g_serial_cap[512];
static size_t g_serial_cap_len = 0;
void kernel_serial_putchar(char c) {
    if (g_serial_cap_len < sizeof(g_serial_cap) - 1)
        g_serial_cap[g_serial_cap_len++] = c;
}

/* ─── Strong overrides of cap.c's weak hooks ─────────────────────────────── */
static uint32_t g_cur_pid = 0;
uint32_t cap_current_pid(void) { return g_cur_pid; }

/* Phase 5 wait-aware park (sections 12-14): cap.c's weak cap_wait_chans is
 * overridden so the test can prove k_chan_wait (empty queue) and
 * k_chan_send (queue full) REQUEST the park with the right data. There is
 * no scheduler in the host test (process.c is not linked), so the override
 * records what it was asked to park on — the resolved channel ids, the
 * request pointer, the syscall the resume would re-run (SYS_SLS_CHAN_WAIT
 * or SYS_SLS_CHAN_SEND), and the absolute deadline ticks (0 = forever) —
 * and returns 0 ("could not park"); the caller then degrades to
 * CAP_ERR_TIMEOUT exactly like the weak default. The real kernel's strong
 * override (process.c) parks the process on the list and re-runs
 * `park_syscall` on wake; this recording is the transport-side proof of
 * the request. */
static int      g_wait_park_calls = 0;
static uint32_t g_wait_chans[CHAN_WAIT_MAX_CHANS];
static uint32_t g_wait_n = 0;
static void*    g_wait_req = 0;
static uint32_t g_wait_syscall = 0;
static uint64_t g_wait_deadline = 0;   /* also the "stored" deadline (see take) */
int cap_wait_chans(const uint32_t* chan_ids, uint32_t n, void* req,
                   uint32_t park_syscall, uint64_t deadline_ticks) {
    g_wait_park_calls++;
    g_wait_n = n;
    g_wait_req = req;
    g_wait_syscall = park_syscall;
    g_wait_deadline = deadline_ticks;
    for (uint32_t i = 0; i < n && i < CHAN_WAIT_MAX_CHANS; i++)
        g_wait_chans[i] = chan_ids[i];
    return 0;   /* no scheduler in the host test: caller returns CAP_ERR_TIMEOUT */
}

/* Phase 5 deadline take (sections 12-14): mimics the real kernel's
 * cap_park_deadline_take (process.c) — return the process's stored park
 * deadline and clear it. Here the "stored" value is what the last
 * cap_wait_chans call recorded, so a RE-RUN call (simulating the resume
 * after a wake) takes the ORIGINAL deadline and the syscall's
 * expired/re-park logic is exercised for real. Reset g_wait_deadline = 0
 * before a fresh first call. */
uint64_t cap_park_deadline_take(void) {
    uint64_t d = g_wait_deadline;
    g_wait_deadline = 0;
    return d;
}

/* Phase 5 wake triggers (section 12): cap.c's weak no-op is overridden so
 * the test can assert that an enqueue (cap_send_msg), an explicit
 * k_chan_close, and a peer-death teardown each fire cap_wake_chan with the
 * channel id — the real kernel's wake makes a parked k_chan_wait re-run
 * and observe the message/event. */
static int      g_wake_calls = 0;
static uint32_t g_wake_chan = 0;
void cap_wake_chan(uint32_t chan_id) {
    g_wake_calls++;
    g_wake_chan = chan_id;
}

/* ─── Process-table pieces cap_create_sidecar reaches for ────────────────── */
/* proc_table/proc_count/alloc_pid normally live in process.c; here they are
 * test-owned so the parent process can be planted directly. */
struct ProcessDescriptor proc_table[PROC_MAX];
uint32_t proc_count = 0;

/* Mirrors the real alloc_pid(): a fresh pid with no collision against
 * active procs (the parent is 100, so the first child is 101). */
/* Real death path for this host test: the kernel's process_exit() calls
 * cap_table_teardown() AND frees the process descriptor slot. cap_table_
 * teardown() alone (cap.c) drops the cap table and registry entry but
 * leaves proc_table[i].active set, so a test that spawns more than
 * PROC_MAX sidecars would hit CAP_ETABLEFULL even after killing some.
 * Mirror the full death path: teardown, then free the slot. */
static void kill_sidecar(uint32_t pid) {
    cap_table_teardown(pid);
    for (int i = 0; i < PROC_MAX; i++) {
        if (proc_table[i].active && proc_table[i].pid == pid) {
            proc_table[i].active = 0;
            if (proc_count > 0) proc_count--;
            break;
        }
    }
}

uint32_t alloc_pid(void) {
    uint32_t best = 0;
    for (int i = 0; i < PROC_MAX; i++)
        if (proc_table[i].active && proc_table[i].pid > best)
            best = proc_table[i].pid;
    return best + 1;
}

/* cap.c allocates the child's dedicated syscall stack via
 * alloc_proc_syscall_stack (normally process.c, whose contiguous-frame
 * allocator the host test does not link). Stub: a fixed non-zero top above
 * the child's image/stack; the host test never schedules the child, so the
 * value only has to be non-zero and stable for assertions. */
uint64_t alloc_proc_syscall_stack(uint32_t partition_id) {
    (void)partition_id;
    return 0x400000007000ULL;
}

/* ─── Frame stubs: real host memory (see file header) ──────────────────────
 * malloc + manual 4 KiB alignment instead of aligned_alloc() (C11, absent
 * from some libcs). The slack per frame is wasted on purpose — nothing in
 * this test is ever freed — and the raw pointers are kept only so the
 * alignment base is not lost. */
static uint8_t* host_align_4096(size_t bytes) {
    uint8_t* raw = malloc(bytes + 4096);
    if (!raw) return 0;
    return (uint8_t*)(((uintptr_t)raw + 4095u) & ~(uintptr_t)4095u);
}

uint64_t frame_pool_reserve_contiguous(uint64_t nframes, uint64_t align_frames) {
    (void)align_frames;
    static uint8_t* arena;
    if (!arena) {
        arena = host_align_4096((size_t)nframes * 4096u);
        if (!arena) return 0;
        memset(arena, 0, (size_t)nframes * 4096u);
    }
    return (uint64_t)(uintptr_t)arena;
}

#define FRAME_POOL_MAX 256   /* 6 frames per spawn (2 image + 4 stack); the test spawns 14+ sidecars and never frees */
static uint8_t* g_frame_pool[FRAME_POOL_MAX];
static int g_frame_count = 0;

void* allocate_physical_ram_frame_for_partition(uint32_t partition_id) {
    (void)partition_id;
    if (g_frame_count >= FRAME_POOL_MAX) return 0;
    uint8_t* raw = malloc(4096 + 4096);
    if (!raw) return 0;
    g_frame_pool[g_frame_count++] = raw;   /* keep the raw base */
    uint8_t* p = (uint8_t*)(((uintptr_t)raw + 4095u) & ~(uintptr_t)4095u);
    memset(p, 0, 4096);
    return p;
}

/* ─── Fake page table: a real 4-level structure in host memory ─────────────
 * user_clone_page_table() returns a zeroed 512-entry PML4; user_map_page()
 * allocates missing intermediate tables and installs a leaf PTE exactly the
 * way arch/x86/user_paging.c's get_or_alloc() does. cap_create_sidecar
 * walks these as raw pointers to find the BIB's frame and the test walks
 * them to read the child's mapped memory. */
static uint64_t* g_pml4 = 0;

/* Page-table buffers MUST be 4 KiB-aligned: the kernel stores their
 * addresses in PTE frame fields and reads them back with
 * USER_PTE_FRAME_MASK, which zeroes the low 12 bits. calloc() only
 * guarantees 16-byte alignment, so the unaligned low bits would leak into
 * the entry as bogus flags and every walk would read from a shifted base
 * (observed: pml4e flags like 0x2d7, BIB copy landing on garbage). Use the
 * same manual 4 KiB alignment the frame pool below uses. */
static uint64_t* pml4_child(uint64_t* parent, size_t idx) {
    if (parent[idx] & USER_PTE_PRESENT)
        return (uint64_t*)(uintptr_t)(parent[idx] & USER_PTE_FRAME_MASK);
    uint64_t* child = (uint64_t*)host_align_4096(4096);
    if (!child) return 0;
    memset(child, 0, 4096);
    parent[idx] = (uint64_t)(uintptr_t)child
                | USER_PTE_PRESENT | USER_PTE_WRITE | USER_PTE_USER;
    return child;
}

uint64_t user_clone_page_table(void) {
    g_pml4 = (uint64_t*)host_align_4096(4096);
    if (!g_pml4) return 0;
    memset(g_pml4, 0, 4096);
    return (uint64_t)(uintptr_t)g_pml4;
}

void user_map_page(uint64_t* pml4, uint64_t vaddr, uint64_t paddr, uint64_t flags) {
    uint64_t* pdpt = pml4_child(pml4, (vaddr >> 39) & 0x1FF);
    uint64_t* pd   = pml4_child(pdpt, (vaddr >> 30) & 0x1FF);
    uint64_t* pt   = pml4_child(pd,   (vaddr >> 21) & 0x1FF);
    pt[(vaddr >> 12) & 0x1FF] = (paddr & USER_PTE_FRAME_MASK) | flags;
}

/* The host test's PML4s are fresh/zeroed (no shared kernel identity map),
 * so identity mapping here is exactly user_map_page at the same address. */
int user_map_identity(uint64_t* pml4, uint64_t phys, uint32_t npages,
                      uint64_t flags) {
    for (uint32_t p = 0; p < npages; p++)
        user_map_page(pml4, phys + (uint64_t)p * 4096, phys + (uint64_t)p * 4096,
                      flags);
    return 0;
}

/* Walk the last-cloned PML4 the way cap_create_sidecar's BIB walk does. */
static uint64_t leaf_pte(uint64_t vaddr) {
    uint64_t pml4e = g_pml4[(vaddr >> 39) & 0x1FF];
    if (!(pml4e & USER_PTE_PRESENT)) return 0;
    uint64_t* pdpt = (uint64_t*)(uintptr_t)(pml4e & USER_PTE_FRAME_MASK);
    uint64_t pdpe = pdpt[(vaddr >> 30) & 0x1FF];
    if (!(pdpe & USER_PTE_PRESENT)) return 0;
    uint64_t* pd = (uint64_t*)(uintptr_t)(pdpe & USER_PTE_FRAME_MASK);
    uint64_t pde = pd[(vaddr >> 21) & 0x1FF];
    if (!(pde & USER_PTE_PRESENT)) return 0;
    uint64_t* pt = (uint64_t*)(uintptr_t)(pde & USER_PTE_FRAME_MASK);
    return pt[(vaddr >> 12) & 0x1FF];
}

static uint8_t* host_ptr(uint64_t vaddr) {
    uint64_t pte = leaf_pte(vaddr);
    return pte ? (uint8_t*)(uintptr_t)(pte & USER_PTE_FRAME_MASK) : 0;
}

/* ─── Harness ─────────────────────────────────────────────────────────────── */
static int g_fail = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); g_fail++; } \
    else         { printf("ok:   %s\n", msg); } \
} while (0)

static uint16_t le16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t le32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}
static uint64_t le64(const uint8_t* p) {
    uint64_t v = 0;
    for (int i = 7; i >= 0; i--) v = (v << 8) | p[i];
    return v;
}

static uint32_t slot_type(uint64_t w) {
    return (uint32_t)((w >> CAP_TYPE_SHIFT) & CAP_TYPE_MASK);
}
static uint32_t slot_state(uint64_t w) {
    return (uint32_t)((w >> CAP_STATE_SHIFT) & CAP_STATE_MASK);
}
static int slot_valid(uint64_t w) {
    return slot_type(w) != CAP_TYPE_NONE && slot_state(w) == CAP_STATE_VALID &&
           !(w & CAP_RSVD_MASK);
}

/* Table index for pid, mirroring cap.c's lookup via .pid. */
static int table_for_pid(uint32_t pid) {
    for (int i = 0; i < CAP_TABLE_MAX; i++)
        if (cap_tables[i].pid == pid) return i;
    return -1;
}

/* ─── Packed manifest blob builder (the kernel's wire format) ────────────── */
#define BLOB_MAX 4096
struct Blob {
    uint8_t  data[BLOB_MAX];
    uint32_t len;
    uint32_t rec_off[8];   /* payload offset of each record (for patching) */
    uint16_t rec_count;
    uint32_t footer_off;   /* offset of the image_kaddr footer */
};

static void blob_put(struct Blob* b, const void* src, uint32_t n) {
    memcpy(b->data + b->len, src, n);
    b->len += n;
}
static void blob_u16(struct Blob* b, uint16_t v) { blob_put(b, &v, 2); }
static void blob_u32(struct Blob* b, uint32_t v) { blob_put(b, &v, 4); }
static void blob_u64(struct Blob* b, uint64_t v) { blob_put(b, &v, 8); }

static void blob_record(struct Blob* b, uint16_t tag, uint16_t payload_len) {
    b->rec_off[b->rec_count] = b->len + 4;   /* payload starts after the hdr */
    b->rec_count++;
    blob_u16(b, tag);
    blob_u16(b, payload_len);
}

/* IEEE CRC-32 (reflected, poly 0xEDB88320) — the same algorithm as
 * cap_sidecar_crc32() in kernel/cap.c and crc32() in user/proto/src/manifest.rs. */
static uint32_t crc32(const uint8_t* data, uint32_t len) {
    uint32_t crc = 0xFFFFFFFFu;
    for (uint32_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++)
            crc = (crc >> 1) ^ (0xEDB88320u & (-(int)(crc & 1)));
    }
    return crc ^ 0xFFFFFFFFu;
}

/* Recompute total_len + body CRC after the body bytes were patched in place. */
static void blob_refinish(struct Blob* b) {
    uint32_t total = b->len;
    memcpy(b->data + 16, &total, 4);
    uint32_t crc = crc32(b->data + SIDECAR_MANIFEST_HEADER_LEN,
                         b->len - SIDECAR_MANIFEST_HEADER_LEN);
    memcpy(b->data + 20, &crc, 4);
}

/* Manifest values used across the test (see cap_create_sidecar's arithmetic
 * for how they drive the layout). */
#define IMAGE_VBASE   0x400000000000ULL   /* USER_PROC_CODE_BASE */
#define IMAGE_SIZE    8192u               /* 2 pages */
#define IMAGE_ENTRY   0u
#define STACK_BYTES   16384u              /* 4 pages */
#define BUDGET_MEM    0x100000u           /* 1 MiB */
#define MEM_BUDGET_BASE 0x20000000ULL     /* 512 MiB — above image end (see -no-pie note) */
#define MEM_BUDGET_PAGES 64u
#define MEM_DMA_BASE    0x20400000ULL

#define MEM_DMA_PAGES   256u
/* The CAP_MEM record carries size in BYTES (Phase 2 §2.2); the kernel
 * converts to whole pages when minting. */
#define MEM_BUDGET_BYTES (MEM_BUDGET_PAGES * 4096u)  /* 262144 */
#define MEM_DMA_BYTES    (MEM_DMA_PAGES * 4096u)     /* 1048576 */

/* Derived from cap_create_sidecar's mapping arithmetic (image 2 pages, then
 * a guard page, then 4 stack pages):
 *   stack_base = IMAGE_VBASE + 2*4096 + 4096            = 0x400000003000
 *   user_rsp   = stack_base + 4*4096 - 16               = 0x400000006FF0
 *   bib_vaddr  = stack_base + 4*4096 - 4096 (top page)  = 0x400000006000
 *   bib stack_top = user_rsp + 16                       = 0x400000007000 */
#define STACK_BASE    0x400000003000ULL
#define EXPECTED_RSP  0x400000006FF0ULL
#define BIB_VADDR     0x400000006000ULL
#define BIB_STACK_TOP 0x400000007000ULL

/* IMAGE + BUDGET + the two MEM records — the preamble every manifest in
 * this test shares. IMAGE and BUDGET stay records 0 and 1 so the
 * error-path patchers (rec_off[0]/rec_off[1]) keep working. */
static void blob_preamble(struct Blob* b) {
    /* Every spawn gets its OWN budget region: cap_create_mem() treats
     * physical regions as exclusive, so a manifest referencing a base that
     * an earlier spawn already claimed would silently skip the mint (and
     * the BIB would be short). A monotonic counter guarantees uniqueness
     * no matter how many sidecars the test spawns. */
    static uint64_t s_mem_off = 0;
    uint64_t base_off = s_mem_off;
    s_mem_off += 0x2000000ULL;   /* 32 MiB per spawn */
    /* The 24-byte manifest header is written by blob_finish() — reserve
     * its space up front so the TLV records (which the kernel parses from
     * offset 24, matching user/proto/src/manifest.rs's wire format) start
     * AFTER the header. Without this the first record's tag/len get
     * overwritten by the header and the parse loop sees tag 0x0000. */
    b->len = SIDECAR_MANIFEST_HEADER_LEN;
    blob_record(b, SIDECAR_TAG_IMAGE, 24);
    blob_u64(b, IMAGE_ENTRY);      /* entry offset */
    blob_u32(b, 0);                /* blob_offset (unused in this path) */
    blob_u32(b, IMAGE_SIZE);
    blob_u64(b, 0);                /* image_kaddr (footer supplies it) */

    blob_record(b, SIDECAR_TAG_BUDGET, 16);
    blob_u64(b, BUDGET_MEM);
    blob_u32(b, STACK_BYTES);
    blob_u32(b, 0x10000);          /* heap_init */

    /* CAP_MEM "budget": name_len u16, name, phys_base u64, size u64
     * (bytes), rights u8 = nlen + 19 bytes — the Phase 2 §2.2 layout the
     * kernel parser and user/proto/src/manifest.rs share. The base is
     * shifted by `base_off` so every spawned sidecar references its OWN
     * physical region: cap_create_mem() rejects overlapping MEM objects
     * (physical regions are exclusive), so reusing the same bases for
     * several spawns would silently skip the later mints. */
    blob_record(b, SIDECAR_TAG_CAP_MEM, 6 + 19);
    blob_u16(b, 6);
    blob_put(b, "budget", 6);
    blob_u64(b, MEM_BUDGET_BASE + base_off);
    blob_u64(b, MEM_BUDGET_BYTES);
    blob_put(b, "\x03", 1);

    blob_record(b, SIDECAR_TAG_CAP_MEM, 3 + 19);
    blob_u16(b, 3);
    blob_put(b, "dma", 3);
    blob_u64(b, MEM_DMA_BASE + base_off);
    blob_u64(b, MEM_DMA_BYTES);
    blob_put(b, "\x03", 1);
}

/* One NAME record (name_len u16 + name) — the sidecar's identity. The
 * kernel registers it in its sidecar registry so later manifests can wire
 * channels to this sidecar by name. */
static void blob_name(struct Blob* b, const char* name) {
    uint16_t nl = (uint16_t)strlen(name);
    blob_record(b, SIDECAR_TAG_NAME, (uint16_t)(2 + nl));
    blob_u16(b, nl);
    blob_put(b, name, nl);
}

/* Append one CAP_CHAN record: name_len u16 + name + peer_len u16 + peer +
 * rights u8 + flags u8 (the Phase 2 §2.2 layout). */
static void blob_chan(struct Blob* b, const char* name, const char* peer) {
    uint16_t nl = (uint16_t)strlen(name);
    uint16_t pl = (uint16_t)strlen(peer);
    blob_record(b, SIDECAR_TAG_CAP_CHAN, (uint16_t)(2 + nl + 2 + pl + 2));
    blob_u16(b, nl);
    blob_put(b, name, nl);
    blob_u16(b, pl);
    blob_put(b, peer, pl);
    blob_put(b, "\x07", 1);   /* rights R|W */
    blob_put(b, "\x00", 1);   /* flags */
}

/* Footer (image_kaddr) + header — call after all TLV records. The footer
 * is the physical address of the image data the kernel copies into the
 * child's frames (appended after all TLV records; the kernel reads it as
 * the first 8 bytes past the walk). */
static void blob_finish(struct Blob* b, uint64_t image_kaddr) {
    b->footer_off = b->len;
    blob_u64(b, image_kaddr);

    /* Header. */
    memcpy(b->data, SIDECAR_MANIFEST_MAGIC, 8);
    uint16_t vmaj = SIDECAR_MANIFEST_VERSION_MAJOR, vmin = 0;
    uint16_t recs = b->rec_count;
    uint16_t flags = 0;
    memcpy(b->data + 8, &vmaj, 2);
    memcpy(b->data + 10, &vmin, 2);
    memcpy(b->data + 12, &recs, 2);
    memcpy(b->data + 14, &flags, 2);
    blob_refinish(b);
}

/* Build the standard valid manifest: IMAGE + BUDGET + CAP_MEM "budget" +
 * CAP_MEM "dma" + NAME "drv.child.0" + CAP_CHAN "peer0" → "drv.peer.0"
 * (a peer planted in the registry below) + CAP_CHAN "console" →
 * "kernel.debug.console" (a kernel service) + the image_kaddr footer.
 * `image_kaddr` must be a host pointer to IMAGE_SIZE bytes (≥ 1 MiB, so
 * the kernel's validity check passes). When `with_unknown` is set, an
 * extra unknown-tag record is appended before the footer (flags=0, so the
 * parse must refuse it). */
static void build_valid_blob(struct Blob* b, uint64_t image_kaddr, int with_unknown) {
    memset(b, 0, sizeof(*b));
    blob_preamble(b);
    blob_name(b, "drv.child.0");
    blob_chan(b, "peer0", "drv.peer.0");
    blob_chan(b, "console", "kernel.debug.console");
    if (with_unknown) {
        blob_record(b, 0x1234, 1);         /* unknown tag, tolerate flag clear */
        blob_put(b, "\xAB", 1);
    }
    blob_finish(b, image_kaddr);
}

/* A service-sidecar manifest: NAME + preamble, no channels. Its NAME is
 * what a later consumer's CAP_CHAN resolves against — the spawn-to-spawn
 * name→pid link (init spawns the ramdisk, then a consumer whose console
 * peer names it). */
static void build_peer_blob(struct Blob* b, uint64_t image_kaddr, const char* name) {
    memset(b, 0, sizeof(*b));
    blob_preamble(b);
    blob_name(b, name);
    blob_finish(b, image_kaddr);
}

/* A consumer manifest: NAME + preamble + ONE CAP_CHAN named "console"
 * whose peer is a sidecar NAME. The kernel resolves that name through the
 * sidecar registry (not a "kernel.*" service), so the channel is minted
 * between two real spawned processes. */
static void build_consumer_blob(struct Blob* b, uint64_t image_kaddr,
                                const char* my_name, const char* peer_name) {
    memset(b, 0, sizeof(*b));
    blob_preamble(b);
    blob_name(b, my_name);
    blob_chan(b, "console", peer_name);
    blob_finish(b, image_kaddr);
}

/* ─── BootInfoBlock reader (mirrors user/proto/src/bootinfo.rs) ───────────── */
struct BibCap {
    char     name[64];
    uint16_t slot;
    uint16_t ty;
    uint16_t rights;
    uint64_t base;
    uint64_t len;
};
struct Bib {
    uint16_t version;
    uint16_t cap_count;
    uint64_t budget_bytes;
    uint64_t stack_top;
    uint32_t total_len;
    struct BibCap caps[SIDECAR_BIB_CAPS_MAX];
};

static struct Bib bib_parse(const uint8_t* p) {
    struct Bib b;
    memset(&b, 0, sizeof(b));
    b.version = le16(p + 8);
    b.cap_count = le16(p + 10);
    b.budget_bytes = le64(p + 12);
    b.stack_top = le64(p + 20);
    b.total_len = le32(p + 28);
    uint32_t off = 32;
    for (uint16_t i = 0; i < b.cap_count && i < SIDECAR_BIB_CAPS_MAX; i++) {
        uint16_t nl = le16(p + off);
        off += 2;
        uint32_t n = nl < 63 ? nl : 63;
        memcpy(b.caps[i].name, p + off, n);
        b.caps[i].name[n] = 0;
        off += nl;
        /* No 2-byte pad: slot follows the name immediately (the kernel
         * writes exactly what user/proto/src/bootinfo.rs parses). */
        b.caps[i].slot = le16(p + off);    off += 2;
        b.caps[i].ty = p[off];             off += 1;
        b.caps[i].rights = p[off];         off += 1;
        b.caps[i].base = le64(p + off);    off += 8;
        b.caps[i].len = le64(p + off);     off += 8;
    }
    return b;
}

/* ─── main ────────────────────────────────────────────────────────────────── */
int main(void) {
    cap_init();

    /* Plant the parent process (the init sidecar, in a real boot). */
    memset(proc_table, 0, sizeof(proc_table));
    proc_table[0].pid = 100;
    proc_table[0].active = 1;
    proc_table[0].state = PROC_SUSPENDED;
    proc_table[0].partition_id = 0;
    proc_table[0].owner_uid = 0;
    proc_table[0].syscall_stack_top = 0x300000;
    proc_count = 0;
    g_cur_pid = 100;

    /* Plant a peer sidecar in the registry so the manifest's CAP_CHAN
     * "peer0" (peer "drv.peer.0") resolves. The peer has no process
     * descriptor — cap_chan_create only needs its cap table, which binds
     * lazily on first use. */
    CHECK(sidecar_registry_register("drv.peer.0", 777) == 0,
          "peer sidecar registered in the sidecar registry");
    CHECK(sidecar_registry_resolve("drv.peer.0") == 777,
          "registry resolves the planted peer");
    CHECK(sidecar_registry_resolve("no.such.peer") == 0,
          "unregistered peer resolves to 0");

    /* The sidecar image bytes the manifest points at. */
    static uint8_t g_image[IMAGE_SIZE];
    for (uint32_t i = 0; i < IMAGE_SIZE; i++) g_image[i] = (uint8_t)(i % 251);
    uint64_t image_kaddr = (uint64_t)(uintptr_t)g_image;

    struct Blob blob;
    build_valid_blob(&blob, image_kaddr, 0);

    /* ── 1. manifest validation error paths ─────────────────────────────── */
    uint16_t ch_r = CAP_NONE;
    CHECK(cap_create_sidecar(100, 0, 64, CAP_NONE, CAP_NONE, &ch_r) == CAP_EINVAL,
          "NULL manifest refused");
    CHECK(cap_create_sidecar(100, blob.data, 4, CAP_NONE, CAP_NONE, &ch_r) == CAP_EINVAL,
          "manifest shorter than the 24-byte header refused");
    CHECK(proc_count == 0, "no process was created by the refusals");

    struct Blob bad;
    build_valid_blob(&bad, image_kaddr, 0);
    bad.data[0] = 'X';   /* bad magic */
    CHECK(cap_create_sidecar(100, bad.data, bad.len, CAP_NONE, CAP_NONE, &ch_r) == CAP_EINVAL,
          "bad manifest magic refused");

    build_valid_blob(&bad, image_kaddr, 0);
    bad.data[8] = 2;     /* version_major = 2 */
    CHECK(cap_create_sidecar(100, bad.data, bad.len, CAP_NONE, CAP_NONE, &ch_r) == CAP_EINVAL,
          "unsupported manifest version refused");

    build_valid_blob(&bad, image_kaddr, 0);
    uint32_t over = bad.len + 1;
    memcpy(bad.data + 16, &over, 4);   /* total_len > actual length */
    CHECK(cap_create_sidecar(100, bad.data, bad.len, CAP_NONE, CAP_NONE, &ch_r) == CAP_ERANGE,
          "total_len beyond the blob refused");

    build_valid_blob(&bad, image_kaddr, 0);
    bad.data[30] ^= 0xFF;   /* corrupt a body byte: CRC must fail */
    CHECK(cap_create_sidecar(100, bad.data, bad.len, CAP_NONE, CAP_NONE, &ch_r) == CAP_EINVAL,
          "CRC mismatch refused before any field is trusted");

    build_valid_blob(&bad, image_kaddr, 0);
    uint16_t too_many = SIDECAR_MANIFEST_MAX_CAPS + 11;
    memcpy(bad.data + 12, &too_many, 2);   /* record_count above the bound */
    CHECK(cap_create_sidecar(100, bad.data, bad.len, CAP_NONE, CAP_NONE, &ch_r) == CAP_ERANGE,
          "record_count above the cap bound refused");

    build_valid_blob(&bad, image_kaddr, 1);   /* unknown tag, tolerate flag clear */
    CHECK(cap_create_sidecar(100, bad.data, bad.len, CAP_NONE, CAP_NONE, &ch_r) == CAP_EINVAL,
          "unknown manifest tag refused without the tolerate flag");

    /* Manifest-level validation of the parsed IMAGE/BUDGET records. */
    build_valid_blob(&bad, image_kaddr, 0);
    uint32_t zero_size = 0;
    memcpy(bad.data + bad.rec_off[0] + 12, &zero_size, 4);   /* image_size = 0 */
    blob_refinish(&bad);
    CHECK(cap_create_sidecar(100, bad.data, bad.len, CAP_NONE, CAP_NONE, &ch_r) == CAP_EINVAL,
          "zero-size image refused");

    build_valid_blob(&bad, image_kaddr, 0);
    uint64_t oob_entry = IMAGE_SIZE;   /* entry == size: not strictly inside */
    memcpy(bad.data + bad.rec_off[0] + 0, &oob_entry, 8);
    blob_refinish(&bad);
    CHECK(cap_create_sidecar(100, bad.data, bad.len, CAP_NONE, CAP_NONE, &ch_r) == CAP_ERANGE,
          "entry point outside the image refused");

    build_valid_blob(&bad, image_kaddr, 0);
    uint32_t small_stack = 2048;
    memcpy(bad.data + bad.rec_off[1] + 8, &small_stack, 4);   /* stack < 4 KiB */
    blob_refinish(&bad);
    CHECK(cap_create_sidecar(100, bad.data, bad.len, CAP_NONE, CAP_NONE, &ch_r) == CAP_EINVAL,
          "stack under 4 KiB refused");

    /* Process/parent-level validation. */
    CHECK(cap_create_sidecar(999, blob.data, blob.len, CAP_NONE, CAP_NONE, &ch_r) == CAP_EINVAL,
          "unknown parent pid refused");

    build_valid_blob(&bad, image_kaddr, 0);
    uint64_t zero_kaddr = 0;
    memcpy(bad.data + bad.footer_off, &zero_kaddr, 8);   /* footer image_kaddr */
    blob_refinish(&bad);
    CHECK(cap_create_sidecar(100, bad.data, bad.len, CAP_NONE, CAP_NONE, &ch_r) == CAP_EINVAL,
          "missing image_kaddr refused");

    CHECK(proc_count == 0, "still no process after every error path");

    /* ── 2. the happy path ──────────────────────────────────────────────── */
    ch_r = CAP_NONE;
    CHECK(cap_create_sidecar(100, blob.data, blob.len, CAP_NONE, CAP_NONE, &ch_r) == 0,
          "cap_create_sidecar succeeds");
    CHECK(ch_r != CAP_NONE, "parent receives a CHAN_R to the child");
    CHECK(proc_count == 1, "one process was created");
    CHECK(per_cpu_data[0].kernel_rsp == 0x300000,
          "parent kernel_rsp restored after the async spawn");

    /* The child process descriptor. */
    struct ProcessDescriptor* child = 0;
    for (int i = 0; i < PROC_MAX; i++)
        if (proc_table[i].active && proc_table[i].pid != 100) { child = &proc_table[i]; break; }
    CHECK(child && child->pid == 101, "child got the next free pid (101)");
    CHECK(child->parent_pid == 100, "child's parent is the spawner");
    CHECK(child->state == PROC_SUSPENDED, "child released from HELD to SUSPENDED");
    CHECK(child->user_rip == IMAGE_VBASE + IMAGE_ENTRY, "entry at USER_PROC_CODE_BASE + manifest entry");
    CHECK(child->user_rsp == EXPECTED_RSP, "initial RSP at stack top - 16");
    CHECK(child->cr3 == (uint64_t)(uintptr_t)g_pml4, "child's PML4 is the cloned one");
    CHECK(child->partition_id == 0 && child->owner_uid == 0,
          "partition and uid inherited from the parent");
    CHECK(child->has_ring3_ctx == 1 && child->waiting_chan == CAP_NONE,
          "synthetic ring-3 context, no park state");

    /* ── 3. page table: image, stack, guard page ────────────────────────── */
    uint8_t* img0 = host_ptr(IMAGE_VBASE);
    uint8_t* img1 = host_ptr(IMAGE_VBASE + 4096);
    CHECK(img0 && img1, "both image pages are mapped");
    if (img0 && img1)
        CHECK(memcmp(img0, g_image, 4096) == 0 &&
              memcmp(img1, g_image + 4096, 4096) == 0,
              "image bytes copied into the child's mapped frames");

    uint64_t img_pte = leaf_pte(IMAGE_VBASE);
    uint64_t stk_pte = leaf_pte(STACK_BASE);
    /* Flat sidecar images are mapped WRITE+EXEC by design — the crt0's
     * .bss boot stack lives inside the image's pages, so the first `call`
     * must be able to push onto them (see the RWX note at the
     * user_map_page() call in cap_create_sidecar). */
    CHECK((img_pte & (USER_PTE_PRESENT | USER_PTE_USER | USER_PTE_WRITE | USER_PTE_NOEXEC))
            == (USER_PTE_PRESENT | USER_PTE_USER | USER_PTE_WRITE),
          "image pages: present+user+write, executable (RWX flat image)");
    CHECK((stk_pte & (USER_PTE_PRESENT | USER_PTE_USER | USER_PTE_WRITE | USER_PTE_NOEXEC))
            == (USER_PTE_PRESENT | USER_PTE_USER | USER_PTE_WRITE | USER_PTE_NOEXEC),
          "stack pages: present+user+write, non-executable");
    CHECK(leaf_pte(IMAGE_VBASE + 2 * 4096) == 0,
          "guard page between image and stack is unmapped");

    /* ── 4. capability table: messenger channel + manifest MEM caps ─────── */
    uint32_t chan_obj = cap_debug_objid(100, ch_r);
    CHECK(chan_obj != 0xFFFFFFFFu, "parent's CHAN_R is a real channel cap");
    CHECK(cap_debug_refcount(chan_obj) == 4,
          "messenger channel object has its 4 endpoint holders");

    int cti = table_for_pid(child->pid);
    CHECK(cti >= 0, "child's capability table is pre-bound");

    uint16_t child_rd = CAP_NONE, child_wr = CAP_NONE;
    uint16_t budget_slot = CAP_NONE, dma_slot = CAP_NONE;
    if (cti >= 0) {
        for (int s = 0; s < CAP_TABLE_ENTRIES; s++) {
            uint64_t w = cap_tables[cti].slots[s].word;
            if (!slot_valid(w)) continue;
            uint32_t oid = cap_debug_objid(child->pid, (uint16_t)s);
            if (slot_type(w) == CAP_TYPE_CHAN_R && oid == chan_obj) child_rd = (uint16_t)s;
            if (slot_type(w) == CAP_TYPE_CHAN_W && oid == chan_obj) child_wr = (uint16_t)s;
            if (slot_type(w) == CAP_TYPE_MEM &&
                cap_debug_refcount(oid) == 1 && budget_slot == CAP_NONE)
                budget_slot = (uint16_t)s;
            if (slot_type(w) == CAP_TYPE_MEM &&
                cap_debug_refcount(oid) == 1 && dma_slot == CAP_NONE &&
                budget_slot != CAP_NONE && (uint16_t)s != budget_slot)
                dma_slot = (uint16_t)s;
        }
    }
    CHECK(child_rd != CAP_NONE && child_wr != CAP_NONE,
          "child has both messenger endpoints (CHAN_R + CHAN_W)");
    CHECK(child_rd != child_wr, "messenger endpoints are distinct slots");
    CHECK(budget_slot != CAP_NONE && dma_slot != CAP_NONE &&
          budget_slot != dma_slot,
          "manifest MEM caps were minted into the child's table");

    /* ── 5. the BootInfoBlock the child's _start would parse ────────────── */
    uint8_t* bib = host_ptr(BIB_VADDR);
    CHECK(bib != 0, "BIB frame is mapped at the stack top page");
    struct Bib b;
    if (bib) {
        b = bib_parse(bib);
        CHECK(memcmp(bib, SIDECAR_BIB_MAGIC, 8) == 0, "BIB magic");
        CHECK(b.version == SIDECAR_BIB_VERSION, "BIB version 1");
        CHECK(b.cap_count == 8, "BIB lists 8 caps (2 messenger + 2 MEM + 4 wired CHAN)");
        CHECK(b.budget_bytes == BUDGET_MEM, "BIB budget matches the manifest");
        /* The async child is scheduled by the kernel, which iretq's from
         * the synthetic ring3_ctx and repoints [gs:8] at the child's own
         * syscall stack — both must be populated for the child to run. */
        CHECK(child->syscall_stack_top == 0x400000007000ULL,
              "child has a dedicated syscall stack (alloc_proc_syscall_stack)");
        CHECK(child->ring3_ctx.rip == child->user_rip &&
              child->ring3_ctx.cs == 0x23 &&
              child->ring3_ctx.rflags == 0x202 &&
              child->ring3_ctx.rsp == child->user_rsp &&
              child->ring3_ctx.ss == 0x1B,
              "synthetic ring3_ctx carries the entry rip/cs/rflags/rsp/ss");
        /* The sidecar crt0 contract: _start receives the BIB pointer in
         * rdi (TaskContext index 9) and saves it before switching to its
         * own boot stack — without this the child's first instruction
         * would save zero and rust_entry would parse garbage. */
        CHECK(child->ring3_ctx.rdi == BIB_VADDR,
              "synthetic ring3_ctx carries the BIB pointer in rdi (crt0 contract)");
        CHECK(b.stack_top == BIB_STACK_TOP, "BIB stack_top = RSP at _start + 16");
        /* Entry = name_len u16 + name + slot u16 + ty u8 + rights u8 +
         * base u64 + len u64: 32 header + 22 (cap0) + 22 (cap1) + 28
         * (cap2 "budget") + 25 (cap3 "dma") + 27+27 (cap4/5 "peer0") +
         * 29+29 (cap6/7 "console"). */
        CHECK(b.total_len == 241,
              "BIB total_len = 32 + 22 + 22 + 28 + 25 + 27 + 27 + 29 + 29");

        CHECK(b.caps[0].name[0] == 0 && b.caps[0].slot == child_rd &&
              b.caps[0].ty == CAP_TYPE_CHAN_R && b.caps[0].rights == CAP_PERM_RECV &&
              b.caps[0].base == 0 && b.caps[0].len == 0,
              "BIB cap 0: unnamed CHAN_R (messenger read end) at the child's slot");
        CHECK(b.caps[1].name[0] == 0 && b.caps[1].slot == child_wr &&
              b.caps[1].ty == CAP_TYPE_CHAN_W && b.caps[1].rights == CAP_PERM_SEND &&
              b.caps[1].base == 0 && b.caps[1].len == 0,
              "BIB cap 1: unnamed CHAN_W (messenger write end) at the child's slot");
        CHECK(strcmp(b.caps[2].name, "budget") == 0 && b.caps[2].slot == budget_slot &&
              b.caps[2].ty == CAP_TYPE_MEM && b.caps[2].rights == 0x03 &&
              b.caps[2].base == MEM_BUDGET_BASE && b.caps[2].len == MEM_BUDGET_BYTES,
              "BIB cap 2: named MEM 'budget' with its real slot/base/byte size");
        CHECK(strcmp(b.caps[3].name, "dma") == 0 && b.caps[3].slot == dma_slot &&
              b.caps[3].ty == CAP_TYPE_MEM && b.caps[3].rights == 0x03 &&
              b.caps[3].base == MEM_DMA_BASE && b.caps[3].len == MEM_DMA_BYTES,
              "BIB cap 3: named MEM 'dma' with its real slot/base/byte size");
        CHECK(strcmp(b.caps[4].name, "peer0") == 0 &&
              b.caps[4].ty == CAP_TYPE_CHAN_R && b.caps[4].rights == CAP_PERM_RECV &&
              b.caps[4].base == 0 && b.caps[4].len == 0,
              "BIB cap 4: named CHAN_R 'peer0' wired to the sidecar peer");
        CHECK(strcmp(b.caps[5].name, "peer0") == 0 &&
              b.caps[5].ty == CAP_TYPE_CHAN_W && b.caps[5].rights == CAP_PERM_SEND &&
              b.caps[5].base == 0 && b.caps[5].len == 0,
              "BIB cap 5: named CHAN_W 'peer0'");
        CHECK(strcmp(b.caps[6].name, "console") == 0 &&
              b.caps[6].ty == CAP_TYPE_CHAN_R && b.caps[6].rights == CAP_PERM_RECV &&
              b.caps[6].base == 0 && b.caps[6].len == 0,
              "BIB cap 6: named CHAN_R 'console' wired to the kernel service");
        CHECK(strcmp(b.caps[7].name, "console") == 0 &&
              b.caps[7].ty == CAP_TYPE_CHAN_W && b.caps[7].rights == CAP_PERM_SEND &&
              b.caps[7].base == 0 && b.caps[7].len == 0,
              "BIB cap 7: named CHAN_W 'console'");
        /* The BIB-reported CHAN slots are live caps in the child's table. */
        {
            uint64_t w4 = cap_tables[cti].slots[b.caps[4].slot].word;
            uint64_t w5 = cap_tables[cti].slots[b.caps[5].slot].word;
            CHECK(slot_valid(w4) && slot_type(w4) == CAP_TYPE_CHAN_R &&
                  slot_valid(w5) && slot_type(w5) == CAP_TYPE_CHAN_W,
                  "peer0's BIB slots are live CHAN_R/CHAN_W caps in the child's table");
        }
    }

    /* ── 5b. wired channels reach their far ends ──────────────────────── */
    if (bib) {
        /* The peer0 channel object (found via the child's CHAN_R) must
         * also appear as two endpoints in the peer sidecar's table. */
        uint32_t peer0_obj = cap_debug_objid(child->pid, b.caps[4].slot);
        int peer_ti = table_for_pid(777);
        int n_in_peer = 0;
        if (peer_ti >= 0) {
            for (int s = 0; s < CAP_TABLE_ENTRIES; s++) {
                uint64_t w = cap_tables[peer_ti].slots[s].word;
                if (!slot_valid(w)) continue;
                if (slot_type(w) != CAP_TYPE_CHAN_R && slot_type(w) != CAP_TYPE_CHAN_W) continue;
                uint32_t oid = (uint32_t)((w >> CAP_OBJ_SHIFT) & CAP_OBJ_MASK);
                if (oid == peer0_obj) n_in_peer++;
            }
        }
        CHECK(n_in_peer == 2,
              "peer sidecar (pid 777) holds both ends of the wired channel");

        /* The console channel's kernel end lives in the kernel context
         * table (pid 0), where a kernel console service would read it. */
        uint32_t cons_obj = cap_debug_objid(child->pid, b.caps[6].slot);
        int kt = table_for_pid(0);
        int n_in_kernel = 0;
        if (kt >= 0) {
            for (int s = 0; s < CAP_TABLE_ENTRIES; s++) {
                uint64_t w = cap_tables[kt].slots[s].word;
                if (!slot_valid(w)) continue;
                if (slot_type(w) != CAP_TYPE_CHAN_R && slot_type(w) != CAP_TYPE_CHAN_W) continue;
                uint32_t oid = (uint32_t)((w >> CAP_OBJ_SHIFT) & CAP_OBJ_MASK);
                if (oid == cons_obj) n_in_kernel++;
            }
        }
        CHECK(n_in_kernel == 2,
              "kernel context table holds the console channel's far end");
    }

    /* The child registered under its manifest NAME; the registry now
     * resolves both it and the planted peer. */
    CHECK(sidecar_registry_resolve("drv.child.0") == child->pid,
          "child registered under its manifest NAME ('drv.child.0')");
    CHECK(sidecar_registry_count() == 2,   /* drv.peer.0 + drv.child.0 */
          "registry holds the planted peer and the new child");

    /* ── 5c. the kernel console service drains wired console channels ──── */
    if (bib) {
        /* BIB cap 7 is the child's console CHAN_W (see the cap checks
         * above). The child sends console text on it; the kernel console
         * service receives it on the kernel context table's far end and
         * writes the payload to serial. */
        uint16_t console_w = b.caps[7].slot;
        const char text[] = "hello from sidecar console\n";
        g_serial_cap_len = 0;
        CHECK(cap_send_msg(child->pid, console_w, text, sizeof(text) - 1,
                           NULL, 0, 0, 0) == 0,
              "child's console message enqueued on its CHAN_W");
        console_service_tick();
        CHECK(console_service_drained() >= 1,
              "console service drained the message from the kernel end");
        CHECK(g_serial_cap_len == sizeof(text) - 1 &&
              memcmp(g_serial_cap, text, sizeof(text) - 1) == 0,
              "console payload reached the serial port verbatim");
    }

    /* ── 6. a second spawn: independent process, independent channel ────── */
    uint16_t ch_r2 = CAP_NONE;
    CHECK(cap_create_sidecar(100, blob.data, blob.len, CAP_NONE, CAP_NONE, &ch_r2) == 0,
          "a second sidecar spawns");
    CHECK(proc_count == 2, "two processes total");
    CHECK(ch_r2 != ch_r && ch_r2 != CAP_NONE,
          "second spawn gets its own parent channel");
    uint32_t chan2 = cap_debug_objid(100, ch_r2);
    CHECK(chan2 != chan_obj && chan2 != 0xFFFFFFFFu,
          "second messenger channel is a distinct object");
    {
        int found = 0;
        for (int i = 0; i < PROC_MAX; i++)
            if (proc_table[i].active && proc_table[i].pid == 102) found = 1;
        CHECK(found, "second child got pid 102");
    }
    /* Re-registration updates in place: the name now points at the latest
     * child, and the registry does not grow. */
    CHECK(sidecar_registry_resolve("drv.child.0") == 102,
          "re-registered NAME now points at the latest child (102)");
    CHECK(sidecar_registry_count() == 2,
          "registry does not grow on re-registration");

    /* ── 7. the syscall wrapper end to end ──────────────────────────────── */
    struct SLSCreateSidecarRequest req;
    memset(&req, 0, sizeof(req));
    req.manifest = blob.data;
    req.manifest_len = blob.len;
    req.ch_w_idx = CAP_NONE;
    req.console_w_idx = CAP_NONE;
    uint64_t wr = sys_sls_create_sidecar(&req);
    CHECK((int64_t)wr >= 0 && wr != (uint64_t)CAP_NONE,
          "sys_sls_create_sidecar returns a channel handle (not an error)");
    if ((int64_t)wr >= 0 && wr != (uint64_t)CAP_NONE)
        CHECK(cap_debug_objid(100, (uint16_t)wr) != 0xFFFFFFFFu,
              "...and it is a real CHAN cap in the caller's table");
    CHECK(proc_count == 3, "three processes total");

    /* ── 8. the spawn-to-spawn name→pid link ──────────────────────────── */
    /* The peer0/777 checks above prove the kernel can wire a channel to a
     * REGISTERED pid. This section proves the registry entry itself came
     * from a real spawn: a sidecar spawned through cap_create_sidecar is
     * registered under its NAME record, and a later manifest's CAP_CHAN
     * resolves that name to the peer's pid — the exact link init uses
     * when it spawns the ramdisk (drv.ramdisk.0) and then a consumer
     * whose console peer names it. No pid is planted here: the peer is a
     * real spawned process, and the channel must land in ITS table. */
    {
        /* Spawn the peer as a real sidecar (not planted). */
        struct Blob peer_blob;
        build_peer_blob(&peer_blob, image_kaddr, "drv.ramdisk.0");
        uint16_t peer_ch = CAP_NONE;
        CHECK(cap_create_sidecar(100, peer_blob.data, peer_blob.len,
                                 CAP_NONE, CAP_NONE, &peer_ch) == 0,
              "the peer sidecar ('drv.ramdisk.0') spawns through cap_create_sidecar");
        CHECK(peer_ch != CAP_NONE, "peer spawn returns its messenger channel");
        CHECK(proc_count == 4, "peer spawn makes four processes total");
        uint32_t peer_pid = sidecar_registry_resolve("drv.ramdisk.0");
        CHECK(peer_pid != 0 && peer_pid != 777 && peer_pid != 100,
              "the spawn registered 'drv.ramdisk.0' under a fresh pid — "
              "the name→pid link comes from the NAME record, not a plant");
        CHECK(table_for_pid(peer_pid) >= 0, "the peer's cap table is pre-bound");

        /* The consumer: NAME + one CAP_CHAN named "console" whose peer is
         * "drv.ramdisk.0" — resolved through the registry to the spawned
         * peer's pid, not a "kernel.*" service. */
        struct Blob cons_blob;
        build_consumer_blob(&cons_blob, image_kaddr, "drv.consumer.0",
                            "drv.ramdisk.0");
        uint16_t cons_ch = CAP_NONE;
        CHECK(cap_create_sidecar(100, cons_blob.data, cons_blob.len,
                                 CAP_NONE, CAP_NONE, &cons_ch) == 0,
              "the consumer spawns with a CAP_CHAN peer 'drv.ramdisk.0'");
        CHECK(proc_count == 5, "consumer spawn makes five processes total");
        uint32_t cons_pid = 0;
        for (int i = 0; i < PROC_MAX; i++)
            if (proc_table[i].active && proc_table[i].pid > peer_pid)
                { cons_pid = proc_table[i].pid; break; }
        CHECK(cons_pid == 105, "consumer got pid 105 (alloc_pid = max + 1)");
        CHECK(sidecar_registry_resolve("drv.consumer.0") == cons_pid,
              "the consumer is registered under its own NAME");

        /* The consumer is the last spawn, so g_pml4 is its clone and the
         * BIB at BIB_VADDR is the consumer's. */
        uint8_t* c_bib = host_ptr(BIB_VADDR);
        CHECK(c_bib != 0, "consumer's BIB is mapped at its stack top");
        if (c_bib) {
            struct Bib cb = bib_parse(c_bib);
            CHECK(cb.cap_count == 6,
                  "consumer BIB lists 6 caps (2 messenger + 2 MEM + 2 wired CHAN)");
            CHECK(cb.total_len == 187,
                  "consumer BIB total_len = 32 + 22 + 22 + 28 + 25 + 29 + 29");
            CHECK(strcmp(cb.caps[4].name, "console") == 0 &&
                  cb.caps[4].ty == CAP_TYPE_CHAN_R &&
                  cb.caps[4].rights == CAP_PERM_RECV,
                  "BIB cap 4: named CHAN_R 'console' wired to the sidecar peer");
            CHECK(strcmp(cb.caps[5].name, "console") == 0 &&
                  cb.caps[5].ty == CAP_TYPE_CHAN_W &&
                  cb.caps[5].rights == CAP_PERM_SEND,
                  "BIB cap 5: named CHAN_W 'console'");

            /* The wired channel's object appears as two endpoints in the
             * consumer's table AND two in the spawned peer's table — R/W
             * endpoints in both processes' tables — and none in the
             * planted 777's table, proving the resolution targeted the
             * registry entry (the spawned peer), not a planted pid. */
            uint32_t cons_obj = cap_debug_objid(cons_pid, cb.caps[4].slot);
            CHECK(cons_obj != 0xFFFFFFFFu,
                  "consumer's console CHAN_R is a real channel cap");
            {
                int n_in_cons = 0;
                int ti = table_for_pid(cons_pid);
                for (int s = 0; s < CAP_TABLE_ENTRIES; s++) {
                    uint64_t w = cap_tables[ti].slots[s].word;
                    if (!slot_valid(w)) continue;
                    if (slot_type(w) != CAP_TYPE_CHAN_R &&
                        slot_type(w) != CAP_TYPE_CHAN_W) continue;
                    uint32_t oid = (uint32_t)((w >> CAP_OBJ_SHIFT) & CAP_OBJ_MASK);
                    if (oid == cons_obj) n_in_cons++;
                }
                CHECK(n_in_cons == 2, "consumer holds both ends of the wired channel");
            }
            {
                int n_in_peer = 0;
                int ti = table_for_pid(peer_pid);
                for (int s = 0; s < CAP_TABLE_ENTRIES; s++) {
                    uint64_t w = cap_tables[ti].slots[s].word;
                    if (!slot_valid(w)) continue;
                    if (slot_type(w) != CAP_TYPE_CHAN_R &&
                        slot_type(w) != CAP_TYPE_CHAN_W) continue;
                    uint32_t oid = (uint32_t)((w >> CAP_OBJ_SHIFT) & CAP_OBJ_MASK);
                    if (oid == cons_obj) n_in_peer++;
                }
                CHECK(n_in_peer == 2,
                      "the spawned peer (drv.ramdisk.0) holds both ends — "
                      "R/W endpoints in both processes' tables");
            }
            {
                int n_in_777 = 0;
                int ti = table_for_pid(777);
                if (ti >= 0) {
                    for (int s = 0; s < CAP_TABLE_ENTRIES; s++) {
                        uint64_t w = cap_tables[ti].slots[s].word;
                        if (!slot_valid(w)) continue;
                        if (slot_type(w) != CAP_TYPE_CHAN_R &&
                            slot_type(w) != CAP_TYPE_CHAN_W) continue;
                        uint32_t oid = (uint32_t)((w >> CAP_OBJ_SHIFT) & CAP_OBJ_MASK);
                        if (oid == cons_obj) n_in_777++;
                    }
                }
                CHECK(n_in_777 == 0,
                      "the planted pid 777 holds none of this channel — "
                      "resolution went through the registry name→pid link");
            }

            /* ── 8b. the spawn-to-spawn channel carries data ──────────── */
            /* The topology checks above prove the endpoints exist in both
             * tables. This sends a message over the consumer's console
             * CHAN_W (BIB cap 5) and receives it on the peer's CHAN_R for
             * the same channel object — proving the full
             * spawn → NAME → registry → resolve → channel path carries
             * bytes, not just capabilities. Direction (same arithmetic as
             * the console service): the consumer (end1) sends to q[0]; the
             * peer (end0, pid 104) reads q[0]. */
            uint16_t cons_w = cb.caps[5].slot;
            const char msg[] = "spawn-to-spawn payload\n";
            CHECK(cap_send_msg(cons_pid, cons_w, msg, sizeof(msg) - 1,
                               NULL, 0, 0, 0) == 0,
                  "consumer's message enqueued on its console CHAN_W");
            {
                /* The peer's read end: a CHAN_R slot in the peer's table
                 * with the same object id as the consumer's console
                 * channel (the R and W ends are the same channel object). */
                uint16_t peer_rd = CAP_NONE;
                int ti = table_for_pid(peer_pid);
                for (int s = 0; s < CAP_TABLE_ENTRIES; s++) {
                    uint64_t w = cap_tables[ti].slots[s].word;
                    if (!slot_valid(w)) continue;
                    if (slot_type(w) != CAP_TYPE_CHAN_R) continue;
                    uint32_t oid = (uint32_t)((w >> CAP_OBJ_SHIFT) & CAP_OBJ_MASK);
                    if (oid == cons_obj) { peer_rd = (uint16_t)s; break; }
                }
                CHECK(peer_rd != CAP_NONE,
                      "peer's table has a CHAN_R for the consumer's console channel");

                char rbuf[128];
                uint32_t got = 0;
                CHECK(peer_rd != CAP_NONE &&
                      cap_recv_msg(peer_pid, peer_rd, rbuf, sizeof(rbuf),
                                   &got, 0, NULL, NULL, NULL, NULL) == 0,
                      "peer received the message on its CHAN_R");
                CHECK(peer_rd != CAP_NONE && got == sizeof(msg) - 1 &&
                      memcmp(rbuf, msg, got) == 0,
                      "payload arrived in the spawned peer's table verbatim");
            }

            /* ── 8c. the reverse direction: peer → consumer ──────────── */
            /* The peer replies on its CHAN_W (end0 sends to q[1]) and the
             * consumer receives on its CHAN_R (BIB cap 4, end1 reads
             * q[1]) — both queues of the spawn-to-spawn channel carry
             * data, not just the forward path. */
            {
                uint16_t peer_wr = CAP_NONE;
                int ti = table_for_pid(peer_pid);
                for (int s = 0; s < CAP_TABLE_ENTRIES; s++) {
                    uint64_t w = cap_tables[ti].slots[s].word;
                    if (!slot_valid(w)) continue;
                    if (slot_type(w) != CAP_TYPE_CHAN_W) continue;
                    uint32_t oid = (uint32_t)((w >> CAP_OBJ_SHIFT) & CAP_OBJ_MASK);
                    if (oid == cons_obj) { peer_wr = (uint16_t)s; break; }
                }
                CHECK(peer_wr != CAP_NONE,
                      "peer's table has a CHAN_W for the consumer's console channel");

                const char reply[] = "reply from drv.ramdisk.0\n";
                CHECK(peer_wr != CAP_NONE &&
                      cap_send_msg(peer_pid, peer_wr, reply, sizeof(reply) - 1,
                                   NULL, 0, 0, 0) == 0,
                      "peer's reply enqueued on its CHAN_W");

                char rbuf2[128];
                uint32_t got2 = 0;
                CHECK(peer_wr != CAP_NONE &&
                      cap_recv_msg(cons_pid, cb.caps[4].slot, rbuf2,
                                   sizeof(rbuf2), &got2, 0, NULL, NULL,
                                   NULL, NULL) == 0,
                      "consumer received the reply on its CHAN_R");
                CHECK(peer_wr != CAP_NONE && got2 == sizeof(reply) - 1 &&
                      memcmp(rbuf2, reply, got2) == 0,
                      "reply arrived in the consumer's table verbatim");
            }
        }
        CHECK(sidecar_registry_resolve("drv.ramdisk.0") == peer_pid,
              "registry still maps 'drv.ramdisk.0' to the spawned peer");
        CHECK(sidecar_registry_count() == 4,
              "registry holds 4 names (drv.peer.0, drv.child.0, "
              "drv.ramdisk.0, drv.consumer.0)");
    }

    /* ── 9. the Phase 5 channel transport syscalls (311-315) ──────────── */
    /* kernel/chan.c — the kabi.rs k_chan_* contract driven through its
     * syscall wrappers, on the spawn-to-spawn channel from section 8.
     * g_cur_pid selects the "calling" sidecar (cap_current_pid). The
     * consumer (105) is end1, the spawned peer (104) is end0; both queues
     * are empty after section 8b/8c. Returns are the transport's positive
     * CAP_ERR_* codes. */
    {
        /* Re-derive the consumer's BIB (g_pml4 is still its clone) and the
         * channel endpoints from the tables. */
        uint8_t* cb9p = host_ptr(BIB_VADDR);
        CHECK(cb9p != 0, "consumer's BIB still mapped for the transport test");
        struct Bib cb9;
        memset(&cb9, 0, sizeof(cb9));
        if (cb9p) cb9 = bib_parse(cb9p);
        uint16_t cons_r = cb9.caps[4].slot;   /* console CHAN_R in 105 */
        uint16_t cons_w = cb9.caps[5].slot;   /* console CHAN_W in 105 */
        uint32_t cons_obj = cap_debug_objid(105, cons_r);
        uint16_t peer_r = CAP_NONE, peer_w = CAP_NONE;
        {
            int ti = table_for_pid(104);
            for (int s = 0; s < CAP_TABLE_ENTRIES; s++) {
                uint64_t w = cap_tables[ti].slots[s].word;
                if (!slot_valid(w)) continue;
                uint32_t oid = (uint32_t)((w >> CAP_OBJ_SHIFT) & CAP_OBJ_MASK);
                if (oid != cons_obj) continue;
                if (slot_type(w) == CAP_TYPE_CHAN_R && peer_r == CAP_NONE)
                    peer_r = (uint16_t)s;
                if (slot_type(w) == CAP_TYPE_CHAN_W && peer_w == CAP_NONE)
                    peer_w = (uint16_t)s;
            }
        }
        CHECK(cons_r != CAP_NONE && cons_w != CAP_NONE &&
              peer_r != CAP_NONE && peer_w != CAP_NONE,
              "both ends of the spawn-to-spawn channel re-derived");

        /* 9a. wait → send → recv round trip through the syscalls. */
        const char m1[] = "transport round trip\n";
        g_cur_pid = 105;
        struct SLSChanSendRequest sreq;
        memset(&sreq, 0, sizeof(sreq));
        sreq.chan = cons_w;
        sreq.tag = 0xABCD;
        sreq.flags = 0;
        sreq.payload = (void*)m1;
        sreq.payload_len = sizeof(m1) - 1;
        sreq.timeout_ns = 0;
        CHECK(sys_sls_chan_send(&sreq) == CAP_ERR_OK,
              "k_chan_send (313): consumer enqueues a tagged message");

        g_cur_pid = 104;
        struct SLSChanWaitRequest wreq;
        memset(&wreq, 0, sizeof(wreq));
        wreq.chans[0] = peer_r;
        wreq.n_chans = 1;
        wreq.timeout_ns = 1000;
        CHECK(sys_sls_chan_wait(&wreq) == CAP_ERR_OK &&
              wreq.out_idx == 0 && wreq.out_kind == CH_KIND_MSG,
              "k_chan_wait (311): reports the queued MSG at index 0");

        char rbuf[128];
        struct SLSChanRecvRequest rreq;
        memset(&rreq, 0, sizeof(rreq));
        memset(rbuf, 0, sizeof(rbuf));
        rreq.chan = peer_r;
        rreq.buf = rbuf;
        rreq.buf_len = sizeof(rbuf);
        rreq.n_slots = 8;
        CHECK(sys_sls_chan_recv(&rreq) == CAP_ERR_OK &&
              rreq.out.kind == CH_KIND_MSG && rreq.out.tag == 0xABCD &&
              rreq.out.len == sizeof(m1) - 1 &&
              memcmp(rbuf, m1, sizeof(m1) - 1) == 0,
              "k_chan_recv (312): MSG payload + tag verbatim at the peer");

        /* 9b. no-loss recv: an undersized buffer reports BUFSZ + needed and
         * does NOT consume the message (transport spec §3.4). */
        const char m2[] = "a longer message that needs a bigger buffer\n";
        g_cur_pid = 105;
        memset(&sreq, 0, sizeof(sreq));
        sreq.chan = cons_w;
        sreq.payload = (void*)m2;
        sreq.payload_len = sizeof(m2) - 1;
        CHECK(sys_sls_chan_send(&sreq) == CAP_ERR_OK,
              "k_chan_send: second message enqueued");
        g_cur_pid = 104;
        memset(&rreq, 0, sizeof(rreq));
        rreq.chan = peer_r;
        rreq.buf = rbuf;
        rreq.buf_len = 4;             /* too small for m2 */
        rreq.n_slots = 8;
        CHECK(sys_sls_chan_recv(&rreq) == CAP_ERR_BUFSZ &&
              rreq.out.needed != 0,
              "undersized recv → BUFSZ with needed, message NOT consumed");
        memset(&wreq, 0, sizeof(wreq));
        wreq.chans[0] = peer_r;
        wreq.n_chans = 1;
        wreq.timeout_ns = 1000;
        CHECK(sys_sls_chan_wait(&wreq) == CAP_ERR_OK &&
              wreq.out_kind == CH_KIND_MSG,
              "message still queued after the refused recv");
        memset(&rreq, 0, sizeof(rreq));
        rreq.chan = peer_r;
        rreq.buf = rbuf;
        rreq.buf_len = sizeof(rbuf);
        rreq.n_slots = 8;
        CHECK(sys_sls_chan_recv(&rreq) == CAP_ERR_OK &&
              rreq.out.len == sizeof(m2) - 1 &&
              memcmp(rbuf, m2, sizeof(m2) - 1) == 0,
              "retry with a big buffer succeeds — no-loss rule holds");

        /* 9c. wait with nothing queued and a finite timeout → TIMEOUT. */
        memset(&wreq, 0, sizeof(wreq));
        wreq.chans[0] = peer_r;
        wreq.n_chans = 1;
        wreq.timeout_ns = 1;
        CHECK(sys_sls_chan_wait(&wreq) == CAP_ERR_TIMEOUT,
              "k_chan_wait on an empty channel times out");

        /* 9d. close → the peer observes a CLOSE event exactly once, then
         * the consumer's further sends fail with STATE. */
        g_cur_pid = 105;
        struct SLSChanCloseRequest creq;
        memset(&creq, 0, sizeof(creq));
        creq.chan = cons_r;
        creq.reason = 0x0001;         /* CLOSE_PEER */
        creq.detail = 42;
        CHECK(sys_sls_chan_close(&creq) == CAP_ERR_OK,
              "k_chan_close (314): consumer closes its endpoint");
        CHECK(sys_sls_chan_close(&creq) == CAP_ERR_OK,
              "...and closing again is idempotent");

        g_cur_pid = 104;
        memset(&wreq, 0, sizeof(wreq));
        wreq.chans[0] = peer_r;
        wreq.n_chans = 1;
        wreq.timeout_ns = 1000;
        CHECK(sys_sls_chan_wait(&wreq) == CAP_ERR_OK &&
              wreq.out_kind == CH_KIND_CLOSE,
              "peer's wait reports the CLOSE event");
        memset(&rreq, 0, sizeof(rreq));
        memset(rbuf, 0, sizeof(rbuf));
        rreq.chan = peer_r;
        rreq.buf = rbuf;
        rreq.buf_len = sizeof(rbuf);
        rreq.n_slots = 8;
        CHECK(sys_sls_chan_recv(&rreq) == CAP_ERR_OK &&
              rreq.out.kind == CH_KIND_CLOSE && rreq.out.len == 8 &&
              rbuf[0] == 0x01 && rbuf[1] == 0x00 &&      /* reason = CLOSE_PEER */
              rbuf[2] == 42 && rbuf[3] == 0 && rbuf[4] == 0 && rbuf[5] == 0,
              "peer recvs the CLOSE body {reason, detail} verbatim");
        CHECK(sys_sls_chan_recv(&rreq) == CAP_ERR_STATE,
              "close event delivered exactly once (queue now drained)");

        g_cur_pid = 105;
        memset(&sreq, 0, sizeof(sreq));
        sreq.chan = cons_w;
        sreq.payload = (void*)m1;
        sreq.payload_len = sizeof(m1) - 1;
        CHECK(sys_sls_chan_send(&sreq) == CAP_ERR_STATE,
              "send on the consumer's closed endpoint → STATE");

        /* 9e. error mapping + validation through the syscall surface. */
        g_cur_pid = 105;
        memset(&rreq, 0, sizeof(rreq));
        rreq.chan = cons_w;           /* a CHAN_W, recv wants CHAN_R */
        rreq.buf = rbuf;
        rreq.buf_len = sizeof(rbuf);
        rreq.n_slots = 8;
        CHECK(sys_sls_chan_recv(&rreq) == CAP_ERR_TYPE,
              "recv on a CHAN_W slot → TYPE");
        memset(&wreq, 0, sizeof(wreq));
        wreq.chans[0] = cb9.caps[2].slot;   /* budget MEM cap */
        wreq.n_chans = 1;
        CHECK(sys_sls_chan_wait(&wreq) == CAP_ERR_TYPE,
              "wait on a MEM slot → TYPE");
        memset(&sreq, 0, sizeof(sreq));
        sreq.chan = cons_w;
        sreq.flags = CH_F_NO_REPLY;
        sreq.payload = (void*)m1;
        sreq.payload_len = sizeof(m1) - 1;
        sreq.n_caps = 1;              /* NO_REPLY may not carry grants */
        sreq.caps[0].slot = cb9.caps[2].slot;
        sreq.caps[0].len = 4096;
        CHECK(sys_sls_chan_send(&sreq) == CAP_ERR_PROTO,
              "NO_REPLY + cap argument → PROTO, nothing sent");
        memset(&sreq, 0, sizeof(sreq));
        sreq.chan = cons_w;
        sreq.payload = (void*)m1;
        sreq.payload_len = CAP_MSG_MAX_PAYLOAD + 1;
        CHECK(sys_sls_chan_send(&sreq) == CAP_ERR_RANGE,
              "payload above the 4 KiB bound → RANGE");

        /* 9f. cap_info introspects both a MEM cap and a CHAN cap. */
        struct SLSCapInfoRequest ireq;
        memset(&ireq, 0, sizeof(ireq));
        ireq.handle = cb9.caps[2].slot;   /* budget MEM */
        CHECK(sys_sls_cap_info(&ireq) == CAP_ERR_OK &&
              ireq.out.ty == CAP_TYPE_MEM &&
              (ireq.out.rights & CAP_PERM_R) &&
              ireq.out.base == cb9.caps[2].base &&
              ireq.out.len == cb9.caps[2].len,
              "k_cap_info (315): budget MEM cap base/len/rights");
        memset(&ireq, 0, sizeof(ireq));
        ireq.handle = cb9.caps[4].slot;   /* console CHAN_R */
        CHECK(sys_sls_cap_info(&ireq) == CAP_ERR_OK &&
              ireq.out.ty == CAP_TYPE_CHAN_R &&
              ireq.out.base == 0 && ireq.out.len == 0,
              "k_cap_info on the console CHAN cap");
    }

    /* ── 10. peer-death close events (cap_table_teardown → CLOSE_PEER_DEAD) */
    /* A FRESH spawn-to-spawn pair — section 9 closed the first pair's
     * endpoint, so its close was already delivered. Spawn peer2
     * (drv.ramdisk.1 → 106) and consumer2 (drv.consumer.1 → 107), tear
     * the peer down through the REAL death path (cap_table_teardown), and
     * verify the consumer observes the death on the channel transport:
     * wait reports CLOSE, recv delivers {CLOSE_PEER_DEAD, detail=106}, and
     * a send after the peer died fails STATE (both sides checked). */
    {
        struct Blob peer2_blob, cons2_blob;
        build_peer_blob(&peer2_blob, image_kaddr, "drv.ramdisk.1");
        build_consumer_blob(&cons2_blob, image_kaddr, "drv.consumer.1",
                            "drv.ramdisk.1");
        uint16_t p2_ch = CAP_NONE, c2_ch = CAP_NONE;
        CHECK(cap_create_sidecar(100, peer2_blob.data, peer2_blob.len,
                                 CAP_NONE, CAP_NONE, &p2_ch) == 0,
              "peer2 (drv.ramdisk.1) spawns through cap_create_sidecar");
        CHECK(cap_create_sidecar(100, cons2_blob.data, cons2_blob.len,
                                 CAP_NONE, CAP_NONE, &c2_ch) == 0,
              "consumer2 spawns with console peer drv.ramdisk.1");
        CHECK(sidecar_registry_resolve("drv.ramdisk.1") == 106 &&
              sidecar_registry_resolve("drv.consumer.1") == 107,
              "the fresh pair is registered (106/107)");
        CHECK(sidecar_registry_count() == 6,
              "registry holds 6 names before the teardown");

        /* Consumer2 is the last spawn, so g_pml4 is its clone; re-derive
         * its console channel (BIB caps 4/5). */
        struct Bib cb10 = bib_parse(host_ptr(BIB_VADDR));
        uint16_t c2_r = cb10.caps[4].slot;   /* console CHAN_R in 107 */
        uint16_t c2_w = cb10.caps[5].slot;   /* console CHAN_W in 107 */
        uint32_t c2_obj = cap_debug_objid(107, c2_r);
        CHECK(c2_r != CAP_NONE && c2_w != CAP_NONE &&
              c2_obj != 0xFFFFFFFFu,
              "consumer2's console channel re-derived from its BIB");

        /* The death path: cap_table_teardown (what process_exit/kill call). */
        kill_sidecar(106);
        CHECK(sidecar_registry_resolve("drv.ramdisk.1") == 0,
              "the dead peer stops resolving (registry entry removed)");
        CHECK(sidecar_registry_count() == 5,
              "registry drops the dead peer's name");

        /* The consumer observes the death through the chan syscalls. */
        g_cur_pid = 107;
        struct SLSChanWaitRequest wreq10;
        memset(&wreq10, 0, sizeof(wreq10));
        wreq10.chans[0] = c2_r;
        wreq10.n_chans = 1;
        wreq10.timeout_ns = 1000;
        CHECK(sys_sls_chan_wait(&wreq10) == CAP_ERR_OK &&
              wreq10.out_idx == 0 && wreq10.out_kind == CH_KIND_CLOSE,
              "consumer2's wait reports CLOSE after the peer dies");

        char rbuf10[64];
        struct SLSChanRecvRequest rreq10;
        memset(&rreq10, 0, sizeof(rreq10));
        memset(rbuf10, 0, sizeof(rbuf10));
        rreq10.chan = c2_r;
        rreq10.buf = rbuf10;
        rreq10.buf_len = sizeof(rbuf10);
        rreq10.n_slots = 8;
        CHECK(sys_sls_chan_recv(&rreq10) == CAP_ERR_OK &&
              rreq10.out.kind == CH_KIND_CLOSE && rreq10.out.len == 8 &&
              rbuf10[0] == 0x02 && rbuf10[1] == 0x00 &&   /* CLOSE_PEER_DEAD */
              rbuf10[2] == 106 && rbuf10[3] == 0 &&
              rbuf10[4] == 0 && rbuf10[5] == 0,
              "consumer2 recvs the CLOSE body {CLOSE_PEER_DEAD, pid 106}");
        CHECK(sys_sls_chan_recv(&rreq10) == CAP_ERR_STATE,
              "death close event delivered exactly once");

        /* Send after the peer died → STATE (either side closed). */
        struct SLSChanSendRequest sreq10;
        memset(&sreq10, 0, sizeof(sreq10));
        sreq10.chan = c2_w;
        sreq10.payload = (void*)"ping";
        sreq10.payload_len = 4;
        CHECK(sys_sls_chan_send(&sreq10) == CAP_ERR_STATE,
              "send after peer death → STATE, nothing enqueued");
    }

    /* ── 11. the console service drops a dead child's kernel end ──────── */
    /* kernel/console_service.c is close-aware: a child's death (teardown
     * marks close_evt on the kernel end of its console channel) makes the
     * next tick print the child's last messages, then REVOKE the kernel's
     * CHAN_R/CHAN_W instead of leaving the channel open forever. Spawn a
     * console-wired child (standard blob: console → kernel.debug.console),
     * drain it live, tear it down, and assert the kernel end is revoked. */
    {
        struct Blob cblob;
        build_valid_blob(&cblob, image_kaddr, 0);
        uint16_t cb_ch = CAP_NONE;
        CHECK(cap_create_sidecar(100, cblob.data, cblob.len,
                                 CAP_NONE, CAP_NONE, &cb_ch) == 0,
              "a console-wired child (108) spawns");
        /* Its BIB is the last clone: caps[6]/[7] = console CHAN_R/CHAN_W. */
        struct Bib cb11 = bib_parse(host_ptr(BIB_VADDR));
        uint16_t c11_r = cb11.caps[6].slot;
        uint16_t c11_w = cb11.caps[7].slot;
        uint32_t c11_obj = cap_debug_objid(108, c11_r);
        CHECK(c11_r != CAP_NONE && c11_w != CAP_NONE &&
              c11_obj != 0xFFFFFFFFu,
              "child 108's console channel re-derived from its BIB");

        /* The kernel's far end: a CHAN_R in table 0 with the same object. */
        int k11_slot = -1;
        for (int s = 0; s < CAP_TABLE_ENTRIES; s++) {
            uint64_t w = cap_tables[0].slots[s].word;
            if (!slot_valid(w) || slot_type(w) != CAP_TYPE_CHAN_R) continue;
            uint32_t oid = (uint32_t)((w >> CAP_OBJ_SHIFT) & CAP_OBJ_MASK);
            if (oid == c11_obj) { k11_slot = s; break; }
        }
        CHECK(k11_slot >= 0, "kernel holds the child's console far end");

        /* A LIVE child: the tick drains and does NOT revoke. */
        const char cmsg[] = "last words before death\n";
        g_serial_cap_len = 0;
        CHECK(cap_send_msg(108, c11_w, cmsg, sizeof(cmsg) - 1,
                           NULL, 0, 0, 0) == 0,
              "child's console message enqueued");
        console_service_tick();
        CHECK(g_serial_cap_len == sizeof(cmsg) - 1 &&
              memcmp(g_serial_cap, cmsg, sizeof(cmsg) - 1) == 0,
              "tick drains a live child's console verbatim");
        CHECK(slot_valid(cap_tables[0].slots[k11_slot].word),
              "live child's kernel-end cap NOT revoked");

        /* The death: teardown marks close_evt on the kernel end; the next
         * tick must notice and drop the kernel end entirely. */
        kill_sidecar(108);
        CHECK(sidecar_registry_resolve("drv.child.0") == 0,
              "the console child stops resolving after teardown");
        console_service_tick();
        CHECK(!slot_valid(cap_tables[0].slots[k11_slot].word),
              "dead child's kernel-end console cap revoked by the next tick");
    }

    /* ── 12. wait-aware park: k_chan_wait (TIMEOUT_NONE) requests the park;
     * send / close / peer-death wake it ───────────────────────────────── */
    /* The real kernel (process.c) parks a TIMEOUT_NONE wait on the listed
     * channels and re-runs the wait on wake. This section proves the
     * transport half of that loop with nothing planted: (a) the park hook
     * receives the RESOLVED channel ids + the request pointer when nothing
     * is ready (and a finite timeout does NOT park), (b) an enqueue fires
     * cap_wake_chan with the channel id and the re-poll finds the MSG, (c)
     * a multi-channel wait parks on BOTH resolved ids, (d) an explicit
     * k_chan_close fires the wake and the re-poll sees the CLOSE event,
     * and (e) a peer-death teardown fires the wake and the re-poll sees
     * CLOSE_PEER_DEAD with the dead pid. */
    {
        /* A fresh spawn-to-spawn pair (drv.ramdisk.2 → drv.consumer.2). */
        struct Blob p3;
        build_peer_blob(&p3, image_kaddr, "drv.ramdisk.2");
        uint16_t p3_ch = CAP_NONE;
        CHECK(cap_create_sidecar(100, p3.data, p3.len, CAP_NONE, CAP_NONE,
                                 &p3_ch) == 0,
              "wait-park peer (drv.ramdisk.2) spawns");
        uint32_t peer3 = sidecar_registry_resolve("drv.ramdisk.2");
        struct Blob c3;
        build_consumer_blob(&c3, image_kaddr, "drv.consumer.2", "drv.ramdisk.2");
        uint16_t c3_ch = CAP_NONE;
        CHECK(cap_create_sidecar(100, c3.data, c3.len, CAP_NONE, CAP_NONE,
                                 &c3_ch) == 0,
              "wait-park consumer (drv.consumer.2) spawns");
        uint32_t cons3 = sidecar_registry_resolve("drv.consumer.2");
        CHECK(peer3 != 0 && cons3 != 0 && peer3 != cons3,
              "wait-park pair resolved via the registry");
        struct Bib c3bib = bib_parse(host_ptr(BIB_VADDR));
        uint16_t c3_r = c3bib.caps[4].slot;   /* console CHAN_R in cons3 */
        uint16_t c3_w = c3bib.caps[5].slot;   /* console CHAN_W in cons3 */
        uint16_t m3_r = c3bib.caps[0].slot;   /* messenger CHAN_R in cons3 */
        uint32_t c3_obj = cap_debug_objid(cons3, c3_r);
        uint32_t c3_chan = (c3_obj != 0xFFFFFFFFu)
                               ? cap_objects[c3_obj].chan_id : CAP_CHAN_MAX;
        CHECK(c3_r != CAP_NONE && c3_w != CAP_NONE && c3_obj != 0xFFFFFFFFu &&
              c3_chan < CAP_CHAN_MAX,
              "consumer3's console channel re-derived (chan id known)");
        /* peer3's console CHAN_W: the peer-end slot of the same object. */
        uint16_t peer3_w = CAP_NONE;
        {
            int ti = table_for_pid(peer3);
            for (int s = 0; s < CAP_TABLE_ENTRIES; s++) {
                uint64_t w = cap_tables[ti].slots[s].word;
                if (!slot_valid(w) || slot_type(w) != CAP_TYPE_CHAN_W) continue;
                uint32_t oid = (uint32_t)((w >> CAP_OBJ_SHIFT) & CAP_OBJ_MASK);
                if (oid == c3_obj) { peer3_w = (uint16_t)s; break; }
            }
        }
        CHECK(peer3_w != CAP_NONE, "peer3's console CHAN_W found in its table");

        /* 12a. TIMEOUT_NONE + empty queue → the park hook fires with the
         * resolved channel id and the request pointer; the syscall returns
         * TIMEOUT when the hook cannot park (host-test posture — the real
         * kernel parks instead of returning). A finite timeout parks WITH
         * a deadline (absolute ticks, rounded up to one ~10 ms tick). */
        g_wait_park_calls = 0;
        g_wait_deadline = 0;
        g_wake_calls = 0;
        g_cur_pid = cons3;
        struct SLSChanWaitRequest w12;
        memset(&w12, 0, sizeof(w12));
        w12.chans[0] = c3_r;
        w12.n_chans = 1;
        w12.timeout_ns = CH_TIMEOUT_NONE;
        CHECK(sys_sls_chan_wait(&w12) == CAP_ERR_TIMEOUT,
              "12a: TIMEOUT_NONE wait on an empty queue returns TIMEOUT when "
              "the park hook cannot park");
        CHECK(g_wait_park_calls == 1 && g_wait_n == 1 &&
              g_wait_chans[0] == c3_chan && g_wait_req == (void*)&w12 &&
              g_wait_deadline == 0,
              "12a: the park hook received the resolved channel id, the "
              "request pointer, and deadline 0 (block forever)");

        g_wait_park_calls = 0;
        g_wait_deadline = 0;
        memset(&w12, 0, sizeof(w12));
        w12.chans[0] = c3_r;
        w12.n_chans = 1;
        w12.timeout_ns = 1000;   /* 1 µs → rounded up to one ~10 ms tick */
        CHECK(sys_sls_chan_wait(&w12) == CAP_ERR_TIMEOUT &&
              g_wait_park_calls == 1 &&
              g_wait_deadline == (uint64_t)kernel_tick_counter + 1,
              "12a: a finite-timeout wait parks WITH a deadline (absolute "
              "tick, rounded up)");

        /* 12b. an enqueue wakes: the peer's k_chan_send fires
         * cap_wake_chan with the channel id (the real kernel's wake makes
         * a parked waiter re-run the wait), and the consumer's re-poll
         * finds the MSG. */
        const char wm[] = "wake me up\n";
        g_wake_calls = 0;
        g_cur_pid = peer3;
        struct SLSChanSendRequest s12;
        memset(&s12, 0, sizeof(s12));
        s12.chan = peer3_w;
        s12.tag = 0x5A5A;
        s12.payload = (void*)wm;
        s12.payload_len = sizeof(wm) - 1;
        s12.timeout_ns = 0;
        CHECK(sys_sls_chan_send(&s12) == CAP_ERR_OK,
              "12b: peer's k_chan_send enqueues");
        CHECK(g_wake_calls == 1 && g_wake_chan == c3_chan,
              "12b: the enqueue fired cap_wake_chan with the channel id");

        g_cur_pid = cons3;
        g_wait_park_calls = 0;
        memset(&w12, 0, sizeof(w12));
        w12.chans[0] = c3_r;
        w12.n_chans = 1;
        w12.timeout_ns = CH_TIMEOUT_NONE;
        CHECK(sys_sls_chan_wait(&w12) == CAP_ERR_OK &&
              w12.out_idx == 0 && w12.out_kind == CH_KIND_MSG &&
              g_wait_park_calls == 0,
              "12b: the re-poll after the wake finds the MSG (no park needed)");
        char wbuf[64];
        struct SLSChanRecvRequest r12;
        memset(&r12, 0, sizeof(r12));
        memset(wbuf, 0, sizeof(wbuf));
        r12.chan = c3_r;
        r12.buf = wbuf;
        r12.buf_len = sizeof(wbuf);
        r12.n_slots = 8;
        CHECK(sys_sls_chan_recv(&r12) == CAP_ERR_OK &&
              r12.out.kind == CH_KIND_MSG && r12.out.tag == 0x5A5A &&
              r12.out.len == sizeof(wm) - 1 &&
              memcmp(wbuf, wm, sizeof(wm) - 1) == 0,
              "12b: consumer receives the woken message verbatim");

        /* 12c. multi-channel park: waiting on the console R AND the
         * messenger R parks on BOTH resolved channel ids. */
        uint32_t m3_obj = cap_debug_objid(cons3, m3_r);
        uint32_t m3_chan = (m3_obj != 0xFFFFFFFFu)
                               ? cap_objects[m3_obj].chan_id : CAP_CHAN_MAX;
        CHECK(m3_obj != 0xFFFFFFFFu && m3_chan < CAP_CHAN_MAX &&
              m3_chan != c3_chan,
              "12c: consumer3's messenger channel re-derived (distinct chan)");
        g_wait_park_calls = 0;
        g_cur_pid = cons3;
        memset(&w12, 0, sizeof(w12));
        w12.chans[0] = c3_r;
        w12.chans[1] = m3_r;
        w12.n_chans = 2;
        w12.timeout_ns = CH_TIMEOUT_NONE;
        CHECK(sys_sls_chan_wait(&w12) == CAP_ERR_TIMEOUT,
              "12c: empty multi-channel wait returns TIMEOUT via the park hook");
        CHECK(g_wait_park_calls == 1 && g_wait_n == 2 &&
              g_wait_chans[0] == c3_chan && g_wait_chans[1] == m3_chan,
              "12c: the park hook received BOTH resolved channel ids");

        /* 12d. an explicit close wakes: the peer's k_chan_close fires
         * cap_wake_chan; the consumer's re-poll sees the CLOSE event and
         * recv delivers the close body (reason + detail). */
        g_wake_calls = 0;
        g_cur_pid = peer3;
        struct SLSChanCloseRequest c12;
        memset(&c12, 0, sizeof(c12));
        c12.chan = peer3_w;
        c12.reason = CLOSE_PEER;
        c12.detail = 0x1234;
        CHECK(sys_sls_chan_close(&c12) == CAP_ERR_OK,
              "12d: peer's k_chan_close succeeds");
        CHECK(g_wake_calls == 1 && g_wake_chan == c3_chan,
              "12d: the close fired cap_wake_chan with the channel id");

        g_cur_pid = cons3;
        memset(&w12, 0, sizeof(w12));
        w12.chans[0] = c3_r;
        w12.n_chans = 1;
        w12.timeout_ns = CH_TIMEOUT_NONE;
        CHECK(sys_sls_chan_wait(&w12) == CAP_ERR_OK &&
              w12.out_kind == CH_KIND_CLOSE,
              "12d: the re-poll after the close wake sees the CLOSE event");
        memset(&r12, 0, sizeof(r12));
        memset(wbuf, 0, sizeof(wbuf));
        r12.chan = c3_r;
        r12.buf = wbuf;
        r12.buf_len = sizeof(wbuf);
        r12.n_slots = 8;
        CHECK(sys_sls_chan_recv(&r12) == CAP_ERR_OK &&
              r12.out.kind == CH_KIND_CLOSE && r12.out.len == 8 &&
              wbuf[0] == (uint8_t)(CLOSE_PEER & 0xFF) &&
              wbuf[1] == (uint8_t)(CLOSE_PEER >> 8) &&
              wbuf[2] == (uint8_t)(0x1234 & 0xFF) &&
              wbuf[3] == (uint8_t)(0x1234 >> 8),
              "12d: recv delivers the CLOSE_PEER body (reason + detail)");

        /* 12e. a peer-death teardown wakes: a FRESH pair; the consumer
         * parks (hook records), the peer's cap_table_teardown fires
         * cap_wake_chan, and the consumer's re-poll sees CLOSE_PEER_DEAD
         * with the dead pid (the teardown scan also wakes the peer's
         * messenger channel — a no-op there, so the wake count is >= 1). */
        struct Blob p4;
        build_peer_blob(&p4, image_kaddr, "drv.ramdisk.3");
        uint16_t p4_ch = CAP_NONE;
        CHECK(cap_create_sidecar(100, p4.data, p4.len, CAP_NONE, CAP_NONE,
                                 &p4_ch) == 0,
              "death-wake peer (drv.ramdisk.3) spawns");
        uint32_t peer4 = sidecar_registry_resolve("drv.ramdisk.3");
        struct Blob c4;
        build_consumer_blob(&c4, image_kaddr, "drv.consumer.3", "drv.ramdisk.3");
        uint16_t c4_ch = CAP_NONE;
        CHECK(cap_create_sidecar(100, c4.data, c4.len, CAP_NONE, CAP_NONE,
                                 &c4_ch) == 0,
              "death-wake consumer (drv.consumer.3) spawns");
        uint32_t cons4 = sidecar_registry_resolve("drv.consumer.3");
        CHECK(peer4 != 0 && cons4 != 0 && peer4 != cons4,
              "death-wake pair resolved via the registry");
        struct Bib c4bib = bib_parse(host_ptr(BIB_VADDR));
        uint16_t c4_r = c4bib.caps[4].slot;
        uint32_t c4_obj = cap_debug_objid(cons4, c4_r);
        uint32_t c4_chan = (c4_obj != 0xFFFFFFFFu)
                               ? cap_objects[c4_obj].chan_id : CAP_CHAN_MAX;
        CHECK(c4_r != CAP_NONE && c4_obj != 0xFFFFFFFFu && c4_chan < CAP_CHAN_MAX,
              "consumer4's console channel re-derived");

        g_wait_park_calls = 0;
        g_wake_calls = 0;
        g_cur_pid = cons4;
        memset(&w12, 0, sizeof(w12));
        w12.chans[0] = c4_r;
        w12.n_chans = 1;
        w12.timeout_ns = CH_TIMEOUT_NONE;
        CHECK(sys_sls_chan_wait(&w12) == CAP_ERR_TIMEOUT &&
              g_wait_park_calls == 1 && g_wait_chans[0] == c4_chan,
              "12e: consumer4 parks on the console channel");

        kill_sidecar(peer4);
        CHECK(g_wake_calls >= 1 && g_wake_chan == c4_chan,
              "12e: the peer's teardown fired cap_wake_chan with the channel id");
        CHECK(sidecar_registry_resolve("drv.ramdisk.3") == 0,
              "12e: the dead peer stops resolving");

        memset(&w12, 0, sizeof(w12));
        w12.chans[0] = c4_r;
        w12.n_chans = 1;
        w12.timeout_ns = CH_TIMEOUT_NONE;
        CHECK(sys_sls_chan_wait(&w12) == CAP_ERR_OK &&
              w12.out_kind == CH_KIND_CLOSE,
              "12e: the re-poll after the death wake sees CLOSE");
        memset(&r12, 0, sizeof(r12));
        memset(wbuf, 0, sizeof(wbuf));
        r12.chan = c4_r;
        r12.buf = wbuf;
        r12.buf_len = sizeof(wbuf);
        r12.n_slots = 8;
        CHECK(sys_sls_chan_recv(&r12) == CAP_ERR_OK &&
              r12.out.kind == CH_KIND_CLOSE && r12.out.len == 8 &&
              wbuf[0] == (uint8_t)(CLOSE_PEER_DEAD & 0xFF) &&
              wbuf[1] == (uint8_t)(CLOSE_PEER_DEAD >> 8) &&
              wbuf[2] == (uint8_t)(peer4 & 0xFF) &&
              wbuf[3] == (uint8_t)(peer4 >> 8),
              "12e: recv delivers the CLOSE_PEER_DEAD body with the dead pid");
    }

    /* ── 13. blocking k_chan_send: a queue-full send (timeout 0) requests
     * the park; a recv freeing a slot wakes it; a close unblocks a parked
     * sender with STATE ──────────────────────────────────────────────── */
    /* The real kernel parks a queue-full sender (timeout_ns == 0) on the
     * channel and re-runs the send on wake. This section proves the
     * transport half: (a) the 17th send against a full 16-deep queue
     * requests the park with the channel id, the request pointer, and
     * park_syscall = SYS_SLS_CHAN_SEND, (b) a finite-timeout send skips
     * the park, (c) a recv freeing a slot fires cap_wake_chan with the
     * channel id and a fresh send then enqueues (what the woken re-run
     * does), and (d) closing the peer endpoint fires the wake and the
     * parked sender's re-run fails CAP_ERR_STATE instead of hanging. */
    {
        /* A fresh spawn-to-spawn pair (drv.ramdisk.4 → drv.consumer.4). */
        struct Blob p5;
        build_peer_blob(&p5, image_kaddr, "drv.ramdisk.4");
        uint16_t p5_ch = CAP_NONE;
        CHECK(cap_create_sidecar(100, p5.data, p5.len, CAP_NONE, CAP_NONE,
                                 &p5_ch) == 0,
              "blocking-send peer (drv.ramdisk.4) spawns");
        uint32_t peer5 = sidecar_registry_resolve("drv.ramdisk.4");
        struct Blob c5;
        build_consumer_blob(&c5, image_kaddr, "drv.consumer.4", "drv.ramdisk.4");
        uint16_t c5_ch = CAP_NONE;
        CHECK(cap_create_sidecar(100, c5.data, c5.len, CAP_NONE, CAP_NONE,
                                 &c5_ch) == 0,
              "blocking-send consumer (drv.consumer.4) spawns");
        uint32_t cons5 = sidecar_registry_resolve("drv.consumer.4");
        CHECK(peer5 != 0 && cons5 != 0 && peer5 != cons5,
              "blocking-send pair resolved via the registry");
        struct Bib c5bib = bib_parse(host_ptr(BIB_VADDR));
        uint16_t c5_r = c5bib.caps[4].slot;   /* console CHAN_R in cons5 */
        uint32_t c5_obj = cap_debug_objid(cons5, c5_r);
        uint32_t c5_chan = (c5_obj != 0xFFFFFFFFu)
                               ? cap_objects[c5_obj].chan_id : CAP_CHAN_MAX;
        CHECK(c5_r != CAP_NONE && c5_obj != 0xFFFFFFFFu &&
              c5_chan < CAP_CHAN_MAX,
              "consumer5's console channel re-derived");
        /* peer5's console CHAN_W: the peer-end slot of the same object. */
        uint16_t peer5_w = CAP_NONE;
        {
            int ti = table_for_pid(peer5);
            for (int s = 0; s < CAP_TABLE_ENTRIES; s++) {
                uint64_t w = cap_tables[ti].slots[s].word;
                if (!slot_valid(w) || slot_type(w) != CAP_TYPE_CHAN_W) continue;
                uint32_t oid = (uint32_t)((w >> CAP_OBJ_SHIFT) & CAP_OBJ_MASK);
                if (oid == c5_obj) { peer5_w = (uint16_t)s; break; }
            }
        }
        CHECK(peer5_w != CAP_NONE, "peer5's console CHAN_W found in its table");

        /* Fill the consumer's receive queue: CHAN_QUEUE_DEPTH (16) sends
         * succeed; the 17th hits the full queue. */
        const char sfill[] = "filler\n";
        g_wait_park_calls = 0;
        g_wake_calls = 0;
        g_cur_pid = peer5;
        struct SLSChanSendRequest s13;
        memset(&s13, 0, sizeof(s13));
        s13.chan = peer5_w;
        s13.payload = (void*)sfill;
        s13.payload_len = sizeof(sfill) - 1;
        s13.timeout_ns = 0;
        for (int i = 0; i < CHAN_QUEUE_DEPTH; i++) {
            s13.tag = (uint32_t)i;
            CHECK(sys_sls_chan_send(&s13) == CAP_ERR_OK,
                  "13: queue fill send succeeds (16 total)");
        }
        CHECK(g_wait_park_calls == 0 && g_wake_calls == CHAN_QUEUE_DEPTH,
              "13: filling never parks; each enqueue fires the wake");

        /* 13a. the 17th send parks: the hook receives the channel id, the
         * request pointer, park_syscall = SYS_SLS_CHAN_SEND, and deadline
         * 0 (timeout 0 = block until enqueued, forever). */
        g_wait_park_calls = 0;
        g_wait_deadline = 0;
        s13.tag = 0xBAD;
        CHECK(sys_sls_chan_send(&s13) == CAP_ERR_TIMEOUT,
              "13a: queue-full send (timeout 0) returns TIMEOUT when the "
              "park hook cannot park");
        CHECK(g_wait_park_calls == 1 && g_wait_n == 1 &&
              g_wait_chans[0] == c5_chan && g_wait_req == (void*)&s13 &&
              g_wait_syscall == SYS_SLS_CHAN_SEND && g_wait_deadline == 0,
              "13a: the park hook received the channel id, the request "
              "pointer, SYS_SLS_CHAN_SEND as the re-run syscall, and "
              "deadline 0 (block forever)");

        /* 13b. a finite-timeout send parks WITH a deadline (absolute tick,
         * rounded up): the timer ISR wakes it at the deadline and the
         * re-run then returns CAP_ERR_TIMEOUT (exercised in section 14). */
        g_wait_park_calls = 0;
        g_wait_deadline = 0;
        s13.timeout_ns = 1000;
        CHECK(sys_sls_chan_send(&s13) == CAP_ERR_TIMEOUT &&
              g_wait_park_calls == 1 &&
              g_wait_deadline == (uint64_t)kernel_tick_counter + 1,
              "13b: a finite-timeout send parks WITH a deadline");
        s13.timeout_ns = 0;

        /* 13c. a recv freeing a slot wakes: the consumer drains one message
         * → cap_wake_chan fires with the channel id (the real kernel's
         * wake resumes the parked sender); a fresh send now enqueues (what
         * the woken re-run does). */
        g_wake_calls = 0;
        g_cur_pid = cons5;
        struct SLSChanRecvRequest r13;
        char rbuf13[64];
        memset(&r13, 0, sizeof(r13));
        memset(rbuf13, 0, sizeof(rbuf13));
        r13.chan = c5_r;
        r13.buf = rbuf13;
        r13.buf_len = sizeof(rbuf13);
        r13.n_slots = 8;
        CHECK(sys_sls_chan_recv(&r13) == CAP_ERR_OK &&
              r13.out.len == sizeof(sfill) - 1,
              "13c: consumer drains one queued message");
        CHECK(g_wake_calls == 1 && g_wake_chan == c5_chan,
              "13c: the dequeue fired cap_wake_chan with the channel id");

        g_cur_pid = peer5;
        s13.tag = 0xCAFE;
        CHECK(sys_sls_chan_send(&s13) == CAP_ERR_OK,
              "13c: the sender's re-run now enqueues (a slot was freed)");

        /* 13d. a close unblocks a parked sender with STATE: the queue is
         * full again after 13c's send, so park the sender, then the
         * consumer closes its endpoint → the wake fires; the sender's
         * re-run fails CAP_ERR_STATE instead of hanging on the full queue. */
        g_wait_park_calls = 0;
        s13.tag = 0xDEAD;
        CHECK(sys_sls_chan_send(&s13) == CAP_ERR_TIMEOUT &&
              g_wait_park_calls == 1 && g_wait_chans[0] == c5_chan,
              "13d: the sender parks on the re-full queue");

        g_wake_calls = 0;
        g_cur_pid = cons5;
        struct SLSChanCloseRequest c13;
        memset(&c13, 0, sizeof(c13));
        c13.chan = c5_r;
        c13.reason = CLOSE_PEER;
        c13.detail = 0x9;
        CHECK(sys_sls_chan_close(&c13) == CAP_ERR_OK,
              "13d: the consumer closes its endpoint");
        CHECK(g_wake_calls == 1 && g_wake_chan == c5_chan,
              "13d: the close fired cap_wake_chan with the channel id");

        g_cur_pid = peer5;
        CHECK(sys_sls_chan_send(&s13) == CAP_ERR_STATE,
              "13d: the parked sender's re-run after the close → STATE, "
              "nothing enqueued");
    }

    /* ── 14. finite deadlines: a park expires at its absolute deadline ── */
    /* The timer ISR (cap_park_deadline_tick, real kernel) wakes a parked
     * wait/send whose deadline passed; the re-run then returns
     * CAP_ERR_TIMEOUT. This section exercises that logic at the transport
     * level with a test-owned clock: cap_park_deadline_take mimics the
     * real take (return + clear the stored deadline), so a simulated
     * re-run call decides expiry vs re-park for real, and a wrong
     * re-computed deadline is caught. */
    {
        /* A fresh pair (drv.ramdisk.5 → drv.consumer.5). */
        struct Blob p6;
        build_peer_blob(&p6, image_kaddr, "drv.ramdisk.5");
        uint16_t p6_ch = CAP_NONE;
        CHECK(cap_create_sidecar(100, p6.data, p6.len, CAP_NONE, CAP_NONE,
                                 &p6_ch) == 0,
              "deadline peer (drv.ramdisk.5) spawns");
        uint32_t peer6 = sidecar_registry_resolve("drv.ramdisk.5");
        struct Blob c6;
        build_consumer_blob(&c6, image_kaddr, "drv.consumer.5", "drv.ramdisk.5");
        uint16_t c6_ch = CAP_NONE;
        CHECK(cap_create_sidecar(100, c6.data, c6.len, CAP_NONE, CAP_NONE,
                                 &c6_ch) == 0,
              "deadline consumer (drv.consumer.5) spawns");
        uint32_t cons6 = sidecar_registry_resolve("drv.consumer.5");
        CHECK(peer6 != 0 && cons6 != 0 && peer6 != cons6,
              "deadline pair resolved via the registry");
        struct Bib c6bib = bib_parse(host_ptr(BIB_VADDR));
        uint16_t c6_r = c6bib.caps[4].slot;   /* console CHAN_R in cons6 */
        uint32_t c6_obj = cap_debug_objid(cons6, c6_r);
        uint32_t c6_chan = (c6_obj != 0xFFFFFFFFu)
                               ? cap_objects[c6_obj].chan_id : CAP_CHAN_MAX;
        CHECK(c6_r != CAP_NONE && c6_obj != 0xFFFFFFFFu &&
              c6_chan < CAP_CHAN_MAX,
              "consumer6's console channel re-derived");
        uint16_t peer6_w = CAP_NONE;
        {
            int ti = table_for_pid(peer6);
            for (int s = 0; s < CAP_TABLE_ENTRIES; s++) {
                uint64_t w = cap_tables[ti].slots[s].word;
                if (!slot_valid(w) || slot_type(w) != CAP_TYPE_CHAN_W) continue;
                uint32_t oid = (uint32_t)((w >> CAP_OBJ_SHIFT) & CAP_OBJ_MASK);
                if (oid == c6_obj) { peer6_w = (uint16_t)s; break; }
            }
        }
        CHECK(peer6_w != CAP_NONE, "peer6's console CHAN_W found in its table");

        /* 14a. wait expiry: a 1-tick-deadline wait parks at absolute tick
         * T+1; advancing the clock past it makes the RE-RUN (the real
         * kernel's timer ISR wake) return CAP_ERR_TIMEOUT WITHOUT
         * re-parking. */
        kernel_tick_counter = 1000;
        g_wait_park_calls = 0;
        g_wait_deadline = 0;
        g_cur_pid = cons6;
        struct SLSChanWaitRequest w14;
        memset(&w14, 0, sizeof(w14));
        w14.chans[0] = c6_r;
        w14.n_chans = 1;
        w14.timeout_ns = 1;   /* 1 ns → rounded up to one ~10 ms tick */
        CHECK(sys_sls_chan_wait(&w14) == CAP_ERR_TIMEOUT &&
              g_wait_park_calls == 1 && g_wait_deadline == 1001,
              "14a: a 1-tick-deadline wait parks at absolute tick 1001");

        kernel_tick_counter = 1005;   /* past the deadline */
        g_wait_park_calls = 0;
        CHECK(sys_sls_chan_wait(&w14) == CAP_ERR_TIMEOUT &&
              g_wait_park_calls == 0,
              "14a: the re-run after the deadline returns TIMEOUT without "
              "re-parking");

        /* 14b. re-parks never extend the deadline: woken early (before the
         * deadline) with nothing ready, the re-run re-parks with the SAME
         * absolute deadline — not now + timeout again. */
        kernel_tick_counter = 2000;
        g_wait_park_calls = 0;
        g_wait_deadline = 0;
        memset(&w14, 0, sizeof(w14));
        w14.chans[0] = c6_r;
        w14.n_chans = 1;
        w14.timeout_ns = 1000000000ULL;   /* 1 s → 100 ticks */
        CHECK(sys_sls_chan_wait(&w14) == CAP_ERR_TIMEOUT &&
              g_wait_park_calls == 1 && g_wait_deadline == 2100,
              "14b: a 1 s-deadline wait parks at absolute tick 2100");

        kernel_tick_counter = 2050;   /* early wake: 50 ticks to go */
        g_wait_park_calls = 0;
        CHECK(sys_sls_chan_wait(&w14) == CAP_ERR_TIMEOUT &&
              g_wait_park_calls == 1 && g_wait_deadline == 2100,
              "14b: the early re-run re-parks with the ORIGINAL deadline "
              "(2100), never extending it");

        /* 14c. send expiry: a full queue + finite deadline → park; the
         * re-run after the deadline returns CAP_ERR_TIMEOUT (still full). */
        kernel_tick_counter = 3000;
        g_wait_park_calls = 0;
        g_wait_deadline = 0;
        g_cur_pid = peer6;
        struct SLSChanSendRequest s14;
        memset(&s14, 0, sizeof(s14));
        s14.chan = peer6_w;
        s14.payload = (void*)"x";
        s14.payload_len = 1;
        s14.timeout_ns = 0;
        for (int i = 0; i < CHAN_QUEUE_DEPTH; i++) {
            s14.tag = (uint32_t)i;
            CHECK(sys_sls_chan_send(&s14) == CAP_ERR_OK,
                  "14c: queue filled (16) for the send-deadline tests");
        }
        s14.tag = 0xBEEF;
        s14.timeout_ns = 1;   /* 1 ns → one tick */
        CHECK(sys_sls_chan_send(&s14) == CAP_ERR_TIMEOUT &&
              g_wait_park_calls == 1 && g_wait_deadline == 3001,
              "14c: a full-queue send with a 1-tick deadline parks at 3001");

        kernel_tick_counter = 3005;
        g_wait_park_calls = 0;
        CHECK(sys_sls_chan_send(&s14) == CAP_ERR_TIMEOUT &&
              g_wait_park_calls == 0,
              "14c: the send re-run after the deadline returns TIMEOUT "
              "without re-parking");

        /* 14d. send re-parks never extend either: early wake → re-park
         * with the original absolute deadline. */
        s14.timeout_ns = 1000000000ULL;   /* 1 s → 100 ticks */
        kernel_tick_counter = 4000;
        g_wait_park_calls = 0;
        g_wait_deadline = 0;
        s14.tag = 0xFACE;
        CHECK(sys_sls_chan_send(&s14) == CAP_ERR_TIMEOUT &&
              g_wait_park_calls == 1 && g_wait_deadline == 4100,
              "14d: a full-queue send with a 1 s deadline parks at 4100");

        kernel_tick_counter = 4050;   /* early wake */
        g_wait_park_calls = 0;
        CHECK(sys_sls_chan_send(&s14) == CAP_ERR_TIMEOUT &&
              g_wait_park_calls == 1 && g_wait_deadline == 4100,
              "14d: the early send re-run re-parks with the ORIGINAL "
              "deadline (4100)");

        /* 14e. ready beats deadline: a queue with data returns the MSG even
         * though the deadline has already passed (the poll runs before the
         * expiry check — a wake that finds its condition met never times
         * out). */
        kernel_tick_counter = 5000;
        g_wait_park_calls = 0;
        g_wait_deadline = 0;
        g_cur_pid = cons6;
        memset(&w14, 0, sizeof(w14));
        w14.chans[0] = c6_r;
        w14.n_chans = 1;
        w14.timeout_ns = 1;
        /* consumer6's queue holds the 16 messages 14c filled — ready. */
        CHECK(sys_sls_chan_wait(&w14) == CAP_ERR_OK &&
              w14.out_kind == CH_KIND_MSG && w14.out_idx == 0 &&
              g_wait_park_calls == 0,
              "14e: a ready queue returns the MSG even with an expired "
              "deadline (ready beats timeout)");

        /* 14f. TIMEOUT_NONE on a send = block forever (deadline 0): the SDK
         * passes TIMEOUT_NONE (u64::MAX) for every blocking send, so the
         * kernel must treat it exactly like timeout 0 — never as a
         * wrapped sub-tick deadline (which would make a full queue expire
         * instantly). The queue is still full and open from 14c/14d. */
        g_wait_park_calls = 0;
        g_wait_deadline = 0;
        g_cur_pid = peer6;
        s14.tag = 0xFEED;
        s14.timeout_ns = CH_TIMEOUT_NONE;
        CHECK(sys_sls_chan_send(&s14) == CAP_ERR_TIMEOUT &&
              g_wait_park_calls == 1 && g_wait_deadline == 0,
              "14f: a TIMEOUT_NONE send on a full queue parks with deadline "
              "0 (block forever)");

        /* 14g. a huge-but-finite timeout saturates instead of wrapping: a
         * near-u64::MAX deadline must record as an enormous tick count,
         * not as now + (wrapped-to-0 ticks) = now, which would expire
         * instantly. */
        g_wait_park_calls = 0;
        g_wait_deadline = 0;
        s14.tag = 0xBEE2;
        s14.timeout_ns = UINT64_MAX - 1;   /* just below TIMEOUT_NONE */
        CHECK(sys_sls_chan_send(&s14) == CAP_ERR_TIMEOUT &&
              g_wait_park_calls == 1 &&
              g_wait_deadline > (uint64_t)kernel_tick_counter,
              "14g: a near-u64::MAX timeout saturates to a huge deadline "
              "(not a wrapped now+0)");
    }

    /* ── 15. SYS_SLS_CREATE_SIDECAR (310): the FULL syscall wrapper — the
     * path the init sidecar actually calls (kabi.rs k_create_sidecar → this
     * syscall, via RealKernel::create_sidecar). Unlike the direct
     * cap_create_sidecar calls above, this drives the wrapper end to end:
     * it must fill the request's out_ch_r / out_ch_w (the parent's
     * messenger CHAN_R and CHAN_W), register the sidecar name, and the
     * messenger must carry data across the spawn. ──────────────────────── */
    {
        g_cur_pid = 100;
        /* Only the parent matters for this final section: cap tables are a
         * fixed pool (CAP_TABLE_MAX == PROC_MAX == 16, kernel table at
         * index 0 leaves 15 process tables), so free every earlier sidecar
         * or the syscall spawn below hits CAP_ETABLEFULL. */
        for (int _k = 0; _k < PROC_MAX; _k++) {
            if (proc_table[_k].active && proc_table[_k].pid != 100)
                kill_sidecar(proc_table[_k].pid);
        }
        struct Blob sysblob;
        build_peer_blob(&sysblob, image_kaddr, "drv.syscall.0");

        struct SLSCreateSidecarRequest creq;
        memset(&creq, 0, sizeof(creq));
        creq.manifest = sysblob.data;
        creq.manifest_len = sysblob.len;
        creq.ch_w_idx = CAP_NONE;
        creq.console_w_idx = CAP_NONE;
        creq.out_ch_r = CAP_NONE;
        creq.out_ch_w = CAP_NONE;

        uint64_t ret = sys_sls_create_sidecar(&creq);
        CHECK(ret < 0x10000,
              "syscall returns a slot-sized handle, not a negative error");
        CHECK(creq.out_ch_r == (uint16_t)ret && creq.out_ch_r != CAP_NONE,
              "out_ch_r filled with the parent's messenger CHAN_R");
        CHECK(creq.out_ch_w != CAP_NONE,
              "out_ch_w filled with the parent's messenger CHAN_W");

        /* Both ends must be valid caps in the parent's (pid 100) table
         * wrapping the SAME channel object. */
        int pti = cap_table_index(100);
        uint64_t wr = cap_tables[pti].slots[creq.out_ch_r].word;
        uint64_t ww = cap_tables[pti].slots[creq.out_ch_w].word;
        CHECK(cap_word_valid(wr) &&
              ((wr >> CAP_TYPE_SHIFT) & CAP_TYPE_MASK) == CAP_TYPE_CHAN_R,
              "out_ch_r slot is a CHAN_R cap");
        CHECK(cap_word_valid(ww) &&
              ((ww >> CAP_TYPE_SHIFT) & CAP_TYPE_MASK) == CAP_TYPE_CHAN_W,
              "out_ch_w slot is a CHAN_W cap");
        uint32_t parent_oid = CAP_OBJECT_MAX;
        if (cap_word_valid(wr) && cap_word_valid(ww)) {
            uint32_t or_ = (uint32_t)((wr >> CAP_OBJ_SHIFT) & CAP_OBJ_MASK);
            uint32_t ow = (uint32_t)((ww >> CAP_OBJ_SHIFT) & CAP_OBJ_MASK);
            parent_oid = or_;
            CHECK(cap_objects[or_].kind == CAP_OBJ_KIND_CHAN &&
                  cap_objects[ow].kind == CAP_OBJ_KIND_CHAN &&
                  cap_objects[or_].chan_id == cap_objects[ow].chan_id,
                  "both ends wrap the same channel object (the messenger)");
        }

        /* The spawned sidecar is registered under its manifest name. */
        uint32_t child_pid = sidecar_registry_resolve("drv.syscall.0");
        CHECK(child_pid != 0 && child_pid != 100,
              "syscall-spawned sidecar registered in the sidecar registry");

        /* The messenger is LIVE across the spawn: find the child's CHAN_R
         * on the same channel, send over the parent's out_ch_w, and have
         * the CHILD receive it through the chan syscalls. */
        if (child_pid != 0) {
            int cti2 = cap_table_index(child_pid);
            uint16_t child_r = CAP_NONE;
            if (cti2 >= 0 && parent_oid < CAP_OBJECT_MAX) {
                for (int s = 0; s < CAP_TABLE_ENTRIES; s++) {
                    uint64_t w = cap_tables[cti2].slots[s].word;
                    if (!cap_word_valid(w)) continue;
                    if (((w >> CAP_TYPE_SHIFT) & CAP_TYPE_MASK) != CAP_TYPE_CHAN_R)
                        continue;
                    uint32_t oid = (uint32_t)((w >> CAP_OBJ_SHIFT) & CAP_OBJ_MASK);
                    if (cap_objects[oid].kind == CAP_OBJ_KIND_CHAN &&
                        cap_objects[oid].chan_id == cap_objects[parent_oid].chan_id) {
                        child_r = (uint16_t)s;
                        break;
                    }
                }
            }
            CHECK(child_r != CAP_NONE, "child's messenger CHAN_R found");

            g_cur_pid = 100;
            struct SLSChanSendRequest csr;
            memset(&csr, 0, sizeof(csr));
            csr.chan = creq.out_ch_w;
            csr.tag = 0x5150;
            csr.flags = 0;
            csr.payload = (void*)"ping";
            csr.payload_len = 4;
            csr.timeout_ns = 0;
            CHECK(sys_sls_chan_send(&csr) == CAP_ERR_OK,
                  "parent sends over the syscall-returned messenger CHAN_W");

            g_cur_pid = child_pid;
            struct SLSChanRecvRequest crr;
            memset(&crr, 0, sizeof(crr));
            char cbuf[64];
            memset(cbuf, 0, sizeof(cbuf));
            crr.chan = child_r;
            crr.buf = cbuf;
            crr.buf_len = sizeof(cbuf);
            crr.n_slots = 8;
            CHECK(sys_sls_chan_recv(&crr) == CAP_ERR_OK &&
                  crr.out.kind == CH_KIND_MSG && crr.out.tag == 0x5150 &&
                  crr.out.len == 4 && memcmp(cbuf, "ping", 4) == 0,
                  "the child receives the parent's messenger message");
        }
    }

    if (g_fail == 0) printf("\nALL PASS\n");
    else             printf("\n%d FAILURE(S)\n", g_fail);
    return g_fail != 0;
}
