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
#include "../net/dspp.h"         // Multi-Node Phase 2 -- partition-row replication over DSPP (announce/withdraw)
#include "stream.h"             // Multi-Node Phase 6 addendum -- stream_relocate_partition() (real migration data movement)
#include "simi_ctx_migrate.h"   // PEC Phase 3 -- simi_ctx_migrate_send_partition()
#include "service_registry.h"  // Orchestration Plan Phase 4 -- service_unregister_partition()

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
            /* Multi-Node Phase 2: tell the cluster. dspp_partition_announce()
             * is silent on an unclustered node (node id 0), so this stays a
             * no-op for every single-node deployment. */
            dspp_partition_announce((uint32_t)i, partition_table[i].name,
                                    partition_owner_table[i].node_id);
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

/* Public wrapper over the same check (Orchestration Plan Phase 4).
 * kernel/service_registry.c needs "is this an active, defined partition"
 * before accepting a registration; a thin wrapper keeps one source of
 * truth rather than a second copy of the rule that could drift. */
int partition_exists(uint32_t partition_id) { return partition_id_valid(partition_id); }

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
            /* Replicate the ownership change -- this is the one choke point
             * every handoff goes through: partition_migrate() (Phase 6) and
             * failover_recover_from()'s adoption both land here. */
            dspp_partition_announce(partition_id, partition_table[partition_id].name, node_id);
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
            dspp_partition_announce(partition_id, partition_table[partition_id].name, node_id);
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

    // Step 3b (Orchestration Plan Phase 4): drop this partition's service
    // registrations. A registration resolves name -> partition -> node, so
    // one left behind for a destroyed partition would keep resolving --
    // handing callers the owner node of a partition that no longer exists,
    // which is a confidently wrong answer rather than an honest "no such
    // service". Dropped here rather than left to expire because the
    // registry has no expiry: it is authoritative, not a cache.
    uint32_t services_dropped = service_unregister_partition(partition_id);
    (void)services_dropped;

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
    /* Multi-Node Phase 2: tell the cluster the row is gone, so followers
     * drop it instead of keeping a partition that no longer exists. */
    dspp_partition_withdraw(partition_id);
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
    /* ─── Remember whether it was already paused ──────────────────────────
     * Every failure path below used to return with the partition still paused.
     * On SUCCESS that is deliberate and documented -- the partition must be
     * resumed on the destination, not here. On FAILURE it is an availability
     * regression the operator never asked for: ownership was not transferred,
     * no frames were reclaimed, the data is untouched and the partition is
     * exactly where it started -- and yet the tenant is now out of the
     * scheduling rotation until somebody notices. The abort message even says
     * "the partition stays here, paused" as though that were the safe outcome,
     * and tells the operator to retry without telling them to resume.
     *
     * The pause is a MEANS to the migration, not a desired end state. So a
     * failed migration restores whatever the pause state was before it started
     * -- restores, not resumes: a partition an operator had deliberately paused
     * must stay paused, or the failure would silently un-pause it. */
    int was_paused_before_migrate = partition_is_paused(partition_id);
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
    /* ─── How many streams SHOULD move, counted before anything moves ──────
     * stream_migrate_send_partition() returns only what it managed to send,
     * and 0 is both the right answer for an empty partition and the symptom
     * of a destination refusing everything. Without the expected count those
     * two are indistinguishable -- which is exactly how a migration that
     * transferred no data went on to hand over ownership and report OK.
     *
     * Observed on a live cluster: page 0 was refused, the source slot was
     * correctly left intact, and then this function moved ownership to the
     * destination and reclaimed the source's frames anyway. The data was on
     * one node and the ownership record pointed at another. */
    int streams_expected = stream_count_for_partition(partition_id);

    int streams_relocated = (cluster_local_node_id() != 0)
        ? stream_migrate_send_partition(partition_id, dest_node_id)
        : stream_relocate_partition(partition_id, dest_node_id);

    /* ─── Abort before the ownership handoff if the data did not follow ─────
     * Ownership is what makes the destination authoritative. Handing it over
     * while the bytes are still here produces a cluster that disagrees with
     * itself, and the operator finds out by reading a stream that is not
     * where its partition says it should be.
     *
     * Returning here leaves the partition PAUSED and still owned locally,
     * with its data intact. That is a recoverable state: fix whatever the
     * destination was complaining about and run the migration again. The
     * alternative -- proceeding -- is not recoverable by retrying, because
     * the second attempt would find the partition already owned elsewhere. */
    if (streams_relocated < streams_expected) {
        kernel_serial_printf(
            "[PARTITION] ERROR: migrate ABORTED for partition %u -- %d of %d stream(s) "
            "were confirmed by node %u. Ownership NOT transferred, no frames "
            "reclaimed, data intact, and the partition's pause state restored to "
            "what it was before the attempt -- nothing changed. Check node %u's "
            "log for the refusal reason, then retry.\n",
            (unsigned)partition_id, streams_relocated, streams_expected,
            (unsigned)dest_node_id, (unsigned)dest_node_id);
        if (!was_paused_before_migrate) partition_resume(partition_id);
        return 1;
    }

    // Step 3b (Persistent Execution Contexts, Phase 3): move this
    // partition's RUNNING computations, not just its data at rest. A
    // checkpointed SIMI context is pushed over the same DSPP transport
    // and resumes on dest_node_id at the exact instruction it stopped at
    // (kernel/simi_ctx_migrate.c).
    //
    // Same cluster_local_node_id() != 0 condition as the stream move
    // above, and for the same reason: without a real cluster identity
    // there is nowhere to send anything. Unlike streams there is no
    // same-disk fallback -- relocating a context to a different slot on
    // the same node is not a migration of anything, so the else branch is
    // genuinely nothing rather than a busy no-op.
    //
    // HONEST SCOPE: the registry this iterates is real, but nothing in
    // this kernel registers contexts into it yet -- no scheduler or
    // service runtime owns long-lived contexts (see
    // kernel/simi_ctx_migrate.h). So this moves zero contexts on every
    // current boot. The transport, checkpointing and resume path are all
    // proven end-to-end by tests/simi_ctx_migrate_host_test.c; what is
    // missing is a producer of live contexts, which is named work rather
    // than an oversight here. Wired now so the capability is reachable
    // from the system the moment that producer exists.
    uint32_t contexts_migrated = (cluster_local_node_id() != 0)
        ? simi_ctx_migrate_send_partition(partition_id, dest_node_id)
        : 0;

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
            "ownership reassignment failed. Frames were NOT reclaimed since "
            "ownership never actually moved, and the pause state is restored to "
            "what it was before the attempt.\n",
            (unsigned)partition_id);
        if (!was_paused_before_migrate) partition_resume(partition_id);
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
    /* "byte-verified" was unconditional, and a stream with no pages verifies
     * no bytes -- so migrating an empty stream reported byte verification
     * that never happened. Say "confirmed by the destination", which is true
     * in both cases: kernel/stream.c waits for a BEGIN_ACK even when there
     * are no pages, and per-page ACKs plus a readback comparison when there
     * are. The per-stream line from stream.c states which of the two applied. */
    kernel_serial_printf(
        "[PARTITION] migrated partition %u: node %u -> node %u. Lease "
        "relinquished=%s, %d stream(s) sent and confirmed by the destination, %u "
        "live context(s) checkpointed and sent, %u physical frame(s) "
        "reclaimed. Partition remains PAUSED -- resume must happen on the "
        "destination node.\n",
        (unsigned)partition_id, (unsigned)source_node_id, (unsigned)dest_node_id,
        lease_existed ? "yes" : "no (none was held)",
        streams_relocated,
        (unsigned)contexts_migrated,
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
    /* ─── The owner node is shown, and used not to be ─────────────────────
     * partition_migrate() refuses when the destination already owns the
     * partition, and only the owning node holds the data to send. So the
     * owner is the single field that decides whether a migration can
     * proceed -- and it had no read surface anywhere: not here, not in
     * /api/partitions (quotas only), not in /api/nodes (a count, not a
     * mapping). The only way to learn it was to attempt a migration and
     * read the error, which cost real operator time more than once.
     *
     * "this node" is marked explicitly rather than left to be inferred from
     * a bare id, because that comparison is exactly what a reader gets
     * wrong when several nodes are open in different terminals. */
    uint32_t me = cluster_local_node_id();
    kernel_serial_print("\n[PARTITION] Defined partitions:\n");
    int shown = 0;
    for (int i = 0; i < PARTITION_MAX; i++) {
        if (!partition_table[i].active) continue;
        uint32_t owner = partition_get_owner_node(partition_table[i].partition_id);
        kernel_serial_printf("  id=%-3u  owner=node %-3u%s  name=%s\n",
                             (unsigned)partition_table[i].partition_id,
                             (unsigned)owner,
                             owner == me ? " (this node)" : "           ",
                             partition_table[i].name);
        shown++;
    }
    kernel_serial_printf(" %u partition(s) total. This node is node %u.\n\n",
                         (unsigned)shown, (unsigned)me);

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

// ─── Multi-Node Partition Scaling Roadmap, Phase 2: replication RX ───────────
// The apply half of partition-table replication. Called from
// dspp_partition_rx() (net/dspp.c) on the wire path -- the timer ISR -- so
// neither function persists DIRECTLY and neither calls anything that blocks
// or does I/O: nvme_write_sync() spins on a doorbell/completion and cannot
// run in interrupt context. Persistence is deferred instead -- a successful
// apply only sets the partition_persist_dirty flag (a volatile byte; safe
// to write from the ISR), and the BSP sweep calls partition_persist_flush()
// on its next iteration, in the same non-ISR context partition_create()/
// partition_destroy() already persist from. A rebooted node therefore keeps
// the cluster view it learned instead of re-converging from zero.
//
// The deferred design is the one that survives the ISR constraint:
//   - Persist directly in the RX path -- impossible: the write blocks on
//     NVMe completion and would re-enter the interrupt machinery it runs in.
//   - Persist via checkpoint_mgr's dirty-region bitmap -- possible, but the
//     checkpoint is IPC-triggered and writes every dirty region, so one
//     partition announce would cost a catalog-wide snapshot write.
//   - This flag + sweep flush -- one volatile byte set in the ISR, cleared
//     and written out by the BSP sweep, coalescing N announces into one
//     write. Exposures, weighed and accepted: a row mutated by an ISR apply
//     mid-flush can land torn in the snapshot (header magic/size guards
//     protect the snapshot's structure, a torn single row self-heals on the
//     next announce, and the flag re-set by that apply forces a rewrite);
//     and a node DOWN during a destroy keeps the destroyed row on disk
//     after reboot, because withdraws are mutation-only and nothing ever
//     re-announces a row's absence -- the known eventual-consistency debt a
//     future periodic full-owned-set reconciliation would close.
//
// Both sides now share the claim-class resolver in the sync paths below
// (and in service_remote_learn()): "last announce wins" no longer holds
// anywhere -- a resurrected owner's stale re-announce must not flap a
// settled partition ownership or a settled service registration. A
// withdraw only applies if the source owned the row. (The service registry
// persists its LOCAL registrations -- persist_services, restored at boot,
// which is exactly why a resurrected owner can re-announce at all -- and
// only its remote cache is runtime-only; this persistence is for the
// partition table, whose rows are the cluster view a node must keep across
// a reboot.)
static volatile uint8_t partition_persist_dirty = 0;
void partition_sync_upsert(uint32_t partition_id, uint32_t owner_node_id,
                           const char* name, uint32_t source_node_id) {
    if (partition_id == PARTITION_SYSTEM || partition_id >= PARTITION_MAX) return;
    if (!name || !name[0] || owner_node_id == 0 || source_node_id == 0) return;

    int had = partition_exists(partition_id);
    uint32_t cur = had ? partition_get_owner_node(partition_id) : 0;
    if (had && cur != 0 && cur != owner_node_id) {
        /* Two nodes claiming one partition id. The old rule -- "last
         * announce wins", mirroring the duplicate-service-name rule --
         * was safe for the case that invented it (two concurrent creators
         * colliding on a local slot id) but FATAL for failover: a
         * resurrected owner restores its stale row from disk at boot and
         * re-announces it, and "last wins" hands the adopted partition
         * back to the node that lost it -- flapping the ownership the
         * adoption just settled. Resolve by claim class instead, still
         * logged loudly (any conflict is operator-visible):
         *   * source == current owner  -- an owner-initiated transfer
         *     (partition_migrate()): the owner is handing the row off,
         *     apply it.
         *   * source == owner == the cluster leader -- the leader's own
         *     claim (failover adoption via partition_set_owner_node, or a
         *     leader re-assert): the leader's view is authoritative,
         *     apply it.
         *   * anything else -- a non-owner, non-leader self-claim
         *     colliding with a live owner: the resurrected-owner shape.
         *     KEEP the live owner and reject the stale claim, so an
         *     adopted partition can never flap back to the node that
         *     lost it. The claimant converges when the leader's own
         *     periodic re-announce reaches it.
         * Same rule as a duplicate service name: this stays an operator
         * error (ids are local slot numbers, so two creators can
         * collide), but it must resolve deterministically in the
         * failover-safe direction, not by arrival order. */
        if (source_node_id == cur) {
            kernel_serial_printf(
                "[PARTITION] sync: partition %u claimed by node %u and node %u -- "
                "owner-initiated transfer, applying.\n",
                (unsigned)partition_id, (unsigned)cur, (unsigned)owner_node_id);
        } else if (owner_node_id == source_node_id &&
                   cluster_leader_id() == source_node_id) {
            kernel_serial_printf(
                "[PARTITION] sync: partition %u claimed by node %u and node %u -- "
                "leader node %u's claim wins, applying.\n",
                (unsigned)partition_id, (unsigned)cur, (unsigned)owner_node_id,
                (unsigned)source_node_id);
        } else {
            kernel_serial_printf(
                "[PARTITION] sync: partition %u claimed by node %u and node %u -- "
                "keeping node %u (live owner), rejecting the stale claim.\n",
                (unsigned)partition_id, (unsigned)cur, (unsigned)owner_node_id,
                (unsigned)cur);
            return;
        }
    }

    if (!had) {
        partition_table[partition_id].partition_id = partition_id;
        partition_table[partition_id].active = 1;
    }
    pt_strcpy(partition_table[partition_id].name, name, PARTITION_NAME_LEN);

    /* Owner row written directly rather than via partition_set_owner_node():
     * that function persists, and this path must not (RX ISR). */
    int found = 0;
    for (int i = 0; i < PARTITION_MAX; i++) {
        if (partition_owner_table[i].active &&
            partition_owner_table[i].partition_id == partition_id) {
            partition_owner_table[i].node_id = owner_node_id;
            found = 1;
            break;
        }
    }
    if (!found) {
        for (int i = 0; i < PARTITION_MAX; i++) {
            if (!partition_owner_table[i].active) {
                partition_owner_table[i].partition_id = partition_id;
                partition_owner_table[i].node_id      = owner_node_id;
                partition_owner_table[i].active       = 1;
                break;
            }
        }
    }

    /* Applied: the row changed (or was created) in the local table, so a
     * reboot must keep it. The ISR cannot write NVMe, so the sweep's
     * partition_persist_flush() is what writes -- see the RX comment. */
    partition_persist_dirty = 1;
    kernel_serial_printf(
        "[PARTITION] sync: partition %u '%s' (owner node %u) learned from node %u.\n",
        (unsigned)partition_id, partition_table[partition_id].name,
        (unsigned)owner_node_id, (unsigned)source_node_id);
}

void partition_sync_withdraw(uint32_t partition_id, uint32_t source_node_id) {
    if (partition_id == PARTITION_SYSTEM || partition_id >= PARTITION_MAX) return;

    uint32_t cur = partition_get_owner_node(partition_id);
    if (cur != source_node_id) {
        kernel_serial_printf(
            "[PARTITION] sync: ignoring withdraw of partition %u from node %u "
            "(owner is node %u) -- a non-owner cannot delete another node's "
            "partition.\n",
            (unsigned)partition_id, (unsigned)source_node_id, (unsigned)cur);
        return;
    }

    partition_table[partition_id].active = 0;
    for (int i = 0; i < PARTITION_MAX; i++) {
        if (partition_owner_table[i].active &&
            partition_owner_table[i].partition_id == partition_id) {
            partition_owner_table[i].active = 0;
            break;
        }
    }
    /* The removal must also survive a reboot, or a node that restarts after
     * the withdraw would resurrect the row from the pre-withdraw snapshot. */
    partition_persist_dirty = 1;
    kernel_serial_printf(
        "[PARTITION] sync: partition %u withdrawn by node %u.\n",
        (unsigned)partition_id, (unsigned)source_node_id);
}

// ─── Deferred persistence of learned rows ──────────────────────────────────
// The sweep's call site sits in net/http.c next to partition_reannounce_tick().
// Clear-then-write: clearing first guarantees that any apply landing
// mid-write re-sets the flag and is covered by the next sweep, so no applied
// change is ever permanently missed -- the worst case is one redundant
// write. The clear is not atomic against the ISR's set, but every
// interleaving stays correct: an apply before the clear is in this write; an
// apply after the clear leaves the flag set for the next flush. NVMe writes
// are already coalesced by the flag, so this is at most one extra write per
// sweep under announce load.
// ─── Owned-set reconciliation (closes the destroy-while-down staleness) ────
// Called from dspp_partition_ownedset_rx() (net/dspp.c) on the RX path --
// the timer ISR -- so like the sync functions it only marks the dirty flag
// and never blocks or does I/O. The payload is the source's COMPLETE owned
// partition-id set; any row in OUR table whose owner is the source but
// whose id is NOT in the set is garbage: either it was destroyed while we
// were down (the durable-staleness case learned-row persistence created --
// withdraws are mutation-only, so nothing else ever re-announces a row's
// absence) or the source never owned it in the first place. Rows owned by
// other nodes are left for THEIR sets; the source's set is the only
// authority on what the source owns.
//
// Ordering: correctness relies on per-source FIFO delivery (single shared
// segment, per-source ring RX), so a set is never processed against rows
// created by a LATER announce the set predates. The generation counter
// catches duplicates (same generation twice) but deliberately ACCEPTS a
// lower one -- the only way a source's counter goes backwards is a reboot,
// and the rebooted source's first set is fresh truth.
void partition_sync_ownedset(uint32_t source_node_id, uint32_t generation,
                             const uint32_t* ids, uint32_t count) {
    if (source_node_id == 0) return;
    if (source_node_id > CLUSTER_NODE_MAX) return;
    if (count > 0 && !ids) return;   /* count==0 with NULL ids is a legitimate EMPTY set */

    static uint32_t partition_ownedset_seen[CLUSTER_NODE_MAX + 1];
    uint32_t* seen = &partition_ownedset_seen[source_node_id];
    if (generation != 0 && generation == *seen) return;   /* duplicate set */
    *seen = generation;

    for (int i = 0; i < PARTITION_MAX; i++) {
        if (!partition_owner_table[i].active) continue;
        if (partition_owner_table[i].node_id != source_node_id) continue;
        uint32_t part_id = partition_owner_table[i].partition_id;
        if (part_id == PARTITION_SYSTEM || part_id >= PARTITION_MAX) continue;
        if (!partition_table[part_id].active) continue;

        int in_set = 0;
        for (uint32_t j = 0; j < count; j++) {
            if (ids[j] == part_id) { in_set = 1; break; }
        }
        if (in_set) continue;

        /* Stale: the source no longer owns this partition. Remove the row
         * AND the owner row, and persist the removal -- an unpersisted GC
         * would resurrect the ghost at the very next reboot. */
        partition_table[part_id].active = 0;
        partition_owner_table[i].active = 0;
        partition_persist_dirty = 1;
        kernel_serial_printf(
            "[PARTITION] sync: partition %u (owner node %u) no longer owned -- "
            "collected from the owned-set.\n",
            (unsigned)part_id, (unsigned)source_node_id);
    }
}

void partition_persist_flush(void) {
    if (!partition_persist_dirty) return;
    partition_persist_dirty = 0;
    persist_partitions();
}

// ─── Multi-Node Partition Scaling Roadmap, Phase 2: periodic re-announce ────
// Announce-on-change converges the nodes that were up for the change, but a
// node that boots AFTER a create never hears it and waits forever for the
// next mutation. On a schedule, each node re-broadcasts the rows it OWNS
// (owner == local node id), so a late joiner converges within one period.
// Rows a node merely LEARNED (owner elsewhere) are not re-announced: the
// owner announces its own, and re-broadcasting someone else's row under
// this node's source id would break the withdraw rule (which requires the
// source to own the row). Rate-limited like the checkpoint broadcast --
// the sweep is load-dependent and unbounded, so the period is measured in
// kernel_tick_counter (nominal 100 Hz), not call count.
#define PARTITION_REANNOUNCE_TICKS 1000u  /* ~10 s at the nominal sweep rate */
/* Every N re-announce periods, each node ALSO broadcasts the full set of
 * partition ids it owns (DSPP_PARTITION_OWNEDSET). The per-row announce
 * converges creates; the owned-set reconciles DESTROYS -- a node that was
 * down for a withdraw keeps the destroyed row (learned rows persist now),
 * and since withdraws are mutation-only, the full set is the only thing
 * that ever tells listeners "this row no longer exists". ~100 s at the
 * nominal sweep rate: slow enough to be cheap (one frame per node), fast
 * enough that a rebooted node's stale rows are collected within a couple
 * of minutes. */
#define PARTITION_OWNEDSET_EVERY 10u

static uint64_t partition_last_reannounce_tick = 0;
static uint32_t partition_reannounce_periods  = 0;
static uint32_t partition_ownedset_generation = 0;
/* Static, not stack: up to PARTITION_MAX ids, past single-local budget. */
static uint32_t partition_ownedset_ids[PARTITION_MAX];

void partition_reannounce_tick(uint64_t now) {
    if (now - partition_last_reannounce_tick < PARTITION_REANNOUNCE_TICKS) return;
    partition_last_reannounce_tick = now;

    uint32_t me = cluster_local_node_id();
    if (me == 0) return;   /* unclustered: nothing to converge and nobody to tell */

    uint32_t owned_count = 0;
    for (int i = 0; i < PARTITION_MAX; i++) {
        if (!partition_owner_table[i].active) continue;
        uint32_t part_id = partition_owner_table[i].partition_id;
        if (part_id == PARTITION_SYSTEM || part_id >= PARTITION_MAX) continue;
        if (partition_owner_table[i].node_id != me) continue;
        if (!partition_table[part_id].active) continue;   /* owner row without a table row is not a real partition */
        dspp_partition_announce(part_id, partition_table[part_id].name, me);
        partition_ownedset_ids[owned_count++] = part_id;
    }

    partition_reannounce_periods++;
    if (partition_reannounce_periods < PARTITION_OWNEDSET_EVERY) return;
    partition_reannounce_periods = 0;
    partition_ownedset_generation++;
    dspp_partition_ownedset_send(partition_ownedset_generation,
                                 partition_ownedset_ids, owned_count);
}
