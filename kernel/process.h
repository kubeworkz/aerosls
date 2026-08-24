#ifndef PROCESS_H
#define PROCESS_H

#include <stdint.h>
#include <stddef.h>
#include "scheduler.h"   /* struct TaskContext */

// ─── Process states ───────────────────────────────────────────────────────────
// Navigator-Parity Gap Roadmap Phase 4: added PROC_HELD as a *distinct* state
// from PROC_SUSPENDED, not a reuse of it. Investigation before writing any
// code here found that PROC_SUSPENDED is NOT an idle "operator hold" state --
// it is the scheduler's own transient "not currently running, but eligible
// for the very next round-robin turn" state, set on every single process on
// every single preemption (schedule_ring3(), process.c) and actively scanned
// for as a scheduling candidate by both pick_next_partition() and
// pick_next_process_in_partition(). The original roadmap draft (see this
// file's own Phase 4 section in the roadmap doc) assumed "schedule_ring3()
// already must skip non-PROC_RUNNING processes, so exposing PROC_SUSPENDED
// per-pid is exposing an existing state transition, not adding a new one" --
// that's factually wrong about what the scheduler does: it does NOT skip
// PROC_SUSPENDED, it actively resumes it. Reusing PROC_SUSPENDED directly for
// an operator-invoked hold would mean the "held" process gets picked up and
// resumed by the very next scheduler tick, defeating the entire point of a
// hold. PROC_HELD is a new, distinct value specifically so every existing
// scheduler check (which all test `state == PROC_SUSPENDED` by exact value,
// confirmed via grep across process.c/syscall_dispatch.c/the host tests
// before this was added) automatically and safely excludes it, with zero
// changes needed to pick_next_partition()/pick_next_process_in_partition()/
// schedule_ring3() themselves.
typedef enum {
    PROC_RUNNING   = 0,
    PROC_SUSPENDED = 1,
    PROC_ZOMBIE    = 2,
    PROC_HELD      = 3,
    PROC_BLOCKED   = 4,   // Seed Kernel Phase 1 (two-party): a process that
                          // spawned a child is blocked inside its syscall until
                          // the child exits — distinct from PROC_HELD (operator
                          // hold); non-schedulable and invisible to
                          // process_find_current()/cap_current_pid() meanwhile
                          // (same exact-value checks that exclude HELD).
} ProcState;

static inline const char* proc_state_name(ProcState s) {
    switch (s) {
        case PROC_RUNNING:   return "RUNNING";
        case PROC_SUSPENDED: return "SUSPENDED";
        case PROC_ZOMBIE:    return "ZOMBIE";
        case PROC_HELD:      return "HELD";
        case PROC_BLOCKED:   return "BLOCKED";
        default:             return "UNKNOWN";
    }
}

// ─── Job priority ─────────────────────────────────────────────────────────────
// Navigator-Parity Gap Roadmap Phase 4: coarse 3-tier priority, matching this
// codebase's "smallest real version first" scoping pattern (same judgment
// call as Phase 3's 3-syscall-then-4 authlist growth) rather than a full
// weighted/decay scheduler. Consulted by pick_next_process_in_partition()
// (process.c): within a partition, ALL runnable HIGH-priority processes are
// offered a turn (round-robin among themselves) before any NORMAL process is
// considered, and all NORMAL before any LOW -- strict priority scheduling
// with round-robin fairness *within* each tier. Every process defaults to
// PROC_PRIO_NORMAL at creation, so a deployment that never touches priority
// behaves identically to the pre-Phase-4 flat round robin (verified by the
// new host test).
typedef enum {
    PROC_PRIO_HIGH   = 0,
    PROC_PRIO_NORMAL = 1,
    PROC_PRIO_LOW    = 2,
} ProcPriority;

static inline const char* proc_priority_name(ProcPriority p) {
    switch (p) {
        case PROC_PRIO_HIGH:   return "HIGH";
        case PROC_PRIO_NORMAL: return "NORMAL";
        case PROC_PRIO_LOW:    return "LOW";
        default:                return "UNKNOWN";
    }
}

