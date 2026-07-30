/*
 * kernel/stubs.c — Provides symbols referenced by legacy subsystem code that
 * have not yet been fully implemented.  Each stub is either a minimal working
 * implementation or a safe no-op that lets the kernel boot cleanly.
 *
 * Also supplies freestanding equivalents of the C-library string functions
 * that GCC may emit calls to even when -ffreestanding is active.
 */

#include <stdint.h>
#include <stddef.h>
#include "kernel_io.h"
#include "object_catalog.h"
#include "process.h"
#include "fault_report.h"

// ─── C-library string functions (freestanding replacements) ──────────────────

int strcmp(const char* a, const char* b) {
    while (*a && *b && *a == *b) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}

int strncmp(const char* a, const char* b, size_t n) {
    while (n-- && *a && *b) {
        if (*a != *b) return (unsigned char)*a - (unsigned char)*b;
        a++; b++;
    }
    return 0;
}

size_t strlen(const char* s) {
    size_t n = 0; while (s[n]) n++; return n;
}

char* strcpy(char* dst, const char* src) {
    char* d = dst; while ((*d++ = *src++)); return dst;
}

void* memcpy(void* dst, const void* src, size_t n) {
    uint8_t* d = (uint8_t*)dst; const uint8_t* s = (const uint8_t*)src;
    while (n--) *d++ = *s++;
    return dst;
}

void* memset(void* dst, int val, size_t n) {
    uint8_t* d = (uint8_t*)dst;
    while (n--) *d++ = (uint8_t)val;
    return dst;
}

int memcmp(const void* a, const void* b, size_t n) {
    const uint8_t* p = (const uint8_t*)a;
    const uint8_t* q = (const uint8_t*)b;
    while (n--) { if (*p != *q) return *p - *q; p++; q++; }
    return 0;
}

// ─── Page fault handler ───────────────────────────────────────────────────────
// Called from isr14_stub.  Error code bit 2 (U/S): set = Ring-3 fault → kill
// the process and return to kernel.  Clear = kernel fault → panic.
void handle_page_fault(unsigned long error_code, unsigned long saved_rip) {
    unsigned long faulting_address;
    __asm__ volatile("mov %%cr2, %0" : "=r"(faulting_address));

    if (error_code & 0x4) {
        kernel_serial_printf(
            "[FAULT] Ring-3 #PF  error=0x%lx  addr=0x%016lx  rip=0x%016lx  — killing process.\n",
            error_code, faulting_address, saved_rip);
        process_exit(139);
        process_exit(139);   /* SIGSEGV-equivalent */
        /* process_exit() restores kernel context and does not return here */
    }
    kernel_serial_printf(
        "\n[FAULT] Kernel #PF  error=0x%lx  addr=0x%016lx  — Halting.\n",
        error_code, faulting_address);
    __asm__ volatile("cli");
    for (;;) __asm__ volatile("hlt");
}

/* The bootstrap stack, from arch/x86/boot.asm. Their ADDRESSES are the bounds;
 * the objects are never read. */
extern char stack_bottom[], stack_top[];

/* Dumps the interrupted stack, anchored at the interrupted RSP rather than at
 * the handler's own frame -- the first version anchored at the handler and
 * stopped three qwords short of the one value that mattered.
 *
 * `rip_slot` points at the saved RIP in the interrupt frame, so the five-qword
 * frame is rip_slot[0..4] = RIP, CS, RFLAGS, RSP, SS, and rip_slot[3] is the
 * stack pointer the interrupted code had. Everything at and above that is the
 * live stack of whatever was running; everything below it, down to the
 * overflowing buffer, is the region its epilogue already popped and which
 * nothing has since overwritten. */
