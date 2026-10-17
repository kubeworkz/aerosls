#ifndef USER_PAGING_H
#define USER_PAGING_H

#include <stdint.h>
#include <stddef.h>   // offsetof — the frozen-offset static asserts below

// ─── Page flag constants for user-space mappings ─────────────────────────────
#define USER_PTE_PRESENT  (1ULL << 0)
#define USER_PTE_WRITE    (1ULL << 1)
#define USER_PTE_USER     (1ULL << 2)   // hardware U/S bit — allows Ring-3 access
#define USER_PTE_ACCESSED (1ULL << 5)
#define USER_PTE_DIRTY    (1ULL << 6)
#define USER_PTE_PWT      (1ULL << 3)  // write-through (device WC)
#define USER_PTE_PCD      (1ULL << 4)  // cache-disabled (device uncached)
// NX bit: bit 63. We define EXEC as absence of NX.
#define USER_PTE_EXEC     0ULL          // no NX bit set = executable
#define USER_PTE_NOEXEC   (1ULL << 63)  // NX bit set = non-executable

#define USER_PTE_FRAME_MASK 0x000FFFFFFFFFF000ULL

// ─── MSR numbers for SYSCALL/SYSRET ──────────────────────────────────────────
#define MSR_EFER         0xC0000080
#define MSR_STAR         0xC0000081
#define MSR_LSTAR        0xC0000082
#define MSR_SFMASK       0xC0000084
#define MSR_GS_BASE      0xC0000101
#define MSR_KERNEL_GS    0xC0000102

#define EFER_SCE         (1ULL << 0)   // SYSCALL Enable
#define EFER_NXE         (1ULL << 11)  // No-Execute Enable (makes bit 63 of PTE valid)

// ─── Per-CPU data structure (swapgs target) ───────────────────────────────────
// syscall_entry_stub accesses [gs:0] and [gs:8] for RSP swapping.
struct PerCPUData {
    uint64_t user_rsp;    // offset 0: scratch for saving user RSP on syscall entry
    uint64_t kernel_rsp;  // offset 8: kernel stack pointer loaded on syscall entry
    // offset 0x10: syscall-entry registers, saved by syscall_entry_stub
    // right after its pushes — [0x10]=r11, [0x18]=rcx, [0x20]=r15,
    // [0x28]=r14, [0x30]=r13, [0x38]=r12, [0x40]=rbx, [0x48]=rbp.
    // Park/resume capture (proc_park_capture, cap_maybe_handoff,
    // sys_sls_yield) reads THESE instead of the entry frame on the syscall
    // stack: the -O2 call chain (do_syscall -> k_chan_* -> ...) may reuse
    // [top-64..top-8] for its own locals before the capture runs, so the
    // stack copy is not safe to read late (caught live: resumed processes
    // sysret'd with garbage rbx/rbp/user_rsp and faulted at 0 / wrote to
    // 0xf000ff53f000ff73). The scratch is per-CPU and overwritten by every
    // syscall entry; it is only read mid-syscall by the current process,
    // so the values always describe the CURRENT syscall.
    uint64_t syscall_regs[8];
    // offset 0x50: E7 (increment 2, Linux ABI Shim Design §3.4) — the six
    // Linux argument registers, captured by syscall_entry_stub BEFORE any
    // call, in a SIBLING block rather than a widening of syscall_regs:
    // [0x50]=rdi, [0x58]=rsi, [0x60]=rdx, [0x68]=r10, [0x70]=r8, [0x78]=r9.
    // A Linux-personality task passes six values where the SLS ABI passes
    // one pointer, and the entry path clobbers the argument registers
    // (`mov rsi,rdi; mov rdi,rax` in .unknown_syscall) before C sees them —
    // so the ONLY safe read is this capture, done before any call for the
    // same reason syscall_regs is: the -O2 call chain may reuse the entry
    // frame. The existing offsets are frozen by the static asserts below;
    // this block is appended, never interleaved.
    uint64_t linux_args[6];
};

/* The offsets the asm addresses literally. These exist so a C-side reorder
 * of PerCPUData becomes a BUILD error instead of a silent desync with
 * arch/x86/syscall.asm, whose [gs:0xNN] literals cannot be checked by the
 * compiler. The asm side of the same contract is held by clause B/C of
 * tests/linux_personality_check.sh. */
