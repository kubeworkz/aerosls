#include "process.h"
#include "object_catalog.h"
#include "kernel_io.h"
#include "loader.h"
#include "partition.h"
#include "frame_pool.h"
#include "../arch/x86/user_paging.h"
#include "../user/permissions.h"
#include "cap.h"   /* Seed Kernel Phase 1: strong cap_current_pid()/cap_proc_cr3() below */
#include "syscall_dispatch.h"   /* Seed Kernel Phase 1.5: do_syscall() from cap_recv_resume() */

/* Seed Kernel Phase 1.5: the resume path iretq's into cap_recv_resume(),
 * which re-runs the parked recv and then jumps to the syscall stub's shared
 * return path (`.syscall_return` in arch/x86/syscall.asm, exported here). */
extern void syscall_return_path(void);

struct ProcessDescriptor proc_table[PROC_MAX];
uint32_t                 proc_count = 0;

static uint32_t next_pid = 100;   // PIDs start above the microkernel service PIDs

// Phase 12 (LPAR): per-partition scheduling cursor — see the definitions
// near pick_next_partition()/pick_next_process_in_partition() below for
// the full design note. Declared here (with proc_table/next_pid) rather
// than next to its user functions purely so process_init() below, which
// resets it, doesn't need a forward declaration.
static int g_last_index_in_partition[PARTITION_MAX];

// Multitenant Isolation Gap Analysis §5 item 8 / §7 item 8: per-partition
// CPU scheduling weight, 0 = unset/default (interpreted as weight 1) --
// see process.h's own header comment on partition_set_cpu_weight() for the
// full design note. BSS zero-init, same "0 is the safe do-nothing default"
// posture partition_frame_quota[] (frame_pool.c) already established.
static uint32_t partition_cpu_weight[PARTITION_MAX];

// How many MORE consecutive turns g_last_partition_scheduled is still
// entitled to before pick_next_partition() must rotate to the next
// candidate. 0 (its BSS-zero-init value) means "no remaining turns owed" --
// with every partition at the default weight of 1, this counter is set to
// (weight - 1) = 0 every time a partition is picked and therefore NEVER
// becomes nonzero, so the weighted branch below never fires and the
// original, unweighted round-robin sequence is reproduced exactly.
static uint32_t g_partition_turns_remaining;

// Effective weight for scheduling purposes: an explicit 0 (never configured,
// or explicitly reset) reads as the neutral default of 1, never 0 itself --
// a partition can never be starved to zero turns per sweep by weight alone.
static uint32_t effective_cpu_weight(uint32_t partition_id) {
    if (partition_id >= PARTITION_MAX) return 1;
    uint32_t w = partition_cpu_weight[partition_id];
    return w == 0 ? 1 : w;
}

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

/* Seed Kernel Phase 1.5/Phase 2: clear every piece of per-process resume /
 * teardown bookkeeping a REUSED descriptor slot must not inherit. Called at
 * spawn init (a fresh process must never resume like its slot's previous
 * occupant — caught live: a partition destroy killed a SUSPENDED child that
 * was still marked resume_sysret after its send handoff; the next spawn
 * reused the slot, inherited the stale flag AND the old occupant's park_ctx,
 * and the "new" child iretq'd into cap_sysret_resume() and sysret'd to the
 * dead child's post-send continuation — exiting code 0 without ever running
 * its main), and at every teardown site (process_exit, process_kill, the
 * deferred schedule_ring3 path) so a killed process cannot leave stale state
 * behind for the next occupant. NOT cleared anywhere a live process needs
 * it: schedule_ring3()/kernel_switch_next() set and consume these flags
 * between a park and its resume. */
static void proc_clear_resume_state(struct ProcessDescriptor* pd) {
    uint64_t* pc = (uint64_t*)&pd->park_ctx;
    for (int i = 0; i < (int)(sizeof(pd->park_ctx) / sizeof(uint64_t)); i++)
        pc[i] = 0;
    pd->park_req       = 0;
    pd->waiting_chan   = CAP_NONE;
    pd->has_ring3_ctx  = 0;
    pd->resume_kernel  = 0;
    pd->resume_sysret  = 0;
    pd->handoff_target = NULL;
    pd->pending_teardown = 0;
}

// ─── process_init ─────────────────────────────────────────────────────────────
void process_init(void) {
    for (int i = 0; i < PROC_MAX; i++) proc_table[i].active = 0;
    // Phase 12: -1 = "never scheduled from this partition yet" for every
    // partition's round-robin cursor (BSS zero-init would give 0, a real
    // proc_table[] index, not a sentinel — must be set explicitly).
    for (int i = 0; i < PARTITION_MAX; i++) g_last_index_in_partition[i] = -1;
    kernel_serial_print("[PROC] Ring-3 process manager initialised.\n");
}

// ─── alloc_pid ───────────────────────────────────────────────────────────────
// Returns a fresh PID that doesn't collide with any currently active process.
// Used by cap_create_sidecar() which bypasses process_create()'s catalog
// lookup (sidecar images are not SLS catalog objects).
uint32_t alloc_pid(void) {
    /* Scan for the highest active PID and return max+1. This is O(PROC_MAX)
     * but that's 16 — negligible, and monotonic PIDs never recycle so no
     * bitmap is needed. */
    uint32_t max_pid = 99;  /* below the 100+ range */
    for (int i = 0; i < PROC_MAX; i++) {
        if (proc_table[i].active && proc_table[i].pid > max_pid)
            max_pid = proc_table[i].pid;
    }
    return max_pid + 1;
}

// ─── nested_ring3_prep ───────────────────────────────────────────────────────
// Seed Kernel Phase 1 (two-party): called immediately before
// kernel_enter_ring3() when the spawner is a live Ring-3 process — i.e. the
// spawn happens from INSIDE a syscall. A nested spawn breaks four invariants
// that a kernel-context spawn (HTTP/shell) never hits; this prep (and the
// symmetric tail of process_exit()) repair them:
//
//   1. GS state at entry. The parent's syscall entry did swapgs, so
//      GS_BASE == &per_cpu_data[0] here. The child must enter Ring-3 with
//      GS_BASE == 0 (the user view) so ITS first syscall entry swapgs lands
//      back on per_cpu_data — otherwise it swapgs's the WRONG direction,
//      `mov rsp,[gs:8]` reads physical address 0x8, and the child triple-
//      faults (verified live: PID 102 'cap_peer' died before its first
//      sls_puts). We swapgs once, before sysret.
//
//   2. Syscall-stack isolation. syscall_entry_stub always pushes onto
//      [gs:8] (per_cpu_data.kernel_rsp). If the CHILD's syscalls pushed onto
//      the shared fixed stack they would overwrite the PARENT's live syscall
//      frame — and the fixed stack sits directly above proc_table in .bss,
//      so frames growing past its bottom would smash kernel state (verified
//      live: proc_table[0].pid became 0x63000000). Each process therefore
//      gets a DEDICATED syscall stack (see syscall_stack_top); we point
//      [gs:8] at it before entering Ring-3.
//
//   3. The parent is BLOCKED while the child runs: it must not be
//      schedulable (schedule_ring3 would switch to its empty ring3_ctx on a
//      timer tick) nor resolve as the current process for cap_current_pid()/
//      process_find_current(). PROC_BLOCKED is excluded by the same
//      exact-value checks that exclude PROC_HELD.
//
//   4. [gs:0] (per_cpu_data.user_rsp) holds the PARENT's user RSP from its
//      syscall entry; the child's syscall entries will overwrite it. We
//      capture it into the child descriptor so process_exit() can restore it
//      before the parent's .syscall_return does `mov rsp,[gs:0]`.
//
// Runs with GS_BASE == &per_cpu_data[0] (inside the syscall).
static void nested_ring3_prep(struct ProcessDescriptor* spawner,
                              struct ProcessDescriptor* child) {
    spawner->state = PROC_BLOCKED;

    __asm__ volatile("movq %%gs:0, %0" : "=r"(child->parent_user_rsp)
                     : : "memory");

    __asm__ volatile("movq %0, %%gs:8" : : "r"(child->syscall_stack_top)
                     : "memory");

    __asm__ volatile("swapgs" : : : "memory");
}

