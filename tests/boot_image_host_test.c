/* tests/boot_image_host_test.c — drives the PURE CORE of the Phase 5
 * boot-time sidecar loader (kernel/boot_image.c) against the REAL,
 * unmodified file: the newc walk, the init-manifest reader (kernel wire
 * format, CRC-verified), the archive/manifest cross-checks, and the device
 * registry builder.
 *
 * The test builds a synthetic initrd byte-for-byte the way the Rust builder
 * (user/bootimage/) does — a newc archive whose entries are packed
 * manifests with the 8-byte image_kaddr footer — so the loader is exercised
 * against the exact wire format it will consume in production. The glue
 * half (boot_image_capture_mb2 / launch_init_sidecar) is kernel-only
 * (multiboot2, frame_pool, cap_create_sidecar) and is verified by
 * construction; the stubs below exist only so this file links.
 *
 * Build and run:
 *   gcc -std=c11 -Wall -Wextra -I . -I kernel \
 *       -o /tmp/boot_image_host_test \
 *       tests/boot_image_host_test.c kernel/boot_image.c
 *   /tmp/boot_image_host_test
 */
#include "kernel/boot_image.h"
#include "kernel/cap.h"          /* SIDECAR_* wire constants */
#include "kernel/process.h"      /* proc_table, PROC_BLOCKED */
#include "tests/process_host_stubs.h"   /* per_cpu_data (weak) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* ─── Link stubs for the glue half (never called by these tests) ─────────── */
void kernel_serial_print(const char* s) { (void)s; }
void kernel_serial_printf(const char* fmt, ...) { (void)fmt; }
void frame_pool_reserve_range(uint64_t lo, uint64_t hi) { (void)lo; (void)hi; }
int cap_create_sidecar(uint32_t pp, const void* m, uint32_t ml,
                       uint16_t cw, uint16_t ccw, uint16_t* o) {
    (void)pp; (void)m; (void)ml; (void)cw; (void)ccw;
    if (o) *o = 0xFFFF;
    return 0;
}
uint32_t pci_read_config(uint8_t b, uint8_t s, uint8_t f, uint8_t o) {
    (void)b; (void)f; (void)o; (void)s;
    return 0xFFFFFFFFu;
}
struct ProcessDescriptor proc_table[PROC_MAX];
void kernel_enter_sidecar(uint64_t* rsp_save, uint64_t* cr3_save,
                          uint64_t cr3, uint64_t rip, uint64_t rsp,
                          uint64_t bib_vaddr) {
    (void)rsp_save; (void)cr3_save; (void)cr3; (void)rip; (void)rsp;
    (void)bib_vaddr;
}

/* ─── test harness ─────────────────────────────────────────────────────────── */
static int g_pass = 0, g_fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) { g_pass++; } \
    else { g_fail++; printf("FAIL: %s (line %d)\n", msg, __LINE__); } \
} while (0)

/* ─── synthetic boot image (mirrors user/bootimage/) ──────────────────────── */

/* IEEE CRC-32 — same algorithm as boot_image.c's boot_crc32. */
static uint32_t crc32(const uint8_t* data, uint32_t len) {
    uint32_t crc = 0xFFFFFFFFu;
    for (uint32_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++)
            crc = (crc >> 1) ^ (0xEDB88320u & (-(int)(crc & 1)));
    }
    return crc ^ 0xFFFFFFFFu;
}

struct Blob { uint8_t data[65536]; uint32_t len; };

