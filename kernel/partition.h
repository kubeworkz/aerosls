/*
 * partition.h — Phase 8 (v0.3) LPAR groundwork: partition-scoped single-
 * level store. See AeroSLS-SIMI-ISA-v0.1.md §15 for the design writeup.
 *
 * This is deliberately *groundwork*, not a hypervisor: there is one
 * physical kernel, one physical object_catalog[], one physical address
 * space. What this file adds is the identity/boundary concept a real
 * LPAR implementation would need to enforce isolation on top of that
 * shared substrate — a small table of defined partitions, a uid ->
 * partition assignment (mirrors role_table's uid -> role assignment
 * exactly), and the one comparison (object.partition_id vs. caller's
 * partition) that object_catalog.c's catalog_check_access() now performs
 * before any role-based logic runs. Every existing authority-checked call
 * path (DB select/insert/update/delete, grant/revoke, SIMI RESOLVE via
 * Phase 7) gets partition isolation for free through that one choke
 * point, without any of those call sites needing to know partitions
 * exist.
 *
 * Backward compatibility is load-bearing here: PARTITION_SYSTEM and
 * PARTITION_DEFAULT are both 0. Every pre-Phase-8 object (partition_id
 * defaults to 0 via struct zero-init) and every pre-Phase-8 uid
 * (unassigned -> partition_get_for_uid() returns 0) stay in the same
 * partition unless a partition is explicitly created and something is
 * explicitly assigned into it — so nothing in Phases 1-7's behavior
 * changes unless Phase 8's new API is actually used.
 */
#ifndef PARTITION_H
#define PARTITION_H

#include <stdint.h>

#define PARTITION_SYSTEM   0   /* uid 0 (kernel) always resolves here, and it's
                                 * also where every object starts out — see the
                                 * top comment on why this equals DEFAULT. */
#define PARTITION_DEFAULT  0   /* unassigned uids resolve here */
#define PARTITION_MAX         16   /* defined partitions, small fixed table like PROC_MAX */
#define PARTITION_ASSIGN_MAX  64   /* uid -> partition assignments, mirrors ROLE_TABLE_MAX */
#define PARTITION_NAME_LEN    32

struct SLSPartitionEntry {
    uint32_t partition_id;
    char     name[PARTITION_NAME_LEN];
    uint8_t  active;
};

struct SLSPartitionAssign {
    uint32_t uid;
    uint32_t partition_id;
    uint8_t  active;
};

extern struct SLSPartitionEntry  partition_table[PARTITION_MAX];
extern struct SLSPartitionAssign partition_assign_table[PARTITION_ASSIGN_MAX];

/* Multi-Node Partition Scaling Roadmap, Phase 2: which node owns a given
 * partition. Kept as its own table, the same "separate table, not a field
 * bolted onto struct SLSPartitionEntry" shape partition_assign_table[]
 * already established for uid->partition assignment -- so a future
 * migration phase can hand off ownership with a single row update, never
 * touching partition identity (name, active flag) itself. */
struct SLSPartitionOwner {
    uint32_t partition_id;
    uint32_t node_id;
    uint8_t  active;
};
extern struct SLSPartitionOwner partition_owner_table[PARTITION_MAX];

/* Pre-populates partition_table[0] as PARTITION_SYSTEM ("system", active).
 * Everything else starts zeroed/inactive — no assignment is required for
 * existing behavior to keep working, see the top comment. Called once
 * from kernel.c's init sequence, near process_init(). */
void partition_init(void);

/* Defines a new partition, returns its id, or 0xFFFFFFFF if the table is
 * full or `name` doesn't fit PARTITION_NAME_LEN-1 bytes. Ids are handed
 * out as the table-slot index, so they're small and stable for a given
 * boot (no persistence yet — see AeroSLS-SIMI-ISA-v0.1.md §15's "what
 * Phase 8 deliberately did not do"). */
uint32_t partition_create(const char* name);

/* Assigns `uid` to `partition_id` (must be PARTITION_SYSTEM or a
 * partition_create()'d id — rejects unknown/inactive ids). Updates an
 * existing assignment in place if one exists for `uid`, otherwise
 * inserts into a free slot. uid 0 can never be (re)assigned — it's
 * permanently PARTITION_SYSTEM, the same "kernel is special" invariant
 * catalog_get_role() already enforces for role 0. Returns 0 on success,
 * 1 on failure (unknown partition, uid==0, or assignment table full). */
int partition_assign_uid(uint32_t uid, uint32_t partition_id);

/* uid 0 -> PARTITION_SYSTEM unconditionally (mirrors catalog_get_role()'s
 * uid==0 special case). Otherwise: the uid's assigned partition if one
 * exists, else PARTITION_DEFAULT. Never fails — every uid has a
 * partition, even if only by default. */
uint32_t partition_get_for_uid(uint32_t uid);

/* Debug/introspection listing, mirrors sys_sls_obj_list()'s style. */
void sys_sls_partition_list(void);