// ─── process_create ───────────────────────────────────────────────────────────
// ─── alloc_proc_syscall_stack ─────────────────────────────────────────────────
// The per-process 8 KiB kernel syscall stack: TWO CONTIGUOUS frames. Historical
// bug (fixed live, Phase 1.5 immediate wake): the per-process stack replaced
// the fixed 8 KiB boot stack but was allocated as a SINGLE frame while still
// addressed as 8192 bytes — syscall_stack_top = frame + 8192 - 8 — so the top
// 4 KiB of every process's syscall stack lived in an UNALLOCATED frame that
// the frame pool handed to other subsystems (page tables, binaries, later
// processes). The allocator's WITHHELD net caught only the pops that hit the
// live stack; the rest were silently reused — the overlap child's address
// space was destroyed mid-run when its syscall-stack upper half (or a frame
// near it) was handed out for the parent's mapping. The frame pool is first-fit
// ASCENDING and the kernel is single-threaded, so two consecutive allocs
// return adjacent frames; if they don't (defensive), free both and retry
// once, then fail. Returns the stack TOP, or 0.
static uint64_t alloc_proc_syscall_stack(uint32_t partition_id) {
    for (int attempt = 0; attempt < 2; attempt++) {
        void* a = allocate_physical_ram_frame_for_partition(partition_id);
        void* b = allocate_physical_ram_frame_for_partition(partition_id);
        if (!a || !b) {
            if (a) free_physical_ram_frame_for_partition(a, partition_id);
            if (b) free_physical_ram_frame_for_partition(b, partition_id);
            return 0;
        }
        uint64_t sa = (uint64_t)(uintptr_t)a;
        uint64_t sb = (uint64_t)(uintptr_t)b;
        if (sb == sa + 4096) return sa + 8192 - 8;   /* contiguous pair */
        free_physical_ram_frame_for_partition(a, partition_id);
        free_physical_ram_frame_for_partition(b, partition_id);
    }
    return 0;
}

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

    // Phase 9 (LPAR): authority-checked spawn. Process spawning never went
    // through the Phase 6/7 authority model at all before this — a real,
    // pre-existing gap, not a regression. catalog_check_access() folds in
    // Phase 8's partition boundary automatically (it's the outermost check
    // inside that function), so this one call closes both gaps at once:
    // a uid outside this object's partition, or one without EXECUTE
    // permission on it, is now denied instead of always succeeding.
    if (!catalog_check_access(req->owner_uid, req->object_name, PERM_EXECUTE)) {
        kernel_serial_printf(
            "[PROC] create: uid %u denied execute access to '%s'.\n",
            req->owner_uid, req->object_name);
        return 0;
    }

    // Phase 13 (LPAR): resolved once, up front, so it can be used both for
    // the quota-checked frame allocations below and the descriptor's
    // partition_id field at step 6 — avoids a second partition_get_for_uid()
    // lookup and guarantees both uses agree.
    uint32_t spawn_partition_id = partition_get_for_uid(req->owner_uid);

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
                             req->object_name, obj->base_vaddr, pml4,
                             spawn_partition_id);

    if (!entry_rip) {
        // No binary: fall back to empty executable pages so the descriptor
        // is created; the process can be reloaded once a binary is uploaded.
        kernel_serial_printf(
            "[PROC] '%s': no binary loaded — process created with empty pages.\n",
            req->object_name);
        for (uint32_t p = 0; p < obj->size_pages; p++) {
            // Phase 13 (LPAR): quota-checked — an unbounded object size_pages
            // is exactly the kind of tenant-attributable, unboundedly-large
            // allocation this phase's quota exists to cap.
            void* frame = allocate_physical_ram_frame_for_partition(spawn_partition_id);
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
        // Phase 13 (LPAR): quota-checked, same rationale as above.
        void* frame = allocate_physical_ram_frame_for_partition(spawn_partition_id);
        if (!frame) break;
        user_map_page(pml4, stack_vaddr + p * 4096,
                      (uint64_t)(uintptr_t)frame,
                      USER_PTE_PRESENT | USER_PTE_USER | USER_PTE_WRITE);
    }
    uint64_t user_rsp = stack_vaddr + PROC_USER_STACK_PAGES * 4096 - 16;

    // 6. Populate the descriptor
    struct ProcessDescriptor* spawner = process_find_current();
    pd->pid             = next_pid++;
    pr_strcpy(pd->name, req->object_name, PROC_NAME_LEN);
    pd->object_id       = obj->object_id;
    pd->cr3             = new_cr3;
    pd->user_rip        = entry_rip;
    pd->user_rsp        = user_rsp;
    pd->owner_uid       = req->owner_uid;
    pd->parent_pid      = spawner ? spawner->pid : 0;   // Phase 1 two-party
    pd->partition_id    = spawn_partition_id;   // Phase 9, resolved once at Phase 13
    pd->state           = PROC_RUNNING;
    pd->priority        = PROC_PRIO_NORMAL;     // Phase 4 (Navigator-Parity): default tier
    pd->active          = 1;
    pd->pending_teardown = 0;   // Phase 2: a reused slot must not inherit a stale flag
    pd->waiting_chan    = CAP_NONE;  // Phase 1.5: not parked on any channel

    // Seed Kernel Phase 1 (two-party): a DEDICATED 8 KiB kernel syscall stack
    // (TWO contiguous frames — see alloc_proc_syscall_stack; a single frame
    // addressed as 8 KiB left the top half in an unallocated frame the pool
    // handed out, corrupting live syscall stacks). The shared fixed stack
    // sits directly above proc_table in .bss, so a nested spawn's child
    // frames must not grow into it. Physical frames are identity-mapped, so
    // the frame address IS the stack address.
    uint64_t sstop = alloc_proc_syscall_stack(spawn_partition_id);
    if (!sstop) {
        kernel_serial_print("[PROC] create: syscall stack allocation failed.\n");
        return 0;
    }
    pd->syscall_stack_top = sstop;

    proc_count++;

    kernel_serial_printf(
        "[PROC] Spawned PID %u: '%s'  RIP=0x%016lx  RSP=0x%016lx  CR3=0x%016lx\n",
        pd->pid, pd->name, pd->user_rip, pd->user_rsp, pd->cr3);
    (void)obj_idx;
    (void)pr_strlen;

    // 7. Enter Ring-3, saving the kernel continuation so process_exit() can
    //    return here cleanly. Nested spawn (parent is a Ring-3 process)
    //    needs the GS/stack/state prep first — see nested_ring3_prep().
    //    Phase 1.5: point [gs:8] at THIS process's dedicated syscall stack
    //    before the first sysret (the direct per_cpu_data write covers
    //    kernel-context spawns; nested_ring3_prep sets it via GS for the
    //    nested case — same value, redundant but harmless).
    per_cpu_data[0].kernel_rsp = pd->syscall_stack_top;
    if (spawner) nested_ring3_prep(spawner, pd);
    kernel_enter_ring3(&pd->kernel_rsp, &pd->kernel_cr3,
                       pd->cr3, pd->user_rip, pd->user_rsp);

    // Resumes here after SYS_SLS_EXIT
    kernel_serial_printf("[PROC] PID %u '%s' returned to kernel.\n",
                         pd->pid, pd->name);
    return pd->pid;
}

// ─── program_spawn_common ───────────────────────────────────────────────────
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
//
// `async` selects the entry mode (Seed Kernel Phase 1.5):
//   - async=0 (program_spawn): synchronous — kernel_enter_ring3() blocks until
//     the child exits (one process runs at a time).
//   - async=1 (program_spawn_nb): the child gets a SYNTHETIC ring3_ctx and is
//     marked runnable; the spawn returns to the caller immediately and the
//     scheduler iretq's into the child. Two processes can then be LIVE at
//     once, genuinely overlapping on timer ticks / kernel park switches.
static uint32_t program_spawn_common(const char* object_name, uint32_t owner_uid,
                                     int async) {
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

    // Phase 9 (LPAR): authority-checked spawn — same gate and rationale as
    // process_create() above; folds in Phase 8's partition boundary too.
    if (!catalog_check_access(owner_uid, object_name, PERM_EXECUTE)) {
        kernel_serial_printf(
            "[PROC] program_spawn: uid %u denied execute access to '%s'.\n",
            owner_uid, object_name);
        return 0;
    }

    // Phase 13 (LPAR): resolved once, same rationale as process_create().
    uint32_t spawn_partition_id = partition_get_for_uid(owner_uid);

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
                             object_name, USER_PROC_CODE_BASE, pml4,
                             spawn_partition_id);
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
        // Phase 13 (LPAR): quota-checked, same rationale as process_create().
        void* frame = allocate_physical_ram_frame_for_partition(spawn_partition_id);
        if (!frame) break;
        user_map_page(pml4, stack_base + (uint64_t)p * 4096,
                      (uint64_t)(uintptr_t)frame,
                      USER_PTE_PRESENT | USER_PTE_USER
                      | USER_PTE_WRITE | USER_PTE_NOEXEC);
    }
    uint64_t user_rsp = stack_base + PROC_USER_STACK_PAGES * 4096 - 16;

    // 6. Populate the process descriptor
    struct ProcessDescriptor* spawner = process_find_current();
    pd->pid          = next_pid++;
    pr_strcpy(pd->name, object_name, PROC_NAME_LEN);
    pd->object_id    = obj->object_id;
    pd->cr3          = new_cr3;
    pd->user_rip     = entry_rip;
    pd->user_rsp     = user_rsp;
    pd->owner_uid    = owner_uid;
    pd->parent_pid   = spawner ? spawner->pid : 0;   // Phase 1 two-party
    pd->partition_id = spawn_partition_id;   // Phase 9, resolved once at Phase 13
    pd->state        = PROC_RUNNING;
    pd->priority     = PROC_PRIO_NORMAL;     // Phase 4 (Navigator-Parity): default tier
    pd->active       = 1;
    proc_clear_resume_state(pd);   // a reused slot must not inherit the
                                   // previous occupant's resume/teardown state
                                   // (has_ring3_ctx is re-set below for async)

    // Seed Kernel Phase 1 (two-party): a DEDICATED 8 KiB kernel syscall stack
    // (TWO contiguous frames — see alloc_proc_syscall_stack) — see
    // process_create() for why the shared fixed stack is unsafe for nesting.
    uint64_t sstop = alloc_proc_syscall_stack(spawn_partition_id);
    if (!sstop) {
        kernel_serial_print("[PROC] program_spawn: syscall stack allocation failed.\n");
        return 0;
    }
    pd->syscall_stack_top = sstop;

    proc_count++;

    kernel_serial_printf(
        "[PROC] program_spawn: PID %u '%s'  "
        "RIP=0x%016lx  RSP=0x%016lx  CR3=0x%016lx  SYSSTK=0x%016lx\n",
        pd->pid, pd->name, pd->user_rip, pd->user_rsp, pd->cr3,
        pd->syscall_stack_top);

    // 7. Enter Ring-3. Every process gets its OWN syscall stack wired into
    //    per_cpu_data[0].kernel_rsp BEFORE the first sysret, so [gs:8] is
    //    correct on its first syscall regardless of who spawned it
    //    (Phase 1.5: with several processes live, the fixed boot stack is
    //    never safe). nested_ring3_prep() also sets it, via GS, for the
    //    nested case — the direct write here covers kernel-context spawns.
    per_cpu_data[0].kernel_rsp = pd->syscall_stack_top;

    if (!async) {
        if (spawner) nested_ring3_prep(spawner, pd);
        kernel_enter_ring3(&pd->kernel_rsp, &pd->kernel_cr3,
                           pd->cr3, pd->user_rip, pd->user_rsp);

        // Resumes here after SYS_SLS_EXIT
        kernel_serial_printf("[PROC] PID %u '%s' returned to kernel.\n",
                             pd->pid, pd->name);
        return pd->pid;
    }

    // Async: craft a synthetic ring-3 context and hand the child to the
    // scheduler. The frame layout is exactly what isr32_stub pushes on a
    // timer preemption (15 GPRs, r15 first, then the iretq frame) — see
    // schedule_ring3()'s own comment. GPRs are zeroed (fresh entry, same
    // convention kernel_enter_ring3 uses); rip/cs/rflags/rsp/ss are the
    // user entry state. sysret-entered processes use CS=0x23, SS=0x1B.
    pd->state        = PROC_SUSPENDED;
    pd->has_ring3_ctx = 1;

    if (spawner) {
        /* Restore [gs:8] to the SPAWNER's own syscall stack. The write
         * above pointed it at the CHILD's stack; for a Ring-3 spawner this
         * is wrong — its NEXT syscall would run on the child's stack, and
         * cap_wait_chan()'s park capture (which reads the CURRENT process's
         * syscall_stack_top, the descriptor's field) would read the wrong
         * frame. Caught live: after the overlap parent's held spawn, its
         * chan_create/release/recv all ran on the child's stack, the recv
         * park captured garbage park_ctx (the stale spawn frame), and the
         * resume re-ran main() — while the child, resuming from its own
         * send, sysret'd to the PARENT's continuation and faulted. The
         * child gets [gs:8] repointed by every kernel_switch_next()/
         * schedule_ring3() that schedules it, so nothing is lost here. */
        per_cpu_data[0].kernel_rsp = spawner->syscall_stack_top;
    }
    {
        uint64_t* ctx = (uint64_t*)&pd->ring3_ctx;
        for (int i = 0; i < 15; i++) ctx[i] = 0;
        ctx[15] = entry_rip;
        ctx[16] = 0x23;      /* ring-3 code */
        ctx[17] = 0x202;     /* IF on */
        ctx[18] = user_rsp;
        ctx[19] = 0x1B;      /* ring-3 data */
    }
    kernel_serial_printf(
        "[PROC] PID %u '%s' queued (non-blocking spawn; runs on next schedule)\n",
        pd->pid, pd->name);
    return pd->pid;
}

