/*
 * qemu_sls_mmu.h — Phase 1: host-MMU shadow page tables for QEMU-SLS.
 * See docs/AeroSLS-QEMU-SLS-Viability-Analysis.md §Phase 1.
 *
 * Steps covered:
 *   1.1  Contiguous GPA→HVA mapping  (qemu_sls_mmu_map_guest_ram)
 *   1.2  Shadow page-table build     (qemu_sls_mmu_shadow_fault, guest PT walk)
 *   1.4  Fault-handler hook          (called from kernel/stubs.c handle_page_fault)
 *
 * Step 1.3 (TCG backend: emit direct MOV instead of softmmu helpers) lives in
 * the QEMU-SLS TCG backend patch, not here.
 */
#ifndef QEMU_SLS_MMU_H
#define QEMU_SLS_MMU_H

#include <stdint.h>

/*
 * Guest physical address space is mapped at this fixed offset in the host VA
 * space.  GPA X → host VA (QEMU_GPA_HOST_BASE + X).
 *
 * Placed at 32 TiB — above the kernel's identity-mapped lower 4 GiB, below the
 * 4-level-paging canonical-address hole that starts at 128 TiB. (This comment
 * said "2 TiB" until it was checked: 0x200000000000 is 2^45, which is 32 TiB,
 * not 2. Both are inside the safe span so nothing was broken, but it is the
 * number the TCG backend patch reasons about.)
 *
 * This window is how the EMULATOR reaches guest memory — reading guest page
 * tables, DMA, image load. It is NOT how translated guest code addresses
 * memory: the shadow PT maps guest virtual addresses directly (see
 * shadow_install() in the .c), so emitted loads and stores use a bare GVA with
 * no base register at all.
 *
 * ⚠ CORRECTION (2026-08-04) — the paragraph above describes a design that has
 * been RETIRED. See AeroSLS-QEMU-SLS-Guest-Address-Space-Design-v0.1.md.
 *
 * Translated code addresses guest memory as `guest address + guest_base`,
 * where guest_base IS this window base and lives in TCG_REG_R12. That is the
 * qemu-user model, it shipped in Step 5, and it measured 86 → 16 bytes of host
 * code per guest access.
 *
 * Two reasons the GVA-direct variant is not being built:
 *
 *   1. It cannot work as the code stands. qemu_sls_mmu_init() copies all 512
 *      kernel PML4 entries BY VALUE, so the shadow shares every lower-level
 *      table with the kernel. user_map_page() follows present entries rather
 *      than cloning them, so installing a guest mapping at a low address does
 *      not shadow the kernel's mapping — it OVERWRITES it, in the kernel's own
 *      live page tables. A paging-off guest at GPA 0..256 MiB would remap the
 *      kernel image (1..221 MiB) out from under itself while executing. That
 *      sharing is harmless at 32 TiB, which is the only range ever mapped so
 *      far, and is what makes the window fix in map_guest_ram() correct.
 *
 *   2. It buys almost nothing. `mov eax,[rbx+r12+disp32]` and
 *      `mov eax,[rbx+disp32]` are both one instruction; the difference is a
 *      SIB byte and one reserved register. Address coincidence only matters
 *      when guest code runs NATIVELY on the host CPU — same-ISA
 *      virtualisation, which KVM owns and which the repositioning plan
 *      retired. In cross-ISA emulation the generated host code can address
 *      guest memory however it likes.
 *
 * Guest PAGING support is unaffected and still required: shadow_fault() will
 * walk the guest's tables and install GVA → (frame + window), inside the
 * window's PML4 subtree where table sharing is already safe.
 *
 * Overridable so a host test can point the window at a real buffer; the
 * production value is asserted separately in tests/qemu_sls_mmu_host_test.c.
 */
#ifndef QEMU_GPA_HOST_BASE
#define QEMU_GPA_HOST_BASE   0x0000200000000000ULL
#endif

/* Maximum guest RAM in 4 KiB pages (256 MiB). */
#define QEMU_GUEST_RAM_PAGES 65536U

#define QEMU_MAX_REGIONS 8

typedef enum { QEMU_REGION_RAM = 0, QEMU_REGION_MMIO = 1 } QemuRegionType;

typedef struct {
    uint64_t       gpa_start;  /* inclusive */
    uint64_t       gpa_end;    /* exclusive */
    uint64_t       hva_base;   /* host VA for gpa == gpa_start */
    QemuRegionType type;
    int            active;
} QemuGuestRegion;