static void blob_u16(struct Blob* b, uint16_t v) {
    b->data[b->len++] = (uint8_t)(v & 0xFF);
    b->data[b->len++] = (uint8_t)(v >> 8);
}
static void blob_u32(struct Blob* b, uint32_t v) {
    b->data[b->len++] = (uint8_t)(v & 0xFF);
    b->data[b->len++] = (uint8_t)((v >> 8) & 0xFF);
    b->data[b->len++] = (uint8_t)((v >> 16) & 0xFF);
    b->data[b->len++] = (uint8_t)((v >> 24) & 0xFF);
}
static void blob_u64(struct Blob* b, uint64_t v) {
    for (int i = 0; i < 8; i++) b->data[b->len++] = (uint8_t)(v >> (8 * i));
}
static void blob_bytes(struct Blob* b, const void* p, uint32_t n) {
    memcpy(b->data + b->len, p, n);
    b->len += n;
}
static void blob_record(struct Blob* b, uint16_t tag, uint16_t rlen) {
    blob_u16(b, tag);
    blob_u16(b, rlen);
    /* caller appends rlen bytes next */
}
static void blob_name_payload(struct Blob* b, const char* s) {
    uint16_t n = (uint16_t)strlen(s);
    blob_u16(b, n);
    blob_bytes(b, s, n);
}

/* Build the init manifest: header + 12 records + image_kaddr footer, with
 * total_len/CRC patched — the exact wire format boot_image.c reads and the
 * kernel's cap_create_sidecar parses. Addresses match the layout the Rust
 * builder assigns for a 0x2000-byte init and 0x4000-byte dm image:
 *   init.image 0x20000000, init.heap 0x20002000 (16 MiB),
 *   dm.image 0x20102000, dm.heap 0x20106000 (256 KiB),
 *   registry 0x20146000 (4 KiB). */
enum {
    IMG_INIT_KADDR    = 0x20000000ULL,
    IMG_DM_KADDR      = 0x20102000ULL,
    IMG_POSIX_KADDR   = 0x20106000ULL,
    IMG_REG_KADDR     = 0x20116000ULL,
    IMG_DM_SIZE       = 0x4000u,
    IMG_POSIX_SIZE    = 0x1000u,
    IMG_REG_SIZE      = 0x1000u,
    IMG_INIT_SIZE     = 0x2000u,
};