// ─── program_spawn / program_spawn_nb ────────────────────────────────────────
// Synchronous (blocks until exit) and non-blocking (Phase 1.5) spawns.
uint32_t program_spawn(const char* object_name, uint32_t owner_uid) {
    return program_spawn_common(object_name, owner_uid, 0);
}

uint32_t program_spawn_nb(const char* object_name, uint32_t owner_uid) {
    return program_spawn_common(object_name, owner_uid, 1);
}

// Phase 1.5 (immediate wake, gated spawn): like program_spawn_nb, but the
// child is created in PROC_HELD — NOT schedulable — so the spawner can
// provision its resources (e.g. mint a channel's far-end caps into the
// child's table) before the child can run. The child starts only after the
// spawner calls process_release(pid) (SYS_SLS_PROC_RELEASE). This closes the
// spawn-then-provision race: without the gate, a timer tick between
// spawn_nb's return and the spawner's chan_create schedules the child
// first, its allocations shift the far-end cap slots, and the hardcoded
// send index hits an empty slot (caught live as a child EBADF cascade).
uint32_t program_spawn_nb_held(const char* object_name, uint32_t owner_uid) {
    uint32_t pid = program_spawn_common(object_name, owner_uid, 1);
    if (pid == 0) return 0;
    for (int i = 0; i < PROC_MAX; i++) {
        if (proc_table[i].active && proc_table[i].pid == pid) {
            proc_table[i].state = PROC_HELD;
            kernel_serial_printf(
                "[PROC] PID %u '%s' held — gate open on release\n",
                pid, proc_table[i].name);
            break;
        }
    }
    return pid;
}

// ─── proc_free_syscall_stack (Phase 2 teardown) ──────────────────────────────
// Returns the process's two contiguous syscall-stack frames to the frame
// pool. Called AFTER the process has stopped using the stack: process_exit()
// invokes it inside the exit syscall, with execution about to switch to the
// kernel's / the parent's / the next process's stack — the freed frames are
// physically intact until that switch and nothing can reallocate them in
// between (single CPU, IF=0 inside the syscall), so this is safe even
// though the exit path itself is still running on them. The lower frame is
// recovered from syscall_stack_top (top = lower + 8192 - 8, the
// alloc_proc_syscall_stack convention). Freeing uses frame_pool_frame_owner()
// so the partition that was charged at allocation is the one decremented. */
static void proc_free_syscall_stack(struct ProcessDescriptor* pd) {
    uint64_t top = pd->syscall_stack_top;
    if (top == 0) return;
    uint64_t lower = top - 8192 + 8;
    for (int f = 0; f < 2; f++) {
        uint64_t addr = lower + (uint64_t)f * 4096;
        /* No machine-owned guard here, deliberately: these frames came
         * from alloc_proc_syscall_stack() (the pool, at addresses between
         * the image and the arena) — the coarse machine-owned watermark
         * covers that whole range and would wrongly skip them (caught
         * live as leaked syscall stacks). free_physical_ram_frame_for_
         * partition() validates alignment/range/bit itself. */
        free_physical_ram_frame_for_partition((void*)(uintptr_t)addr,
                                              frame_pool_frame_owner(addr / 4096));
    }
    pd->syscall_stack_top = 0;
}

// ─── process_exit ────────────────────────────────────────────────────────────
// Called from SYS_SLS_EXIT dispatch.  Finds the running Ring-3 process,
// marks it ZOMBIE, restores the kernel's RSP and CR3 saved by
// kernel_enter_ring3(), then rets back into program_spawn() / process_create().
// Phase 1.5: forward declarations for the async-exit switch helpers defined
// with the other cap hooks below (static, so they need an in-file prototype).
static struct ProcessDescriptor* pick_next_runnable(void);
__attribute__((noreturn)) static void kernel_switch_next(struct ProcessDescriptor* next);
void process_exit(uint32_t exit_code) {
    for (int i = 0; i < PROC_MAX; i++) {
        struct ProcessDescriptor* pd = &proc_table[i];
        if (!pd->active || pd->state != PROC_RUNNING) continue;
        if (!pd->kernel_rsp && !pd->has_ring3_ctx) continue;

        uint64_t saved_rsp = pd->kernel_rsp;
        uint64_t saved_cr3 = pd->kernel_cr3;

        pd->kernel_rsp = 0;
        pd->kernel_cr3 = 0;
        pd->state      = PROC_ZOMBIE;

        kernel_serial_printf(
            "[PROC] PID %u '%s' exited (code=%u).\n",
            pd->pid, pd->name, exit_code);

        /* Phase 2: resume/teardown flags on a process that exits normally
         * are moot — the full teardown below reclaims everything anyway.
         * Cleared (including any stale resume_sysret/resume_kernel/park_ctx
         * from a handoff that never resumed) so a reused slot starts clean. */
        proc_clear_resume_state(pd);

        /* ── Phase 2 teardown: reclaim everything this process owned. ──
         * Runs here, at the top of the exit path, for all three resume
         * shapes (kernel-context spawn, nested sync, async): from this
         * point on execution lives entirely in the kernel half of the
         * page tables (shared by pointer, never freed by the walker), and
         * the freed frames stay physically intact until the CR3 switch
         * at the bottom of this function abandons the address space — no
         * allocation can re-hand them in between (single CPU, IF=0 inside
         * the syscall). Order is load-bearing:
         *   1. cap_table_teardown() — drop caps (freeing arena frames /
         *      deactivating channels at object refcount 0), drain queues
         *      nobody can read, unmap cap-derived PTEs, unbind the table.
         *   2. user_destroy_page_table() — free the user-half page tables
         *      and the process's own leaf frames (binary/stack/SIMI
         *      scratch); the cap PTEs are already cleared, and arena /
         *      SIMI-cache frames are refused by the walker regardless.
         *   3. proc_free_syscall_stack() — the two contiguous syscall-
         *      stack frames.
         * pd->active stays 1 until after step 1 because
         * cap_proc_cr3(pid) (used by the teardown's unmap walk) requires
         * an active descriptor. */
        cap_table_teardown(pd->pid);
        user_destroy_page_table(pd->cr3);
        proc_free_syscall_stack(pd);

        pd->active = 0;
        proc_count--;

        /* Seed Kernel Phase 1.5: three resume shapes now exist.
         *
         *  A) parent_pid == 0 — kernel-context spawn (shell/HTTP): the
         *     spawner is blocked in kernel_enter_ring3; ret into it.
         *  B) parent_pid != 0 && saved_rsp != 0 — synchronous nested spawn
         *     (the Phase-1 two-party flow): the PARENT is blocked inside its
         *     own SYS_SLS_PROGRAM_SPAWN; restore its kernel continuation.
         *  C) parent_pid != 0 && saved_rsp == 0 — ASYNC child (Phase 1.5):
         *     there is NO kernel_enter_ring3 continuation to return to. Hand
         *     the CPU to the next runnable process (usually the parent,
         *     woken from its blocking recv by this child's send) via the
         *     kernel switch path; if nobody is runnable, halt (a parked
         *     process was never woken — a liveness bug, not a crash). */

        // Restore kernel page table and stack, then return to kernel_enter_ring3
        // caller.  The 'ret' pops the return address from [saved_rsp].
        //
        // GS-state bookkeeping — two cases (see nested_ring3_prep()):
        //
        //  NON-nested (kernel-context spawn, parent_pid == 0): the child's
        //  exit syscall entry swapgs'd, so GS_BASE == &per_cpu_data[0] here.
        //  The spawner (HTTP/shell) runs with GS_BASE == 0, so we swapgs once
        //  to restore the pre-syscall GS state — the same swapgs
        //  .syscall_return would have done. (Historical bug fixed before
        //  Phase 4 SIMI verification: without it, the NEXT process's syscall
        //  entry swapgs'd the wrong direction and read [gs:8] from physical
        //  address 0x8 — a garbage RSP and a triple fault.)
        //
        //  NESTED (parent is a Ring-3 process, parent_pid != 0): the parent
        //  is blocked INSIDE its own syscall and expects GS_BASE ==
        //  &per_cpu_data[0] when its continuation resumes — so we must NOT
        //  swapgs. We also restore the parent's saved user RSP (the child's
        //  syscall entries overwrote gs:0 with the child's own RSP) and mark
        //  the parent RUNNING again so it can syscall and be scheduled.
        int nested = (pd->parent_pid != 0 && saved_rsp != 0);
        if (pd->parent_pid != 0 && saved_rsp == 0) {
            /* Case C: async child. Switch to the next runnable process.
             * GS_BASE is &per_cpu_data[0] (we are inside the exit syscall);
             * kernel_switch_next() swapgs's before the iretq so the resume
             * path (ring-3 frame, or cap_recv_resume's own swapgs) sees the
             * same GS state as a timer-driven switch. */
            struct ProcessDescriptor* next = pick_next_runnable();
            if (!next) {
                kernel_serial_print(
                    "[PROC] async exit: no runnable process left; halting.\n");
                for (;;) __asm__ volatile("hlt");
            }
            kernel_serial_printf(
                "[PROC] async exit: handing CPU to PID %u '%s'\n",
                next->pid, next->name);
            kernel_switch_next(next);   /* noreturn */
            __builtin_unreachable();
        } else if (nested) {
            for (int j = 0; j < PROC_MAX; j++) {
                if (proc_table[j].active &&
                    proc_table[j].pid == pd->parent_pid) {
                    proc_table[j].state = PROC_RUNNING;
                    /* The child's spawn clobbered [gs:8] with ITS stack top
                     * (program_spawn_common); point it back at the parent's
                     * own syscall stack so the parent's NEXT syscall (after
                     * its spawn returns) lands on ITS stack, not the dead
                     * child's. Latent in Phase 1 (two-party) — the dead
                     * child's frame was never reused, so the stale [gs:8]
                     * happened to stay valid; with live async children the
                     * stale stack can be the one another process is about to
                     * use. */
                    per_cpu_data[0].kernel_rsp =
                        proc_table[j].syscall_stack_top;
                    break;
                }
            }
            /* GS_BASE is &per_cpu_data[0]: write the parent's user RSP so
             * the parent's .syscall_return does `mov rsp,[gs:0]` correctly. */
            __asm__ volatile("movq %0, %%gs:0" : : "r"(pd->parent_user_rsp)
                             : "memory");
            __asm__ volatile(
                "mov %0, %%cr3\n\t"   /* restore kernel (parent's) page tables */
                "mov %1, %%rsp\n\t"   /* restore parent's syscall stack       */
                "pop %%r15\n\t"         /* restore kernel_enter_ring3's saved    */
                "pop %%r14\n\t"         /* callee-saved regs (see its prologue)  */
                "pop %%r13\n\t"
                "pop %%r12\n\t"
                "pop %%rbp\n\t"
                "pop %%rbx\n\t"
                "ret\n\t"              /* return to kernel_enter_ring3 call site */
                :
                : "r"(saved_cr3), "r"(saved_rsp)
                : "memory"
            );
        } else {
            __asm__ volatile(
                "swapgs\n\t"           /* undo syscall_entry_stub's entry swapgs */
                "mov %0, %%cr3\n\t"   /* restore kernel page tables */
                "mov %1, %%rsp\n\t"   /* restore kernel stack         */
                "pop %%r15\n\t"         /* restore kernel_enter_ring3's saved    */
                "pop %%r14\n\t"         /* callee-saved regs (see its prologue)  */
                "pop %%r13\n\t"
                "pop %%r12\n\t"
                "pop %%rbp\n\t"
                "pop %%rbx\n\t"
                "ret\n\t"              /* return to kernel_enter_ring3 call site */
                :
                : "r"(saved_cr3), "r"(saved_rsp)
                : "memory"
            );
        }
        __builtin_unreachable();
    }
    kernel_serial_print("[PROC] exit: no active Ring-3 process found.\n");
}