// ─── Process descriptor ───────────────────────────────────────────────────────
#define PROC_MAX       16
#define PROC_NAME_LEN  64
#define PROC_USER_STACK_PAGES  8    // 32 KiB ring-3 stack per process
                                      // (16 KiB could not hold SLSUploadRequest,
                                      //  a 16,457-byte ring-3 ABI struct — see
                                      //  loader.h; the upload syscall was
                                      //  unusable from ring-3 until this grew)

// User register state captured when a blocking cap_recv parks a process
// mid-syscall — exactly the eight values syscall_entry_stub pushed on entry
// (r11 = user RFLAGS, rcx = user RIP, then the six callee-saved regs) plus
// the user RSP that lives in per_cpu_data[0].user_rsp at park time.
// cap_recv_resume() pushes these back onto a fresh syscall stack so the
// stub's .syscall_return path can pop them and sysret with the user's
// registers intact. Field order is load-bearing (process.c's resume asm
// indexes it as [0..8]).
struct CapParkCtx {
    uint64_t r11;                 /* user RFLAGS (sysret needs R11)  */
    uint64_t rcx;                 /* user RIP (sysret target)        */
    uint64_t r15, r14, r13, r12;  /* user callee-saved               */
    uint64_t rbx, rbp;
    uint64_t user_rsp;            /* [gs:0] at park time             */
};

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
    uint64_t   syscall_stack_top;     // Seed Kernel Phase 1 (two-party): top of
                                       // this process's DEDICATED kernel syscall
                                       // stack (an allocated, identity-mapped
                                       // frame). The shared fixed syscall stack
                                       // sits directly above proc_table in
                                       // .bss, so nested spawns' child frames
                                       // would otherwise grow down into kernel
                                       // state; [gs:8] is pointed here at
                                       // kernel_enter_ring3 time.
    uint64_t   parent_user_rsp;       // Seed Kernel Phase 1 (two-party): the
                                       // parent's Ring-3 RSP at the moment it
                                       // spawned this process (captured from
                                       // gs:0 by the nested-spawn prep; the
                                       // child's syscall entries overwrite gs:0,
                                       // so process_exit() restores it).
    uint32_t   owner_uid;
    uint32_t   parent_pid;            // Seed Kernel Phase 1 (two-party cap
                                       // handoff): pid of the process that
                                       // spawned this one, resolved via
                                       // process_find_current() at spawn time
                                       // (0 = spawned from kernel context).
    uint32_t   partition_id;          // Phase 9 (LPAR): partition_get_for_uid(owner_uid)
                                       // at spawn time — see partition.h
    ProcState  state;
    ProcPriority priority;             // Navigator-Parity Gap Roadmap Phase 4:
                                       // defaults to PROC_PRIO_NORMAL at spawn
                                       // (process_create()/program_spawn()) —
                                       // see pick_next_process_in_partition()
                                       // (process.c) for how this is consulted.

    // ── Seed Kernel Phase 1.5 (blocking cap_recv / live overlap) ─────────
    // A process parked mid-syscall by a blocking cap_recv saves the user
    // state its syscall entry frame held here. On wake, cap_recv_resume()
    // (process.c) rebuilds a FRESH syscall entry frame from these values and
    // re-runs the recv — the old kernel call chain on the syscall stack is
    // abandoned, never unwound (one dead frame per park; bounded, Phase-1
    // posture). Layout order is load-bearing: [r11, rcx, r15..rbp] match
    // syscall_entry_stub's push order exactly.
    struct CapParkCtx park_ctx;
    uint64_t   park_req;              // the request pointer the resume re-runs
                                       // (SLSCapRecvRequest for a recv park,
                                       // SLSChanWaitRequest for a chan-wait park)
    uint32_t   park_syscall;          // the syscall the resume re-runs — set at park
                                       // time (SYS_SLS_CAP_RECV or SYS_SLS_CHAN_WAIT)
                                       // and consumed by cap_recv_resume()'s
                                       // do_syscall re-entry. Generalizes the resume
                                       // so ONE entry point serves both the
                                       // single-channel recv park and the Phase-5
                                       // multi-channel k_chan_wait park.
    uint16_t   waiting_chan;          // channel id this process is BLOCKED on (recv
                                       // park), or CAP_NONE (0xFFFF) when not parked
    uint16_t   waiting_chans[CHAN_WAIT_MAX_CHANS];  // Phase 5 wait park: the channels
                                       // a k_chan_wait / k_chan_send process is
                                       // blocked on — ANY of them waking it resumes
                                       // the syscall. waiting_nchans == 0 means the
                                       // single-channel waiting_chan form (recv park).
    uint8_t    waiting_nchans;        // number of channels in waiting_chans[] (0 =
                                       // not a multi-channel park)
    uint64_t   waiting_deadline;      // Phase 5: ABSOLUTE kernel tick count by which
                                       // the park must resolve, or 0 = block forever.
                                       // Set by cap_wait_chans; NOT cleared by
                                       // cap_wake_chan (the resume re-run consumes it
                                       // via cap_park_deadline_take, so re-parks never
                                       // extend a finite deadline) and cleared by the
                                       // deadline tick's wake only in the sense that
                                       // the re-run takes it. Cleared at slot reuse.
    uint8_t    has_ring3_ctx;         // ring3_ctx holds a valid iretq frame: set at
                                       // async spawn (synthetic) and whenever a timer
                                       // preemption saves real context. kernel_rsp
                                       // alone no longer implies schedulability —
                                       // async children never call kernel_enter_ring3.
    uint8_t    resume_kernel;         // the next schedule of this process must resume
                                       // via the kernel iretq path (cap_recv_resume),
                                       // not ring3_ctx — set by cap_wake_chan(),
                                       // consumed by schedule_ring3()/kernel_switch_next()
    uint8_t    resume_sysret;         // Seed Kernel Phase 1.5 (immediate wake): the
                                       // next schedule of this process must resume at
                                       // .syscall_return (cap_sysret_resume) — its
                                       // ORIGINAL syscall entry frame is still on its
                                       // syscall stack at [top-8..top-64], so the
                                       // process sysrets to ring-3 as if its syscall
                                       // (send, yield) had simply taken a while. Set
                                       // by cap_maybe_handoff()/sys_sls_yield(),
                                       // consumed by schedule_ring3()/kernel_switch_next().
    struct ProcessDescriptor* handoff_target;  // Phase 1.5: the process this one
                                       // just WOKE with cap_wake_chan() — the send
                                       // hands the CPU to it immediately
                                       // (cap_maybe_handoff) instead of letting the
                                       // receiver wait for the next tick.
    uint8_t    pending_teardown;   // Phase 2: a kill targeted this process while it
                                       // was RUNNING (self-kill via SYS_SLS_PROC_KILL
                                       // with one's own pid, or any hypothetical
                                       // cross-CPU kill). Its page tables are live,
                                       // so teardown was deferred: the next timer
                                       // tick saves its context, runs the full
                                       // Phase-2 teardown, and switches away (see
                                       // schedule_ring3). Reset at spawn/exit.
    uint8_t    active;
};