static void fault_dump_stack(const uint64_t* rip_slot) {
    const uint64_t* lo = (const uint64_t*)(void*)stack_bottom;
    const uint64_t* hi = (const uint64_t*)(void*)stack_top;

    if (!rip_slot) {
        kernel_serial_print(
            "[FAULT] could not locate the interrupt frame -- no rip/cs pair matching the\n"
            "[FAULT] reported values was found on the handler's stack. Dump skipped rather\n"
            "[FAULT] than printed from a guessed offset.\n");
        return;
    }

    uint64_t irq_rip    = rip_slot[0];
    uint64_t irq_rsp    = rip_slot[3];
    uint64_t irq_rflags = rip_slot[2];
    const uint64_t* sp  = (const uint64_t*)(uintptr_t)irq_rsp;

    kernel_serial_printf("[FAULT] interrupted rsp=0x%016lx  rflags=0x%lx\n",
                         (unsigned long)irq_rsp, (unsigned long)irq_rflags);

    if (sp < lo || sp > hi) {
        kernel_serial_printf(
            "[FAULT] that rsp is OUTSIDE the bootstrap stack [%p,%p) -- the stack pointer\n"
            "[FAULT] itself was corrupted, or this fault came from a core with its own\n"
            "[FAULT] stack. Not dumping memory at an address that may not be mapped.\n",
            (void*)lo, (void*)hi);
        return;
    }
    kernel_serial_printf(
        "[FAULT] stack is [%p,%p), 64 KiB; the interrupted frame was %lu byte(s) deep.\n",
        (void*)lo, (void*)hi, (unsigned long)((const char*)hi - (const char*)sp));

    /* The measurement that names the buffer. */
    int poison = fault_poison_byte(irq_rip);
    if (poison >= 0 && sp > lo && sp[-1] == irq_rip) {
        const uint64_t* base = fault_poison_run_base(sp - 1, lo, irq_rip);
        unsigned long run = (unsigned long)((const char*)sp - (const char*)base);
        kernel_serial_printf(
            "[FAULT] POISON RUN: 0x%016lx .. 0x%016lx = %lu byte(s) of 0x%02x, ending at\n"
            "[FAULT] the smashed frame. The LOW end is the base of the buffer that overran,\n"
            "[FAULT] and %lu is how far it wrote. Look for a local array of about that size\n"
            "[FAULT] in whatever was on the stack here.\n",
            (unsigned long)(uintptr_t)base, (unsigned long)(uintptr_t)(sp - 1),
            run, (unsigned)poison, run);
    }

    /* Window straddling the boundary: 8 qwords below the interrupted rsp (the
     * popped, poisoned region) and 16 above (the frames still live). Where the
     * two stop looking alike is the top of the overrun. */
    const uint64_t* from = (sp - 8 < lo) ? lo : sp - 8;
    const uint64_t* to   = (sp + 16 > hi) ? hi : sp + 16;
    kernel_serial_printf("[FAULT] stack %p..%p (interrupted rsp marked >>):\n",
                         (void*)from, (void*)to);
    for (const uint64_t* p = from; p < to; p += 2)
        kernel_serial_printf("[FAULT] %s %p: %016lx %016lx\n",
                             (p == sp) ? ">>" : "  ", (void*)p,
                             (unsigned long)p[0],
                             (unsigned long)((p + 1 < to) ? p[1] : 0));
}

static void fault_explain(unsigned long saved_rip) {
    int poison = fault_poison_byte(saved_rip);
    if (fault_noncanonical(saved_rip))
        kernel_serial_print(
            "[FAULT] rip is NON-CANONICAL -- the CPU never fetched from it. This is a\n"
            "[FAULT] return address (or indirect target) that was overwritten, not a\n"
            "[FAULT] bad jump: #GP(0) is raised while loading rip, hence error=0x0.\n");
    if (poison >= 0)
        kernel_serial_printf(
            "[FAULT] rip is the byte 0x%02x repeated 8 times -- it is DATA, and that byte\n"
            "[FAULT] identifies the writer. Search the payload of whatever was in flight\n"
            "[FAULT] for 0x%02x; the buffer holding it is the one that overran.\n",
            (unsigned)poison, (unsigned)poison);
}

// ─── General Ring-3 fault handler (#UD/#GP/#SS/#NP) ─────────────────────────
// saved_cs bits 0-1 = CPL; CPL==3 → Ring-3 → kill process.  Else panic.
void handle_ring3_fault(unsigned long error_code, unsigned long saved_cs, unsigned long saved_rip) {
    if ((saved_cs & 3) == 3) {
        kernel_serial_printf(
            "[FAULT] Ring-3 fault  cs=0x%lx  error=0x%lx  rip=0x%016lx  — killing process.\n",
            saved_cs, error_code, saved_rip);
        process_exit(134);
    }
    kernel_serial_printf(
        "\n[FAULT] Kernel fault  cs=0x%lx  error=0x%lx  rip=0x%016lx  — Halting.\n",
        saved_cs, error_code, saved_rip);
    fault_explain(saved_rip);
    /* Search upward from this frame for the rip/cs pair the CPU pushed. 64
     * qwords is far more than any stub prologue and still nowhere near the
     * top of the stack. */
    {
        const uint64_t* base = (const uint64_t*)__builtin_frame_address(0);
        fault_dump_stack(fault_find_iret_frame(base, base + 64,
                                               (uint64_t)saved_rip,
                                               (uint64_t)saved_cs));
    }
    __asm__ volatile("cli");
    for (;;) __asm__ volatile("hlt");
}