// ─── Phase 12 (LPAR): partition-fair scheduling helpers ─────────────────────
// Both are pure functions of proc_table[]'s current state — no interrupt-
// frame or CR3 manipulation — deliberately factored out of schedule_ring3()
// so the fairness algorithm itself can be exercised by a host-side unit
// test. schedule_ring3() as a whole can't be: its tail unconditionally
// executes a privileged `mov cr3` instruction that would fault outside
// ring 0, so nothing that reaches that line is safely host-callable. See
// AeroSLS-LPAR-Roadmap-v0.1.md §6.
//
// The problem this closes: the pre-Phase-12 flat round robin visited every
// active proc_table[] slot once per cycle regardless of partition, which
// gives each PROCESS an equal share but not each PARTITION one — a
// partition with 10 processes collectively gets 10x the CPU turns of a
// sibling partition with 1, even though "one partition shouldn't be able
// to buy more CPU share just by spawning more processes" is exactly the
// property partition isolation is supposed to provide. The fix: round-
// robin across PARTITIONS first (skipping ones with nothing runnable),
// then round-robin within the chosen partition's processes — so a
// partition with 1 runnable process gets the same fraction of scheduling
// turns as a sibling with 10, and each of those 10 individually gets a
// smaller share of their partition's turn instead of the same share as
// the lone process next door.
//
// g_last_partition_scheduled persists across calls (module-level, same
// lifetime as proc_table[] itself) so consecutive picks rotate forward
// through partitions instead of re-deriving from scratch each tick.
static uint32_t g_last_partition_scheduled = PARTITION_SYSTEM;

// Seed Kernel Phase 1.5: is this process schedulable? A SUSPENDED process
// with a valid resume target: kernel_rsp (entered via kernel_enter_ring3),
// has_ring3_ctx (async spawn's synthetic frame, or a real context after a
// timer save), resume_kernel (woken from a blocking cap_recv park — its
// next schedule must go through the kernel iretq path, not ring3_ctx), or
// resume_sysret (yielded mid-syscall — its original entry frame is still on
// its syscall stack; resume at .syscall_return). PROC_BLOCKED / PROC_HELD /
// PROC_ZOMBIE are excluded by the state check.
static int proc_runnable(const struct ProcessDescriptor* pd) {
    return pd->active &&
           pd->state == PROC_SUSPENDED &&
           (pd->kernel_rsp != 0 || pd->has_ring3_ctx || pd->resume_kernel ||
            pd->resume_sysret);
}

// Finds the next partition (after `last_partition`, wrapping, inclusive of
// revisiting `last_partition` itself once nothing else remains) that has
// at least one active, PROC_SUSPENDED, kernel_rsp!=0 process — i.e. a
// process schedule_ring3() could actually switch to. Returns 1 and sets
// *out_partition on success, 0 if no partition has anything runnable
// (mirrors the pre-Phase-12 "!next" fallback one level up).
static int pick_next_partition(uint32_t last_partition, uint32_t* out_partition) {
    // Multitenant Isolation Gap Analysis §5 item 8 / §7 item 8: weighted CPU
    // scheduling. Before rotating to the NEXT candidate in the ring, first
    // check whether last_partition itself is still owed consecutive turns
    // under its configured weight (g_partition_turns_remaining > 0) -- if
    // so, and it's not paused, and it still has a runnable process, give it
    // another turn right now instead of moving on. At the default weight
    // (1, effective_cpu_weight()'s neutral case) g_partition_turns_remaining
    // is always 0 here, so this branch never fires and the function falls
    // straight through to the original, unweighted rotation below --
    // reproducing the pre-existing behavior exactly, not approximately.
    if (g_partition_turns_remaining > 0 && !partition_is_paused(last_partition)) {
        for (int i = 0; i < PROC_MAX; i++) {
            if (proc_runnable(&proc_table[i]) &&
                proc_table[i].partition_id == last_partition) {
                *out_partition = last_partition;
                g_partition_turns_remaining--;
                return 1;
            }
        }
        // last_partition has nothing left to run -- it forfeits any
        // remaining owed turns (they don't carry over to whatever gets
        // picked next) and the normal rotation below takes over.
        g_partition_turns_remaining = 0;
    }

    for (uint32_t p = 1; p <= PARTITION_MAX; p++) {
        uint32_t candidate = (last_partition + p) % PARTITION_MAX;
        // Phase 14 (LPAR): a paused partition is skipped entirely, without
        // even scanning its processes -- pause is a scheduling exclusion,
        // not a "nothing runnable" state, so this check comes before the
        // inner scan rather than folding into its condition.
        if (partition_is_paused(candidate)) continue;
        for (int i = 0; i < PROC_MAX; i++) {
            if (proc_runnable(&proc_table[i]) &&
                proc_table[i].partition_id == candidate) {
                *out_partition = candidate;
                // This turn is turn #1 of candidate's weight-many turns --
                // it owes (weight - 1) MORE before the next rotation.
                g_partition_turns_remaining = effective_cpu_weight(candidate) - 1;
                return 1;
            }
        }
    }
    return 0;
}

// Per-partition round-robin cursor (declared up top with proc_table[],
// initialized to -1 for every entry in process_init()): the proc_table[]
// index last scheduled FROM each partition, tracked independently per
// partition_id (-1 = never scheduled from yet).
//
// This replaces an earlier version of pick_next_process_in_partition()
// that searched from the shared `current_idx` instead — real execution
// (a host-side unit test threading consecutive schedule_ring3()-style
// ticks together, see scheduler_fairness_host_test.c) caught a genuine
// starvation bug in that version: with proc_table laid out as [SYSTEM,
// SYSTEM, SYSTEM, SYSTEM, SYSTEM, other-partition], alternating between
// PARTITION_SYSTEM and the other partition made current_idx bounce
// between exactly two values (0 and 5) forever, and PARTITION_SYSTEM's
// wraparound scan from current_idx=5 always landed on slot 0 first no
// matter how many ticks ran — slots 1-4 were never reached, in an
// infinite loop. A per-partition cursor doesn't have this failure mode:
// each partition remembers its OWN last-scheduled index independent of
// which OTHER partition ran in between, so PARTITION_SYSTEM's rotation
// (0, 1, 2, 3, 4, 0, ...) keeps advancing regardless of how many times
// some other partition's turn interleaves with it. Same category of bug
// as Phase 5's reg_disp() off-by-one and Phase 6's RV64 jalr bug — caught
// by execution, not visible from reading the code.

