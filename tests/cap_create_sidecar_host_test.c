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
 *      on both a MEM and a CHAN cap.
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

/* ─── Process-table pieces cap_create_sidecar reaches for ────────────────── */
/* proc_table/proc_count/alloc_pid normally live in process.c; here they are
 * test-owned so the parent process can be planted directly. */
struct ProcessDescriptor proc_table[PROC_MAX];
uint32_t proc_count = 0;

/* Mirrors the real alloc_pid(): a fresh pid with no collision against
 * active procs (the parent is 100, so the first child is 101). */
uint32_t alloc_pid(void) {
    uint32_t best = 0;
    for (int i = 0; i < PROC_MAX; i++)
        if (proc_table[i].active && proc_table[i].pid > best)
            best = proc_table[i].pid;
    return best + 1;
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

#define FRAME_POOL_MAX 64
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

static uint64_t* pml4_child(uint64_t* parent, size_t idx) {
    if (parent[idx] & USER_PTE_PRESENT)
        return (uint64_t*)(uintptr_t)(parent[idx] & USER_PTE_FRAME_MASK);
    uint64_t* child = calloc(512, sizeof(uint64_t));
    parent[idx] = (uint64_t)(uintptr_t)child
                | USER_PTE_PRESENT | USER_PTE_WRITE | USER_PTE_USER;
    return child;
}

uint64_t user_clone_page_table(void) {
    g_pml4 = calloc(512, sizeof(uint64_t));
    return (uint64_t)(uintptr_t)g_pml4;
}

void user_map_page(uint64_t* pml4, uint64_t vaddr, uint64_t paddr, uint64_t flags) {
    uint64_t* pdpt = pml4_child(pml4, (vaddr >> 39) & 0x1FF);
    uint64_t* pd   = pml4_child(pdpt, (vaddr >> 30) & 0x1FF);
    uint64_t* pt   = pml4_child(pd,   (vaddr >> 21) & 0x1FF);
    pt[(vaddr >> 12) & 0x1FF] = (paddr & USER_PTE_FRAME_MASK) | flags;
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
     * kernel parser and user/proto/src/manifest.rs share. */
    blob_record(b, SIDECAR_TAG_CAP_MEM, 6 + 19);
    blob_u16(b, 6);
    blob_put(b, "budget", 6);
    blob_u64(b, MEM_BUDGET_BASE);
    blob_u64(b, MEM_BUDGET_BYTES);
    blob_put(b, "\x03", 1);

    blob_record(b, SIDECAR_TAG_CAP_MEM, 3 + 19);
    blob_u16(b, 3);
    blob_put(b, "dma", 3);
    blob_u64(b, MEM_DMA_BASE);
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
    CHECK((img_pte & (USER_PTE_PRESENT | USER_PTE_USER | USER_PTE_WRITE | USER_PTE_NOEXEC))
            == (USER_PTE_PRESENT | USER_PTE_USER),
          "image pages: present+user, no write, executable");
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
              ireq.out.base == MEM_BUDGET_BASE &&
              ireq.out.len == MEM_BUDGET_BYTES,
              "k_cap_info (315): budget MEM cap base/len/rights");
        memset(&ireq, 0, sizeof(ireq));
        ireq.handle = cb9.caps[4].slot;   /* console CHAN_R */
        CHECK(sys_sls_cap_info(&ireq) == CAP_ERR_OK &&
              ireq.out.ty == CAP_TYPE_CHAN_R &&
              ireq.out.base == 0 && ireq.out.len == 0,
              "k_cap_info on the console CHAN cap");
    }

    if (g_fail == 0) printf("\nALL PASS\n");
    else             printf("\n%d FAILURE(S)\n", g_fail);
    return g_fail != 0;
}
