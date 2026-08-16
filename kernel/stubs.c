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
#include "qemu_sls_mmu.h"

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
/* The bootstrap stack, from arch/x86/boot.asm. Their ADDRESSES are the bounds;
 * the objects are never read. Declared here rather than below fault_dump_stack()
 * because handle_page_fault() now checks them too -- taking the address of an
 * extern array is a link-time constant, so this costs no memory access, which
 * matters on a path that runs when memory access is what failed. */
extern char stack_bottom[], stack_top[];

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
    /* Phase 1: shadow PT miss — walk guest PT, install mapping, retry. */
    if (qemu_sls_mmu_shadow_fault((uint64_t)faulting_address,
                                   (uint32_t)error_code) == 0)
        return;

    /* ─── M7: a genuine guest #PF ────────────────────────────────────────
     * shadow_fault() refused, and a kernel #PF is the usual reason. But when
     * ALL of these hold -- the fault was taken by translated guest code
     * (guest active), the guest has paging enabled, and the address is
     * inside the guest window -- the ONLY remaining reason is that the
     * guest's OWN page tables do not map the address. On hardware that is
     * guest #PF(14) with CR2 = the faulting VA, and this build has no
     * exception delivery, so M7's gate is fault RECORDING: sls_i386_guest_pf
     * (sls-launcher.c) writes vector 14, CR2, the error code and the
     * faulting RIP into the launcher's fault record and returns to the
     * launcher -- a launch ending in a guest fault is a recorded, diagnosable
     * outcome instead of a kernel panic. CR2 is converted to the guest VA
     * (the faulting host address is guest_va + the window base).
     *
     * The in-window test is the same PML4-slot check shadow_fault() uses
     * (shadow_va_in_window); everything failing the three conditions is a
     * real kernel fault and falls through to the panic below, unchanged. */
    if (qemu_sls_guest_active && qemu_sls_guest_paging_on &&
        ((faulting_address >> 39) & 0x1FF) ==
            ((QEMU_GUEST_WINDOW_BASE >> 39) & 0x1FF)) {
        extern void sls_i386_guest_pf(uint64_t cr2, uint32_t error_code,
                                      uint64_t rip);
        sls_i386_guest_pf(faulting_address - QEMU_GUEST_WINDOW_BASE,
                          (uint32_t)error_code, saved_rip);
        /* does not return */
    }
    /* ─── kernel_panic_puts(), NOT kernel_serial_printf() ──────────────────
     * This line used kernel_serial_printf(). While an HTTP shell command is
     * running, user/shell.c:457 has redirected kernel_serial_putchar() into a
     * memory buffer, and a path that halts never reaches the _stop() that
     * flushes it -- so this message was written and then thrown away. A kernel
     * page fault produced a completely empty log for an entire session, with
     * gdb eventually showing the CPU stopped on the `hlt` two instructions
     * below. See kernel/kernel_io.h's panic-path note. */
    uint64_t sp_now;
    __asm__ volatile("mov %%rsp, %0" : "=r"(sp_now));

    kernel_panic_puts("\n[FAULT] Kernel #PF  error=");
    kernel_panic_hex64((uint64_t)error_code);
    kernel_panic_puts("  addr=");
    kernel_panic_hex64((uint64_t)faulting_address);
    kernel_panic_puts("\n[FAULT] rip=");
    kernel_panic_hex64((uint64_t)saved_rip);
    kernel_panic_puts("  rsp=");
    kernel_panic_hex64(sp_now);
    kernel_panic_puts("  stack=[");
    kernel_panic_hex64((uint64_t)(uintptr_t)stack_bottom);
    kernel_panic_puts(",");
    kernel_panic_hex64((uint64_t)(uintptr_t)stack_top);
    kernel_panic_puts(")\n");

    /* ─── The check that would have named this bug on sight ────────────────
     * The fault that motivated all of the above had RSP 208 KiB BELOW
     * stack_bottom -- the kernel stack had overflowed its 64 KiB region and
     * was running down through sls_heap, overwriting it. Nothing reported
     * that. The frame-pool guards added earlier watch the ALLOCATOR handing
     * out stack frames; none of them watches the stack pointer leaving its own
     * region, which is a different failure and was undefended.
     *
     * Printed before any interpretation of the faulting address, because when
     * the stack is gone the faulting address is usually a consequence rather
     * than a cause, and chasing it wastes the session. */
    if (sp_now < (uint64_t)(uintptr_t)stack_bottom ||
        sp_now >= (uint64_t)(uintptr_t)stack_top) {
        uint64_t under = (sp_now < (uint64_t)(uintptr_t)stack_bottom)
                       ? (uint64_t)(uintptr_t)stack_bottom - sp_now : 0;
        kernel_panic_puts(
            "[FAULT] *** STACK OVERFLOW: rsp is OUTSIDE the bootstrap stack.\n"
            "[FAULT] *** It has run ");
        kernel_panic_dec(under);
        kernel_panic_puts(
            " byte(s) past stack_bottom and has been\n"
            "[FAULT] *** overwriting whatever lies below it. The faulting\n"
            "[FAULT] *** address above is most likely a CONSEQUENCE of this,\n"
            "[FAULT] *** not its cause -- fix the overflow first.\n");
    }

    kernel_panic_puts("[FAULT] -- Halting.\n");
    __asm__ volatile("cli");
    for (;;) __asm__ volatile("hlt");
}

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

    /* ─── The measurement ──────────────────────────────────────────────
     * Scanned UPWARD from the interrupted rsp. Below it is the five-qword
     * frame the CPU just pushed, which sits on top of whatever the faulting
     * epilogue had popped -- reading down there reports the hardware's own
     * RIP/CS/RFLAGS/RSP/SS back as if they were program data, which is exactly
     * what the first version of this did.
     *
     * At and above rsp is the live stack of every frame that had not returned
     * yet, untouched by the interrupt. */
    int poison = fault_poison_byte(irq_rip);
    const uint64_t* end = sp;
    if (poison >= 0 && sp < hi && sp[0] == irq_rip) {
        end = fault_poison_run_end(sp, hi, irq_rip);
        unsigned long run = (unsigned long)((const char*)end - (const char*)sp);
        kernel_serial_printf(
            "[FAULT] POISON RUN: 0x%016lx .. 0x%016lx = %lu byte(s) of 0x%02x, starting AT\n"
            "[FAULT] the interrupted rsp and running UP through the live caller frames.\n",
            (unsigned long)(uintptr_t)sp, (unsigned long)(uintptr_t)end,
            run, (unsigned)poison);

        if (end >= hi) {
            /* Nothing survived. A local array cannot do this -- it would stop
             * where its own overrun stopped, leaving the outer frames intact. */
            kernel_serial_printf(
                "[FAULT] The run reaches the TOP OF THE STACK. Every live frame is payload,\n"
                "[FAULT] so this is NOT a local array running off its end -- that would stop\n"
                "[FAULT] somewhere and leave the outer frames readable. Something wrote over\n"
                "[FAULT] the stack wholesale. The top stack page is 0x%016lx; check whether\n"
                "[FAULT] the frame allocator ever handed it out, and check any DMA target.\n",
                (unsigned long)((uintptr_t)hi - 4096) & ~(uintptr_t)4095);
        } else {
            kernel_serial_printf(
                "[FAULT] First surviving qword is at 0x%016lx = %016lx -- if that is a code\n"
                "[FAULT] address it is the return address of the outermost frame the overrun\n"
                "[FAULT] reached, and the writer is below it.\n",
                (unsigned long)(uintptr_t)end, (unsigned long)end[0]);
        }
    } else if (poison >= 0) {
        kernel_serial_printf(
            "[FAULT] rsp does NOT point at 0x%02x -- the poison is not contiguous with the\n"
            "[FAULT] return point, so the smashed slot was isolated rather than part of a run.\n",
            (unsigned)poison);
    }

    /* Dump the boundary rather than the whole run: the run is uniform by
     * definition and its top edge is the only part with information in it.
     * When there is no run, `end` is rsp and this degenerates to the window
     * around the return point. */
    const uint64_t* from = (end - 4 < sp) ? sp : end - 4;
    const uint64_t* to   = (end + 8 > hi) ? hi : end + 8;
    kernel_serial_printf(
        "[FAULT] stack %p..%p (>> = interrupted rsp, ^^ = first surviving qword):\n",
        (void*)from, (void*)to);
    for (const uint64_t* p = from; p < to; p++)
        kernel_serial_printf("[FAULT] %s %p: %016lx\n",
                             (p == sp) ? ">>" : (p == end && end > sp) ? "^^" : "  ",
                             (void*)p, (unsigned long)p[0]);
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
    /* ─── The essential facts first, by the path that cannot be swallowed ──
     * This path halts, so anything it prints through kernel_serial_printf()
     * while an HTTP shell command is running goes into that command's capture
     * buffer and is never flushed. handle_page_fault(), kernel_panic(),
     * sls_abort() and the TCG helper stubs were all fixed for this; this was
     * the last one, and it is the one with the richest output to lose.
     *
     * Ordered deliberately: cs, error and rip go out via kernel_panic_puts()
     * FIRST, because those three identify the fault and must survive even if
     * .bss is what broke. Only then is the capture released, so the detailed
     * report below -- fault_explain(), the stack dump, the poison-run scan --
     * reaches the wire as well. If capture_stop() itself faults on a corrupted
     * .bss, the facts that matter are already out. */
    kernel_panic_puts("\n[FAULT] Kernel fault  cs=");
    kernel_panic_hex64((uint64_t)saved_cs);
    kernel_panic_puts("  error=");
    kernel_panic_hex64((uint64_t)error_code);
    kernel_panic_puts("  rip=");
    kernel_panic_hex64((uint64_t)saved_rip);
    kernel_panic_puts("  — Halting.\n");

    /* Now let the rich report through. Nothing below can be trusted to reach
     * the wire without this, and nothing below is worth more than the three
     * values already printed. */
    kernel_serial_capture_stop();

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
    /* Diagnostic until the real PTE-walk lands (see the comment above):
     * the write-lease campaign strips this partition's write permission
     * (force_read_only=1) and ONLY a lease quorum restores it (=0). The
     * 2-node failover guard gates on these lines: a survivor that can
     * never win the 2-of-2 lease quorum must log the strip on every
     * campaign and NEVER a restore -- reads-only holding at the
     * page-permission call site, not just in the partition table. */
    kernel_serial_printf(
        "[MMU-LEASE] partition %u: page permissions force_read_only=%u\n",
        (unsigned)partition_id, (unsigned)force_read_only);
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