// Within `partition_id` only, finds the next runnable process after that
// partition's own cursor (wrapping), never returning `exclude_idx` (the
// process schedule_ring3() just preempted — same "don't immediately
// reschedule the process we just suspended" property the pre-Phase-12
// flat scan had, preserved here as an explicit parameter instead of an
// implicit search-base). Returns the proc_table[] index and advances that
// partition's cursor to it, or -1 if partition_id has no other eligible
// process (the sole survivor in a single-process partition, after
// excluding itself — schedule_ring3()'s existing "!next -> resume
// current immediately" fallback handles that case, unchanged).
// Navigator-Parity Gap Roadmap Phase 4: priority is layered on top of the
// existing per-partition round-robin cursor rather than replacing it. Three
// passes over the SAME cursor -- HIGH first, then NORMAL, then LOW -- so any
// runnable HIGH-priority process in the partition is always offered a turn
// before a NORMAL one gets considered, and any NORMAL before any LOW (strict
// priority scheduling), while still round-robining fairly *within* whichever
// tier actually has runnable work (the cursor only ever advances to the slot
// that was chosen, so the next call resumes from there regardless of which
// tier it came from). When every process in a partition is the default
// PROC_PRIO_NORMAL (true for any deployment that never touches priority),
// the HIGH pass always finds nothing and falls straight through to the
// NORMAL pass, which is byte-for-byte the pre-Phase-4 scan -- confirmed by
// the new host test asserting identical ordering when no priority is set.
static int pick_next_process_in_partition(uint32_t partition_id, int exclude_idx) {
    for (int tier = PROC_PRIO_HIGH; tier <= PROC_PRIO_LOW; tier++) {
        int start = g_last_index_in_partition[partition_id];
        for (int step = 1; step <= PROC_MAX; step++) {
            int idx = (start + step) % PROC_MAX;
            if (idx == exclude_idx) continue;
            if (proc_runnable(&proc_table[idx]) &&
                proc_table[idx].partition_id == partition_id &&
                proc_table[idx].priority == (ProcPriority)tier) {
                g_last_index_in_partition[partition_id] = idx;
                return idx;
            }
        }
    }
    return -1;
}

// ─── schedule_ring3 ──────────────────────────────────────────────────────────
// Called from isr32_stub (Ring-3 timer preemption path).
// ctx_rsp points to a 20-qword area on the interrupt stack:
//   [0..14] = GPRs in struct TaskContext order (r15 first, rax last)
//   [15..19] = CPU iretq frame (rip, cs, rflags, user_rsp, user_ss)
//
// Partition-fair round-robin (Phase 12) between RUNNING/SUSPENDED Ring-3
// processes — see pick_next_partition()/pick_next_process_in_partition()
// above for the algorithm. Writes the chosen process's context to the
// interrupt stack and returns ctx_rsp (same pointer; caller pops GPRs +
// iretq from the updated stack).
uint64_t schedule_ring3(uint64_t ctx_rsp) {
    uint64_t* frame = (uint64_t*)ctx_rsp;

    // Find current running Ring-3 process (Phase 1.5: async children never
    // call kernel_enter_ring3, so kernel_rsp may be 0 while has_ring3_ctx=1)
    struct ProcessDescriptor* current = NULL;
    int current_idx = -1;
    for (int i = 0; i < PROC_MAX; i++) {
        if (proc_table[i].active &&
            proc_table[i].state == PROC_RUNNING &&
            (proc_table[i].kernel_rsp != 0 || proc_table[i].has_ring3_ctx)) {
            current     = &proc_table[i];
            current_idx = i;
            break;
        }
    }
    if (!current) return ctx_rsp;  /* nothing to schedule */

    // Save current Ring-3 context from interrupt stack
    uint64_t* dst = (uint64_t*)&current->ring3_ctx;
    for (int i = 0; i < 20; i++) dst[i] = frame[i];
    current->state = PROC_SUSPENDED;
    current->has_ring3_ctx = 1;   /* ring3_ctx now holds a real frame */

    /* Phase 2: a deferred kill (self-kill / cross-CPU) targeted this
     * process while it was running. Its context is saved above, so it is
     * no longer executing; run the FULL teardown right here. This is safe
     * for the same reason process_exit()'s teardown is: from here on the
     * kernel's execution lives in the shared kernel half of the page
     * tables (never freed), and the freed frames' contents stay physically
     * intact until the CR3 switch below — no allocation can re-hand them
     * in between (single CPU, IF=0 inside the timer ISR). Making the
     * process inactive now also excludes it from the pick below. */
    if (current->pending_teardown) {
        proc_clear_resume_state(current);
        kernel_serial_printf(
            "[PROC] PID %u '%s' deferred teardown (killed while running)\n",
            current->pid, current->name);
        cap_table_teardown(current->pid);
        user_destroy_page_table(current->cr3);
        proc_free_syscall_stack(current);
        current->active = 0;
        proc_count--;
    }

    // Phase 12: pick a partition first (round-robin, skipping ones with
    // nothing runnable), then pick a process within it.
    struct ProcessDescriptor* next = NULL;
    uint32_t chosen_partition;
    if (pick_next_partition(g_last_partition_scheduled, &chosen_partition)) {
        int next_idx = pick_next_process_in_partition(chosen_partition, current_idx);
        if (next_idx >= 0) {
            next = &proc_table[next_idx];
            g_last_partition_scheduled = chosen_partition;
        }
    }

    if (!next) {
        if (!current->active) {
            /* The only runnable process was just torn down by a deferred
             * kill — nothing is left to resume. Mirrors process_exit()'s
             * async-exit halt path (same shape: no runnable process, the
             * BSP parks rather than iretq'ing into a dead address space). */
            kernel_serial_print(
                "[PROC] deferred kill left no runnable process; halting.\n");
            for (;;) __asm__ volatile("hlt");
        }
        // Only one Ring-3 process — resume it immediately
        current->state = PROC_RUNNING;
        return ctx_rsp;   /* frame already has correct context */
    }

    if (next->resume_sysret) {
        // Yielded mid-syscall (immediate-wake handoff, or an explicit
        // yield): resume via the KERNEL iretq path into cap_sysret_resume,
        // which jumps straight to .syscall_return — the process's ORIGINAL
        // entry frame is still on its syscall stack, so it sysrets to
        // ring-3 right after its (already completed) syscall.
        next->resume_sysret = 0;
        for (int i = 0; i < 15; i++) frame[i] = 0;
        frame[9]  = (uint64_t)next;              /* rdi — cap_sysret_resume arg */
        frame[15] = (uint64_t)cap_sysret_resume; /* rip — kernel code */
        frame[16] = 0x08;                        /* cs — kernel code */
        frame[17] = 0x202;                       /* rflags — IF on */
        frame[18] = next->syscall_stack_top;     /* rsp — fresh syscall stack */
        frame[19] = 0x10;                        /* ss — kernel data */
    } else if (next->resume_kernel) {
        // Woken from a blocking cap_recv park: resume via the KERNEL iretq
        // path (cap_recv_resume re-runs the recv, then .syscall_return
        // sysrets to ring-3). The interrupt stack becomes the resume frame;
        // rdi carries the descriptor (isr32_stub pops rdi at index 9). GS
        // here is 0 (we interrupted ring-3), which is exactly the state
        // cap_recv_resume expects before its own swapgs.
        next->resume_kernel = 0;
        for (int i = 0; i < 15; i++) frame[i] = 0;
        frame[9]  = (uint64_t)next;              /* rdi — cap_recv_resume arg */
        frame[15] = (uint64_t)cap_recv_resume;   /* rip — kernel code */
        frame[16] = 0x08;                        /* cs — kernel code */
        frame[17] = 0x202;                       /* rflags — IF on */
        frame[18] = next->syscall_stack_top;     /* rsp — fresh syscall stack */
        frame[19] = 0x10;                        /* ss — kernel data */
    } else {
        // Replace interrupt stack with next process's saved ring-3 context
        uint64_t* src = (uint64_t*)&next->ring3_ctx;
        for (int i = 0; i < 20; i++) frame[i] = src[i];
    }
    next->state = PROC_RUNNING;

    // Phase 1.5: the next process's syscalls must land on ITS OWN syscall
    // stack — [gs:8] is read by syscall_entry_stub on its next entry.
    per_cpu_data[0].kernel_rsp = next->syscall_stack_top;

    // Switch to the next process's page table
    __asm__ volatile("mov %0, %%cr3" : : "r"(next->cr3) : "memory");

    return ctx_rsp;
}

// ─── process_find_current ────────────────────────────────────────────────────
// Phase 7: identical scan to the one schedule_ring3() does internally to
// find "the process currently executing" — active, PROC_RUNNING, and
// actually entered via kernel_enter_ring3 (kernel_rsp != 0). Used by
// simi_runtime.c's authority-checked RESOLVE to find the calling
// process's owner_uid. Returns NULL when called from pure kernel context
// (no Ring-3 process running), which callers should treat as uid 0 /
// ROLE_SYSTEM_KERNEL — always-passes, per catalog_check_access().
struct ProcessDescriptor* process_find_current(void) {
    for (int i = 0; i < PROC_MAX; i++) {
        if (proc_table[i].active &&
            proc_table[i].state == PROC_RUNNING &&
            (proc_table[i].kernel_rsp != 0 || proc_table[i].has_ring3_ctx)) {
            return &proc_table[i];
        }
    }
    return NULL;
}

