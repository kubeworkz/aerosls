#include "process.h"
#include "object_catalog.h"
#include "kernel_io.h"
#include "loader.h"
#include "../arch/x86/user_paging.h"

extern void* allocate_physical_ram_frame(void);

struct ProcessDescriptor proc_table[PROC_MAX];
uint32_t                 proc_count = 0;

static uint32_t next_pid = 100;   // PIDs start above the microkernel service PIDs

// Virtual address at which user process code is mapped.
// Must be a canonical user-space address (bit 47 = 0, bits 63:48 = 0).
// x86-64 canonical user range: 0x0000000000000000 – 0x00007FFFFFFFFFFF.
// 0x0000400000000000 = 64 TiB, well within range and clear of SLS objects.
#define USER_PROC_CODE_BASE 0x0000400000000000ULL

// ─── String helpers ───────────────────────────────────────────────────────────
static size_t pr_strlen(const char* s) { size_t n=0; while(s[n]) n++; return n; }
static int    pr_streq(const char* a, const char* b) {
    while (*a && *b) { if (*a!=*b) return 0; a++; b++; }
    return *a == *b;
}
static void pr_strcpy(char* d, const char* s, size_t n) {
    size_t i; for (i=0; i<n-1&&s[i]; i++) d[i]=s[i]; d[i]='\0';
}

// ─── process_init ─────────────────────────────────────────────────────────────
void process_init(void) {
    for (int i = 0; i < PROC_MAX; i++) proc_table[i].active = 0;
    kernel_serial_print("[PROC] Ring-3 process manager initialised.\n");
}

// ─── process_create ───────────────────────────────────────────────────────────
uint32_t process_create(struct ProcCreateRequest* req) {
    if (!req) return 0;

    // 1. Look up the SERVICE_PROCESS object in the SLS catalog
    struct SLSObjectEntry* obj = 0;
    uint32_t               obj_idx = 0;
    for (uint32_t i = 0; i < object_catalog_count; i++) {
        if (!object_catalog[i].active) continue;
        if (object_catalog[i].type != OBJ_TYPE_SERVICE_PROCESS) continue;
        if (pr_streq(object_catalog[i].name, req->object_name)) {
            obj     = &object_catalog[i];
            obj_idx = i;
            break;
        }
    }
    if (!obj) {
        kernel_serial_printf("[PROC] create: SERVICE_PROCESS '%s' not found.\n",
                             req->object_name);
        return 0;
    }

    // 2. Find a free process slot
    struct ProcessDescriptor* pd = 0;
    for (int i = 0; i < PROC_MAX; i++) {
        if (!proc_table[i].active) { pd = &proc_table[i]; break; }
    }
    if (!pd) {
        kernel_serial_print("[PROC] create: process table full.\n");
        return 0;
    }

    // 3. Clone the kernel page table (upper half only) for this process
    uint64_t new_cr3 = user_clone_page_table();
    if (!new_cr3) {
        kernel_serial_print("[PROC] create: page table clone failed.\n");
        return 0;
    }

    uint64_t* pml4 = (uint64_t*)(uintptr_t)new_cr3;

    // 4. Load the binary (ELF64 or flat) into the process page table.
    //    If no binary is registered yet, map empty execute pages (allows
    //    a later 'upload' + 'load' to populate the frames).
    uint64_t entry_rip = loader_load_into_process(
                             req->object_name, obj->base_vaddr, pml4);

    if (!entry_rip) {
        // No binary: fall back to empty executable pages so the descriptor
        // is created; the process can be reloaded once a binary is uploaded.
        kernel_serial_printf(
            "[PROC] '%s': no binary loaded — process created with empty pages.\n",
            req->object_name);
        for (uint32_t p = 0; p < obj->size_pages; p++) {
            void* frame = allocate_physical_ram_frame();
            if (!frame) break;
            user_map_page(pml4, obj->base_vaddr + (uint64_t)p * 4096,
                          (uint64_t)(uintptr_t)frame,
                          USER_PTE_PRESENT | USER_PTE_USER | USER_PTE_EXEC);
        }
        entry_rip = obj->base_vaddr;
    }

    // 5. Allocate and map a user-space stack
    uint64_t stack_vaddr = obj->base_vaddr + (uint64_t)obj->size_pages * 4096 + 4096;
    for (uint32_t p = 0; p < PROC_USER_STACK_PAGES; p++) {
        void* frame = allocate_physical_ram_frame();
        if (!frame) break;
        user_map_page(pml4, stack_vaddr + p * 4096,
                      (uint64_t)(uintptr_t)frame,
                      USER_PTE_PRESENT | USER_PTE_USER | USER_PTE_WRITE);
    }
    uint64_t user_rsp = stack_vaddr + PROC_USER_STACK_PAGES * 4096 - 16;

    // 6. Populate the descriptor
    pd->pid             = next_pid++;
    pr_strcpy(pd->name, req->object_name, PROC_NAME_LEN);
    pd->object_id       = obj->object_id;
    pd->cr3             = new_cr3;
    pd->user_rip        = entry_rip;
    pd->user_rsp        = user_rsp;
    pd->owner_uid       = req->owner_uid;
    pd->state           = PROC_RUNNING;
    pd->active          = 1;
    proc_count++;

    kernel_serial_printf(
        "[PROC] Spawned PID %u: '%s'  RIP=0x%016lx  RSP=0x%016lx  CR3=0x%016lx\n",
        pd->pid, pd->name, pd->user_rip, pd->user_rsp, pd->cr3);
    (void)obj_idx;
    (void)pr_strlen;

    // 7. Enter Ring-3, saving the kernel continuation so process_exit() can
    //    return here cleanly.
    kernel_enter_ring3(&pd->kernel_rsp, &pd->kernel_cr3,
                       pd->cr3, pd->user_rip, pd->user_rsp);

    // Resumes here after SYS_SLS_EXIT
    kernel_serial_printf("[PROC] PID %u '%s' returned to kernel.\n",
                         pd->pid, pd->name);
    return pd->pid;
}