/* ─── Multi-Node Partition Scaling Roadmap, Phase 2: ownership & node
 * pinning ────────────────────────────────────────────────────────────────
 * partition_create() (and partition_init() for PARTITION_SYSTEM) stamp a
 * new partition's owner as cluster_local_node_id() (net/consensus.h,
 * Phase 1) by default -- so on a system that never calls cluster_init()
 * (every deployment today; Phase 1 deliberately didn't wire that call
 * into boot), that default is node id 0, the same "uninitialized"
 * sentinel cluster_local_node_id() itself returns before cluster_init()
 * runs. Both sides of partition_is_local()'s comparison read that same
 * sentinel in that case, so every partition reads as locally owned --
 * the correct, honest behavior for a single-kernel-instance deployment
 * that hasn't opted into cluster identity at all, not a false claim of
 * real multi-node awareness. */

/* Returns the node_id that owns partition_id, or 0 if no ownership row
 * exists for it (0 doubles as both "unowned" and the Phase 1
 * uninitialized-node sentinel -- see above; never fails for an
 * out-of-range/inactive partition_id, mirroring partition_get_for_uid()'s
 * own never-fails posture). */
uint32_t partition_get_owner_node(uint32_t partition_id);

/* Sets/overwrites which node owns partition_id. Returns 0 on success, 1
 * if partition_id isn't a currently-active, defined partition. This is
 * deliberately just a table write -- no side effects (killing processes,
 * moving pages, revoking a consensus lease) are attempted here. A future
 * migration phase orchestrates those separately and calls this only as
 * its final ownership-handoff step. */
int partition_set_owner_node(uint32_t partition_id, uint32_t node_id);

/* The local/remote decision point this phase exists to establish: 1 if
 * partition_id is owned by this node (cluster_local_node_id()), 0
 * otherwise -- including for an unowned or undefined partition_id. What a
 * future phase does with a "0" answer (forward the request, deny it,
 * queue it) is deliberately NOT decided here -- see the roadmap doc's own
 * Phase 2 scope note. */
int partition_is_local(uint32_t partition_id);

/* ─── Phase 14 (LPAR): partition lifecycle ──────────────────────────────────
 * partition_destroy() is real teardown, not just a table row removal:
 * kills every process in partition_id (process_kill_partition()), vfrees
 * every catalog object in it (catalog_vfree_partition()), resets its frame-
 * usage accounting (partition_reset_frame_usage() -- accounting only, see
 * frame_pool.h's comment on that function for why physical frames are NOT
 * reclaimed), clears every partition_assign_table[] row pointing at it, and
 * finally deactivates the partition_table[] entry, freeing the slot for a
 * future partition_create(). PARTITION_SYSTEM can never be destroyed
 * (mirrors partition_assign_uid()'s permanent uid==0 special case).
 * Persists the table afterward (Phase 10). Returns 0 on success, 1 if
 * partition_id is PARTITION_SYSTEM or not a currently-active partition. */
int partition_destroy(uint32_t partition_id);

/* partition_pause()/partition_resume() are lightweight, non-destructive:
 * they only set/clear a runtime (non-persisted) flag that Phase 12's
 * pick_next_partition() (kernel/process.c) checks and skips -- no process
 * is killed, no object is freed, no frame accounting changes. A paused
 * partition's processes stay exactly as they were; they simply never get
 * chosen for a scheduling turn until resumed. Both return 0 on success, 1
 * if partition_id is not a currently-active partition. */
int partition_pause(uint32_t partition_id);
int partition_resume(uint32_t partition_id);

/* Returns 1 if partition_id is currently paused, 0 otherwise -- including
 * for an inactive/out-of-range partition_id (never fails, mirrors
 * partition_get_for_uid()'s posture). Consulted by pick_next_partition(). */
int partition_is_paused(uint32_t partition_id);

// ─── Syscalls ─────────────────────────────────────────────────────────────
#define SYS_SLS_PARTITION_CREATE  210
#define SYS_SLS_PARTITION_ASSIGN  211
#define SYS_SLS_PARTITION_LIST    212
#define SYS_SLS_PARTITION_DESTROY 214
#define SYS_SLS_PARTITION_PAUSE   215
#define SYS_SLS_PARTITION_RESUME  216

struct SLSPartitionCreateRequest {
    char name[PARTITION_NAME_LEN];
};
struct SLSPartitionAssignRequest {
    uint32_t uid;
    uint32_t partition_id;
};

/* Thin syscall wrappers, same shape as sys_sls_role_set()/sys_sls_valloc()
 * elsewhere — return a 64-bit value regardless of the underlying function's
 * natural width, matching do_syscall()'s uint64_t ABI. */
uint64_t sys_sls_partition_create(struct SLSPartitionCreateRequest* req);
uint64_t sys_sls_partition_assign(struct SLSPartitionAssignRequest* req);

/* Phase 14: plain-uint32-arg syscall wrappers, same shape as process_kill()'s
 * dispatch case (do_syscall() passes the raw arg cast to uint32_t, no
 * request struct needed for a single id). */
uint64_t sys_sls_partition_destroy(uint32_t partition_id);
uint64_t sys_sls_partition_pause(uint32_t partition_id);
uint64_t sys_sls_partition_resume(uint32_t partition_id);

#endif /* PARTITION_H */
