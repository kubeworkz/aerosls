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