// ─── Legacy SLS allocation (syscall 105) ─────────────────────────────────────
// Called directly from the syscall assembly stub.  Delegates to the object
// catalog lookup introduced in Phase 1.
void* sys_sls_allocate(void* arg) {
    if (!arg) return 0;
    struct { uint64_t object_id; uint64_t size; uint32_t flags; }* req = arg;
    for (uint32_t i = 0; i < object_catalog_count; i++) {
        if (object_catalog[i].active &&
            object_catalog[i].object_id == req->object_id)
            return (void*)(uintptr_t)object_catalog[i].base_vaddr;
    }
    return 0;
}

// ─── FNV-1a hash (used by lockfree_map.c) ────────────────────────────────────
uint64_t generate_unique_object_id(const char* key, size_t length) {
    uint64_t h = 0xcbf29ce484222325ULL;
    for (size_t i = 0; i < length; i++) {
        h ^= (uint8_t)key[i];
        h *= 0x00000100000001B3ULL;
    }
    return h;
}

// ─── SLS address space allocator (used by lockfree_map.c) ────────────────────
// This mirrors the allocator in object_catalog.c; lockfree_map.c calls it
// for older-style direct allocations.
#define LM_SLS_BASE 0x0000700000000000ULL
static uint64_t lm_next_vaddr = LM_SLS_BASE;
static uint64_t lm_next_disk  = 1000;

struct SLSObjectLM { uint64_t start_virtual_address; uint64_t size_in_bytes; };

struct SLSObjectLM create_persistent_region(size_t size) {
    struct SLSObjectLM obj;
    size_t aligned = (size + 4095) & ~(size_t)4095;
    obj.start_virtual_address = lm_next_vaddr;
    obj.size_in_bytes         = aligned;
    lm_next_vaddr            += aligned;
    lm_next_disk             += aligned / 512;
    return obj;
}

// ─── Flush daemon globals ─────────────────────────────────────────────────────
// flush_daemon.c iterates these to find dirty SLS pages.  Phase 1+ stores
// objects in the object_catalog instead; keep these as a bridge.
struct SLSObjectLM global_sls_object_table[256];
size_t             total_active_sls_objects = 0;

// ─── Storage I/O stubs ────────────────────────────────────────────────────────
uint64_t get_object_disk_block_mapping(uint64_t virtual_address) {
    (void)virtual_address;
    return 0;   // disk sector mapping resolved via NVMe path in Phase A+
}

void storage_write_block(uint64_t disk_block_id, void* ram_frame) {
    (void)disk_block_id;
    (void)ram_frame;
    // NVMe write is handled by the flush daemon's direct DMA path.
}

// ─── I/O priority broker stub ────────────────────────────────────────────────
void block_thread_on_storage_token(uint32_t thread_id,
                                    uint16_t command_id,
                                    uint64_t faulting_vaddr) {
    (void)thread_id; (void)command_id; (void)faulting_vaddr;
    // Full implementation: add thread to the storage-wait queue, yield.
    // For now, operations are synchronous so no blocking is needed.
}

// ─── Consensus page-table stub ────────────────────────────────────────────────
// Called by consensus.c during split-brain to strip write permissions.
void update_page_table_permissions_globally(uint32_t force_read_only) {
    (void)force_read_only;
    // Full implementation: iterate all page tables, clear PTE_WRITABLE.
    // Deferred until page table management is complete.
}

// Multi-Node Partition Scaling Roadmap Phase 4: partition-scoped sibling of
// the stub above, called by consensus.c's new per-partition lease election/
// heartbeat logic (net/consensus.c) instead of the global one -- narrows
// split-brain write-stripping to just the objects of the partition whose
// lease is being contested/regained, not every SLS object on the node.
// Still a stub for the identical reason the global one is: real
// implementation means iterating page tables (this time filtered to the
// given partition's processes only) and clearing/restoring PTE_WRITABLE,
// deferred until page table management is complete. Both stubs will need
// real bodies together, not one before the other.
void update_page_table_permissions_for_partition(uint32_t partition_id, uint32_t force_read_only) {
    (void)partition_id;
    (void)force_read_only;
}

// ─── Security matrix verification stub ───────────────────────────────────────
// Called by secure_api.c to check OWNER capability.
int verify_expanded_matrix_access(uint32_t uid, uint32_t gid,
                                   uint64_t object_id, uint32_t needed_mask) {
    (void)gid;
    // Delegate to the Phase 2 catalog check
    for (uint32_t i = 0; i < object_catalog_count; i++) {
        if (!object_catalog[i].active ||
            object_catalog[i].object_id != object_id) continue;
        if (object_catalog[i].owner_uid == uid) return 1;
        return (object_catalog[i].perm_mask & needed_mask) == needed_mask;
    }
    return uid == 0 ? 1 : 0;   // root always passes
}