/* ─── Freestanding string functions the vendored mbedTLS needs ─────────────
 * gcc emits calls to these regardless of -ffreestanding, and mbedTLS's X.509
 * code calls them directly. Each was added because the LINKER named it, not
 * because a list somewhere said a kernel should have them:
 *
 *   memmove  x509write_crt.c, x509write_csr.c -- DER is built back-to-front
 *            and then shifted, so the regions overlap
 *   strncpy  x509write_crt.c, copying validity strings
 *   strstr   x509_crt.c, matching a wildcard in a SAN
 *
 * memmove is the one that matters. memcpy above copies forward and is
 * undefined on overlap; using it here would corrupt certificates in a way
 * that depends on buffer addresses, which is to say intermittently. */

void* memmove(void* dst, const void* src, size_t n) {
    unsigned char* d = (unsigned char*)dst;
    const unsigned char* s = (const unsigned char*)src;
    if (d == s || n == 0) return dst;
    if (d < s) {
        for (size_t i = 0; i < n; i++) d[i] = s[i];
    } else {
        /* Backwards, so a forward-overlapping move does not read bytes it has
         * already overwritten. This is the entire reason memmove exists and
         * the entire reason memcpy cannot stand in for it. */
        for (size_t i = n; i > 0; i--) d[i - 1] = s[i - 1];
    }
    return dst;
}