// ─── Syscall numbers ──────────────────────────────────────────────────────────
#define SYS_SLS_PROC_CREATE 160
#define SYS_SLS_PROC_KILL   161
#define SYS_SLS_PROC_LIST   162
#define SYS_SLS_EXIT        164  // Ring-3 self-exit; returns control to kernel

// Navigator-Parity Gap Roadmap Phase 4: job priority + hold/release.
// 245-247 are the next free numbers after Phase 3's own additions topped out
// at 244 (SYS_SLS_AUTHLIST_LIST) -- confirmed via grep across every header
// defining SYS_SLS_* before picking these.
#define SYS_SLS_PROC_HOLD          245
#define SYS_SLS_PROC_RELEASE       246
#define SYS_SLS_PROC_PRIORITY_SET  247

// Seed Kernel Phase 1 (two-party verification): the child resolves its
// spawner's pid so it can call cap_chan_create(far_pid=...) and have the
// far-end caps minted directly into the parent's capability table. 298 is
// the next free number after the capability layer's 289-297.
#define SYS_SLS_GETPPID            298

// Seed Kernel Phase 1.5 (live overlap): NON-BLOCKING spawn. The child is
// created with a synthetic ring-3 context and marked runnable; the spawn
// returns to the caller immediately and the scheduler iretq's into the
// child on the next tick (or when the caller blocks). 299 is the next free
// number after SYS_SLS_GETPPID — confirmed via grep across every kernel
// header defining SYS_SLS_* before picking, per this repo's convention.
#define SYS_SLS_PROGRAM_SPAWN_NB   299