// ─── Capability-layer hooks (Seed Kernel Phase 1) ────────────────────────────
// Strong definitions that override cap.c's weak defaults in the real kernel
// (cap.c defines the weak versions so it can be host-tested in isolation;
// tests/cap_lifecycle_host_test.c overrides both with fake pids/pml4s).
uint32_t cap_current_pid(void) {
    struct ProcessDescriptor* p = process_find_current();
    return p ? p->pid : 0;   /* 0 = kernel context: uses the kernel cap table */
}

uint64_t cap_proc_cr3(uint32_t pid) {
    for (int i = 0; i < PROC_MAX; i++) {
        if (proc_table[i].active && proc_table[i].pid == pid)
            return proc_table[i].cr3;
    }
    return 0;
}

int cap_release_pid(uint32_t pid) { return process_release(pid); }

// ─── Seed Kernel Phase 1.5: blocking cap_recv park / wake ───────────────────
// The kernel side of a blocking recv. cap.c calls cap_wait_chan() when its
// recv found the queue empty with block=1; this parks the CURRENT process
// mid-syscall and switches to the next runnable one. cap_send() calls
// cap_wake_chan() after a successful enqueue, which makes the parked process
// runnable again via the KERNEL resume path (cap_recv_resume).

// Next runnable process, using the same partition-fair pickers the timer
// preemption path uses (round-robin across partitions, then within).
static struct ProcessDescriptor* pick_next_runnable(void) {
    uint32_t chosen_partition;
    if (!pick_next_partition(g_last_partition_scheduled, &chosen_partition))
        return NULL;
    int idx = pick_next_process_in_partition(chosen_partition, -1);
    if (idx < 0) return NULL;
    g_last_partition_scheduled = chosen_partition;
    return &proc_table[idx];
}

/* Switch the CPU to `next` from KERNEL context (a syscall). Never returns.
 *
 * Frame shapes:
 *  - resume_sysret=1: iretq to kernel code — cap_sysret_resume(pd) on the
 *    process's own fresh syscall stack, which jumps straight into
 *    .syscall_return: the ORIGINAL entry frame (from the process's send or
 *    yield syscall) is still on its stack, so it sysrets to ring-3 as if
 *    the syscall had taken a while. (Phase 1.5 immediate wake: the sender
 *    of a message that woke a parked receiver hands the CPU to it here.)
 *  - resume_kernel=1: iretq to kernel code — cap_recv_resume(pd) on the
 *    process's own fresh syscall stack (CS=0x08/SS=0x10), which re-runs the
 *    parked recv and then jumps to .syscall_return to sysret to ring-3.
 *  - otherwise:        iretq to the process's saved ring-3 context.
 *
 * GS: the caller is inside a syscall (GS_BASE == &per_cpu_data[0]), so we
 * swapgs ONCE before the iretq. For a ring-3 frame that lands the user view
 * (GS_BASE=0, the sysret convention); for the kernel-resume frame
 * cap_recv_resume()/cap_sysret_resume() swapgs's back before touching
 * [gs:0]/[gs:8].
 */
__attribute__((noreturn))
static void kernel_switch_next(struct ProcessDescriptor* next) {
    per_cpu_data[0].kernel_rsp = next->syscall_stack_top;
    next->state = PROC_RUNNING;   /* schedule_ring3 does this too — the park
                                     switch must, or the child's syscalls
                                     can't resolve it as the current process */
    uint64_t cr3 = next->cr3;
    uint64_t f[20];
    for (int i = 0; i < 15; i++) f[i] = 0;
    if (next->resume_sysret) {
        next->resume_sysret = 0;
        f[9]  = (uint64_t)next;              /* rdi — cap_sysret_resume arg */
        f[15] = (uint64_t)cap_sysret_resume; /* rip — kernel code */
        f[16] = 0x08;                        /* cs — kernel code */
        f[17] = 0x202;                       /* rflags — IF on */
        f[18] = next->syscall_stack_top;     /* rsp — fresh syscall stack */
        f[19] = 0x10;                        /* ss — kernel data */
    } else if (next->resume_kernel) {
        next->resume_kernel = 0;
        f[9]  = (uint64_t)next;              /* rdi — cap_recv_resume arg */
        f[15] = (uint64_t)cap_recv_resume;   /* rip — kernel code */
        f[16] = 0x08;                        /* cs — kernel code */
        f[17] = 0x202;                       /* rflags — IF on */
        f[18] = next->syscall_stack_top;     /* rsp — fresh syscall stack */
        f[19] = 0x10;                        /* ss — kernel data */
    } else {
        uint64_t* src = (uint64_t*)&next->ring3_ctx;
        for (int i = 0; i < 20; i++) f[i] = src[i];
    }
    __asm__ volatile("mov %0, %%cr3" : : "r"(cr3) : "memory");
    /* f lives on the abandoned (parked/exiting) kernel stack — its physical
     * frame stays identity-mapped in both page tables, so the pointer
     * remains valid after the CR3 switch. Single asm block: rsp is moved to
     * f and immediately consumed, nothing between that could clobber it. */
    __asm__ volatile(
        "mov %0, %%rsp\n\t"
        "pop %%r15\n\tpop %%r14\n\tpop %%r13\n\tpop %%r12\n\tpop %%r11\n\t"
        "pop %%r10\n\tpop %%r9\n\tpop %%r8\n\tpop %%rbp\n\tpop %%rdi\n\t"
        "pop %%rsi\n\tpop %%rdx\n\tpop %%rcx\n\tpop %%rbx\n\tpop %%rax\n\t"
        "swapgs\n\t"
        "iretq\n\t"
        :
        : "r"(f)
        : "memory");
    __builtin_unreachable();
}

/* Strong override of cap.c's weak hook. Returns 0 if the process could NOT
 * park (kernel context, or no other runnable process — the caller returns
 * CAP_EAGAIN and the SDK retries); never returns when it parks (the
 * iretq above hands the CPU to another process). */
int cap_wait_chan(uint32_t chan_id, void* recv_req) {
    struct ProcessDescriptor* cur = process_find_current();
    if (!cur) return 0;   /* kernel context: no process to park */

    /* Capture the user state the syscall entry stub pushed onto this
     * process's syscall stack, plus the user RSP from [gs:0]. cap_recv_resume()
     * rebuilds a fresh syscall frame from these. The entry frame sits at
     * [syscall_stack_top-8 .. -64] because entry did `mov rsp,[gs:8]` and
     * pushed eight qwords before calling do_syscall, in this order (see
     * arch/x86/syscall.asm): rbp [top-8], rbx, r12, r13, r14, r15, rcx,
     * r11 [top-64]. The mapping is load-bearing: .syscall_return pops r11,
     * rcx, r15..rbp and sysret needs rcx=user RIP, r11=user RFLAGS. (A
     * mirrored capture once shipped here and passed the overlap test only
     * by luck — the resumed code's rsp/rax/rdi were correct and it never
     * read the wrong callee-saved values; fixed once the mirror was proven.) */
    uint64_t* entry = (uint64_t*)cur->syscall_stack_top;
    cur->park_ctx.r11 = entry[-8];   /* user RFLAGS  */
    cur->park_ctx.rcx = entry[-7];   /* user RIP     */
    cur->park_ctx.r15 = entry[-6];
    cur->park_ctx.r14 = entry[-5];
    cur->park_ctx.r13 = entry[-4];
    cur->park_ctx.r12 = entry[-3];
    cur->park_ctx.rbx = entry[-2];
    cur->park_ctx.rbp = entry[-1];
    __asm__ volatile("movq %%gs:0, %0" : "=r"(cur->park_ctx.user_rsp)
                     : : "memory");
    cur->park_req      = (uint64_t)recv_req;
    cur->waiting_chan  = (uint16_t)chan_id;
    cur->state         = PROC_BLOCKED;
    kernel_serial_printf(
        "[CAP] recv block: parked PID %u on channel %u\n", cur->pid, chan_id);

    struct ProcessDescriptor* next = pick_next_runnable();
    if (!next) {
        /* Nobody to run — don't park; the caller returns CAP_EAGAIN and the
         * SDK retries. This also guarantees at least one runnable process
         * exists whenever a process parks or an async child exits, which is
         * what keeps the async-exit halt path unreachable in practice. */
        cur->state         = PROC_RUNNING;
        cur->waiting_chan  = CAP_NONE;
        kernel_serial_printf(
            "[CAP] recv block: no runnable process; returning EAGAIN\n");
        return 0;
    }
    kernel_serial_printf("[CAP] recv block: switching to PID %u '%s'\n",
                         next->pid, next->name);
    kernel_switch_next(next);   /* noreturn */
    return 1;   /* unreachable */
}

/* Strong override of cap.c's weak hook: mark the first process parked on
 * chan_id runnable again. The next schedule of it (timer tick, or the
 * current process parking/exiting) iretq's it into cap_recv_resume(). */
void cap_wake_chan(uint32_t chan_id) {
    for (int i = 0; i < PROC_MAX; i++) {
        struct ProcessDescriptor* pd = &proc_table[i];
        if (!pd->active || pd->state != PROC_BLOCKED) continue;
        if (pd->waiting_chan != (uint16_t)chan_id) continue;
        pd->state         = PROC_SUSPENDED;
        pd->resume_kernel = 1;
        pd->waiting_chan  = CAP_NONE;
        kernel_serial_printf("[CAP] woken PID %u (channel %u)\n",
                             pd->pid, chan_id);
        /* Phase 1.5 (immediate wake): remember WHO we woke so the sender's
         * cap_maybe_handoff() can hand the CPU to them right now instead of
         * letting them wait for the next tick. NULL in kernel context. */
        struct ProcessDescriptor* cur = process_find_current();
        if (cur) cur->handoff_target = pd;
        return;
    }
}

/* Strong override of cap.c's weak hook (called by cap_send right after
 * cap_wake_chan). If this send just woke a parked process, hand the CPU to
 * it IMMEDIATELY — the receiver runs before the sender's send syscall even
 * returns to ring-3 (seL4-style direct handoff). The sender's entry frame
 * stays untouched on its syscall stack; it is marked resume_sysret so its
 * next schedule sysrets it right after the send. Never called in kernel
 * context (the shell's cap_send wakes nobody parked, and if it did, there
 * is no process to hand off FROM — process_find_current returns NULL). */
