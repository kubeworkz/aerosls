// kernel.c — AeroSLS BSP entry point and system bootstrap

#include <stdint.h>
#include "kernel_io.h"
#include "entropy.h"
#include "rtc.h"
int  sls_tls_memory_init(void);
int  sls_tls_time_init(void);
#include "../arch/x86/vga.h"
#include "scheduler.h"
#include "microkernel.h"
#include "../arch/x86/gdt.h"
#include "../arch/x86/idt.h"
#include "../arch/x86/lapic.h"
#include "../arch/x86/isr_stubs.h"
#include "timer.h"
#include "process.h"
#include "frame_pool.h"
#include "cap.h"   // Seed Kernel Phase 1 -- cap_init() after the RAM top is bounded
#include "qemu_sls_mmu.h"
#include "qemu_sls_tcache.h"
#include "qemu_sls_pgo.h"
#include "qemu_sls_vm.h"
/* sls-launcher.h is in the sibling qemu repo, accessed at build time */
extern int sls_launch_guest(const void *image, uint32_t len,
                             uint64_t entry_gpa, uint32_t max_insns);
#include "boot_params.h"   // boot-time cluster identity (node=<n>)
#include "boot_image.h"     // Phase 5: initrd boot image + init sidecar launch
#include "smp.h"           // AP bring-up + the uniprocessor fallback
#include "failover.h"      // Step 5 -- peer liveness + checkpoint recovery
#include "partition.h"
#include "service_registry.h"
#include "service_mesh.h"
#include "workload.h"
#include "workload_ctx.h"
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
#include "../kernel/constraint.h"
#include "../kernel/cursor.h"
#include "../kernel/aggregate.h"
#include "../kernel/mqt.h"
#include "../kernel/stream.h"
#include "../kernel/agent.h"
#include "../kernel/rowstore.h"
#include "../kernel/row_index.h"
#include "../kernel/mvcc.h"
#include "../kernel/row_constraint.h"
#include "../kernel/row_journal.h"
#include "../kernel/vecstore.h"
#include "../kernel/vec_index.h"
#include "persist.h"
#include "../drivers/nvme_admin.h"  // Navigator-Parity Gap Roadmap Phase 2 --
                                     // nvme_identify_namespace() prototype (this
                                     // file previously called init_nvme_controller()/
                                     // nvme_io_init() below via implicit int
                                     // declaration with no header at all; adding
                                     // this one include doesn't retroactively fix
                                     // those two, which is a real, separate, pre-
                                     // existing gap named here rather than
                                     // silently left alone or over-broadly "fixed"
                                     // by an unrelated header sweep in this phase)

extern void sls_shell_loop(void);


// ─── Multiboot2 memory map + hardware diagnostics ────────────────────────────
static uint64_t print_hw_info(uint32_t mb2_magic, uint32_t mb2_phys) {
    // 1. Verify magic
    uint64_t top_usable = 0;   /* highest end-of-RAM seen in the mmap */

    /* ── Multiboot v1 path ──────────────────────────────────────────────
     * GRUB 2.12's `module` command only works with `multiboot` (v1).
     * v1 passes 0x2BADB002 and the info struct has mem_lower/mem_upper
     * at fixed offsets (no tag walk needed). */
    if (mb2_magic == 0x2BADB002u && mb2_phys != 0) {
        const uint32_t* v1 = (const uint32_t*)(uintptr_t)mb2_phys;
        uint32_t mem_lower = v1[1];  /* KiB conventional (max 640) */
        uint32_t mem_upper = v1[2];  /* KiB extended (above 1 MiB) */
        (void)mem_lower;
        top_usable = 0x100000ULL + (uint64_t)mem_upper * 1024ULL;
        kernel_serial_printf("[HW] Multiboot v1: mem_upper=%u KiB, top=0x%llx\n",
                             mem_upper, (unsigned long long)top_usable);
        return top_usable;
    }

    if (mb2_magic != (uint32_t)MULTIBOOT2_MAGIC) {
        kernel_serial_printf("[MB2] WARNING: bad magic 0x%x (expected 0x36d76289)\n",
                             mb2_magic);
        return 0;
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
                if (e->type == MB2_MEM_AVAILABLE) {
                    usable_bytes += e->length;
                    /* Track the TOP of usable RAM, not the sum: the frame
                     * allocator indexes by physical address, and a summed
                     * total says nothing about where the holes are. */
                    uint64_t region_end = e->base_addr + e->length;
                    if (region_end > top_usable) top_usable = region_end;
                }
                e = (const struct mb2_mmap_entry*)((const uint8_t*)e + mm->entry_size);
            }
        }
        tag = mb2_tag_next(tag);
    }
    if (usable_bytes)
        kernel_serial_printf("[HW] Usable RAM: %u MiB\n",
                             (uint32_t)(usable_bytes >> 20));
    return top_usable;
}

