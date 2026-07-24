/*
 * partition.c — Phase 8 (v0.3) LPAR groundwork. See partition.h for the
 * contract and AeroSLS-SIMI-ISA-v0.1.md §15 for the design writeup.
 *
 * Deliberately shaped to mirror object_catalog.c's existing role_table
 * pattern (find-or-insert-by-uid, active flag, linear scan over a small
 * fixed table) rather than invent a new idiom — partition assignment
 * *is* role assignment's sibling concept, just gating a different axis
 * (which partition's single-level store you can see) instead of role
 * (what you're allowed to do to what you can see).
 */
#include "partition.h"
#include "kernel_io.h"
#include "persist.h"
#include "process.h"
#include "object_catalog.h"
#include "frame_pool.h"
#include "../net/consensus.h"   // Multi-Node Partition Scaling Roadmap Phase 2 -- cluster_local_node_id()
#include "stream.h"              // Multi-Node Phase 6 addendum -- stream_relocate_partition() (real migration data movement)

struct SLSPartitionEntry  partition_table[PARTITION_MAX];
struct SLSPartitionAssign partition_assign_table[PARTITION_ASSIGN_MAX];
struct SLSPartitionOwner  partition_owner_table[PARTITION_MAX];

/* Phase 14 (LPAR): pause/resume state. Deliberately kept as separate,
 * ephemeral runtime state rather than a field on struct SLSPartitionEntry
 * -- it's scheduling state, not partition identity, so it doesn't survive
 * a reboot (there is no persist_partitions() call anywhere in this file
 * that touches it) any more than a process's "currently running" state
 * does. BSS zero-init = "not paused", the correct default for every
 * partition until partition_pause() is explicitly called, same
 * backward-compatible-by-construction discipline as every other LPAR
 * phase's new state. */
static uint8_t partition_paused[PARTITION_MAX];

/* ─── String helpers (no libc dependency, same discipline as every other
 * kernel source file) ────────────────────────────────────────────────── */
static void pt_strcpy(char* dst, const char* src, int n) {
    int i; for (i = 0; i < n - 1 && src[i]; i++) dst[i] = src[i]; dst[i] = '\0';
}
static int pt_strlen(const char* s) { int n = 0; while (s[n]) n++; return n; }

void partition_init(void) {
    for (int i = 0; i < PARTITION_MAX; i++) partition_table[i].active = 0;
    for (int i = 0; i < PARTITION_ASSIGN_MAX; i++) partition_assign_table[i].active = 0;
    for (int i = 0; i < PARTITION_MAX; i++) partition_owner_table[i].active = 0;   // Phase 2 (Multi-Node)

    /* Slot 0 == PARTITION_SYSTEM, pre-populated and permanent — this is
     * the partition every pre-existing object/uid is already implicitly
     * in (see partition.h's backward-compatibility note), so it's given
     * an explicit, named entry rather than just being "whatever id 0
     * happens to mean." */
    partition_table[0].partition_id = PARTITION_SYSTEM;
    pt_strcpy(partition_table[0].name, "system", PARTITION_NAME_LEN);
    partition_table[0].active = 1;

    /* Phase 2 (Multi-Node Partition Scaling Roadmap): PARTITION_SYSTEM is
     * owned by whichever node this is, per cluster_local_node_id() --
     * node id 0 (the Phase 1 uninitialized sentinel) on every deployment
     * that hasn't called cluster_init(), which is every deployment today.
     * See partition.h's own comment on why that's the correct, honest
     * default rather than a fabricated claim of real ownership. */
    partition_owner_table[0].partition_id = PARTITION_SYSTEM;
    partition_owner_table[0].node_id      = cluster_local_node_id();
    partition_owner_table[0].active       = 1;

    kernel_serial_print("[PARTITION] LPAR groundwork initialised (1 partition: system).\n");
}