static void build_init_manifest(struct Blob* b) {
    memset(b, 0, sizeof(*b));
    blob_bytes(b, SIDECAR_MANIFEST_MAGIC, 8);
    blob_u16(b, SIDECAR_MANIFEST_VERSION_MAJOR);
    blob_u16(b, 0);                       /* version_minor */
    uint32_t rc_off = b->len;             /* record_count patched below */
    blob_u16(b, 0);
    blob_u16(b, 0);                       /* flags */
    blob_u32(b, 0);                       /* total_len patched */
    blob_u32(b, 0);                       /* crc patched */
    uint16_t rc = 0;

    blob_record(b, 0x0001, 15 + 2); rc++; /* personality (name_len + 15) */
    blob_name_payload(b, "aerosls.init.v1");
    blob_record(b, SIDECAR_TAG_NAME, 14 + 2); rc++;  /* name_len + 14 */
    blob_name_payload(b, "aerosls.init.0");
    blob_record(b, SIDECAR_TAG_IMAGE, 24); rc++;
    blob_u64(b, 0);                       /* entry_offset */
    blob_u32(b, 0);                       /* blob_offset (informational) */
    blob_u32(b, IMG_INIT_SIZE);           /* image_size */
    blob_u64(b, 0);                       /* in-record image_kaddr: footer */
    blob_record(b, 0x0003, 16); rc++;     /* budget */
    blob_u64(b, 16u * 1024 * 1024);       /* mem_bytes */
    blob_u32(b, 128 * 1024);              /* stack_bytes */
    blob_u32(b, 4 * 1024 * 1024);         /* heap_initial */
    blob_record(b, 0x0004, 3); rc++;      /* cpu: share u16 + preemptible u8 */
    blob_u16(b, 300);
    blob_bytes(b, "\x00", 1);
    blob_record(b, 0x0005, 10); rc++;     /* limits: 5 x u16 */
    blob_u16(b, 1); blob_u16(b, 32); blob_u16(b, 64);
    blob_u16(b, 32); blob_u16(b, 16);

    /* budget MEM cap */
    blob_record(b, SIDECAR_TAG_CAP_MEM, 6 + 19); rc++;
    blob_name_payload(b, "budget");
    blob_u64(b, 0x20002000ULL);
    blob_u64(b, 16u * 1024 * 1024);
    blob_bytes(b, "\x03", 1);
    /* console CHAN cap */
    blob_record(b, 0x0007, 7 + 20 + 6); rc++;
    blob_name_payload(b, "console");
    blob_name_payload(b, "kernel.debug.console");
    blob_bytes(b, "\x07\x00", 2);
    /* device_registry MEM cap */
    blob_record(b, SIDECAR_TAG_CAP_MEM, 15 + 19); rc++;
    blob_name_payload(b, "device_registry");
    blob_u64(b, IMG_REG_KADDR);
    blob_u64(b, IMG_REG_SIZE);
    blob_bytes(b, "\x01", 1);
    /* dm.image MEM cap */
    blob_record(b, SIDECAR_TAG_CAP_MEM, 8 + 19); rc++;
    blob_name_payload(b, "dm.image");
    blob_u64(b, IMG_DM_KADDR);
    blob_u64(b, IMG_DM_SIZE);
    blob_bytes(b, "\x01", 1);
    /* posix.image MEM cap */
    blob_record(b, SIDECAR_TAG_CAP_MEM, 11 + 19); rc++;
    blob_name_payload(b, "posix.image");
    blob_u64(b, IMG_POSIX_KADDR);
    blob_u64(b, IMG_POSIX_SIZE);
    blob_bytes(b, "\x01", 1);

    blob_record(b, 0x0008, 13); rc++;     /* bootstrap: name_len + 7 + 4 */
    blob_name_payload(b, "console");
    blob_bytes(b, "\x01", 1);            /* log_level */
    blob_bytes(b, "\x00\x00\x00", 3);    /* pad */
    blob_record(b, 0x0009, 4); rc++;      /* flags */
    blob_u32(b, 0);

    b->data[rc_off] = (uint8_t)(rc & 0xFF);
    b->data[rc_off + 1] = (uint8_t)(rc >> 8);

    /* Footer: image_kaddr. */
    blob_u64(b, IMG_INIT_KADDR);

    /* Patch total_len + body CRC to cover records + footer. */
    uint32_t total = b->len;
    memcpy(b->data + 16, &total, 4);
    uint32_t crc = crc32(b->data + SIDECAR_MANIFEST_HEADER_LEN,
                         total - SIDECAR_MANIFEST_HEADER_LEN);
    memcpy(b->data + 20, &crc, 4);
}

/* newc writer — the subset boot_image.c's boot_newc_find walks. */
static void newc_entry(struct Blob* b, const char* name, const void* data,
                       uint32_t dlen) {
    char hdr[110];
    uint32_t ns = (uint32_t)strlen(name) + 1;
    memset(hdr, '0', sizeof(hdr));
    memcpy(hdr + 0, "070701", 6);
    snprintf(hdr + 6,  9, "%08x", 0u);            /* c_ino */
    snprintf(hdr + 14, 9, "%08x", 0100644u);      /* c_mode */
    snprintf(hdr + 22, 9, "%08x", 0u);            /* uid */
    snprintf(hdr + 30, 9, "%08x", 0u);            /* gid */
    snprintf(hdr + 38, 9, "%08x", 1u);            /* nlink */
    snprintf(hdr + 46, 9, "%08x", 0u);            /* mtime */
    snprintf(hdr + 54, 9, "%08x", dlen);          /* filesize */
    snprintf(hdr + 62, 9, "%08x", 0u);            /* devmajor */
    snprintf(hdr + 70, 9, "%08x", 0u);            /* devminor */
    snprintf(hdr + 78, 9, "%08x", 0u);            /* rdevmajor */
    snprintf(hdr + 86, 9, "%08x", 0u);            /* rdevminor */
    snprintf(hdr + 94, 9, "%08x", ns);            /* namesize (incl. NUL) */
    snprintf(hdr + 102, sizeof(hdr) - 102, "%08x", 0u); /* check */
    blob_bytes(b, hdr, sizeof(hdr));
    blob_bytes(b, name, ns);
    /* Pad namesize to 4 bytes (matches Rust newc.rs align4). */
    { uint32_t np = ((4 - (ns & 3)) & 3);
      for (uint32_t p = 0; p < np; p++) b->data[b->len++] = 0; }
    blob_bytes(b, data, dlen);
    /* Pad data to 4 bytes (matches Rust newc.rs align4). */
    { uint32_t dp = ((4 - (dlen & 3)) & 3);
      for (uint32_t p = 0; p < dp; p++) b->data[b->len++] = 0; }
}