// Seed Kernel Phase 1.5 (immediate wake): VOLUNTARY yield. The process's
// syscall entry frame stays on its syscall stack and the scheduler switches
// to the next runnable process; on its next schedule it resumes at
// .syscall_return (cap_sysret_resume) exactly as if the yield syscall had
// taken a while. Used by the overlap test to hand the CPU back to an async
// child so it can exit cleanly after the parent's last recv. 300 is the next
// free number after SYS_SLS_PROGRAM_SPAWN_NB.
#define SYS_SLS_YIELD              300

// Seed Kernel Phase 1.5 (gated async spawn): like SYS_SLS_PROGRAM_SPAWN_NB
// but the child starts PROC_HELD (non-schedulable) until the spawner
// releases it via SYS_SLS_PROC_RELEASE. 301 is the next free number after
// SYS_SLS_YIELD.
#define SYS_SLS_PROGRAM_SPAWN_NB_HELD  301

struct SLSProcPrioritySetRequest {
    uint32_t pid;
    uint32_t priority;   // ProcPriority value; validated in process_priority_set()
};

// Multitenant Isolation Gap Analysis §5 item 8 / §7 item 8: weighted
// per-partition CPU scheduling. 273/274 are the next free numbers after
// usage_metering.h's SYS_SLS_USAGE_REPORT = 272 -- confirmed via a fresh
// grep across every kernel/*.h and net/*.h SYS_SLS_* define before picking
// these, matching this codebase's own "reconfirm the next free number at
// implementation time" convention.
#define SYS_SLS_PARTITION_CPU_WEIGHT_SET  273
#define SYS_SLS_PARTITION_CPU_WEIGHT_LIST 274

struct SLSPartitionCpuWeightSetRequest {
    uint32_t partition_id;
    uint32_t weight;   // 0 = reset to the default (1); validated in partition_set_cpu_weight()
};

// ─── Syscall argument for process creation ────────────────────────────────────
struct ProcCreateRequest {
    char     object_name[PROC_NAME_LEN];  // name of the SERVICE_PROCESS SLS object
    uint32_t owner_uid;
};

// ─── Public API ───────────────────────────────────────────────────────────────
extern struct ProcessDescriptor proc_table[PROC_MAX];
extern uint32_t                 proc_count;

void     process_init(void);
uint32_t alloc_pid(void);  // returns a fresh PID (no collision with existing active procs)

// Allocates a process's dedicated 8 KiB kernel syscall stack (TWO contiguous
// frames — see alloc_proc_syscall_stack's comment in process.c). Returns the
// stack TOP, or 0 on failure. Shared by process_create()/program_spawn_common()
// and cap_create_sidecar() (whose async children run through the scheduler
// and must have their own syscall stack before their first syscall).
uint64_t alloc_proc_syscall_stack(uint32_t partition_id);

uint32_t process_create(struct ProcCreateRequest* req);  // returns PID or 0
uint32_t program_spawn_nb(const char* object_name, uint32_t owner_uid);