_Static_assert(offsetof(struct PerCPUData, syscall_regs) == 0x10,
               "syscall_regs must stay at [gs:0x10]: syscall.asm addresses it literally");
_Static_assert(offsetof(struct PerCPUData, linux_args) == 0x50,
               "linux_args must stay at [gs:0x50]: syscall.asm addresses it literally");
_Static_assert(sizeof(struct PerCPUData) == 0x80,
               "PerCPUData grows only by appending — every existing offset is frozen");

extern struct PerCPUData per_cpu_data[4];  // one slot per core

// ─── Public API ───────────────────────────────────────────────────────────────

// Configure IA32_STAR / IA32_LSTAR / IA32_SFMASK / EFER.SCE
// Call once from BSP during kernel_main before spawning any user process.
void syscall_gate_init(void);

// Clone the current PML4, copying kernel (upper-half) entries only.
// Returns the physical address of the new PML4, or 0 on failure.
uint64_t user_clone_page_table(void);

// Map one 4-KiB page in the given PML4 with the specified flags.
// Allocates intermediate tables from the physical frame pool as needed.
void user_map_page(uint64_t* pml4, uint64_t vaddr, uint64_t paddr, uint64_t flags);

// Map `npages` of physical memory into the process's address space at the
// SAME virtual addresses (identity) with Ring-3 access — the sidecar BIB
// MEM cap contract (the BIB carries the cap's physical base and the sidecar
// reads it directly). Unlike user_map_page() this is SAFE over the shared
// kernel identity map: the walk re-points only the child's own copies of
// the path (fresh PDPT/PD with shared entries copied, fresh PTs per touched
// 2 MiB chunk with the huge page replicated at 4 KiB) and never writes into
// the kernel's tables or treats a huge-page PD entry as a table pointer.
// See the definition in user_paging.c. Returns 0 on success, -1 on frame
// exhaustion.
int user_map_identity(uint64_t* pml4, uint64_t phys, uint32_t npages,
                      uint64_t flags);

/* Strong override of cap.c's weak cap_arch_identity_map_user: called by the
 * kernel after a MEM cap is granted across a channel (cap_recv_msg) so the
 * receiver can read the region at its physical base in ring 3. Derives the
 * leaf flags from the cap's CAP_PERM_* rights and identity-maps into the
 * receiver's PML4. */
int cap_arch_identity_map_user(uint64_t pml4_phys, uint64_t phys,
                               uint32_t npages, uint32_t cap_perms);

// Clear the leaf PTE for vaddr (if any level of the walk exists). The
// intermediate tables are left in place -- Phase 1 does no page-table
// garbage collection, same posture as the rest of the kernel. Non-
// allocating: unlike get_or_alloc(), this never creates missing levels.
void user_unmap_page(uint64_t* pml4, uint64_t vaddr);

// Phase 2 teardown: free EVERY frame a process's address space owns -- the
// user-half intermediate tables, the process's own leaf data frames
// (binary/stack/SIMI-scratch; the cap arena and the shared SIMI activation
// cache are refused), and finally the PML4 itself. Kernel-half slots are
// shared by pointer and never touched. Called from process_exit()/
// process_kill() AFTER cap_table_teardown() has unmapped all cap-derived
// PTEs. See the definition in user_paging.c for the full safety argument.
void user_destroy_page_table(uint64_t pml4_phys);

/* Reads CR3. A named function rather than inline asm at each site because
 * `mov %%cr3` is privileged: any host test that links a translation unit
 * containing it dies before main() gets anywhere -- which is exactly what
 * happened to kernel/qemu_sls_mmu.c, whose entire shadow-PT layer was
 * untestable for that one instruction.
 *
 * NOT inline, deliberately: a static inline would still emit the instruction
 * into the caller and the seam would not exist. kernel/qemu_sls_mmu.c uses
 * this; arch/x86/walk_page_tables_x86.c, kernel/smp.c and this file's own
 * user_paging.c:90 still open-code it and should be converted when something
 * needs to test them. */
uint64_t arch_read_cr3(void);

#endif /* USER_PAGING_H */