static void newc_finish(struct Blob* b) {
    newc_entry(b, "TRAILER!!!", 0, 0);
}

static void build_archive(struct Blob* out, struct Blob* init_manifest) {
    uint8_t init_bin[IMG_INIT_SIZE];
    uint8_t dm_bin[IMG_DM_SIZE];
    uint8_t posix_bin[IMG_POSIX_SIZE];
    for (uint32_t i = 0; i < IMG_INIT_SIZE; i++) init_bin[i] = (uint8_t)(0xAA + i);
    for (uint32_t i = 0; i < IMG_DM_SIZE; i++)   dm_bin[i]   = (uint8_t)(0xBB + i);
    for (uint32_t i = 0; i < IMG_POSIX_SIZE; i++) posix_bin[i] = (uint8_t)(0xCC + i);
    memset(out, 0, sizeof(*out));
    newc_entry(out, BOOT_INIT_BIN_PATH, init_bin, sizeof(init_bin));
    newc_entry(out, BOOT_INIT_MANIFEST_PATH, init_manifest->data, init_manifest->len);
    newc_entry(out, BOOT_DM_BIN_PATH, dm_bin, sizeof(dm_bin));
    newc_entry(out, BOOT_DM_MANIFEST_PATH, "fake", 4);
    newc_entry(out, BOOT_POSIX_BIN_PATH, posix_bin, sizeof(posix_bin));
    newc_entry(out, BOOT_POSIX_MANIFEST_PATH, "fake", 4);
    newc_entry(out, BOOT_LAYOUT_PATH, "AEROSLS-BOOT-LAYOUT 1\n", 22);
    newc_finish(out);
}

/* ─── fake PCI scan for the registry tests ─────────────────────────────────── */
static int fake_scan(int slot, struct BootDeviceEntry* e) {
    if (slot == 0) {
        memset(e, 0, sizeof(*e));
        e->class_code = 0x01; e->subclass = 0x08;   /* NVMe */
        e->vendor_id = 0x144D; e->device_id = 0xA808;
        e->bar0_phys = 0xFEBF0000; e->irq_line = 11; e->is_64bit_bar = 1;
        e->pci_slot = 0;
        return 0;
    }
    if (slot == 1) {
        memset(e, 0, sizeof(*e));
        e->class_code = 0x02; e->subclass = 0x00;   /* ethernet */
        e->vendor_id = 0x8086; e->device_id = 0x100E;
        e->bar0_phys = 0xFEBE0000; e->pci_slot = 1;
        return 0;
    }
    if (slot == 3) {
        memset(e, 0, sizeof(*e));
        e->class_code = 0x03; e->subclass = 0x00;   /* VGA — no driver yet */
        e->pci_slot = 3;
        return 0;
    }
    return -1;   /* empty slot */
}