uint32_t partition_create(const char* name) {
    if (!name || pt_strlen(name) >= PARTITION_NAME_LEN) return 0xFFFFFFFFu;
    for (int i = 1; i < PARTITION_MAX; i++) {   /* slot 0 is permanently PARTITION_SYSTEM */
        if (!partition_table[i].active) {
            partition_table[i].partition_id = (uint32_t)i;
            pt_strcpy(partition_table[i].name, name, PARTITION_NAME_LEN);
            partition_table[i].active = 1;

            /* Phase 2 (Multi-Node): a newly created partition is owned by
             * this node by default -- see the header comment on why this
             * is a correct no-op for single-node deployments. */
            partition_owner_table[i].partition_id = (uint32_t)i;
            partition_owner_table[i].node_id      = cluster_local_node_id();
            partition_owner_table[i].active       = 1;

            kernel_serial_printf("[PARTITION] created '%s' (id=%u, owner node=%u).\n",
                                 name, (unsigned)i, (unsigned)partition_owner_table[i].node_id);
            persist_partitions();   // Phase 10 (now also covers partition_owner_table[], Phase 2)
            return (uint32_t)i;
        }
    }
    kernel_serial_print("[PARTITION] ERROR: partition table full.\n");
    return 0xFFFFFFFFu;
}

static int partition_id_valid(uint32_t partition_id) {
    if (partition_id == PARTITION_SYSTEM) return 1;
    if (partition_id >= PARTITION_MAX) return 0;
    return partition_table[partition_id].active;
}

int partition_assign_uid(uint32_t uid, uint32_t partition_id) {
    if (uid == 0) {
        kernel_serial_print("[PARTITION] ERROR: uid 0 (kernel) cannot be reassigned.\n");
        return 1;
    }
    if (!partition_id_valid(partition_id)) {
        kernel_serial_printf("[PARTITION] ERROR: partition id %u is not defined.\n",
                             (unsigned)partition_id);
        return 1;
    }

    /* Update an existing assignment in place, if any. */
    for (int i = 0; i < PARTITION_ASSIGN_MAX; i++) {
        if (partition_assign_table[i].active && partition_assign_table[i].uid == uid) {
            partition_assign_table[i].partition_id = partition_id;
            kernel_serial_printf("[PARTITION] uid %u reassigned to partition %u.\n",
                                 (unsigned)uid, (unsigned)partition_id);
            persist_partitions();   // Phase 10
            return 0;
        }
    }
    /* Otherwise insert into a free slot. */
    for (int i = 0; i < PARTITION_ASSIGN_MAX; i++) {
        if (!partition_assign_table[i].active) {
            partition_assign_table[i].uid          = uid;
            partition_assign_table[i].partition_id = partition_id;
            partition_assign_table[i].active       = 1;
            kernel_serial_printf("[PARTITION] uid %u assigned to partition %u.\n",
                                 (unsigned)uid, (unsigned)partition_id);
            persist_partitions();   // Phase 10
            return 0;
        }
    }
    kernel_serial_print("[PARTITION] ERROR: assignment table full.\n");
    return 1;
}

uint32_t partition_get_for_uid(uint32_t uid) {
    if (uid == 0) return PARTITION_SYSTEM;
    for (int i = 0; i < PARTITION_ASSIGN_MAX; i++) {
        if (partition_assign_table[i].active && partition_assign_table[i].uid == uid)
            return partition_assign_table[i].partition_id;
    }
    return PARTITION_DEFAULT;
}

// ─── Multi-Node Partition Scaling Roadmap, Phase 2: ownership & node pinning ──
uint32_t partition_get_owner_node(uint32_t partition_id) {
    for (int i = 0; i < PARTITION_MAX; i++) {
        if (partition_owner_table[i].active && partition_owner_table[i].partition_id == partition_id)
            return partition_owner_table[i].node_id;
    }
    return 0;   /* no ownership row -- see partition.h's comment on why 0 is the honest answer */
}