char* strncpy(char* dst, const char* src, size_t n) {
    size_t i = 0;
    for (; i < n && src[i]; i++) dst[i] = src[i];
    /* C requires the remainder be zero-filled, not merely terminated. Callers
     * rely on it, and mbedTLS's validity-string handling is one of them. */
    for (; i < n; i++) dst[i] = '\0';
    return dst;
}

char* strchr(const char* s, int c) {
    for (; *s; s++) if (*s == (char)c) return (char*)s;
    /* A search for '\0' finds the terminator, not NULL -- C requires it and
     * x509_create.c's name parser relies on it to find the end of a field. */
    return (c == 0) ? (char*)s : 0;
}

char* strstr(const char* hay, const char* needle) {
    if (!*needle) return (char*)hay;
    for (; *hay; hay++) {
        const char* h = hay;
        const char* n = needle;
        while (*h && *n && *h == *n) { h++; n++; }
        if (!*n) return (char*)hay;
    }
    return 0;
}

/* ─── A deliberately small snprintf ────────────────────────────────────────
 * NOT a general printf. It implements the conversions mbedTLS's oid.c and
 * x509.c actually use, extracted from those files: %s %u %c %d %x %X, an
 * optional zero-padded width (%02X), and %%. Anything else stops the format
 * and is reported through the return value, because a formatter that silently
 * skips a directive it does not understand emits a plausible wrong string --
 * and these strings end up in certificate subject names.
 *
 * Returns the length that WOULD have been written, C99 semantics, because
 * mbedTLS's callers add the return to a pointer and compare against the
 * remaining size to detect truncation. Returning the truncated length instead
 * would make them believe a truncated name was complete. */