void cap_maybe_handoff(void) {
    struct ProcessDescriptor* cur = process_find_current();
    if (!cur) return;   /* kernel context: nothing to hand off from */
    struct ProcessDescriptor* target = cur->handoff_target;
    cur->handoff_target = NULL;
    if (!target || target == cur) return;
    if (target->state != PROC_SUSPENDED || !target->resume_kernel) return;

    /* Capture the FULL syscall-entry frame into park_ctx (all eight regs +
     * user RSP) BEFORE the chain below can reuse the region: cap_sysret_resume()
     * REBUILDS the resume frame from these, because the original entry frame
     * at [top-64..top-8] is not guaranteed to survive this syscall's own
     * execution (GCC may reuse the region for kernel_switch_next's f[20]
     * array). Historically only user_rsp was captured here and the resume
     * popped the ORIGINAL frame — the original was found clobbered live
     * (the resumed process sysret'd to 0), and switching the resume to a
     * park_ctx rebuild REQUIRES this full capture. The user RSP is still in
     * [gs:0] (nobody has run since this syscall's entry). */
    uint64_t* entry = (uint64_t*)cur->syscall_stack_top;
    cur->park_ctx.r11 = entry[-8];
    cur->park_ctx.rcx = entry[-7];
    cur->park_ctx.r15 = entry[-6];
    cur->park_ctx.r14 = entry[-5];
    cur->park_ctx.r13 = entry[-4];
    cur->park_ctx.r12 = entry[-3];
    cur->park_ctx.rbx = entry[-2];
    cur->park_ctx.rbp = entry[-1];
    __asm__ volatile("movq %%gs:0, %0" : "=r"(cur->park_ctx.user_rsp)
                     : : "memory");
    cur->state         = PROC_SUSPENDED;
    cur->resume_sysret = 1;
    kernel_serial_printf(
        "[CAP] send handoff: PID %u yielded to woken PID %u\n",
        cur->pid, target->pid);
    kernel_switch_next(target);   /* noreturn — target resumes via cap_recv_resume */
    __builtin_unreachable();
}

/* Kernel-mode resume entry point for a process yielded mid-syscall (a send
 * that handed off to its woken receiver, or an explicit SYS_SLS_YIELD).
 * Runs on the process's own syscall stack with GS_BASE == 0. We swapgs to
 * the kernel view, repoint [gs:0] at the process's saved user RSP, REBUILD
 * the syscall-entry frame from park_ctx, set RAX = 0 (the send/yield
 * success value — .syscall_return never touches RAX), and jump into the
 * shared return path with rsp = top-64: it pops the rebuilt
 * r11/rcx/callee-saved regs and sysrets to ring-3 exactly as if the
 * syscall had simply taken a while.
 *
 * The frame is REBUILT from park_ctx — NOT popped from the original entry
 * frame — because the original is not guaranteed to survive: the resumed
 * process's own syscall chain (do_syscall -> cap_send -> ... ->
 * kernel_switch_next, all inlined with -O2) runs ON THIS STACK between the
 * entry pushes and the switch-away, and GCC freely reuses the dead
 * [top-64..top-8] region for its locals — including kernel_switch_next's
 * 160-byte `f[20]` iretq-frame array, which was caught live overwriting
 * the suspended process's r11/rcx slots (the resumed process sysret'd to
 * 0 and took a Ring-3 #PF). park_ctx is captured by cap_maybe_handoff() /
 * sys_sls_yield() BEFORE that chain runs, so it always holds the true
 * user registers. (Rebuilding does not re-execute the syscall — the send
 * already completed and its result, 0, is written into RAX explicitly.) */
__attribute__((noreturn))
void cap_sysret_resume(struct ProcessDescriptor* pd) {
    uint64_t* p   = (uint64_t*)&pd->park_ctx;
    uint64_t  top = pd->syscall_stack_top;

    __asm__ volatile("swapgs" : : : "memory");   /* GS_BASE: 0 -> &per_cpu_data */
    per_cpu_data[0].user_rsp = pd->park_ctx.user_rsp;

    /* Push park_ctx in EXACTLY the order syscall_entry_stub pushed the
     * original: rbp [top-8], rbx, r12, r13, r14, r15, rcx, r11 [top-64]
     * (.syscall_return pops r11 first … rbp last — see cap_recv_resume's
     * identical asm for the mirror-bug history). ONE asm block, with the
     * jump inside, so GCC cannot re-adjust rsp between the pushes and the
     * pop (inline-asm rsp changes are invisible to it — the delta-20
     * clobber). */
    __asm__ volatile(
        "mov %0, %%rsp\n\t"
        "pushq 56(%1)\n\t"   /* rbp */
        "pushq 48(%1)\n\t"   /* rbx */
        "pushq 40(%1)\n\t"   /* r12 */
        "pushq 32(%1)\n\t"   /* r13 */
        "pushq 24(%1)\n\t"   /* r14 */
        "pushq 16(%1)\n\t"   /* r15 */
        "pushq 8(%1)\n\t"    /* rcx — user RIP (sysret target) */
        "pushq 0(%1)\n\t"    /* r11 — user RFLAGS */
        "xor %%eax, %%eax\n\t"   /* syscall result: 0 = success */
        "jmp syscall_return_path\n\t"
        :
        : "r"(top), "r"(p)
        : "rax", "memory");
    __builtin_unreachable();
}

/* SYS_SLS_YIELD (300): voluntarily give up the CPU. The entry frame stays
 * on this process's syscall stack; we mark it resume_sysret and switch to
 * the next runnable process. On its next schedule it resumes at
 * .syscall_return — the yield appears to ring-3 as a syscall that simply
 * took a while. Returns 0 if there was nobody to yield to (the caller
 * continues immediately). Kernel context: returns 0 (nothing to do). */
uint32_t sys_sls_yield(void) {
    struct ProcessDescriptor* cur = process_find_current();
    if (!cur) return 0;
    /* Pick BEFORE marking ourselves SUSPENDED so we are naturally excluded
     * (proc_runnable requires SUSPENDED; a RUNNING process is not a
     * candidate). Order matters: picking after would let us pick ourselves
     * and spin forever. */
    struct ProcessDescriptor* next = pick_next_runnable();
    if (!next) return 0;   /* nobody else: continue immediately */
    /* Capture the FULL entry frame (all eight regs + user RSP) into
     * park_ctx, exactly like cap_wait_chan()/cap_maybe_handoff().
     * cap_sysret_resume() REBUILDS the resume frame from these — the
     * original entry frame at [top-64..top-8] is not guaranteed to survive
     * this syscall's own chain (the compiler may reuse the region for
     * kernel_switch_next's f[20] array), so it must not be relied on. */
    uint64_t* entry = (uint64_t*)cur->syscall_stack_top;
    cur->park_ctx.r11 = entry[-8];
    cur->park_ctx.rcx = entry[-7];
    cur->park_ctx.r15 = entry[-6];
    cur->park_ctx.r14 = entry[-5];
    cur->park_ctx.r13 = entry[-4];
    cur->park_ctx.r12 = entry[-3];
    cur->park_ctx.rbx = entry[-2];
    cur->park_ctx.rbp = entry[-1];
    __asm__ volatile("movq %%gs:0, %0" : "=r"(cur->park_ctx.user_rsp)
                     : : "memory");
    cur->state         = PROC_SUSPENDED;
    cur->resume_sysret = 1;
    kernel_serial_printf("[PROC] PID %u yielded to PID %u\n",
                         cur->pid, next->pid);
    kernel_switch_next(next);   /* noreturn */
    __builtin_unreachable();
}

/* Kernel-mode resume entry point for a woken blocked recv. Runs on the
 * process's own syscall stack with GS_BASE == 0 (see kernel_switch_next and
 * schedule_ring3 — the timer path never swapgs's). We swapgs to the kernel
 * view, rebuild the syscall-entry frame the stub would have pushed, re-run
 * the parked SYS_SLS_CAP_RECV, and jump into the stub's shared return path
 * (.syscall_return) so the process sysrets to ring-3 exactly as if the
 * syscall had simply taken longer. */
__attribute__((noreturn))
void cap_recv_resume(struct ProcessDescriptor* pd) {
    uint64_t* p   = (uint64_t*)&pd->park_ctx;
    uint64_t  req = pd->park_req;
    uint64_t  top = pd->syscall_stack_top;

    __asm__ volatile("swapgs" : : : "memory");   /* GS_BASE: 0 -> &per_cpu_data */

    /* .syscall_return does `mov rsp,[gs:0]` — the user RSP must be the
     * parked process's, not whoever last made a syscall. */
    per_cpu_data[0].user_rsp = p[8];

    /* Rebuild the entry frame in EXACTLY the order syscall_entry_stub
     * pushed it: rbp [top-8], rbx, r12, r13, r14, r15, rcx, r11 [top-64].
     * .syscall_return's pops run the opposite way — pop r11 first (reads
     * [top-64]) … pop rbp last ([top-8]) — so each value must land in the
     * slot its pop reads. (The mirror of this — pushing r11 first — put
     * park_ctx.rbx in the rcx slot and sysret jumped to the user's stack;
     * caught live as a #PF at the request-struct address.)
     *
     * ONE asm block, deliberately: the do_syscall call and the jump into
     * .syscall_return happen inside it, with rsp parked at top-64 when the
     * jump runs. If the call were separate, GCC would re-adjust the stack
     * after the pushes (inline asm rsp changes are invisible to it) and
     * the call's own return address + frame would overwrite the pushed
     * frame — verified live: the resumed process sysret'd to 0x3eb023, a
     * stale return-address value, after such a clobber. Inside one block,
     * the C chain (do_syscall and below) grows strictly BELOW top-72 and
     * never touches [top-8..top-64]. */
    __asm__ volatile(
        "mov %0, %%rsp\n\t"
        "pushq 56(%1)\n\t"   /* rbp */
        "pushq 48(%1)\n\t"   /* rbx */
        "pushq 40(%1)\n\t"   /* r12 */
        "pushq 32(%1)\n\t"   /* r13 */
        "pushq 24(%1)\n\t"   /* r14 */
        "pushq 16(%1)\n\t"   /* r15 */
        "pushq 8(%1)\n\t"    /* rcx — user RIP (sysret target) */
        "pushq 0(%1)\n\t"    /* r11 — user RFLAGS */
        "mov %2, %%rsi\n\t"  /* arg = parked SLSCapRecvRequest */
        "mov %3, %%edi\n\t"  /* num = SYS_SLS_CAP_RECV */
        "call do_syscall\n\t"
        "jmp syscall_return_path\n\t"
        :
        : "r"(top), "r"(p), "r"(req), "i"(SYS_SLS_CAP_RECV)
        : "rax", "rcx", "rdx", "rsi", "rdi", "r8", "r9", "r10", "r11",
          "memory");
    __builtin_unreachable();
}