// ─── Lazy FPU per-task state (Gap Remediation SIMI Phase 10) ──────────────────
// lazy_fpu.c calls this to find the current task's AVX-512 save buffer.
// This used to unconditionally return NULL. That comment claimed
// "lazy_fpu.c guards against this," which is only true for the FIRST #NM
// trap ever taken (fpu_hardware_owner and current_task both NULL, so
// handle_device_not_available_fault()'s early-return fires and nothing
// crashes). Every trap after that hits the exact same NULL==NULL early
// return, unconditionally -- meaning no task's FPU/AVX-512 state was ever
// actually saved or restored across a context switch. Not a crash, but
// silent cross-task register-state leakage, which "guards against this"
// was never actually true protection against.
//
// Fixed with a real, persistent per-thread-id side table, using
// kernel_get_current_thread_id() (kernel/scheduler.c) as the current
// execution identity -- the only one this kernel has today.
//
// arch/x86/scheduler_extended.h (the real struct ExtendedTask) is
// deliberately NOT #included here, and not even via a bare extern of the
// type: this file already #includes "process.h" above, which itself
// #includes "scheduler.h" -- and scheduler.h's own `enum TaskState`
// shares every enumerator name with scheduler_extended.h's `enum
// TaskState` (TASK_READY/RUNNING/BLOCKED all collide; confirmed by
// actually trying it first and hitting real "redeclaration of enumerator"
// errors under the kernel's exact X86_CFLAGS, not assumed). C does not
// allow two enums to redeclare the same enumerator names in one
// translation unit, and process.h's existing include makes that
// unavoidable here.
//
// struct ExtendedTaskShadow below is a byte-for-byte layout-compatible
// mirror of scheduler_extended.h's real struct ExtendedTask (uint32_t id;
// a 4-byte enum; uint64_t rsp; a 64-byte-aligned 2688-byte buffer) built
// from plain integer types instead, so no enum needs to cross the
// boundary at all. lazy_fpu.c's xsave/xrstor only ever touch the
// avx512_state_buffer bytes through its own real struct ExtendedTask*
// view of this SAME memory (kernel_get_current_task_struct() hands back
// a `void*`, exactly as it always did) -- the two views only need to
// agree on layout, not be the literal same named type.
//
// Honest scope note: this ties FPU/AVX-512 ownership to scheduler.c's
// cooperative kernel-thread id space, not to a separate user-process
// identity -- this kernel has no distinct "current user process"
// accessor today (kernel/process.c has none). Correct and sufficient to
// unblock Phase 10 (float opcodes); revisit if/when SIMI user processes
// get their own distinct scheduling identity apart from this one.
struct ExtendedTaskShadow {
    uint32_t id;
    uint32_t state;   /* enum TaskState's real width; value only ever set
                        * to TASK_RUNNING's numeric constant (1) here,
                        * never otherwise inspected by this file. */
    uint64_t rsp;
    __attribute__((aligned(64))) uint8_t avx512_state_buffer[2688];
};

extern uint32_t kernel_get_current_thread_id(void);

#define FPU_TASK_STATES_MAX 64   /* mirrors kernel/scheduler.h's MAX_TASKS */
static struct ExtendedTaskShadow g_fpu_task_states[FPU_TASK_STATES_MAX];
static uint8_t                   g_fpu_task_state_used[FPU_TASK_STATES_MAX];

void* kernel_get_current_task_struct(void) {
    uint32_t tid = kernel_get_current_thread_id();
    for (uint32_t i = 0; i < FPU_TASK_STATES_MAX; i++) {
        if (g_fpu_task_state_used[i] && g_fpu_task_states[i].id == tid)
            return &g_fpu_task_states[i];
    }
    for (uint32_t i = 0; i < FPU_TASK_STATES_MAX; i++) {
        if (!g_fpu_task_state_used[i]) {
            g_fpu_task_state_used[i] = 1;
            g_fpu_task_states[i].id    = tid;
            g_fpu_task_states[i].state = 1;   /* TASK_RUNNING */
            g_fpu_task_states[i].rsp   = 0;
            return &g_fpu_task_states[i];
        }
    }
    return 0;   /* table full -- matches this kernel's established
                 * static-array-exhaustion convention; lazy_fpu.c's own
                 * NULL-guard (fpu_hardware_owner == current_task) still
                 * applies here as a safety net, same as before this fix. */
}