#include <stdarg.h>

static void sn_putc(char* buf, unsigned long size, unsigned long* n, char c) {
    if (*n + 1 < size) buf[*n] = c;
    (*n)++;
}

static void sn_str(char* buf, unsigned long size, unsigned long* n, const char* s) {
    if (!s) s = "(null)";
    while (*s) sn_putc(buf, size, n, *s++);
}

static void sn_num(char* buf, unsigned long size, unsigned long* n,
                   unsigned long long v, unsigned base, int upper,
                   unsigned width, int zero_pad) {
    char tmp[32];
    const char* digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    unsigned i = 0;
    if (v == 0) tmp[i++] = '0';
    while (v && i < sizeof tmp) { tmp[i++] = digits[v % base]; v /= base; }
    while (i < width && i < sizeof tmp) tmp[i++] = zero_pad ? '0' : ' ';
    while (i) sn_putc(buf, size, n, tmp[--i]);
}

int sls_tls_snprintf(char* buf, unsigned long size, const char* fmt, ...) {
    va_list ap;
    unsigned long n = 0;
    va_start(ap, fmt);

    for (const char* p = fmt; *p; p++) {
        if (*p != '%') { sn_putc(buf, size, &n, *p); continue; }
        p++;
        if (*p == '%') { sn_putc(buf, size, &n, '%'); continue; }

        int zero_pad = 0;
        unsigned width = 0;
        if (*p == '0') { zero_pad = 1; p++; }
        while (*p >= '0' && *p <= '9') { width = width * 10 + (unsigned)(*p - '0'); p++; }

        switch (*p) {
            case 's': sn_str(buf, size, &n, va_arg(ap, const char*)); break;
            case 'c': sn_putc(buf, size, &n, (char)va_arg(ap, int)); break;
            case 'u': sn_num(buf, size, &n, va_arg(ap, unsigned int), 10, 0, width, zero_pad); break;
            case 'd': {
                int v = va_arg(ap, int);
                if (v < 0) { sn_putc(buf, size, &n, '-'); sn_num(buf, size, &n, (unsigned long long)(-(long long)v), 10, 0, width, zero_pad); }
                else sn_num(buf, size, &n, (unsigned long long)v, 10, 0, width, zero_pad);
                break;
            }
            case 'x': sn_num(buf, size, &n, va_arg(ap, unsigned int), 16, 0, width, zero_pad); break;
            case 'X': sn_num(buf, size, &n, va_arg(ap, unsigned int), 16, 1, width, zero_pad); break;
            default:
                /* Unsupported directive. Stop rather than guess: the caller
                 * sees a short result and a visibly incomplete string. */
                p--;
                goto done;
        }
    }
done:
    va_end(ap);
    if (size > 0) buf[(n < size) ? n : size - 1] = '\0';
    return (int)n;
}