// Phase 1.5 (immediate wake, gated spawn): like program_spawn_nb, but the
// child starts in PROC_HELD — not schedulable — until the spawner calls
// process_release(pid) (SYS_SLS_PROC_RELEASE). The gated child lets the
// spawner provision resources (channel far-end caps) before the child can
// run, closing the spawn-then-provision timer race.
uint32_t program_spawn_nb_held(const char* object_name, uint32_t owner_uid);
void     process_kill(uint32_t pid);
void     sys_sls_proc_list(void);

// ─── Seed Kernel Phase 1.5: blocking cap_recv park/wake hooks ──────────────
// Strong overrides of cap.c's weak defaults (which let the capability layer
// stay host-testable without the scheduler). cap_wait_chan() parks the
// calling process on the channel and switches to the next runnable process;
// it returns only when it could NOT park (no runnable process — the caller
// then returns CAP_EAGAIN). cap_wait_chans() is the Phase-5 analogue for
// k_chan_wait (TIMEOUT_NONE): same park, but on a LIST of channels — a wake
// on ANY of them resumes the process to re-run the wait syscall.
// cap_wake_chan() marks the parked process runnable again (resume via the
// kernel path). cap_recv_resume() is the kernel-mode resume entry point
// (iretq'd to after wake); it re-runs whatever syscall park_syscall names.
int  cap_wait_chan(uint32_t chan_id, void* recv_req);
int  cap_wait_chans(const uint32_t* chan_ids, uint32_t n, void* req,
                    uint32_t park_syscall);
void cap_wake_chan(uint32_t chan_id);
void cap_recv_resume(struct ProcessDescriptor* pd);   /* noreturn */

// Seed Kernel Phase 1.5 (immediate wake): strong override of cap.c's weak
// hook, called by cap_send() right after cap_wake_chan(). If the wake
// parked-process is stashed in cur->handoff_target, the sender yields the
// CPU to it NOW (direct handoff, seL4-style) instead of letting it wait
// for the next timer tick — the receiver runs before the sender's send
// even returns to ring-3. cap_sysret_resume() is the kernel-mode resume
// entry point that sysrets the yielded process back into its own syscall.
void cap_maybe_handoff(void);
void cap_sysret_resume(struct ProcessDescriptor* pd);   /* noreturn */

// Phase 14 (LPAR): kills every active process in partition_id, reusing
// process_kill() per-pid. Returns the number of processes killed. Called
// from partition_destroy().
uint32_t process_kill_partition(uint32_t partition_id);

// ─── Navigator-Parity Gap Roadmap Phase 4: hold/release + priority ───────────
// process_hold(): transitions an active process from PROC_SUSPENDED to the
// new, distinct PROC_HELD state (see ProcState's comment above for why these
// must not be the same value). Deliberately scoped to require the target
// already be PROC_SUSPENDED -- NOT PROC_RUNNING. Holding the process that is
// *currently executing on the CPU* would require schedule_ring3() to
// recognize a state change that happened out from under it between timer
// ticks; schedule_ring3() identifies "the process it must save and preempt"
// by scanning for state == PROC_RUNNING, so flipping that same slot to
// PROC_HELD asynchronously (from a syscall handler, not from inside the
// timer ISR) would make it invisible to that scan and never get preempted at
// all. Rather than add an unproven asynchronous-preemption path this pass
// doesn't need, hold is scoped to "processes waiting for their next turn,"
// which is already exactly what job hold means for a job that isn't
// mid-quantum. Returns 0 on success, -1 if pid isn't found/active, -2 if pid
// is running (right now, not eligible), -3 if pid is already held, -4 if pid
// is a zombie.
int process_hold(uint32_t pid);

// process_release(): transitions a PROC_HELD process back to PROC_SUSPENDED
// (its normal "waiting for next round-robin turn" state) -- not directly to
// PROC_RUNNING; it simply re-enters the same pool pick_next_partition()/
// pick_next_process_in_partition() already scan, and gets its next turn on
// the same fair basis as every other suspended process. Returns 0 on
// success, -1 if pid isn't found/active, -2 if pid isn't currently held.
int process_release(uint32_t pid);