#ifdef __cplusplus
extern "C"
#endif
void kernel_main(uint32_t mb2_magic, uint32_t mb2_phys) {

    // ── 1a. VGA text-mode HMI — initialise before serial so the screen is
    //        ready as soon as the first kernel_serial_print() fires.
    vga_init();

    // ── 1b. Serial output ─────────────────────────────────────────────────────
    serial_init();
    kernel_serial_print(
        "[AEROSLS BOOT LOGGER V1.0.0 RUNNING]\n"
        "----------------------------------------------------------------------------------\n");

    // ── 2. CPU descriptor tables ───────────────────────────────────────────────
    kernel_serial_print("[BSP] Loading GDT and IDT...\n");
    init_gdt();
    init_idt();

    // ── 2b. Hardware info (CPU + memory map from multiboot2) ──────────────────
    /* Reserve the kernel image BEFORE anything can allocate. Without this
     * the allocator hands out the kernel's own .text/.data/.bss as free
     * pages -- see frame_pool.h. This must stay ahead of process_init(),
     * partition_init() and loader_init() below, all of which allocate. */
    frame_pool_init();

    /* Capture the boot command line before anything consults it. This is a
     * separate, earlier walk than print_hw_info()'s below: identity has to
     * be settled before partition_init(), and print_hw_info() runs here only
     * for the memory map. */
    boot_params_scan_mb2(mb2_magic, mb2_phys);

    /* Remember the first Multiboot2 module — the Phase 5 initrd (the
     * sidecars.cpio boot image). Captured early, alongside the command
     * line, while the mb2 info block is fresh; launch_init_sidecar()
     * consumes it at step 7d. */
    boot_image_capture_mb2(mb2_magic, mb2_phys);

    uint64_t top_usable = print_hw_info(mb2_magic, mb2_phys);
    /* ...and bound the top, so the pool never offers memory the machine
     * does not have. The bitmap spans a fixed 4 GiB regardless of the
     * real amount installed. */
    frame_pool_limit_ram(top_usable);

    // ── 2b½. Seed Kernel Phase 1: capability layer ───────────────────────
    // Must run AFTER frame_pool_limit_ram(): the shared-memory arena carve
    // (frame_pool_reserve_contiguous) scans the frame-pool bitmap for a
    // free run, and scanning before the top bound is set would happily
    // pick memory the machine does not have. Before any process can spawn.
    cap_init();

    // ── 2c. QEMU-SLS Phase 1: shadow page table subsystem ─────────────────
    qemu_sls_mmu_init();

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

    /* ── 4c-ante. Cluster identity, from the boot command line ─────────────
     * This MUST precede partition_init(), which stamps PARTITION_SYSTEM's
     * owner with cluster_local_node_id() (kernel/partition.c:66). Resolve
     * identity afterwards and partition 0 is recorded as owned by node 0
     * while the node believes it is node 3 -- every ownership check
     * downstream then reads that split wrongly.
     *
     * Reading the MAC instead was rejected for exactly this reason: it does
     * not exist until e1000_init(), far below. See boot_params.h. */
    boot_params_apply_node_identity();

    // ── 4c-bis. LPAR groundwork: partition table (Phase 8) ─────────────────
    partition_init();
    service_registry_init();   // Orchestration Plan Phase 4 -- name -> partition/node/endpoint
    mesh_init();               // Orchestration Plan Phase 6 -- circuit breakers
    workload_init();           // Orchestration Plan Phase 5 -- declarative workloads (reconciler OFF by default)
    wlctx_init();              // live execution contexts -- the producer partition_migrate() needs
    failover_init();           // Step 5 -- peer liveness + checkpoint recovery (after cluster identity)

    // ── 4d. Service binary loader ───────────────────────────────────────────
    loader_init();

    // ── 4e. Web asset store ────────────────────────────────────────────────
    webapp_init();   // installs built-in Navigator welcome page

    // ── 4e½. Entropy and wall clock (TLS Phase 0/1) ───────────────────────
    // Placed BEFORE auth_init() so that anything which later wants to issue a
    // real token has a CSPRNG available, and early enough that the console
    // carries the result where a boot log will show it.
    //
    // Both are allowed to fail and neither is fatal. entropy_init() refuses
    // when no source reaches the seeding threshold, and rtc_init() refuses a
    // clock it cannot trust -- see kernel/entropy.h and kernel/rtc.h. A node
    // that boots without them still serves HTTP; it just cannot start TLS,
    // which is the intended fail-closed behaviour rather than a reason to
    // halt.
    //
    // These calls did not exist until the boot-diversity gate ran against a
    // real cluster and every node answered ready=false. The subsystem
    // compiled, linked, and passed 31 host tests -- all of which call
    // entropy_init() themselves -- while being completely inert in the actual
    // kernel. Nothing in a unit test can catch "the boot path never invokes
    // this"; only running the real thing can.
    if (entropy_init() != ENTROPY_OK) {
        kernel_serial_print("[BOOT] entropy NOT seeded -- TLS will refuse to "
                            "start on this node. See the [ENTROPY] lines above.\n");
    }
    if (rtc_init() != RTC_OK) {
        kernel_serial_print("[BOOT] no trusted wall clock -- certificate "
                            "validity cannot be checked. See the [RTC] line above.\n");
    }

    /* mbedTLS's fixed pool. Must come before any mbedtls_* call, because its
     * calloc has nowhere to allocate from until this runs -- and mbedTLS does
     * not check, it dereferences.
     *
     * Called here for the same reason entropy_init() is: the last time a
     * subsystem in this file was written, linked and left uncalled, it took a
     * cluster test and four wrong diagnoses to notice. tests/boot_init_check.sh
     * now makes that a build failure rather than a discovery. */
    sls_tls_memory_init();

    /* NOT fatal, and not a listener gate either -- there is no TLS listener
     * yet. It reports whether one COULD start, so the answer is on the console
     * from the first boot rather than discovered when the listener is written. */
    if (sls_tls_time_init() != 0) {
        kernel_serial_print("[BOOT] TLS could not start on this node: no "
                            "trusted clock for certificate validity.\n");
    }

    // ── 4f. Token authentication registry ─────────────────────────────────
    auth_init();     // registers 4 demo tokens; prints them to serial log

    // ── 4g. Journal subsystem (IBM i-style before/after-image journaling) ──
    journal_init();

    // ── 4h. Row-level lock manager (Read-Committed isolation) ───────────────
    lock_mgr_init();

    // ── 4i. Secondary index manager ─────────────────────────────────
    index_mgr_init();

    // ── 4j. Constraint engine (UNIQUE / NOT_NULL / RANGE / REFERENCE) ──────
    constraint_init();

    // ── 4k. Server-side cursor engine ────────────────────────────────
    cursor_mgr_init();

    // ── 4l. Materialized Query Table engine ──────────────────────────
    mqt_init();
    // ── 4l-bis. Row-set storage engine (Phase 16, relational layer) ────
    // RAM-only init here (zeroes table_headers[]/page cache); the real
    // NVMe restore happens inside persist_restore_all() (step 7b below),
    // same relationship stream_init() has with persist_restore_all().
    rowstore_init();
    // ── 4l-ter. B-tree row index engine (Phase 17, relational layer) ───
    // RAM-only, no persistence this phase (see row_index.h's design
    // comment) -- no restore step needed at 7b, unlike rowstore_init()
    // above. Must come after rowstore_init() so table_headers[] is ready
    // if an index is ever created eagerly during boot (not done today,
    // but keeps the ordering meaningful).
    row_index_init();
    // ── 4l-quater. MVCC concurrency control (Phase 21, relational layer) ───
    // RAM-only, no persistence this phase (see mvcc.h's design comment,
    // same non-goal row_index_init() above already established) -- no
    // restore step needed at 7b. Ordering relative to rowstore_init()/
    // row_index_init() doesn't matter functionally (mvcc.c looks tables up
    // by name at call time, not at init time), but is placed alongside them
    // for the same "relational-layer subsystems init together" readability
    // this boot sequence already groups by.
    mvcc_init();
    // ── 4l-quinquies. Row-set constraints + journaling (Phase 23, relational
    // layer) ───────────────────────────────────────────────────────────────
    // Both RAM-only, same non-goal as row_index_init()/mvcc_init() above --
    // no restore step at 7b. Must come after mvcc_init() since mvcc.c is the
    // only caller of either subsystem (constraint checks/journal notifies
    // are wired into mvcc_row_insert/update/delete, not called directly by
    // sql_exec.c or anything else), though nothing here actually depends on
    // mvcc_init() having run first -- placed after it for the same
    // call-graph-order readability, not a real ordering requirement.
    row_constraint_init();
    row_journal_init();
    // ── 4l-sexies. Vector store engine (Vector Store Roadmap Phase 1) ───────
    // A wholly separate subsystem from the relational engine above (see
    // docs/AeroSLS-VectorStore-Roadmap-v0.1.md §0's own "new parallel
    // subsystem, not an extension" decision) -- its own page pool, its own
    // collection headers, no dependency on rowstore.c/mvcc.c at all. RAM-only
    // this phase (see vecstore.h's header comment), so -- like row_index_init()
    // and mvcc_init() above -- no restore step at 7b.
    vecstore_init();
    // ── 4l-septies. Approximate nearest-neighbor index (Vector Store
    // Roadmap Phase 6) ─────────────────────────────────────────────────────
    // A third parallel structure over vecstore.c, same relationship
    // row_index.c has to rowstore.c (see vec_index.h's own header
    // comment). RAM-only, no restore step, same reasoning as vecstore_init()
    // immediately above.
    vec_index_init();
    // ── 4n. AI agent engine ───────────────────────────────────────────────────
    agent_init();    // ── 4m. Stream object store (OBJ_TYPE_STREAM) ─────────────────
    // nvme_io_init + stream_init run after the PCI scan (step 7) so that
    // nvme_ctrl is fully set up before we attempt I/O queue creation.
    // Placeholder: stream_init is deferred to step 7b below.
    // ── 5. Microkernel (IPC + 5 services + tier manager) ──────────────────
    microkernel_init();

    // ── 6. Boot Application Processor (Core 1) ─────────────────────────
    kernel_serial_print("[BSP] Waking Core 1...\n");
    /* Returns 0 on a single-CPU machine rather than spinning forever, which
     * is what it used to do -- see smp.h. Everything below works either way;
     * the BSP picks up the AP's periodic work from its own idle points. */
    boot_application_processors(1);
    kernel_serial_print("[BSP] Core 1 online.\n");

    // ── 7. Network stack (PCI scan → e1000 role assignment → bring-up) ─────────────
    kernel_serial_print("[NET] PCI scanning for e1000...\n");
    {
        extern uint32_t pci_read_config(uint8_t, uint8_t, uint8_t, uint8_t);
        // Scan bus 0 for e1000 (Intel vendor 8086, device 100e/10d3/107c).
        // Collect EVERY e1000, not the first: with two cards one belongs to
        // the kernel's mgmt/cluster stack and one can be handed to a user
        // driver sidecar (drv.e1000.0, Driver SDK ABI v0.1 §7).
        int found = 0;
        uint64_t nic_base[E1000_MAX_NICS];
        uint8_t  nic_slot[E1000_MAX_NICS];
        for (int slot = 0; slot < 32 && found < E1000_MAX_NICS; slot++) {
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
                    kernel_serial_printf("[NET] e1000 #%d at PCI slot %d MMIO 0x%lx\n",
                                         found, slot, base);
                    nic_base[found] = base;
                    nic_slot[found] = (uint8_t)slot;
                    found++;
                }
            }
        }

        if (found > 0) {
            /* Assign roles BEFORE bring-up, so the loop below can skip the
             * NICs that belong to user drivers. Command line first
             * (`nic0=both nic1=none` — grub.cfg reserves NIC #1 for
             * drv.e1000.0); enumeration order only when none was given. */
            if (e1000_assign_roles(found, boot_params_cmdline()))
                kernel_serial_print("[NET] NIC roles taken from the command line.\n");
            else if (found > 1)
                kernel_serial_print("[NET] NIC roles by enumeration order "
                                    "(no nicN= given): nic0=mgmt nic1=cluster.\n");
            int up = 0;
            for (int i = 0; i < found; i++) {
                if (e1000_nic_roles(i) != (uint8_t)NIC_ROLE_NONE) {
                    e1000_init(i, nic_base[i], nic_slot[i], e1000_nic_roles(i));
                    up++;
                } else {
                    /* Role-less NIC: never programmed or polled by this
                     * stack — handed to the user driver sidecar. */
                    e1000_driver_handoff(i, nic_base[i], nic_slot[i]);
                }
            }
            if (up > 0) {
                net_init();   // sends gratuitous ARP
                dhcp_start(); // DISCOVER → OFFER → REQUEST → ACK; updates net_my_ip
            }
            kernel_serial_printf(
                "[NET] %d e1000 interface(s) online, %d reserved for user drivers.\n",
                up, found - up);
        } else {
            kernel_serial_print("[NET] e1000 not found — network disabled.\n");
        }
    }
    // ── 7b. NVMe PCI scan + I/O queue + stream persistence ────────────────
    {
        extern uint32_t pci_read_config(uint8_t, uint8_t, uint8_t, uint8_t);
        for (int slot = 0; slot < 32; slot++) {
            uint32_t vid_did = pci_read_config(0, (uint8_t)slot, 0, 0x00);
            if (vid_did == 0xFFFFFFFF) continue;
            // NVMe class code = 0x01 0x08 0x02 (bits 31:8 of class register)
            uint32_t cls = pci_read_config(0, (uint8_t)slot, 0, 0x08);
            uint8_t base_cls = (uint8_t)((cls >> 24) & 0xFF);
            uint8_t sub_cls  = (uint8_t)((cls >> 16) & 0xFF);
            uint8_t prog_if  = (uint8_t)((cls >>  8) & 0xFF);
            if (base_cls == 0x01 && sub_cls == 0x08 && prog_if == 0x02) {
                uint32_t bar0 = pci_read_config(0, (uint8_t)slot, 0, 0x10);
                uint32_t bar1 = pci_read_config(0, (uint8_t)slot, 0, 0x14);
                int is64 = ((bar0 & 0x06) == 0x04);
                uint64_t nvme_mmio = (uint64_t)(bar0 & 0xFFFFF000UL);
                if (is64) nvme_mmio |= ((uint64_t)(bar1) << 32);
                kernel_serial_printf("[NVME] Controller at PCI slot %d MMIO 0x%lx\n",
                                      slot, nvme_mmio);
                if (nvme_mmio && nvme_mmio < 0x100000000ULL) {
                    // Address is within the 4 GiB identity map — safe to access
                    if (init_nvme_controller(nvme_mmio)) {
                        kernel_serial_print("[NVME] Admin queue ready.\n");
                        // Navigator-Parity Gap Roadmap Phase 2: only needs the
                        // admin queue (not the I/O queue below), so this runs
                        // unconditionally here rather than nested inside the
                        // nvme_io_init() branch -- gives real disk-capacity
                        // reporting even in the (currently unseen but
                        // possible) case where I/O queue setup itself fails.
                        nvme_identify_namespace(1);
                        if (nvme_io_init()) {
                            persist_restore_all();  // restore L2 catalog/records/schemas/programs
                            stream_init();
                        } else {
                            kernel_serial_print("[NVME] I/O queue setup failed; stream cold start.\n");
                            stream_init();
                        }
                    } else {
                        kernel_serial_print("[NVME] Controller init failed; stream cold start.\n");
                        stream_init();
                    }
                } else {
                    kernel_serial_printf("[NVME] MMIO above 4 GiB (0x%lx) \u2014 stream cold start.\n",
                                         nvme_mmio);
                    stream_init();
                }
                break;
            }
        }
    }

    // ── 7c-ante. QEMU-SLS Phase 2: restore translation cache ─────────────────
    qemu_sls_tcache_init();
    qemu_sls_pgo_init();
    qemu_sls_vm_init();

    // ── 7c. Seed default demo-account authority roles ──────────────────────
    // Must run after persist_restore_all() above (step 7b), not from
    // auth_init() (step 4f) -- see auth_seed_default_roles()'s own comment
    // for why: role_table[] is restored by the same persist mechanism as
    // object_catalog[], so seeding it any earlier would just get overwritten
    // by that restore. Fixes catalog_check_access() silently treating every
    // demo account as ROLE_GUEST (its default for an unregistered uid),
    // which denied vector-store/table writes even for dave's DB_ADMIN token.
    auth_seed_default_roles();

    // ── 7d. Sidecar subsystem: boot image + init sidecar launch ─────────────
    // Phase 5 (self-hosted): reserve the boot-image span, copy the init/DM
    // images to their declared addresses, build the device registry, and
    // create the init sidecar (which then spawns the Device Manager at
    // runtime via SYS_SLS_CREATE_SIDECAR). Non-fatal: with no initrd the
    // kernel boots as before.
    launch_init_sidecar();

    kernel_serial_print(
        "----------------------------------------------------------------------------------\n"
        "[SLS] System ready. Launching secure shell...\n"
        "----------------------------------------------------------------------------------\n");

    // ── 8. HTTP server runs on Core 1 alongside the flush/service loop ─────────
    // In the current build the BSP runs the shell; Core 1 already runs
    // microkernel_service_poll() from smp.c.  Kick the HTTP server on the BSP
    // as a foreground loop only if the NIC is present; otherwise fall through
    // to the shell.
    /* Was `if (e1000_mmio_base)`, a global the driver no longer keeps now
     * that it holds one struct per interface. Same question, asked of the
     * driver instead of a variable it happened to export. */
    if (e1000_nic_count() > 0) {
        http_server_run();  // does not return — serves REST API on port 3000
    }

    // Phase 5 self-hosted: with an initrd the sidecars own the console —
    // the AP core's console_service_tick drains their channels to serial
    // and forwards typed input back. The legacy shell's blocking
    // read_line() would steal every serial byte from that path (typed
    // input echoed twice/garbled, or never reaching the sidecar), so the
    // BSP idles instead of entering it. The LAPIC timer on this core
    // still fires console_service_tick as the uniprocessor fallback, and
    // the AP core's service poll handles it on SMP boots.
    if (boot_image_loaded()) {
        for (;;) __asm__ volatile("hlt");
    }

    // ── Shell (does not return) ─────────────────────────────────────────────
    sls_shell_loop();
}