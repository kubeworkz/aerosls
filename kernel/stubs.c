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

// ─── Lazy FPU task struct stub ────────────────────────────────────────────────
// lazy_fpu.c calls this to find the current task's FPU save buffer.
void* kernel_get_current_task_struct(void) {
    return 0;   // Returns NULL; lazy_fpu.c guards against this.
}