/* ─── main ─────────────────────────────────────────────────────────────────── */
int main(void) {
    struct Blob manifest;
    build_init_manifest(&manifest);
    struct Blob archive;
    build_archive(&archive, &manifest);    /* ── 1. boot_newc_find: walk + padding + terminator ─────────────────── */
    {
        uint32_t off = 0, size = 0;
        CHECK(boot_newc_find(archive.data, archive.len, BOOT_INIT_BIN_PATH,
                             &off, &size) == 0, "init.bin found");
        CHECK(size == IMG_INIT_SIZE, "init.bin size");
        CHECK(archive.data[off] == 0xAA && archive.data[off + 1] == 0xAB,
              "init.bin data at the found offset");
        CHECK(boot_newc_find(archive.data, archive.len, BOOT_INIT_MANIFEST_PATH,
                             &off, &size) == 0, "init.manifest found");
        CHECK(size == manifest.len, "manifest size matches the writer");
        CHECK(memcmp(archive.data + off, manifest.data, manifest.len) == 0,
              "manifest bytes round-trip");
        CHECK(boot_newc_find(archive.data, archive.len, BOOT_DM_BIN_PATH,
                             &off, &size) == 0 && size == IMG_DM_SIZE,
              "dm.bin found");
        CHECK(boot_newc_find(archive.data, archive.len, BOOT_LAYOUT_PATH,
                             &off, &size) == 0, "layout found");
        CHECK(boot_newc_find(archive.data, archive.len, "boot/nope.bin",
                             &off, &size) == BOOT_ERR_NOTFOUND,
              "missing entry -> NOTFOUND (terminator stops the walk)");
    }

    /* ── 2. boot_image_parse: fields + cross-checks ─────────────────────── */
    {
        uint8_t manifest_out[2048];
        uint32_t mlen = 0;
        struct BootImageInfo info;
        int r = boot_image_parse(archive.data, archive.len, manifest_out,
                                 sizeof(manifest_out), &mlen, &info);
        CHECK(r == 0, "parse succeeds");
        CHECK(mlen == manifest.len, "manifest length out");
        CHECK(memcmp(manifest_out, manifest.data, manifest.len) == 0,
              "manifest copied out verbatim");
        CHECK(info.init_kaddr == IMG_INIT_KADDR, "init image kaddr = footer");
        CHECK(info.init_size == IMG_INIT_SIZE, "init size from IMAGE record");
        CHECK(info.dm_kaddr == IMG_DM_KADDR, "dm kaddr from dm.image cap");
        CHECK(info.dm_size == IMG_DM_SIZE, "dm size from dm.image cap");
        CHECK(info.reg_kaddr == IMG_REG_KADDR, "registry kaddr from cap");
        CHECK(info.reg_size == IMG_REG_SIZE, "registry size from cap");
        CHECK(info.boot_base == IMG_INIT_KADDR, "boot_base = init kaddr");
        CHECK(info.boot_total == (IMG_REG_KADDR + IMG_REG_SIZE) - IMG_INIT_KADDR,
              "boot_total = span to registry end");
        CHECK(info.init_bin_len == IMG_INIT_SIZE &&
              info.dm_bin_len == IMG_DM_SIZE, "image lengths");
        /* The images live inside the reserved span. */
        CHECK(info.init_kaddr >= info.boot_base &&
              info.dm_kaddr + info.dm_size <= info.boot_base + info.boot_total,
              "images inside the boot span");
    }

    /* ── 3. parse failures are refused, never half-applied ──────────────── */
    {
        uint8_t manifest_out[2048];
        uint32_t mlen = 0;
        struct BootImageInfo info;
        /* 3a. not a newc archive */
        uint8_t garbage[64];
        memset(garbage, 0x11, sizeof(garbage));
        CHECK(boot_image_parse(garbage, sizeof(garbage), manifest_out,
                               sizeof(manifest_out), &mlen, &info) != 0,
              "garbage archive refused");
        /* 3b. truncated archive (valid newc, cut in half) */
        CHECK(boot_image_parse(archive.data, archive.len / 2, manifest_out,
                               sizeof(manifest_out), &mlen, &info) != 0,
              "truncated archive refused");
        /* 3c. missing init.manifest */
        struct Blob no_manifest;
        build_archive(&no_manifest, &manifest);
        /* overwrite the manifest name so the walk can't find it */
        uint32_t off = 0, size = 0;
        CHECK(boot_newc_find(no_manifest.data, no_manifest.len,
                             BOOT_INIT_MANIFEST_PATH, &off, &size) == 0,
              "locate manifest for corruption");
        /* off points at the DATA; the name's last byte is off-1 (the NUL),
         * but the find compares only the name bytes, so flip a LETTER
         * instead — 'm' of "manifest" (name is 19 bytes incl. the NUL,
         * padded to 20, so the last letter sits at off-7). */
        if (off >= 7) no_manifest.data[off - 7] ^= 0xFF;
        CHECK(boot_image_parse(no_manifest.data, no_manifest.len, manifest_out,
                               sizeof(manifest_out), &mlen, &info) == BOOT_ERR_NOMANIFEST,
              "missing manifest -> NOMANIFEST");
        /* 3d. corrupted manifest (bad CRC) refused before field use */
        struct Blob bad_manifest;
        build_init_manifest(&bad_manifest);
        bad_manifest.data[30] ^= 0xFF;   /* inside a record payload */
        struct Blob bad_archive;
        build_archive(&bad_archive, &bad_manifest);
        CHECK(boot_image_parse(bad_archive.data, bad_archive.len, manifest_out,
                               sizeof(manifest_out), &mlen, &info) == BOOT_ERR_BADMANIFEST,
              "corrupted manifest (CRC) refused");
        /* 3e. manifest size disagrees with the archive image -> MISMATCH */
        struct Blob wrong;
        build_init_manifest(&wrong);
        /* patch the IMAGE record's image_size to a different value (find it
         * via a re-parse-independent scan: image record is the 3rd record;
         * walk to it by tags like the reader does) */
        {
            uint32_t o = SIDECAR_MANIFEST_HEADER_LEN;
            for (int rec = 0; rec < 12; rec++) {
                uint16_t tag = (uint16_t)(wrong.data[o] | (wrong.data[o + 1] << 8));
                uint16_t rlen = (uint16_t)(wrong.data[o + 2] | (wrong.data[o + 3] << 8));
                if (tag == SIDECAR_TAG_IMAGE) {
                    /* image_size is at payload + 12: 0x2000 -> 0x2001. */
                    wrong.data[o + 4 + 12] = 0x01;
                    break;
                }
                o += 4 + rlen;
            }
            /* Re-patch the body CRC so the ONLY defect is the size
             * mismatch (a corrupt CRC alone is covered by 3d). */
            uint32_t total = wrong.len;
            uint32_t crc = crc32(wrong.data + SIDECAR_MANIFEST_HEADER_LEN,
                                 total - SIDECAR_MANIFEST_HEADER_LEN);
            memcpy(wrong.data + 20, &crc, 4);
        }
        struct Blob wrong_archive;
        build_archive(&wrong_archive, &wrong);
        CHECK(boot_image_parse(wrong_archive.data, wrong_archive.len, manifest_out,
                               sizeof(manifest_out), &mlen, &info) == BOOT_ERR_MISMATCH,
              "manifest/archive size mismatch -> MISMATCH");
    }

    /* ── 4. boot_build_registry: count + 64-byte entries (devreg.rs layout) ─ */
    {
        uint8_t reg[1024];
        int n = boot_build_registry(reg, sizeof(reg), fake_scan);
        CHECK(n == 3, "3 devices from the fake scan");
        uint32_t count = 0;
        memcpy(&count, reg, 4);
        CHECK(count == 3, "count u32 first");
        /* entry 0: NVMe */
        const uint8_t* e0 = reg + 4;
        CHECK(e0[0] == 0x01 && e0[1] == 0x08, "NVMe class/subclass");
        CHECK(e0[2] == 0x4D && e0[3] == 0x14, "NVMe vendor LE");
        CHECK(e0[4] == 0x08 && e0[5] == 0xA8, "NVMe device LE");
        CHECK(e0[6] == 0 && e0[7] == 0, "slot/bus");
        CHECK(e0[16] == 11 && e0[17] == 1, "irq line + is64bit");
        CHECK(memcmp(e0 + 20, "drv.nvme.0\0", 11) == 0,
              "NVMe driver manifest");
        /* entry 1: e1000 */
        const uint8_t* e1 = reg + 4 + 64;
        CHECK(e1[0] == 0x02 && e1[1] == 0x00, "e1000 class/subclass");
        CHECK(e1[2] == 0x86 && e1[3] == 0x80, "e1000 vendor LE");
        CHECK(memcmp(e1 + 20, "drv.e1000.0\0", 12) == 0,
              "e1000 driver manifest");
        /* entry 2: VGA — no driver yet, empty manifest name */
        const uint8_t* e2 = reg + 4 + 2 * 64;
        CHECK(e2[0] == 0x03 && e2[1] == 0x00, "VGA class/subclass");
        int all_zero = 1;
        for (int i = 20; i < 64; i++) if (e2[i] != 0) all_zero = 0;
        CHECK(all_zero, "unknown device: empty driver manifest");
        /* entry data is 64 bytes apart */
        CHECK(e1 == e0 + 64 && e2 == e1 + 64, "64-byte entry stride");
    }

    /* ── 5. registry edge cases ──────────────────────────────────────────── */
    {
        CHECK(boot_build_registry(0, 1024, fake_scan) == -1, "null dst refused");
        uint8_t tiny[4];
        CHECK(boot_build_registry(tiny, 4, fake_scan) == 0,
              "cap < 4 + 64: no entries, count 0");
        CHECK(boot_build_registry(tiny, sizeof(tiny), 0) == -1, "null scan refused");
        CHECK(boot_driver_for_class(0x01, 0x08) != 0 &&
              strcmp(boot_driver_for_class(0x01, 0x08), "drv.nvme.0") == 0,
              "driver map: NVMe");
        CHECK(strcmp(boot_driver_for_class(0x02, 0x00), "drv.e1000.0") == 0,
              "driver map: e1000");
        CHECK(strcmp(boot_driver_for_class(0x03, 0x00), "") == 0,
              "driver map: unknown -> empty");
    }

    /* ── 6. the newc walk matches the Rust builder's offsets ─────────────── */
    {
        /* The archive is deterministic; verify the entry offsets the parser
         * reports are internally consistent with a raw re-walk. */
        uint32_t off = 0;
        uint32_t expect = 0;
        int ok = 1;
        for (;;) {
            if (off + 110 > archive.len) { ok = 0; break; }
            uint32_t ns, fs;
            sscanf((const char*)archive.data + off + 94, "%08x", &ns);
            sscanf((const char*)archive.data + off + 54, "%08x", &fs);
            if (memcmp(archive.data + off, "070701", 6) != 0) { ok = 0; break; }
            uint32_t data_off = off + 110 + ns + ((4 - (ns & 3)) & 3);
            if (memcmp(archive.data + off + 110, "TRAILER!!!", 10) == 0) break;
            uint32_t eoff = 0, esize = 0;
            char nm[64];
            memcpy(nm, archive.data + off + 110, ns - 1);
            nm[ns - 1] = 0;
            if (boot_newc_find(archive.data, archive.len, nm, &eoff, &esize) == 0) {
                if (eoff != data_off || esize != fs) ok = 0;
            } else ok = 0;
            expect++;
            off = data_off + fs + ((4 - (fs & 3)) & 3);
        }
        CHECK(ok && expect == 7, "parser offsets agree with a raw re-walk");
    }

    printf("%d checks, %d passed, %d failed\n", g_pass + g_fail, g_pass, g_fail);
    return g_fail != 0;
}