// process_priority_set(): validates priority is one of the three
// ProcPriority values and applies it to the matching active pid. Returns 0
// on success, -1 if pid isn't found/active, -2 if priority is out of range.
int process_priority_set(uint32_t pid, ProcPriority priority);

uint64_t sys_sls_proc_priority_set(struct SLSProcPrioritySetRequest* req);

// ─── Multitenant Isolation Gap Analysis §5 item 8 / §7 item 8: weighted
// per-partition CPU scheduling ─────────────────────────────────────────────
// LPAR Phase 12 built round-robin partition fairness deliberately scoped to
// "starvation prevention," not proportional fairness -- every partition
// gets an equal number of turns regardless of weight. This closes that gap
// with a real, if simple, weighted-round-robin mechanism: pick_next_
// partition() (process.c) grants a partition `weight` CONSECUTIVE turns
// (as long as it still has runnable work) before rotating to the next
// candidate in the ring, instead of always rotating after exactly one turn.
// A partition's weight defaults to 1 (BSS-zero-init on partition_cpu_weight[]
// means "unset", interpreted as 1, the same "0 means the neutral/default
// case" idiom partition_set_frame_quota() uses for "0 = unlimited") -- so a
// deployment that never calls partition_set_cpu_weight() at all reduces,
// by construction, to the exact pre-existing round-robin behavior, not an
// approximation of it. This is a burst-style weighted round robin (a
// weight-3 partition gets 3 turns in a row, then yields), not a smooth
// proportional/CFS-style interleave -- named honestly as the simpler of the
// two, matching this whole roadmap's "groundwork, not a hypervisor"
// posture (see partition.h's own top-of-file comment) the same way LPAR
// Phase 12 itself was honest about not building full weighted fair
// queueing.
//
// Sets partition_id's CPU scheduling weight. Returns 0 on success, 1 if
// partition_id is out of range ([0, PARTITION_MAX)). Does not validate that
// partition_id was actually partition_create()'d -- mirrors partition_set_
// frame_quota()'s own "never fails, every id maps to something" posture,
// since a weight can usefully be pre-configured before a partition exists.
int partition_set_cpu_weight(uint32_t partition_id, uint32_t weight);

// Introspection. Returns 1 (the default weight) for an out-of-range
// partition_id or one that was never explicitly configured -- deliberately
// NOT a sentinel-style "invalid" value the way partition_get_frame_quota()
// uses 0xFFFF...F, since 1 IS the honest, correct answer for "what weight
// does this partition currently schedule at" in both cases.
uint32_t partition_get_cpu_weight(uint32_t partition_id);

uint64_t sys_sls_partition_cpu_weight_set(struct SLSPartitionCpuWeightSetRequest* req);
void     sys_sls_partition_cpu_weight_list(void);

// Spawn a Ring-3 process from an OBJ_TYPE_PROGRAM catalog object.
// Identical pipeline to process_create() but accepts PROGRAM type,
// making OBJ_TYPE_PROGRAM the SLS-native replacement for a filesystem exec.
// Returns the new PID on success, 0 on failure.
uint32_t program_spawn(const char* object_name, uint32_t owner_uid);

// Kernel-side exit handler: restores kernel context saved by kernel_enter_ring3.
void process_exit(uint32_t exit_code);

// Phase 7: find the Ring-3 process currently executing, for callers that
// need to know "who is asking" (e.g. simi_runtime.c's authority-checked
// RESOLVE). Mirrors schedule_ring3()'s own scan for the running process
// (active && state==PROC_RUNNING && kernel_rsp!=0 — kernel_rsp is only
// nonzero while the kernel has actually entered that process via
// kernel_enter_ring3). Returns NULL if no Ring-3 process is currently
// running (e.g. a call made from pure kernel context).
struct ProcessDescriptor* process_find_current(void);
uint32_t sys_sls_getppid(void);
uint32_t sys_sls_yield(void);   /* SYS_SLS_YIELD (300) — Phase 1.5 immediate-wake handback */

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
