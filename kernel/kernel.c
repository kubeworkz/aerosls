// kernel.c — AeroSLS BSP entry point and system bootstrap

#include <stdint.h>
#include "kernel_io.h"
#include "scheduler.h"
#include "microkernel.h"
#include "../arch/x86/gdt.h"
#include "../arch/x86/idt.h"
#include "../arch/x86/lapic.h"
#include "../arch/x86/isr_stubs.h"
#include "timer.h"
#include "process.h"
#include "loader.h"
#include "../kernel/webapp.h"
#include "../kernel/auth.h"
#include "../net/dhcp.h"
#include "../arch/x86/user_paging.h"
#include "../net/net.h"
#include "../net/e1000.h"
#include "../net/http.h"
#include "../arch/x86/multiboot2.h"
#include "../kernel/journal.h"
#include "../kernel/lock_mgr.h"
#include "../kernel/index_mgr.h"

extern void sls_shell_loop(void);
extern void boot_application_processors(uint8_t apic_id);

// ─── Multiboot2 memory map + hardware diagnostics ────────────────────────────
static void print_hw_info(uint32_t mb2_magic, uint32_t mb2_phys) {
    // 1. Verify magic
    if (mb2_magic != (uint32_t)MULTIBOOT2_MAGIC) {
        kernel_serial_printf("[MB2] WARNING: bad magic 0x%x (expected 0x36d76289)\n",
                             mb2_magic);
        return;
    }

    // 2. CPU vendor string via CPUID leaf 0
    uint32_t eax, ebx, ecx, edx;
    __asm__ volatile("cpuid" : "=a"(eax),"=b"(ebx),"=c"(ecx),"=d"(edx) : "a"(0));
    char vendor[13];
    ((uint32_t*)vendor)[0] = ebx;
    ((uint32_t*)vendor)[1] = edx;
    ((uint32_t*)vendor)[2] = ecx;
    vendor[12] = '\0';
    __asm__ volatile("cpuid" : "=a"(eax) : "a"(1) : "ebx","ecx","edx");
    uint32_t family  = ((eax >> 8) & 0xF)  + ((eax >> 20) & 0xFF);
    uint32_t model   = ((eax >> 4) & 0xF)  | ((eax >> 12) & 0xF0);
    kernel_serial_printf("[HW] CPU %s family=%u model=%u\n", vendor, family, model);

    // 3. Walk MB2 tags for memory map
    const struct mb2_info* info = (const struct mb2_info*)(uintptr_t)mb2_phys;
    const struct mb2_tag* tag   = (const struct mb2_tag*)(info + 1);
    const struct mb2_tag* end   = (const struct mb2_tag*)(
                                  (uint8_t*)(uintptr_t)mb2_phys + info->total_size);
    uint64_t usable_bytes = 0;

    while (tag < end && tag->type != MB2_TAG_END) {
        if (tag->type == MB2_TAG_MMAP) {
            const struct mb2_tag_mmap* mm = (const struct mb2_tag_mmap*)tag;
            const struct mb2_mmap_entry* e =
                (const struct mb2_mmap_entry*)((const uint8_t*)mm + sizeof(*mm));
            const struct mb2_mmap_entry* me =
                (const struct mb2_mmap_entry*)((const uint8_t*)mm + mm->size);
            kernel_serial_print("[HW] Memory map:\n");
            while (e < me) {
                static const char* const types[] = {
                    "?", "RAM", "Reserved", "ACPI", "NVS", "Bad" };
                const char* tname = (e->type <= 5) ? types[e->type] : "?";
                kernel_serial_printf("[HW]   %016lx + %8lu KiB  %s\n",
                    e->base_addr, (uint32_t)(e->length / 1024), tname);
                if (e->type == MB2_MEM_AVAILABLE) usable_bytes += e->length;
                e = (const struct mb2_mmap_entry*)((const uint8_t*)e + mm->entry_size);
            }
        }
        tag = mb2_tag_next(tag);
    }
    if (usable_bytes)
        kernel_serial_printf("[HW] Usable RAM: %u MiB\n",
                             (uint32_t)(usable_bytes >> 20));
}