int partition_set_owner_node(uint32_t partition_id, uint32_t node_id) {
    if (!partition_id_valid(partition_id)) {
        kernel_serial_printf(
            "[PARTITION] ERROR: cannot set owner -- partition id %u is not an "
            "active, defined partition.\n", (unsigned)partition_id);
        return 1;
    }
    for (int i = 0; i < PARTITION_MAX; i++) {
        if (partition_owner_table[i].active && partition_owner_table[i].partition_id == partition_id) {
            partition_owner_table[i].node_id = node_id;
            kernel_serial_printf("[PARTITION] partition %u ownership set to node %u.\n",
                                 (unsigned)partition_id, (unsigned)node_id);
            persist_partitions();
            return 0;
        }
    }
    /* No existing row (shouldn't normally happen -- partition_create()/
     * partition_init() always create one -- but handled rather than
     * silently dropped, e.g. for a partition restored from a pre-Phase-2
     * persisted snapshot that predates this table). */
    for (int i = 0; i < PARTITION_MAX; i++) {
        if (!partition_owner_table[i].active) {
            partition_owner_table[i].partition_id = partition_id;
            partition_owner_table[i].node_id      = node_id;
            partition_owner_table[i].active       = 1;
            kernel_serial_printf("[PARTITION] partition %u ownership row created, set to node %u.\n",
                                 (unsigned)partition_id, (unsigned)node_id);
            persist_partitions();
            return 0;
        }
    }
    kernel_serial_print("[PARTITION] ERROR: owner table full.\n");
    return 1;
}

int partition_is_local(uint32_t partition_id) {
    return partition_get_owner_node(partition_id) == cluster_local_node_id();
}

// ─── Phase 14 (LPAR): partition lifecycle ──────────────────────────────────
int partition_destroy(uint32_t partition_id) {
    if (partition_id == PARTITION_SYSTEM) {
        kernel_serial_print("[PARTITION] ERROR: PARTITION_SYSTEM can never be destroyed.\n");
        return 1;
    }
    if (!partition_id_valid(partition_id)) {
        kernel_serial_printf(
            "[PARTITION] ERROR: cannot destroy -- partition id %u is not an "
            "active, defined partition.\n", (unsigned)partition_id);
        return 1;
    }

    // Step 1: kill every process in this partition (process.c, reuses
    // process_kill() per-pid).
    uint32_t killed = process_kill_partition(partition_id);

    // Step 2: vfree every catalog object in this partition
    // (object_catalog.c, mirrors sys_sls_vfree()'s own per-entry actions).
    uint32_t freed_objects = catalog_vfree_partition(partition_id);

    // Step 3 (Multi-Node Partition Scaling Roadmap Phase 3): really reclaim
    // this partition's physical frames, not just reset the usage counter.
    // frame_pool.c now tracks a real per-frame owner tag, populated at both
    // allocate_physical_ram_frame_for_partition() call sites (process.c's
    // three, loader.c's two -- the ones LPAR Phase 13 already made
    // partition-aware at allocation time), so partition_reclaim_all_
    // frames() can walk every frame this partition actually holds and free
    // each one for real -- closing the gap LPAR Phase 14's own findings
    // named here (see the LPAR roadmap's §8 addendum) without needing the
    // separate, still-unsafe per-process page-table walker also named
    // there: this never touches a process's page table, only frame_pool.c's
    // own ownership tracking. See frame_pool.h's own comment on
    // partition_reclaim_all_frames() for the full design writeup, including
    // the frames this still does NOT reclaim (unaccounted/kernel-
    // infrastructure allocations, permanently attributed to
    // PARTITION_SYSTEM, which can never itself be destroyed).
    uint32_t frames_reclaimed = partition_reclaim_all_frames(partition_id);

    // Step 4: clear every uid assignment pointing at this partition. Those
    // uids fall back to PARTITION_DEFAULT automatically the next time
    // partition_get_for_uid() is called -- no assignment row means
    // "unassigned," exactly the same fallback every never-assigned uid
    // already gets.
    uint32_t cleared_assignments = 0;
    for (int i = 0; i < PARTITION_ASSIGN_MAX; i++) {
        if (partition_assign_table[i].active &&
            partition_assign_table[i].partition_id == partition_id) {
            partition_assign_table[i].active = 0;
            cleared_assignments++;
        }
    }

    // Step 5: deactivate the table entry itself, freeing the slot for a
    // future partition_create(), and clear any stale pause flag so a
    // reused slot doesn't inherit "paused" from a previous partition that
    // happened to occupy the same id. Also clears the Phase 2 (Multi-Node)
    // ownership row for the same reason -- a reused partition id must not
    // inherit a stale owner node from whatever partition previously held
    // that slot; partition_create() stamps a fresh one when the slot is
    // reused.
    partition_table[partition_id].active = 0;
    partition_paused[partition_id] = 0;
    for (int i = 0; i < PARTITION_MAX; i++) {
        if (partition_owner_table[i].active && partition_owner_table[i].partition_id == partition_id) {
            partition_owner_table[i].active = 0;
            break;
        }
    }

    kernel_serial_printf(
        "[PARTITION] destroyed partition %u: %u process(es) killed, "
        "%u object(s) freed, %u uid assignment(s) cleared, %u physical "
        "frame(s) reclaimed.\n",
        (unsigned)partition_id, (unsigned)killed, (unsigned)freed_objects,
        (unsigned)cleared_assignments, (unsigned)frames_reclaimed);

    persist_partitions();   // Phase 10 -- partition_table[]/partition_assign_table[] mutated
    return 0;
}

