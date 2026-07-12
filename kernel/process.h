#ifndef PROCESS_H
#define PROCESS_H

#include <stdint.h>
#include <stddef.h>

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
    uint32_t   owner_uid;
    ProcState  state;
    uint8_t    active;
};

// ─── Syscall numbers ──────────────────────────────────────────────────────────
#define SYS_SLS_PROC_CREATE 160
#define SYS_SLS_PROC_KILL   161
#define SYS_SLS_PROC_LIST   162

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

// Low-level: enter user space via sysretq (does not return in kernel context)
void enter_user_process(uint64_t cr3, uint64_t rip, uint64_t rsp);

#endif /* PROCESS_H */