/* Written by TCG vCPU on every emulated MOV to CR3. */
extern uint64_t qemu_sls_guest_cr3;

/* ─── Does the guest have paging enabled? ──────────────────────────────────
 * Set from the emulated CR0.PG bit; 0 at reset, as on real hardware.
 *
 * This is not a detail. With paging OFF the guest's virtual addresses ARE its
 * physical addresses, so map_guest_ram()'s contiguous window already resolves
 * every access and no fault should ever reach the shadow walker. If one does,
 * it means the guest touched an unbacked GPA -- a real error that must be
 * reported, not "resolved".
 *
 * Walking qemu_sls_guest_cr3 in that state is worse than useless: with paging
 * off, CR3 holds whatever the guest last wrote or nothing at all, and a walk
 * over arbitrary guest memory can easily find bit patterns with the PRESENT
 * bit set. The walker would then install a mapping to a frame chosen by
 * garbage, return "handled", and the guest would quietly read the wrong page.
 * A wrong answer that looks like a right one -- which is the single most
 * expensive failure shape this project has met.
 */
extern int qemu_sls_guest_paging_on;

/* ─── The GUEST window: where emitted code sees guest memory ───────────────
 *
 * Two windows, two purposes. They must be separate PML4 slots, and the reason
 * is not tidiness -- it is that one linear address cannot have two meanings.
 *
 *   QEMU_GPA_HOST_BASE   (32 TiB, slot 64)  EMULATOR window.
 *       Identity: GPA + base → frame(GPA). Present in BOTH the kernel root
 *       and the shadow root, and never changes. This is how the emulator
 *       reads guest page tables, does DMA, and loads images -- including
 *       from inside shadow_fault(), which runs while the shadow root is
 *       loaded, so it has to be in both.
 *
 *   QEMU_GUEST_WINDOW_BASE (64 TiB, slot 128)  GUEST window.
 *       guest_base for emitted code: a guest access at V compiles to a host
 *       access at V + this. Present in the SHADOW root ONLY, because nothing
 *       but translated guest code should ever address memory this way.
 *       Identity while the guest has paging off (V == GPA). Once the guest
 *       enables paging its own tables decide, and shadow_fault() populates
 *       this window from them.
 *
 * With a single shared window, enabling guest paging is unimplementable: guest
 * code needs V + base to mean frame(P), the emulator needs P + base to mean
 * frame(P), and with V != P one of them silently reads the wrong frame -- no
 * fault, no log line. Splitting them costs one PML4 slot and removes the
 * conflict entirely, with no page-table cloning and no world switch.
 *
 * Overridable for host tests, like the emulator window. Note the asymmetry:
 * the emulator window must point at real, dereferenceable memory because
 * gpa_to_hva() reads through it. The guest window is never dereferenced by
 * this file -- it is only ever an address handed to user_map_page() -- so a
 * test may point it anywhere, as long as it is a different PML4 slot.
 */
#ifndef QEMU_GUEST_WINDOW_BASE
#define QEMU_GUEST_WINDOW_BASE  0x0000400000000000ULL
#endif

/* ─── Turning paging ON needs a SECOND window. Not yet built. ──────────────
 *
 * With paging off, map_guest_ram() identity-maps (GPA + window) → frame(GPA),
 * and that mapping is SHARED between the shadow root and the kernel root
 * (init copies all 512 PML4 entries by value; map_guest_ram publishes the
 * window entry back). Emitted guest code and the emulator therefore agree,
 * because with paging off guest virtual == guest physical and there is only
 * one correct answer.
 *
 * The moment the guest enables paging that stops being true:
 *
 *   - guest code at GVA V accesses V + window, which must resolve to
 *     frame(P), where the guest's own tables translate V → P
 *   - the emulator must still read GPA P at P + window to walk those very
 *     tables (gpa_to_hva)
 *
 * One linear address, two required meanings, one shared page table. Whichever
 * is installed, the other silently reads the wrong frame -- no fault, no log
 * line. Exactly the failure shape qemu_sls_guest_paging_on was added to stop.
 *
 * The fix is NOT copy-on-write page tables. It is two windows:
 *
 *   EMULATOR window  (this one, QEMU_GPA_HOST_BASE): identity GPA → frame,
 *       in both roots, never changes. How the emulator reaches guest memory.
 *   GUEST window     (a second PML4 slot): guest_base for emitted code, in
 *       the SHADOW root only. Identity while paging is off; populated by
 *       shadow_fault from the guest's tables once it is on.
 *
 * Two slots, two purposes, no cloning and no world switch. Until that exists,
 * qemu_sls_guest_paging_on must never be set to 1 in production -- see the
 * refusal in sls-launcher.c's MOV CR0 handler.
 */