#ifdef __cplusplus
extern "C"
#endif
void kernel_main(uint32_t mb2_magic, uint32_t mb2_phys) {

    // ── 1. Serial output — must be first so all subsequent prints are visible ──
    serial_init();
    kernel_serial_print(
        "[AEROSLS BOOT LOGGER V1.0.0 RUNNING]\n"
        "----------------------------------------------------------------------------------\n");

    // ── 2. CPU descriptor tables ───────────────────────────────────────────────
    kernel_serial_print("[BSP] Loading GDT and IDT...\n");
    init_gdt();
    init_idt();

    // ── 2b. Hardware info (CPU + memory map from multiboot2) ──────────────────
    print_hw_info(mb2_magic, mb2_phys);

    // ── 3. Local APIC + timer IRQ ──────────────────────────────────────────
    init_local_apic_registers();
    init_timer();
    kernel_serial_print("[BSP] LAPIC and IRQ0 timer online.\n");

    // ── 4. Scheduler ──────────────────────────────────────────────────────
    init_scheduler();
    kernel_serial_print("[BSP] Round-robin task scheduler initialised.\n");

    // ── 4b. SYSCALL/SYSRET gate ───────────────────────────────────────────
    syscall_gate_init();   // configure STAR/LSTAR/SFMASK/EFER, per-CPU GS

    // ── 4c. Process manager ────────────────────────────────────────────
    process_init();

    // ── 4d. Service binary loader ───────────────────────────────────────────
    loader_init();

    // ── 4e. Web asset store ────────────────────────────────────────────────
    webapp_init();   // installs built-in Navigator welcome page

    // ── 4f. Token authentication registry ─────────────────────────────────
    auth_init();     // registers 4 demo tokens; prints them to serial log

    // ── 4g. Journal subsystem (IBM i-style before/after-image journaling) ──
    journal_init();

    // ── 4h. Row-level lock manager (Read-Committed isolation) ───────────────
    lock_mgr_init();

    // ── 4i. Secondary index manager ─────────────────────────────────
    index_mgr_init();

    // ── 5. Microkernel (IPC + 5 services + tier manager) ──────────────────
    microkernel_init();

    // ── 6. Boot Application Processor (Core 1) ─────────────────────────
    kernel_serial_print("[BSP] Waking Core 1...\n");
    boot_application_processors(1);
    kernel_serial_print("[BSP] Core 1 online.\n");

    // ── 7. Network stack (PCI scan → e1000 init → gratuitous ARP) ─────────────
    kernel_serial_print("[NET] PCI scanning for e1000...\n");
    {
        extern uint32_t pci_read_config(uint8_t, uint8_t, uint8_t, uint8_t);
        // Scan bus 0 for e1000 (Intel vendor 8086, device 100e/10d3/107c).
        // Store both the MMIO base and the PCI slot so e1000_init can use
        // the correct slot for Bus Master Enable without another scan.
        uint8_t found_slot = 0;
        for (int slot = 0; slot < 32; slot++) {
            uint32_t vid_did = pci_read_config(0, (uint8_t)slot, 0, 0x00);
            if (vid_did == 0xFFFFFFFF) continue;
            uint32_t vid = vid_did & 0xFFFF;
            uint32_t did = (vid_did >> 16) & 0xFFFF;
            if (vid == 0x8086 && (did == 0x100e || did == 0x10d3 || did == 0x107c)) {
                uint32_t bar0 = pci_read_config(0, (uint8_t)slot, 0, 0x10);
                uint32_t bar1 = pci_read_config(0, (uint8_t)slot, 0, 0x14);
                int is64 = ((bar0 & 0x06) == 0x04);
                uint64_t base = (uint64_t)(bar0 & 0xFFFFFFF0);
                if (is64) base |= ((uint64_t)bar1 << 32);
                if ((bar0 & 0x1) == 0 && base != 0) {
                    e1000_mmio_base = base;
                    found_slot      = (uint8_t)slot;
                    kernel_serial_printf("[NET] e1000 at PCI slot %d MMIO 0x%lx\n",
                                         slot, base);
                    break;
                }
            }
        }
        if (e1000_mmio_base) {
            e1000_init(e1000_mmio_base, found_slot);
            net_init();   // sends gratuitous ARP
            dhcp_start(); // DISCOVER → OFFER → REQUEST → ACK; updates net_my_ip
            kernel_serial_print("[NET] e1000 RX/TX rings online.\n");
        } else {
            kernel_serial_print("[NET] e1000 not found — network disabled.\n");
        }
    }

    kernel_serial_print(
        "----------------------------------------------------------------------------------\n"
        "[SLS] System ready. Launching secure shell...\n"
        "----------------------------------------------------------------------------------\n");

    // ── 8. HTTP server runs on Core 1 alongside the flush/service loop ─────────
    // In the current build the BSP runs the shell; Core 1 already runs
    // microkernel_service_poll() from smp.c.  Kick the HTTP server on the BSP
    // as a foreground loop only if the NIC is present; otherwise fall through
    // to the shell.
    if (e1000_mmio_base) {
        http_server_run();  // does not return — serves REST API on port 3000
    }

    // ── Shell (does not return) ─────────────────────────────────────────────
    sls_shell_loop();
}