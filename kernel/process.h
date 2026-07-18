#ifndef PROCESS_H
#define PROCESS_H

#include <stdint.h>
#include <stddef.h>
#include "scheduler.h"   /* struct TaskContext */

// ─── Process states ───────────────────────────────────────────────────────────
typedef enum {
    PROC_RUNNING   = 0,
    PROC_SUSPENDED = 1,
    PROC_ZOMBIE    = 2,
} ProcState;

static inline const char* proc_state_name(ProcState s) {
    switch (s) {
        case PROC_RUNNING:   return "RUNNING";
        case PROC_SUSPENDED: return "SUSPENDED";
        case PROC_ZOMBIE:    return "ZOMBIE";
        default:             return "UNKNOWN";
    }
}

// ─── Process descriptor ───────────────────────────────────────────────────────
#define PROC_MAX       16
#define PROC_NAME_LEN  64
#define PROC_USER_STACK_PAGES  4    // 16 KiB ring-3 stack per process

struct ProcessDescriptor {
    uint32_t   pid;
    char       name[PROC_NAME_LEN];   // mirrors the SERVICE_PROCESS object name
    uint64_t   object_id;             // FNV-1a of the source SLS object
    uint64_t   cr3;                   // physical address of the process's PML4
    uint64_t   user_rip;              // entry point in user space
    uint64_t   user_rsp;              // current user-space stack pointer
    uint64_t   user_stack_phys;       // physical base of the stack allocation
    uint64_t   kernel_rsp;            // saved kernel RSP — restored on exit
    uint64_t   kernel_cr3;            // saved kernel CR3 — restored on exit
    struct TaskContext ring3_ctx;     // full Ring-3 context saved by timer ISR
    uint32_t   owner_uid;
    uint32_t   partition_id;          // Phase 9 (LPAR): partition_get_for_uid(owner_uid)
                                       // at spawn time — see partition.h
    ProcState  state;
    uint8_t    active;
};

// ─── Syscall numbers ──────────────────────────────────────────────────────────
#define SYS_SLS_PROC_CREATE 160
#define SYS_SLS_PROC_KILL   161
#define SYS_SLS_PROC_LIST   162
#define SYS_SLS_EXIT        164  // Ring-3 self-exit; returns control to kernel
// ─── Syscall argument for process creation ────────────────────────────────────
struct ProcCreateRequest {
    char     object_name[PROC_NAME_LEN];  // name of the SERVICE_PROCESS SLS object
    uint32_t owner_uid;
};

// ─── Public API ───────────────────────────────────────────────────────────────
extern struct ProcessDescriptor proc_table[PROC_MAX];
extern uint32_t                 proc_count;

void     process_init(void);
uint32_t process_create(struct ProcCreateRequest* req);  // returns PID or 0
void     process_kill(uint32_t pid);
void     sys_sls_proc_list(void);

// Phase 14 (LPAR): kills every active process in partition_id, reusing
// process_kill() per-pid. Returns the number of processes killed. Called
// from partition_destroy().
uint32_t process_kill_partition(uint32_t partition_id);

// Spawn a Ring-3 process from an OBJ_TYPE_PROGRAM catalog object.
// Identical pipeline to process_create() but accepts PROGRAM type,
// making OBJ_TYPE_PROGRAM the SLS-native replacement for a filesystem exec.
// Returns the new PID on success, 0 on failure.
uint32_t program_spawn(const char* object_name, uint32_t owner_uid);

// Kernel-side exit handler: restores kernel context saved by kernel_enter_ring3.
void process_exit(uint32_t exit_code);

// Phase 7: find the Ring-3 process currently executing, for callers that
// need to know "who is asking" (e.g. timi_runtime.c's authority-checked
// RESOLVE). Mirrors schedule_ring3()'s own scan for the running process
// (active && state==PROC_RUNNING && kernel_rsp!=0 — kernel_rsp is only
// nonzero while the kernel has actually entered that process via
// kernel_enter_ring3). Returns NULL if no Ring-3 process is currently
// running (e.g. a call made from pure kernel context).
struct ProcessDescriptor* process_find_current(void);

// Called from isr32_stub when a Ring-3 timer interrupt fires.
// Saves the current Ring-3 process context from the interrupt stack,
// selects the next Ring-3 process (or the same if only one), writes its
// context back to the interrupt stack, and returns the interrupt RSP
// (unchanged — the interrupt stack is the arena for all context frames).
uint64_t schedule_ring3(uint64_t ctx_rsp);

// Low-level: save kernel RSP/CR3, enter user space via sysretq.
// Returns after process_exit() restores the kernel continuation.
void kernel_enter_ring3(uint64_t* rsp_save, uint64_t* cr3_save,
                         uint64_t cr3, uint64_t rip, uint64_t rsp);

// Low-level: enter user space via sysretq (does not return in kernel context)
void enter_user_process(uint64_t cr3, uint64_t rip, uint64_t rsp);

#endif /* PROCESS_H */