// ─── program_spawn ────────────────────────────────────────────────────────────
// Spawn a Ring-3 process from an OBJ_TYPE_PROGRAM catalog object.
//
// Design notes:
//   - OBJ_TYPE_PROGRAM is the SLS-native executable concept: the type tag in
//     the catalog IS the execute permission. No filesystem, no chmod +x.
//   - The binary image was uploaded via SYS_SLS_UPLOAD_BINARY and lives in the
//     ServiceBinary store keyed by object name, exactly as SERVICE_PROCESS.
//   - Page-table mapping reuses loader_load_into_process() which handles both
//     ELF64 and flat binaries. NX bits are set per ELF segment flags.
//   - User stack is placed immediately above the object's virtual range, with
//     a one-page guard gap, identical to the SERVICE_PROCESS layout.
uint32_t program_spawn(const char* object_name, uint32_t owner_uid) {
    if (!object_name) return 0;

    // 1. Look up the OBJ_TYPE_PROGRAM object
    struct SLSObjectEntry* obj = 0;
    for (uint32_t i = 0; i < object_catalog_count; i++) {
        if (!object_catalog[i].active) continue;
        if (object_catalog[i].type != OBJ_TYPE_PROGRAM) continue;
        if (pr_streq(object_catalog[i].name, object_name)) {
            obj = &object_catalog[i];
            break;
        }
    }
    if (!obj) {
        kernel_serial_printf(
            "[PROC] program_spawn: OBJ_TYPE_PROGRAM '%s' not found in catalog.\n",
            object_name);
        return 0;
    }

    // 2. Find a free process slot
    struct ProcessDescriptor* pd = 0;
    for (int i = 0; i < PROC_MAX; i++) {
        if (!proc_table[i].active) { pd = &proc_table[i]; break; }
    }
    if (!pd) {
        kernel_serial_print("[PROC] program_spawn: process table full.\n");
        return 0;
    }

    // 3. Clone the kernel page table (upper half only) — fresh address space
    uint64_t new_cr3 = user_clone_page_table();
    if (!new_cr3) {
        kernel_serial_print("[PROC] program_spawn: page table clone failed.\n");
        return 0;
    }
    uint64_t* pml4 = (uint64_t*)(uintptr_t)new_cr3;

    // 4. Map the program binary into the new address space.
    //    loader_load_into_process() handles ELF64 (per-segment NX/RW flags)
    //    and flat binaries (mapped executable at obj->base_vaddr).
    //    OBJ_TYPE_PROGRAM pages get NX cleared only for executable segments —
    //    data/BSS segments keep NX set, enforcing W^X at the page level.
    uint64_t entry_rip = loader_load_into_process(
                             object_name, USER_PROC_CODE_BASE, pml4);
    if (!entry_rip) {
        kernel_serial_printf(
            "[PROC] program_spawn: '%s' has no binary. "
            "Upload via SYS_SLS_UPLOAD_BINARY first.\n", object_name);
        return 0;   // refuse to spawn an empty PROGRAM object
    }

    // 5. Allocate and map a user-space stack (W, no-exec, user-accessible)
    //    Guard gap: leave one page between the text segment and the stack.
    uint64_t stack_base = USER_PROC_CODE_BASE
                        + (uint64_t)obj->size_pages * 4096
                        + 4096; /* guard page */
    for (uint32_t p = 0; p < PROC_USER_STACK_PAGES; p++) {
        void* frame = allocate_physical_ram_frame();
        if (!frame) break;
        user_map_page(pml4, stack_base + (uint64_t)p * 4096,
                      (uint64_t)(uintptr_t)frame,
                      USER_PTE_PRESENT | USER_PTE_USER
                      | USER_PTE_WRITE | USER_PTE_NOEXEC);
    }
    uint64_t user_rsp = stack_base + PROC_USER_STACK_PAGES * 4096 - 16;

    // 6. Populate the process descriptor
    pd->pid          = next_pid++;
    pr_strcpy(pd->name, object_name, PROC_NAME_LEN);
    pd->object_id    = obj->object_id;
    pd->cr3          = new_cr3;
    pd->user_rip     = entry_rip;
    pd->user_rsp     = user_rsp;
    pd->owner_uid    = owner_uid;
    pd->state        = PROC_RUNNING;
    pd->active       = 1;
    proc_count++;

    kernel_serial_printf(
        "[PROC] program_spawn: PID %u '%s'  "
        "RIP=0x%016lx  RSP=0x%016lx  CR3=0x%016lx\n",
        pd->pid, pd->name, pd->user_rip, pd->user_rsp, pd->cr3);

    // 7. Enter Ring-3, saving the kernel continuation so process_exit() can
    //    return here cleanly.
    kernel_enter_ring3(&pd->kernel_rsp, &pd->kernel_cr3,
                       pd->cr3, pd->user_rip, pd->user_rsp);

    // Resumes here after SYS_SLS_EXIT
    kernel_serial_printf("[PROC] PID %u '%s' returned to kernel.\n",
                         pd->pid, pd->name);
    return pd->pid;
}