int partition_pause(uint32_t partition_id) {
    if (!partition_id_valid(partition_id)) {
        kernel_serial_printf(
            "[PARTITION] ERROR: cannot pause -- partition id %u is not an "
            "active, defined partition.\n", (unsigned)partition_id);
        return 1;
    }
    partition_paused[partition_id] = 1;
    kernel_serial_printf(
        "[PARTITION] partition %u paused -- excluded from scheduling "
        "rotation until resumed.\n", (unsigned)partition_id);
    return 0;
}

int partition_resume(uint32_t partition_id) {
    if (!partition_id_valid(partition_id)) {
        kernel_serial_printf(
            "[PARTITION] ERROR: cannot resume -- partition id %u is not an "
            "active, defined partition.\n", (unsigned)partition_id);
        return 1;
    }
    partition_paused[partition_id] = 0;
    kernel_serial_printf("[PARTITION] partition %u resumed.\n", (unsigned)partition_id);
    return 0;
}

int partition_is_paused(uint32_t partition_id) {
    if (partition_id >= PARTITION_MAX) return 0;
    return partition_paused[partition_id];
}

// ─── Multi-Node Partition Scaling Roadmap, Phase 6: cold partition migration ──
int partition_migrate(uint32_t partition_id, uint32_t dest_node_id) {
    if (partition_id == PARTITION_SYSTEM) {
        kernel_serial_print("[PARTITION] ERROR: PARTITION_SYSTEM can never be migrated.\n");
        return 1;
    }
    if (!partition_id_valid(partition_id)) {
        kernel_serial_printf(
            "[PARTITION] ERROR: cannot migrate -- partition id %u is not an "
            "active, defined partition.\n", (unsigned)partition_id);
        return 1;
    }
    if (dest_node_id == 0) {
        // 0 is the reserved "uninitialized" sentinel (Phase 1) -- migrating
        // TO it is almost certainly a caller bug, not a real destination
        // identity, the same rigor PARTITION_SYSTEM's own guard above gets.
        kernel_serial_print(
            "[PARTITION] ERROR: cannot migrate to node 0 -- that is the "
            "reserved 'uninitialized' sentinel (Phase 1), not a real "
            "destination node.\n");
        return 1;
    }
    uint32_t source_node_id = partition_get_owner_node(partition_id);
    if (dest_node_id == source_node_id) {
        kernel_serial_printf(
            "[PARTITION] ERROR: partition %u is already owned by node %u -- "
            "nothing to migrate.\n", (unsigned)partition_id, (unsigned)dest_node_id);
        return 1;
    }

    // Step 1 (LPAR Phase 14): pause -- excludes the partition from scheduling
    // for the duration of the move. Reuses partition_pause() directly rather
    // than re-deriving the same runtime flag a second way.
    partition_pause(partition_id);

    // Step 2 (Multi-Node Phase 4/6): relinquish this node's write-lease claim
    // for partition_id, if any was ever held. Deliberately voluntary and
    // local-only -- see net/consensus.h's own comment on partition_lease_
    // step_down() for why this doesn't transmit a handoff message (no RX
    // dispatcher exists anywhere in this codebase to receive one).
    int lease_existed = (partition_lease_step_down(partition_id) == 0);

    // Step 3 (Multi-Node Phase 5): net/dspp.c's dspp_page_read_allowed()/
    // _write_allowed() are per-packet GATING checks ("should this request be
    // serviced right now"), not an object-catalog reassignment mechanism --
    // there is no function anywhere in net/dspp.c that moves or retags a
    // catalog object's partition_id, because no object-to-physical-frame
    // resolution plumbing exists to move rowstore/vecstore table pages yet
    // (that is Storage Isolation Roadmap Phase 1's job -- per-partition page
    // indexing, not yet built -- a real prerequisite dependency, not an
    // oversight here). A partition's catalog objects (kernel/object_catalog.c)
    // therefore still keep their existing object_id/partition_id completely
    // unchanged by a migrate() call -- Step 4 below, the ownership-table
    // update, is what makes dspp_page_read_allowed()/_write_allowed() on the
    // NEW owner node start returning true.
    //
    // What DOES move now (Multi-Node Phase 6 addendum -- real migration data
    // movement, Multitenant Isolation Gap Analysis §7 item 7): this
    // partition's stream/blob storage. Scoped to streams only for the
    // reason above; rowstore/vecstore table data still only moves in the
    // ownership-record sense described above until Storage Isolation Phase
    // 1 lands.
    //
    // Phase 7 addendum (real cross-node data movement): which primitive
    // actually runs depends on whether a real cluster is configured.
    // cluster_local_node_id() == 0 is Phase 1's own "uninitialized"
    // sentinel -- cluster_init() is never called from any real boot path
    // today (every existing deployment and all 58+ pre-Phase-7 host tests),
    // so this is the default, common case, and it keeps stream_relocate_
    // partition()'s exact prior behavior completely unchanged: a same-disk
    // relocate to a fresh slot, since there is genuinely nowhere else to
    // send the bytes without a real cluster. Once cluster_init() HAS been
    // called with a real node id, stream_migrate_send_partition() takes
    // over instead -- the real DSPP-wire push to dest_node_id's own storage
    // (net/dspp.c/kernel/stream.c, this phase's new code). Both return the
    // identical "count of slots moved" convention, so the logging and
    // return-value handling below needs no branch of its own.
    int streams_relocated = (cluster_local_node_id() != 0)
        ? stream_migrate_send_partition(partition_id, dest_node_id)
        : stream_relocate_partition(partition_id, dest_node_id);

    // Step 4 (Multi-Node Phase 2): the actual, load-bearing ownership
    // handoff -- a pure table write that already persists internally
    // (persist_partitions(), Phase 10). Frames are deliberately NOT
    // reclaimed unless this succeeds: reclaiming this node's physical
    // frames before ownership has genuinely moved would free memory a
    // partition that STILL thinks it's locally owned here might still be
    // using -- the same "don't reclaim until the state that justifies it is
    // real" discipline partition_destroy()'s own step ordering follows.
    if (partition_set_owner_node(partition_id, dest_node_id) != 0) {
        kernel_serial_printf(
            "[PARTITION] ERROR: migration of partition %u aborted -- "
            "ownership reassignment failed. Partition remains paused; "
            "frames were NOT reclaimed since ownership never actually moved.\n",
            (unsigned)partition_id);
        return 1;
    }

    // Step 5 (Multi-Node Phase 3): only now, after ownership has genuinely
    // moved, is it correct to free this node's physical frames -- reuses
    // the same real reclamation partition_destroy() already established,
    // not a second implementation.
    uint32_t frames_reclaimed = partition_reclaim_all_frames(partition_id);

    // Step 6 ("resume on the destination"), deliberately NOT done here: this
    // function runs entirely on the SOURCE node. Calling partition_resume()
    // at this point would resume the partition on the node that no longer
    // owns it -- partition_is_local(partition_id) is now false here (see
    // Step 4 above). Real resumption on the destination node would require
    // THAT node to itself notice the ownership change and act. Phase 7
    // (real cross-node data movement) DID add a genuine, live DSPP RX
    // dispatcher (net/net.c's ETHERTYPE_DSPP branch, net/dspp.c's
    // dspp_rx_dispatch()) -- Phase 5's own "no RX dispatcher exists at all"
    // finding no longer holds unconditionally -- but no opcode or handler
    // for "a partition's ownership changed, notice and resume it" exists in
    // that new protocol; only stream-data migration (DSPP_MIGRATE_*) and
    // consensus/lease traffic are wired up. Resuming on the destination
    // remains a real, separate, not-yet-built piece of work, honestly
    // named as such rather than conflated with "no transport exists yet"
    // now that one genuinely does for a different purpose. The partition is
    // intentionally left PAUSED when this function returns.
    kernel_serial_printf(
        "[PARTITION] migrated partition %u: node %u -> node %u. Lease "
        "relinquished=%s, %d stream(s) relocated/sent and byte-verified, %u "
        "physical frame(s) reclaimed. Partition remains PAUSED -- resume "
        "must happen on the destination node.\n",
        (unsigned)partition_id, (unsigned)source_node_id, (unsigned)dest_node_id,
        lease_existed ? "yes" : "no (none was held)",
        streams_relocated,
        (unsigned)frames_reclaimed);

    return 0;
}