// ─── sys_sls_getppid ──────────────────────────────────────────────────────────
// Seed Kernel Phase 1 (two-party verification): SYS_SLS_GETPPID (298).
// Returns the pid of the process that spawned the caller — the primitive the
// child uses to mint a channel's far-end caps directly into the parent's
// capability table (cap_chan_create(far_pid=parent)). The parent pid is
// recorded at spawn time by process_create()/program_spawn() via
// process_find_current(). Kernel context (the shell) returns 0.
uint32_t sys_sls_getppid(void) {
    struct ProcessDescriptor* cur = process_find_current();
    return cur ? cur->parent_pid : 0;
}

// ─── process_kill ─────────────────────────────────────────────────────────────
void process_kill(uint32_t pid) {
    for (int i = 0; i < PROC_MAX; i++) {
        if (!proc_table[i].active || proc_table[i].pid != pid) continue;
        kernel_serial_printf("[PROC] PID %u terminated.\n", pid);
        if (proc_table[i].state == PROC_RUNNING) {
            /* Self-kill (SYS_SLS_PROC_KILL with one's own pid) or any
             * hypothetical cross-CPU kill: the target is executing RIGHT
             * NOW with its CR3 live, so the Phase-2 teardown here would
             * free the page tables the running CPU is using. Defer: mark
             * pending_teardown and leave state RUNNING so the next timer
             * tick still finds it as "current", saves its context, runs
             * the full teardown, and switches away (schedule_ring3). */
            proc_table[i].pending_teardown = 1;
            kernel_serial_printf(
                "[PROC] PID %u kill deferred (running) — teardown at next "
                "schedule\n", pid);
            return;
        }
        proc_table[i].state  = PROC_ZOMBIE;
        /* Phase 2 teardown, same three steps and same ordering as
         * process_exit(): the target is not executing (its CR3 is not
         * active — in this kernel a process is only RUNNING on the BSP and
         * the killer is either that same process, another ring-3 process
         * making the syscall, or kernel context, none of which can observe
         * a second running process), so everything it owns can be reclaimed
         * now. active stays 1 until the cap teardown's unmap walk
         * (cap_proc_cr3 requires it). Clear the resume state FIRST so a
         * reused slot never inherits the killed process's stale
         * resume_sysret/park_ctx (caught live — see proc_clear_resume_state). */
        proc_clear_resume_state(&proc_table[i]);
        cap_table_teardown(proc_table[i].pid);
        user_destroy_page_table(proc_table[i].cr3);
        proc_free_syscall_stack(&proc_table[i]);
        proc_table[i].active = 0;
        proc_count--;
        return;
    }
    kernel_serial_printf("[PROC] kill: PID %u not found.\n", pid);
}

// ─── process_kill_partition ────────────────────────────────────────────────────
// Phase 14 (LPAR): kills every active process whose partition_id matches,
// reusing process_kill() per-pid (rather than duplicating its ZOMBIE/active
// bookkeeping here) as the roadmap's scope explicitly called for. Pids are
// collected into a snapshot first, then killed, so the "who matches" scan
// and the mutation itself stay clearly separate. Returns the number killed.
uint32_t process_kill_partition(uint32_t partition_id) {
    uint32_t pids[PROC_MAX];
    int n = 0;
    for (int i = 0; i < PROC_MAX; i++) {
        if (proc_table[i].active && proc_table[i].partition_id == partition_id) {
            pids[n++] = proc_table[i].pid;
        }
    }
    for (int i = 0; i < n; i++) process_kill(pids[i]);
    return (uint32_t)n;
}

// ─── sys_sls_proc_list ────────────────────────────────────────────────────────
void sys_sls_proc_list(void) {
    kernel_serial_printf(
        "\n[PROC] Process Table\n"
        " %-5s  %-24s  %-10s  %-8s  %-4s  %s\n"
        " -----  ------------------------  ----------  --------  ----  ------------------\n",
        "PID", "Name", "State", "Priority", "UID", "User RIP");

    uint32_t shown = 0;
    for (int i = 0; i < PROC_MAX; i++) {
        struct ProcessDescriptor* pd = &proc_table[i];
        if (!pd->active) continue;
        kernel_serial_printf(
            " %-5u  %-24s  %-10s  %-8s  %-4u  0x%016lx\n",
            pd->pid, pd->name,
            proc_state_name(pd->state),
            proc_priority_name(pd->priority),
            pd->owner_uid, pd->user_rip);
        shown++;
    }
    if (!shown) kernel_serial_print(" (no processes)\n");
    kernel_serial_printf(" %u process(es).\n\n", shown);
}

// ─── process_hold / process_release / process_priority_set ───────────────────
// Navigator-Parity Gap Roadmap Phase 4. See process.h's comments on
// PROC_HELD and these three declarations for the full rationale (why hold is
// scoped to PROC_SUSPENDED targets only, and why release lands back on
// PROC_SUSPENDED rather than PROC_RUNNING).
int process_hold(uint32_t pid) {
    for (int i = 0; i < PROC_MAX; i++) {
        if (!proc_table[i].active || proc_table[i].pid != pid) continue;
        switch (proc_table[i].state) {
            case PROC_SUSPENDED:
                proc_table[i].state = PROC_HELD;
                kernel_serial_printf("[PROC] PID %u held.\n", pid);
                return 0;
            case PROC_RUNNING:
                kernel_serial_printf(
                    "[PROC] hold: PID %u is currently running; retry once it "
                    "yields its turn (hold cannot preempt the running job "
                    "directly in this pass).\n", pid);
                return -2;
            case PROC_HELD:
                kernel_serial_printf("[PROC] hold: PID %u is already held.\n", pid);
                return -3;
            case PROC_ZOMBIE:
            default:
                kernel_serial_printf("[PROC] hold: PID %u is not eligible.\n", pid);
                return -4;
        }
    }
    kernel_serial_printf("[PROC] hold: PID %u not found.\n", pid);
    return -1;
}

int process_release(uint32_t pid) {
    for (int i = 0; i < PROC_MAX; i++) {
        if (!proc_table[i].active || proc_table[i].pid != pid) continue;
        if (proc_table[i].state != PROC_HELD) {
            kernel_serial_printf("[PROC] release: PID %u is not held.\n", pid);
            return -2;
        }
        proc_table[i].state = PROC_SUSPENDED;
        kernel_serial_printf("[PROC] PID %u released.\n", pid);
        return 0;
    }
    kernel_serial_printf("[PROC] release: PID %u not found.\n", pid);
    return -1;
}

int process_priority_set(uint32_t pid, ProcPriority priority) {
    if (priority != PROC_PRIO_HIGH && priority != PROC_PRIO_NORMAL && priority != PROC_PRIO_LOW)
        return -2;
    for (int i = 0; i < PROC_MAX; i++) {
        if (!proc_table[i].active || proc_table[i].pid != pid) continue;
        proc_table[i].priority = priority;
        kernel_serial_printf("[PROC] PID %u priority set to %s.\n",
                             pid, proc_priority_name(priority));
        return 0;
    }
    kernel_serial_printf("[PROC] priority: PID %u not found.\n", pid);
    return -1;
}

uint64_t sys_sls_proc_priority_set(struct SLSProcPrioritySetRequest* req) {
    if (!req) return 1;
    return process_priority_set(req->pid, (ProcPriority)req->priority) == 0 ? 0 : 1;
}

// ─── Multitenant Isolation Gap Analysis §5 item 8 / §7 item 8: weighted
// per-partition CPU scheduling ─────────────────────────────────────────────
// See process.h's header comment on these two functions for the full
// design note (burst-style weighted round robin, weight 0 = default 1).
int partition_set_cpu_weight(uint32_t partition_id, uint32_t weight) {
    if (partition_id >= PARTITION_MAX) return 1;
    partition_cpu_weight[partition_id] = weight;
    kernel_serial_printf("[SCHED] partition %u: CPU weight set to %u%s\n",
                         partition_id, weight,
                         weight == 0 ? " (0 == default weight 1)" : "");
    return 0;
}

uint32_t partition_get_cpu_weight(uint32_t partition_id) {
    return effective_cpu_weight(partition_id);
}

uint64_t sys_sls_partition_cpu_weight_set(struct SLSPartitionCpuWeightSetRequest* req) {
    if (!req) return 1;
    return (uint64_t)partition_set_cpu_weight(req->partition_id, req->weight);
}

void sys_sls_partition_cpu_weight_list(void) {
    kernel_serial_print("\n[SCHED] Per-partition CPU scheduling weight:\n");
    int shown = 0;
    for (uint32_t i = 0; i < PARTITION_MAX; i++) {
        if (partition_cpu_weight[i] == 0) continue;   // still at the unconfigured default -- nothing interesting to report
        kernel_serial_printf(" partition %u: weight=%u\n", i, partition_cpu_weight[i]);
        shown++;
    }
    kernel_serial_printf(" %d partition(s) with an explicitly configured weight (all others schedule at the default weight of 1).\n\n", shown);
}