// ─── process_exit ────────────────────────────────────────────────────────────
// Called from SYS_SLS_EXIT dispatch.  Finds the running Ring-3 process,
// marks it ZOMBIE, restores the kernel's RSP and CR3 saved by
// kernel_enter_ring3(), then rets back into program_spawn() / process_create().
void process_exit(uint32_t exit_code) {
    for (int i = 0; i < PROC_MAX; i++) {
        struct ProcessDescriptor* pd = &proc_table[i];
        if (!pd->active || pd->state != PROC_RUNNING) continue;
        if (!pd->kernel_rsp) continue;   // not entered via kernel_enter_ring3

        uint64_t saved_rsp = pd->kernel_rsp;
        uint64_t saved_cr3 = pd->kernel_cr3;

        pd->kernel_rsp = 0;
        pd->kernel_cr3 = 0;
        pd->state      = PROC_ZOMBIE;
        pd->active     = 0;
        proc_count--;

        kernel_serial_printf(
            "[PROC] PID %u '%s' exited (code=%u).\n",
            pd->pid, pd->name, exit_code);

        // Restore kernel page table and stack, then return to kernel_enter_ring3
        // caller.  The 'ret' pops the return address from [saved_rsp].
        __asm__ volatile(
            "mov %0, %%cr3\n\t"   /* restore kernel page tables */
            "mov %1, %%rsp\n\t"   /* restore kernel stack         */
            "ret\n\t"              /* return to kernel_enter_ring3 call site */
            :
            : "r"(saved_cr3), "r"(saved_rsp)
            : "memory"
        );
        __builtin_unreachable();
    }
    kernel_serial_print("[PROC] exit: no active Ring-3 process found.\n");
}

// ─── process_kill ─────────────────────────────────────────────────────────────
void process_kill(uint32_t pid) {
    for (int i = 0; i < PROC_MAX; i++) {
        if (!proc_table[i].active || proc_table[i].pid != pid) continue;
        proc_table[i].state  = PROC_ZOMBIE;
        proc_table[i].active = 0;
        proc_count--;
        kernel_serial_printf("[PROC] PID %u terminated.\n", pid);
        return;
    }
    kernel_serial_printf("[PROC] kill: PID %u not found.\n", pid);
}

// ─── sys_sls_proc_list ────────────────────────────────────────────────────────
void sys_sls_proc_list(void) {
    kernel_serial_printf(
        "\n[PROC] Process Table\n"
        " %-5s  %-24s  %-10s  %-4s  %s\n"
        " -----  ------------------------  ----------  ----  ------------------\n",
        "PID", "Name", "State", "UID", "User RIP");

    uint32_t shown = 0;
    for (int i = 0; i < PROC_MAX; i++) {
        struct ProcessDescriptor* pd = &proc_table[i];
        if (!pd->active) continue;
        kernel_serial_printf(
            " %-5u  %-24s  %-10s  %-4u  0x%016lx\n",
            pd->pid, pd->name,
            proc_state_name(pd->state),
            pd->owner_uid, pd->user_rip);
        shown++;
    }
    if (!shown) kernel_serial_print(" (no processes)\n");
    kernel_serial_printf(" %u process(es).\n\n", shown);
}