uint64_t sys_sls_partition_migrate(struct SLSPartitionMigrateRequest* req) {
    if (!req) return 1;
    return (uint64_t)partition_migrate(req->partition_id, req->dest_node_id);
}

uint64_t sys_sls_partition_destroy(uint32_t partition_id) {
    return (uint64_t)partition_destroy(partition_id);
}
uint64_t sys_sls_partition_pause(uint32_t partition_id) {
    return (uint64_t)partition_pause(partition_id);
}
uint64_t sys_sls_partition_resume(uint32_t partition_id) {
    return (uint64_t)partition_resume(partition_id);
}

uint64_t sys_sls_partition_create(struct SLSPartitionCreateRequest* req) {
    if (!req) return 0xFFFFFFFFu;
    return (uint64_t)partition_create(req->name);
}

uint64_t sys_sls_partition_assign(struct SLSPartitionAssignRequest* req) {
    if (!req) return 1;
    return (uint64_t)partition_assign_uid(req->uid, req->partition_id);
}

void sys_sls_partition_list(void) {
    kernel_serial_print("\n[PARTITION] Defined partitions:\n");
    int shown = 0;
    for (int i = 0; i < PARTITION_MAX; i++) {
        if (!partition_table[i].active) continue;
        kernel_serial_printf("  id=%-3u  name=%s\n",
                             (unsigned)partition_table[i].partition_id,
                             partition_table[i].name);
        shown++;
    }
    kernel_serial_printf(" %u partition(s) total.\n\n", (unsigned)shown);

    kernel_serial_print("[PARTITION] UID assignments:\n");
    int nassign = 0;
    for (int i = 0; i < PARTITION_ASSIGN_MAX; i++) {
        if (!partition_assign_table[i].active) continue;
        kernel_serial_printf("  uid=%-6u -> partition %u\n",
                             (unsigned)partition_assign_table[i].uid,
                             (unsigned)partition_assign_table[i].partition_id);
        nassign++;
    }
    kernel_serial_printf(" %u assignment(s), all other uids -> partition %u (default/system).\n\n",
                         (unsigned)nassign, (unsigned)PARTITION_DEFAULT);
}