#define QEMU_GUEST_WINDOW_UNIMPLEMENTED 1

/* Physical address of the shadow PML4; load into CR3 before running TCG code. */
extern uint64_t qemu_sls_shadow_cr3;

/* Nonzero only while translated guest code is executing. The launcher sets it
 * around sls_exec_run(). qemu_sls_mmu_shadow_fault() refuses to resolve
 * anything when it is clear: the shadow table maps guest VIRTUAL addresses, so
 * nothing about a faulting address distinguishes a guest access from a kernel
 * bug, and "resolving" a kernel bug turns a diagnosable halt into an infinite
 * fault loop. */
extern int qemu_sls_guest_active;

/* invlpg on one page. A function, not inline asm at the call site, so a host
 * test can observe the invalidation instead of executing a privileged
 * instruction -- same reason as arch_read_cr3(). */
void qemu_sls_invlpg(uint64_t va);

/* Full non-global TLB flush (CR3 reload). A function, not inline asm, for the
 * same reason as invlpg and arch_read_cr3(): a host test must be able to see
 * that it happened without executing a privileged instruction. */
void qemu_sls_flush_tlb(void);

/*
 * Called when the guest sets CR0.PG. Drops the guest window's identity
 * mappings -- correct only while guest virtual == guest physical -- so that
 * subsequent accesses fault and resolve through the guest's own page tables.
 * Requires qemu_sls_guest_cr3 to have been set already.
 *
 * Returns 0 on success, -1 if refused (not initialised, or CR3 still 0).
 * Idempotent.
 */
int qemu_sls_mmu_guest_paging_enable(void);

/* ─── Self-modifying guest code: the only way to see a guest store ─────────
 *
 * With tcg_use_softmmu false, a guest store compiles to a bare host MOV. There
 * is no helper call, no TLB lookup, nothing to hook -- so a guest that writes
 * to a page it has already executed produces NO signal, and the translation
 * cache goes on serving blocks compiled from bytes that no longer exist.
 * Stale code executing silently is the worst failure this layer can produce.
 *
 * So a page that translated code was generated from is write-protected in the
 * guest window. The next guest store to it takes a #PF, which is the hook that
 * did not otherwise exist: qemu_sls_mmu_shadow_fault() bumps the page's
 * generation counter (invalidating every TB compiled from it), restores write
 * permission, and returns resolved so the store retries and succeeds.
 *
 * The cost is one fault per page per modification, which is what QEMU pays for
 * the same guarantee. The alternative is not paying it -- and not knowing.
 *
 * Returns 0 on success, -1 if the GPA is not backed.
 */
int qemu_sls_mmu_write_protect_gpa(uint64_t gpa);

/* One-time boot init: allocates shadow PML4 inheriting all kernel PT entries. */
int qemu_sls_mmu_init(void);

/*
 * Allocate physical frames for [gpa_start, gpa_start + size_pages*4096) and
 * map them in the shadow PT at host VA (QEMU_GPA_HOST_BASE + gpa_start).
 * Returns 0 on success, -1 on OOM or range overflow.
 */
int qemu_sls_mmu_map_guest_ram(uint64_t gpa_start, uint32_t size_pages);

/*
 * Called from handle_page_fault() for kernel-mode faults in the guest VA space.
 * Walks qemu_sls_guest_cr3's page table, installs the shadow PTE, and returns 0
 * (resolved → isr14_stub iretq re-executes the faulting instruction).
 * Returns 1 when the guest itself has no mapping (caller should inject guest #PF).
 */
int qemu_sls_mmu_shadow_fault(uint64_t faulting_gva, uint32_t error_code);

/* Returns the guest-region record whose HVA range contains hva, or NULL. */
const QemuGuestRegion *qemu_sls_mmu_find_region(uint64_t hva);

/* Zero-copy DMA (Phase 4): map guest physical address to host pointers. */
void    *qemu_sls_dma_host_ptr(uint64_t gpa);    /* host VA for direct R/W */
uint64_t qemu_sls_dma_frame_phys(uint64_t gpa);  /* physical frame for DMA hardware */

#endif /* QEMU_SLS_MMU_H */
