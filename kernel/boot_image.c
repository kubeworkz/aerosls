/* boot_image.c — the Phase 5 boot-time sidecar loader: consumes the initrd
 * the boot-image builder (user/bootimage/) produced and creates the init
 * sidecar at boot. See boot_image.h for the contract and the launch steps.
 *
 * Structure: the PURE CORE (newc walk, manifest read, registry build) is
 * dependency-free and host-tested by tests/boot_image_host_test.c; the BOOT
 * GLUE (mb2 capture, frame-pool reservation, image copies, parent planting,
 * cap_create_sidecar) is kernel-only and verified by construction.
 */
#include "boot_image.h"
#include "cap.h"            /* SIDECAR_* manifest wire format + CAP_NONE */
#include "frame_pool.h"     /* frame_pool_reserve_range */
#include "kernel_io.h"      /* kernel_serial_print/printf */
#include "process.h"        /* proc_table, PROC_BLOCKED, ProcessDescriptor */
#include "../arch/x86/multiboot2.h"
#include "../arch/x86/user_paging.h"   /* per_cpu_data */
#include <stdint.h>

/* Freestanding string primitives (implemented in kernel/stubs.c). */
extern void* memcpy(void* dst, const void* src, unsigned long n);
extern void* memset(void* dst, int val, unsigned long n);
extern int   memcmp(const void* a, const void* b, unsigned long n);
extern unsigned long strlen(const char* s);

/* ═══ PURE CORE ═════════════════════════════════════════════════════════ */

/* IEEE CRC-32 (reflected, poly 0xEDB88320) — the same algorithm as
 * cap_sidecar_crc32 (kernel/cap.c) and crc32 (user/proto/src/manifest.rs).
 * The manifest's body CRC is verified before any field is trusted, exactly
 * like cap_create_sidecar's parser. */
static uint32_t boot_crc32(const uint8_t* data, uint32_t len) {
    uint32_t crc = 0xFFFFFFFFu;
    for (uint32_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++)
            crc = (crc >> 1) ^ (0xEDB88320u & (-(int)(crc & 1)));
    }
    return crc ^ 0xFFFFFFFFu;
}

/* One 8-hex-char ASCII field (newc) / 8-byte LE field (manifest), read as a
 * u32. Newc fields are ASCII hex; manifest u32 fields are little-endian. */
static uint32_t boot_hex32(const uint8_t* p) {
    uint32_t v = 0;
    for (int i = 0; i < 8; i++) {
        uint8_t c = p[i];
        uint32_t d;
        if (c >= '0' && c <= '9')      d = (uint32_t)(c - '0');
        else if (c >= 'a' && c <= 'f') d = (uint32_t)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') d = (uint32_t)(c - 'A' + 10);
        else return 0xFFFFFFFFu;
        v = (v << 4) | d;
    }
    return v;
}

static uint64_t boot_le64(const uint8_t* p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) v |= (uint64_t)p[i] << (8 * i);
    return v;
}

static uint32_t boot_le32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint16_t boot_le16(const uint8_t* p) {
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

/* Walk a newc archive for the entry `name`. On success *out_off/*out_size
 * hold the entry's DATA offset and size within the archive. Mirrors the
 * reference parser in user/bootimage/src/newc.rs (and the header's field
 * table): 110-byte header, name padded to 4, data padded to 4, and a
 * "TRAILER!!!" terminator. */
int boot_newc_find(const uint8_t* a, uint32_t len, const char* name,
                   uint32_t* out_off, uint32_t* out_size) {
    uint32_t want = (uint32_t)strlen(name);
    uint32_t off = 0;
    for (;;) {
        if (off + 110 > len) return BOOT_ERR_TRUNCATED;
        if (memcmp(a + off, "070701", 6) != 0) return BOOT_ERR_BADARCHIVE;
        uint32_t filesize = boot_hex32(a + off + 54);
        uint32_t namesize = boot_hex32(a + off + 94);   /* includes the NUL */
        if (namesize == 0xFFFFFFFFu || filesize == 0xFFFFFFFFu ||
            namesize < 1) return BOOT_ERR_BADARCHIVE;
        uint32_t name_off = off + 110;
        if (name_off + namesize > len) return BOOT_ERR_TRUNCATED;
        const char* n = (const char*)(a + name_off);
        if (want + 1 == namesize && memcmp(n, name, want) == 0) {
            uint32_t data_off = name_off + namesize + ((4 - (namesize & 3)) & 3);
            if (data_off + filesize > len) return BOOT_ERR_TRUNCATED;
            if (out_off) *out_off = data_off;
            if (out_size) *out_size = filesize;
            return 0;
        }
        /* Not the entry we want: stop at the terminator, else advance. */
        if (namesize == 11 && memcmp(n, "TRAILER!!!", 10) == 0)
            return BOOT_ERR_NOTFOUND;
        off = name_off + namesize + ((4 - (namesize & 3)) & 3)
            + filesize + ((4 - (filesize & 3)) & 3);
    }
}

/* The fields the loader needs out of the INIT manifest: the IMAGE record's
 * image_size, the dm.image + device_registry MEM caps, and the footer's
 * image_kaddr. A bounded TLV walk identical in shape to cap.c's parser
 * (SIDECAR_MANIFEST_HEADER_LEN + record_count records + 8-byte footer). */
struct BootManifestInfo {
    uint64_t init_kaddr;
    uint32_t init_size;
    uint64_t dm_kaddr, dm_size;
    uint64_t posix_kaddr, posix_size;
    uint64_t ramdisk_kaddr, ramdisk_size;
    uint64_t storage_kaddr, storage_size;
    uint64_t net_kaddr, net_size;
    uint64_t reg_kaddr, reg_size;
};

static int boot_manifest_read(const uint8_t* blob, uint32_t len,
                              struct BootManifestInfo* m) {
    if (len < SIDECAR_MANIFEST_HEADER_LEN) return BOOT_ERR_BADMANIFEST;
    if (memcmp(blob, SIDECAR_MANIFEST_MAGIC, 8) != 0) return BOOT_ERR_BADMANIFEST;
    if (boot_le16(blob + 8) != SIDECAR_MANIFEST_VERSION_MAJOR)
        return BOOT_ERR_BADMANIFEST;
    uint32_t total_len = boot_le32(blob + 16);
    if (total_len < SIDECAR_MANIFEST_HEADER_LEN || total_len > len)
        return BOOT_ERR_BADMANIFEST;
    /* CRC over the records + footer (bytes after the header) — a corrupted
     * manifest is refused before any field is trusted (cap.c posture). */
    if (boot_le32(blob + 20) != boot_crc32(blob + SIDECAR_MANIFEST_HEADER_LEN,
                                           total_len - SIDECAR_MANIFEST_HEADER_LEN))
        return BOOT_ERR_BADMANIFEST;

    memset(m, 0, sizeof(*m));
    uint32_t off = SIDECAR_MANIFEST_HEADER_LEN;
    uint16_t record_count = boot_le16(blob + 12);
    for (uint16_t rec = 0; rec < record_count; rec++) {
        if (off + 4 > total_len) return BOOT_ERR_BADMANIFEST;
        uint16_t tag  = boot_le16(blob + off);
        uint16_t rlen = boot_le16(blob + off + 2);
        if (off + 4 + rlen > total_len) return BOOT_ERR_BADMANIFEST;
        const uint8_t* rp = blob + off + 4;
        switch (tag) {
        case SIDECAR_TAG_IMAGE:
            /* entry u64(0), blob_offset u32(8), image_size u32(12),
             * image_kaddr u64(16, in-record — the footer supplies it). */
            if (rlen < 24) return BOOT_ERR_BADMANIFEST;
            m->init_size = boot_le32(rp + 12);
            break;
        case SIDECAR_TAG_CAP_MEM: {
            /* name_len u16, name, phys_base u64, size u64, rights u8. */
            if (rlen < 2) return BOOT_ERR_BADMANIFEST;
            uint16_t nlen = boot_le16(rp);
            if ((int)rlen < (int)nlen + 19) return BOOT_ERR_BADMANIFEST;
            const char* n = (const char*)(rp + 2);
            if (nlen == 8 && memcmp(n, "dm.image", 8) == 0) {
                m->dm_kaddr = boot_le64(rp + 2 + nlen);
                m->dm_size  = boot_le64(rp + 2 + nlen + 8);
            } else if (nlen == 11 && memcmp(n, "posix.image", 11) == 0) {
                m->posix_kaddr = boot_le64(rp + 2 + nlen);
                m->posix_size  = boot_le64(rp + 2 + nlen + 8);
            } else if (nlen == 15 && memcmp(n, "device_registry", 15) == 0) {
                m->reg_kaddr = boot_le64(rp + 2 + nlen);
                m->reg_size  = boot_le64(rp + 2 + nlen + 8);
            } else if (nlen == 13 && memcmp(n, "ramdisk.image", 13) == 0) {
                m->ramdisk_kaddr = boot_le64(rp + 2 + nlen);
                m->ramdisk_size  = boot_le64(rp + 2 + nlen + 8);
            } else if (nlen == 7 && memcmp(n, "storage", 7) == 0) {
                m->storage_kaddr = boot_le64(rp + 2 + nlen);
                m->storage_size  = boot_le64(rp + 2 + nlen + 8);
            } else if (nlen == 9 && memcmp(n, "net.image", 9) == 0) {
                m->net_kaddr = boot_le64(rp + 2 + nlen);
                m->net_size  = boot_le64(rp + 2 + nlen + 8);
            }
            break;
        }
        default:
            break;   /* informational records — nothing the loader needs */
        }
        off += 4 + rlen;
    }
    /* Footer: image_kaddr u64 right after the last record. */
    if (off + 8 > total_len) return BOOT_ERR_BADMANIFEST;
    m->init_kaddr = boot_le64(blob + off);
    if (m->init_kaddr < 0x100000ULL || m->init_size == 0 ||
        m->dm_kaddr == 0 || m->dm_size == 0 ||
        m->reg_kaddr == 0 || m->reg_size == 0)
        return BOOT_ERR_BADMANIFEST;
    /* ramdisk.image and storage are optional (older images may lack them) */
    return 0;
}

int boot_image_parse(const uint8_t* archive, uint32_t archive_len,
                     uint8_t* manifest_out, uint32_t manifest_cap,
                     uint32_t* manifest_len_out, struct BootImageInfo* info) {
    if (!archive || !manifest_out || !manifest_len_out || !info)
        return BOOT_ERR_BADARCHIVE;

    uint32_t moff, msize;
    int r = boot_newc_find(archive, archive_len, BOOT_INIT_MANIFEST_PATH,
                           &moff, &msize);
    if (r != 0) return BOOT_ERR_NOMANIFEST;
    if (msize > manifest_cap) return BOOT_ERR_BADMANIFEST;
    memcpy(manifest_out, archive + moff, msize);
    *manifest_len_out = msize;

    struct BootManifestInfo mi;
    r = boot_manifest_read(manifest_out, msize, &mi);
    if (r != 0) return r;

    uint32_t ioff, isize, doff, dsize, poff, psize, roff, rsize;
    r = boot_newc_find(archive, archive_len, BOOT_INIT_BIN_PATH, &ioff, &isize);
    if (r != 0) return BOOT_ERR_NOIMAGE;
    r = boot_newc_find(archive, archive_len, BOOT_DM_BIN_PATH, &doff, &dsize);
    if (r != 0) return BOOT_ERR_NOIMAGE;
    r = boot_newc_find(archive, archive_len, BOOT_POSIX_BIN_PATH, &poff, &psize);
    if (r != 0) return BOOT_ERR_NOIMAGE;
    /* Ramdisk binary is optional for older images. */
    r = boot_newc_find(archive, archive_len, BOOT_RAMDISK_BIN_PATH, &roff, &rsize);
    if (r != 0) { roff = 0; rsize = 0; }  /* absent: ramdisk_kaddr/size remain 0 */
    /* Network binary is optional for older images. */
    uint32_t noff = 0, nsize = 0;
    r = boot_newc_find(archive, archive_len, BOOT_NET_BIN_PATH, &noff, &nsize);
    if (r != 0) { noff = 0; nsize = 0; }
    /* Rootfs is optional — older images boot in console-only mode. */
    uint32_t rootfs_off = 0, rootfs_len = 0;
    r = boot_newc_find(archive, archive_len, BOOT_ROOTFS_BIN_PATH, &rootfs_off, &rootfs_len);
    if (r != 0) { rootfs_off = 0; rootfs_len = 0; }

    /* Cross-checks: the archive's image bytes must be exactly what the
     * manifest declares — a mismatch means a rebuilt sidecar without a
     * rebuilt manifest, which must be refused, not half-applied. */
    if (mi.init_size != isize || mi.dm_size != dsize || mi.posix_size != psize) return BOOT_ERR_MISMATCH;
    if (mi.ramdisk_size != 0 && mi.ramdisk_size != rsize) return BOOT_ERR_MISMATCH;
    if (mi.net_size != 0 && mi.net_size != nsize) return BOOT_ERR_MISMATCH;

    /* The boot-image span [init kaddr, registry end) must live inside the
     * 4 GiB identity map (cap_create_mem's bound). */
    if (mi.init_kaddr < 0x100000ULL ||
        mi.reg_kaddr + mi.reg_size > 0x100000000ULL)
        return BOOT_ERR_RANGE;

    info->init_kaddr = mi.init_kaddr;
    info->init_size  = isize;
    info->dm_kaddr   = mi.dm_kaddr;
    info->dm_size    = dsize;
    info->posix_kaddr = mi.posix_kaddr;
    info->posix_size  = psize;
    info->ramdisk_kaddr = mi.ramdisk_kaddr;
    info->ramdisk_size  = mi.ramdisk_size;
    info->storage_kaddr = mi.storage_kaddr;
    info->storage_size  = mi.storage_size;
    info->net_kaddr = mi.net_kaddr;
    info->net_size  = mi.net_size;
    info->reg_kaddr  = mi.reg_kaddr;
    info->reg_size   = mi.reg_size;
    info->boot_base  = mi.init_kaddr;
    info->boot_total = (mi.reg_kaddr + mi.reg_size) - mi.init_kaddr;
    info->init_bin_off = ioff;
    info->init_bin_len = isize;
    info->dm_bin_off   = doff;
    info->dm_bin_len   = dsize;
    info->posix_bin_off = poff;
    info->posix_bin_len = psize;
    info->ramdisk_bin_off = roff;
    info->ramdisk_bin_len = rsize;
    info->net_bin_off = noff;
    info->net_bin_len = nsize;
    info->rootfs_bin_off = rootfs_off;
    info->rootfs_bin_len = rootfs_len;

    /* Sanity: the DM image and registry sit inside the reserved span
     * (contiguous by the builder's construction). */
    if (info->dm_kaddr < info->boot_base ||
        info->dm_kaddr + info->dm_size > info->boot_base + info->boot_total ||
        info->posix_kaddr < info->boot_base ||
        info->posix_kaddr + info->posix_size > info->boot_base + info->boot_total ||
        (info->ramdisk_kaddr != 0 && (info->ramdisk_kaddr < info->boot_base ||
         info->ramdisk_kaddr + info->ramdisk_size > info->boot_base + info->boot_total)) ||
        info->reg_kaddr < info->boot_base ||
        info->reg_kaddr + info->reg_size > info->boot_base + info->boot_total ||
        (info->net_kaddr != 0 && (info->net_kaddr < info->boot_base ||
         info->net_kaddr + info->net_size > info->boot_base + info->boot_total)))
        return BOOT_ERR_RANGE;
    return 0;
}

int boot_build_registry(uint8_t* dst, uint32_t cap,
                        int (*scan)(int slot, struct BootDeviceEntry* e)) {
    if (!dst || cap < 4 || !scan) return -1;
    uint32_t count = 0;
    for (int slot = 0; slot < 32 && count < BOOT_REGISTRY_MAX_DEVICES; slot++) {
        struct BootDeviceEntry e;
        memset(&e, 0, sizeof(e));
        if (scan(slot, &e) != 0) continue;   /* empty slot */
        /* Auto-fill the driver manifest from the class/subclass table if the
         * scan left it empty — so the registry carries the driver name the
         * init sidecar needs for device-manager dispatch. */
        if (e.driver_manifest[0] == '\0') {
            const char* drv = boot_driver_for_class(e.class_code, e.subclass);
            if (drv && drv[0]) {
                uint32_t dlen = (uint32_t)strlen(drv);
                if (dlen >= BOOT_DRIVER_MANIFEST_LEN) dlen = BOOT_DRIVER_MANIFEST_LEN - 1;
                memcpy(e.driver_manifest, drv, dlen);
            }
        }
        if (4u + (count + 1u) * BOOT_DEVICE_ENTRY_SIZE > cap) break;
        memcpy(dst + 4 + count * BOOT_DEVICE_ENTRY_SIZE, &e, sizeof(e));
        count++;
    }
    memcpy(dst, &count, 4);
    return (int)count;
}

const char* boot_driver_for_class(uint8_t class_code, uint8_t subclass) {
    if (class_code == 0x01 && subclass == 0x08) return "drv.nvme.0";
    if (class_code == 0x02 && subclass == 0x00) return "drv.e1000.0";
    return "";
}

/* ═══ BOOT GLUE ══════════════════════════════════════════════════════════ */

static uint32_t g_initrd_start = 0;
static uint32_t g_initrd_end   = 0;

void boot_image_capture_mb2(uint32_t mb2_magic, uint32_t mb2_phys) {
    g_initrd_start = g_initrd_end = 0;
    if (mb2_phys == 0) goto try_appended;

    /* ── Multiboot v1 path ──────────────────────────────────────────────
     * GRUB 2.12's `module` command only works with `multiboot` (v1), not
     * `multiboot2` (v2). The Phase 5 grub.cfg uses v1 to pass the initrd.
     * v1 info: flags at +0, mods_addr at +88, mods_count at +92.
     * Multiboot v1 magic: GRUB passes 0x2BADB002 (0x1BADB002 | 1). */
    if (mb2_magic == 0x2BADB002u) {
        const uint32_t* info1 = (const uint32_t*)(uintptr_t)mb2_phys;
        uint32_t flags = info1[0];
        kernel_serial_printf(
            "[SIDECAR] multiboot v1 detected, flags=0x%x phys=0x%x\n", flags, mb2_phys);
        if (flags & (1u << 3)) {   /* bit 3 = mods_count/mods_addr valid */
            uint32_t mods_count = info1[5];  /* byte 20 */
            uint32_t mods_addr  = info1[6];  /* byte 24 */
            kernel_serial_printf(
                "[SIDECAR] v1 mods_count=%u mods_addr=0x%x\n",
                mods_count, mods_addr);
            if (mods_count > 0 && mods_addr != 0) {
                /* Module entry: mod_start(4) + mod_end(4) + string(4) + pad(4) */
                const uint32_t* mod = (const uint32_t*)(uintptr_t)mods_addr;
                g_initrd_start = mod[0];
                g_initrd_end   = mod[1];
                kernel_serial_printf(
                    "[SIDECAR] v1 initrd module at 0x%x..0x%x (%u bytes)\n",
                    g_initrd_start, g_initrd_end,
                    g_initrd_end - g_initrd_start);
                return;
            }
        }
        goto try_appended;
    }

    /* ── Multiboot2 path ──────────────────────────────────────────────── */
    if (mb2_magic == (uint32_t)MULTIBOOT2_MAGIC) {
        const struct mb2_info* info = (const struct mb2_info*)(uintptr_t)mb2_phys;
        const struct mb2_tag*  tag  = (const struct mb2_tag*)(info + 1);
        const struct mb2_tag*  end  = (const struct mb2_tag*)(
                                      (const uint8_t*)(uintptr_t)mb2_phys + info->total_size);
        while (tag < end && tag->type != MB2_TAG_END) {
            if (tag->type == MB2_TAG_MODULE) {
                const struct mb2_tag_module* m = (const struct mb2_tag_module*)tag;
                g_initrd_start = m->mod_start;
                g_initrd_end   = m->mod_end;
                kernel_serial_printf(
                    "[SIDECAR] initrd module at 0x%x..0x%x (%u bytes)\n",
                    g_initrd_start, g_initrd_end, g_initrd_end - g_initrd_start);
                return;   /* the first module is the initrd */
            }
            tag = mb2_tag_next(tag);
        }
    }

try_appended:
    /* No mb1/mb2 module tag found — nothing to do. */
    kernel_serial_print("[SIDECAR] no initrd module found\n");
}

/* The real PCI scan for the registry: bus 0, func 0, slots 0..31 — the same
 * shape as the e1000/NVMe scans in kernel.c, generalised to every device. */
static int boot_pci_scan_slot(int slot, struct BootDeviceEntry* e) {
    extern uint32_t pci_read_config(uint8_t, uint8_t, uint8_t, uint8_t);
    uint32_t vid_did = pci_read_config(0, (uint8_t)slot, 0, 0x00);
    if (vid_did == 0xFFFFFFFFu) return -1;   /* nothing at this slot */
    e->vendor_id = (uint16_t)(vid_did & 0xFFFF);
    e->device_id = (uint16_t)((vid_did >> 16) & 0xFFFF);
    uint32_t cls = pci_read_config(0, (uint8_t)slot, 0, 0x08);
    e->class_code = (uint8_t)((cls >> 24) & 0xFF);
    e->subclass   = (uint8_t)((cls >> 16) & 0xFF);
    uint32_t bar0 = pci_read_config(0, (uint8_t)slot, 0, 0x10);
    uint32_t bar1 = pci_read_config(0, (uint8_t)slot, 0, 0x14);
    int is64 = ((bar0 & 0x06) == 0x04);
    uint64_t base = (uint64_t)(bar0 & 0xFFFFFFF0u);
    if (is64) base |= ((uint64_t)bar1 << 32);
    e->bar0_phys    = base;
    e->is_64bit_bar = (uint8_t)is64;
    e->irq_line     = (uint8_t)(pci_read_config(0, (uint8_t)slot, 0, 0x3C) & 0xFF);
    e->pci_slot     = (uint8_t)slot;
    e->pci_bus      = 0;
    const char* drv = boot_driver_for_class(e->class_code, e->subclass);
    size_t dl = strlen(drv);
    if (dl >= sizeof(e->driver_manifest)) dl = sizeof(e->driver_manifest) - 1;
    memcpy(e->driver_manifest, drv, dl);
    return 0;
}

/* The synthetic parent for the boot-time init sidecar. cap_create_sidecar
 * requires a live parent descriptor (partition, uid, syscall stack) and the
 * kernel context (pid 0) has none. Planted with pid 99 — below alloc_pid()'s
 * 100 floor, so it never collides with a real sidecar — and PROC_BLOCKED
 * with no resume targets, so proc_runnable() never schedules it. Its
 * messenger channel's parent ends land in pid 99's lazily-bound cap table,
 * NOT table 0, so the kernel console service (which drains every CHAN_R in
 * table 0) never mistakes the messenger for a console channel. */
#define BOOT_PARENT_PID 99u
static void boot_plant_parent(uint64_t kernel_stack_top) {
    memset(&proc_table[0], 0, sizeof(proc_table[0]));
    proc_table[0].pid = BOOT_PARENT_PID;
    proc_table[0].active = 1;
    proc_table[0].state = PROC_BLOCKED;   /* never runnable */
    proc_table[0].partition_id = 0;       /* PARTITION_SYSTEM */
    proc_table[0].owner_uid = 0;
    proc_table[0].syscall_stack_top = kernel_stack_top;  /* the restore point */
    memcpy(proc_table[0].name, "kboot", 6);
    kernel_serial_printf("[SIDECAR] boot parent planted: slot0 pid=%u active=%d state=%d\n",
                         proc_table[0].pid, proc_table[0].active,
                         proc_table[0].state);
}

void launch_init_sidecar(void) {
    if (g_initrd_start == 0 || g_initrd_end <= g_initrd_start) {
        kernel_serial_print("[SIDECAR] no initrd module — init sidecar not launched\n");
        return;
    }
    uint32_t alen = g_initrd_end - g_initrd_start;
    const uint8_t* archive = (const uint8_t*)(uintptr_t)g_initrd_start;

    /* The init manifest can be ~600 bytes; the archive's own copy is what
     * the kernel validates and spawns from. */
    static uint8_t init_manifest[2048];
    struct BootImageInfo info;
    uint32_t mlen = 0;
    int r = boot_image_parse(archive, alen, init_manifest, sizeof(init_manifest),
                             &mlen, &info);
    if (r != 0) {
        kernel_serial_printf(
            "[SIDECAR] boot image parse failed (%d) — init sidecar not launched\n", r);
        return;
    }
    kernel_serial_printf(
        "[SIDECAR] boot image: init @0x%llx (%llu B) dm @0x%llx (%llu B) "
        "posix @0x%llx (%llu B) ramdisk @0x%llx (%llu B) net @0x%llx (%llu B) "
        "registry @0x%llx (%llu B)\n",
        (unsigned long long)info.init_kaddr, (unsigned long long)info.init_size,
        (unsigned long long)info.dm_kaddr,   (unsigned long long)info.dm_size,
        (unsigned long long)info.posix_kaddr, (unsigned long long)info.posix_size,
        (unsigned long long)info.ramdisk_kaddr, (unsigned long long)info.ramdisk_size,
        (unsigned long long)info.net_kaddr, (unsigned long long)info.net_size,
        (unsigned long long)info.reg_kaddr,  (unsigned long long)info.reg_size);

    /* 1. Reserve the whole boot-image span before anything can allocate it
     *    (the heaps and registry are inside it; cap_create_mem refuses any
     *    MEM cap that would overlap an existing object, and the sidecars'
     *    budget caps mint against these exact pages). */
    frame_pool_reserve_range(info.boot_base, info.boot_base + info.boot_total);

    /* 2. Copy the images to their declared physical addresses. The archive
     *    itself lives wherever the bootloader placed the module; the
     *    declared addresses are the contract, so the bytes move there.
     *    cap_create_sidecar then copies them into fresh frames. */
    memcpy((void*)(uintptr_t)info.init_kaddr, archive + info.init_bin_off,
           info.init_bin_len);
    memcpy((void*)(uintptr_t)info.dm_kaddr, archive + info.dm_bin_off,
           info.dm_bin_len);
    memcpy((void*)(uintptr_t)info.posix_kaddr, archive + info.posix_bin_off,
           info.posix_bin_len);
    memcpy((void*)(uintptr_t)info.ramdisk_kaddr, archive + info.ramdisk_bin_off,
           info.ramdisk_bin_len);
    if (info.net_kaddr != 0 && info.net_bin_len != 0) {
        memcpy((void*)(uintptr_t)info.net_kaddr, archive + info.net_bin_off,
               info.net_bin_len);
    }

    /* 2b. Copy the rootfs image to the ramdisk's storage region, if present.
     *     The storage address comes from the ramdisk manifest's storage cap
     *     (the init manifest doesn't declare it to avoid cap overlap).
     *     For the Phase 5 layout the storage region is always contiguous
     *     after ramdisk.heap: ramdisk_heap_end is the canonical address.
     *     The region is identity-mapped by cap_create_sidecar. */
    if (info.rootfs_bin_len != 0) {
        /* Storage region starts after ramdisk.heap (page-aligned). */
        uint64_t storage_addr = (info.ramdisk_kaddr + info.ramdisk_size
                                 + 0x40000u + 4095u) & ~4095ULL;
        /* Cap the copy to the remaining space below 4 GiB. */
        uint64_t max_size = 0x100000000ULL - storage_addr;
        uint32_t copy_len = info.rootfs_bin_len;
        if ((uint64_t)copy_len > max_size) copy_len = (uint32_t)max_size;
        memcpy((void*)(uintptr_t)storage_addr, archive + info.rootfs_bin_off,
               copy_len);
        kernel_serial_printf("[SIDECAR] rootfs: %u bytes @ 0x%llx\n",
                             (unsigned)copy_len,
                             (unsigned long long)storage_addr);
    }

    /* 3. Build the device registry (devreg.rs wire format) at the address
     *    the init manifest's device_registry cap declares. */
    int ndev = boot_build_registry((uint8_t*)(uintptr_t)info.reg_kaddr,
                                   (uint32_t)info.reg_size, boot_pci_scan_slot);
    kernel_serial_printf("[SIDECAR] device registry: %d device(s) @0x%llx\n",
                         ndev, (unsigned long long)info.reg_kaddr);

    /* 4. Plant the kernel-bootstrap parent. */
    boot_plant_parent(per_cpu_data[0].kernel_rsp);

    /* 5. Create the init sidecar from boot/init.manifest. The manifest's
     *    caps (budget, console, device_registry, dm.image) are minted and
     *    wired by cap_create_sidecar; the child is released PROC_SUSPENDED
     *    with a synthetic ring3_ctx (BIB pointer in rdi). */
    uint16_t ch_r = CAP_NONE;
    r = cap_create_sidecar(BOOT_PARENT_PID, init_manifest, mlen,
                           CAP_NONE, CAP_NONE, &ch_r);
    if (r != 0) {
        kernel_serial_printf("[SIDECAR] init sidecar create failed (%d)\n", r);
        return;
    }
    /* The child is the newest active process (cap_create_sidecar's async
     * spawn; the synthetic parent never runs). */
    uint32_t child_pid = 0;
    struct ProcessDescriptor* child = 0;
    for (int i = 0; i < PROC_MAX; i++)
        if (proc_table[i].active && proc_table[i].pid > child_pid) {
            child_pid = proc_table[i].pid;
            child = &proc_table[i];
        }
    kernel_serial_printf(
        "[SIDECAR] init sidecar created (PID %u, messenger CHAN_R %u) — "
        "entering ring 3\n",
        (unsigned)child_pid, (unsigned)ch_r);
    if (!child) return;

    /* 6. Hand the boot to the init sidecar. At boot nothing is ever in
     *    ring 3 (the shell and HTTP server run in ring 0), so the ring-3
     *    timer scheduler NEVER fires and a SUSPENDED child would sit
     *    unrun forever. Enter the child directly via the sysret path with
     *    the BIB pointer in rdi (kernel_enter_sidecar — the kernel_enter_
     *    ring3 variant that does not zero rdi, which the sidecar crt0
     *    contract requires). This never returns while init runs: init
     *    parks in its event loop and the timer scheduler takes over from
     *    there (preempting init in ring 3, switching to the DM child init
     *    spawns, and so on). The pre-Phase-5 shell/HTTP boot is skipped
     *    when an initrd is present — the sidecar system IS the boot.
     *    kernel_rsp is saved by the asm before sysret, so on the first
     *    timer preemption schedule_ring3 finds init as "current". */
    child->state = PROC_RUNNING;
    per_cpu_data[0].kernel_rsp = child->syscall_stack_top;
    kernel_enter_sidecar(&child->kernel_rsp, &child->kernel_cr3,
                         child->cr3, child->user_rip, child->user_rsp,
                         ((uint64_t*)&child->ring3_ctx)[9]);  /* rdi = BIB */
    /* Only reached if init exits — which it never does (it parks in the
     * event loop); belt-and-braces, mirror the create-failed posture. */
    kernel_serial_printf("[SIDECAR] init sidecar exited (PID %u)\n",
                         (unsigned)child_pid);
}
