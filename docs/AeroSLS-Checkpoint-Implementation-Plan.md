# AeroSLS Checkpoint Implementation Plan

Based on the 5 Next Steps from `AeroSLS-Core-Backup-Strategies.md`, mapped against existing codebase.

---

## Status Summary

| # | Step | Status | Existing Foundation |
|---|------|--------|---------------------|
| 1 | Basic IPC interface for checkpoint service | **Done** | `kernel/checkpoint_mgr.c/h`, IPC port 0x1007, REST + shell |
| 2 | State tree manager (serialize/deserialize) | **Done** | `kernel/state_tree.c/h`, 27-test host suite passes |
| 3 | Test with 2-node cluster | **Done** | `net/dspp_checkpoint.c`, `tests/checkpoint_cluster_host_test.c` (21 tests pass) |
| 4 | Incremental checkpointing | **Done** | `kernel/checkpoint_delta.c/h`, dirty tracking in persist.c, 25-test suite |
| 5 | Recovery scenarios (node failure, partition) | **Done** | `kernel/failover.c/h`, 15-test suite (heartbeat + failover + partition adoption) |

---

## Step 1: Checkpoint IPC Interface

**Goal:** Register a dedicated IPC port for checkpoint coordination, allowing the kernel's existing persistence machinery to be triggered on demand and reported on.

**What exists:**
- `kernel/ipc.h` defines 6 ports (`0x1001`–`0x1006`) with per-port circular queues
- `kernel/persist.c` already serializes all kernel state arrays to NVMe
- `kernel/simi_ckpt.c` handles per-context checkpoint/restore (a pure serializer)

**What to build:**
1. Add `IPC_PORT_CKPTMGR 0x1007` to `kernel/ipc.h`
2. Define opcodes:
   - `CKPT_OP_TRIGGER 0x0701` — request a full checkpoint
   - `CKPT_OP_STATUS  0x0702` — query last checkpoint time/seq/size
   - `CKPT_OP_LIST    0x0703` — enumerate available checkpoints
   - `CKPT_OP_RESTORE 0x0704` — initiate restore from a given sequence number
3. Implement handler in a new `kernel/checkpoint_mgr.c` that:
   - Calls `persist_catalog()`, `persist_records()`, `persist_partitions()`, etc. as an atomic batch
   - Records a checkpoint header (magic, sequence, timestamp, object count) to a dedicated NVMe LBA
   - Returns status via the IPC reply_token mechanism
4. Wire into `microkernel_service_poll()` so the port is serviced on the BSP/AP tick

**Files to create:**
- `kernel/checkpoint_mgr.c`
- `kernel/checkpoint_mgr.h`

**Files to modify:**
- `kernel/ipc.h` — add port + opcodes
- `kernel/ipc.c` — add queue slot
- `kernel/microkernel.c` — dispatch in service poll
- `Makefile` — add `kernel/checkpoint_mgr.x86.o`

**Depends on:** Nothing new — all primitives exist.

---

## Step 2: State Tree Manager

**Goal:** Build a unified, hierarchical snapshot of all kernel state — objects, pages, processes, connections — suitable for incremental diffing and cross-node replication.

**What exists:**
- `kernel/persist.c` writes flat arrays (`object_catalog[]`, `partition_table[]`, `rowstore_tables[]`, etc.) directly to NVMe LBA regions
- `kernel/object_catalog.c` enumerates all objects with type/size/partition_id
- `kernel/process.c` tracks processes with state/stack/heap
- `kernel/frame_pool.c` tracks per-frame ownership and usage
- `net/dspp.c` tracks active connections

**What to build:**
1. Define a `state_tree_node_t` struct (id, type, parent_id, data_offset, data_size, dirty flag)
2. Define node types: `NODE_ROOT`, `NODE_PARTITION`, `NODE_OBJECT`, `NODE_PROCESS`, `NODE_PAGE`, `NODE_CONNECTION`
3. Implement `state_tree_build()`:
   - Root node
   - Children = partitions (from `partition_table[]`)
   - Per-partition children = objects (from `object_catalog[]` filtered by `partition_id`)
   - Leaf nodes = frames (from `frame_pool` per object_id)
4. Implement `state_tree_serialize(buf, max_size)` → flat buffer
5. Implement `state_tree_diff(old, new)` → list of dirty nodes (foundation for Step 4)
6. Implement `state_tree_deserialize(buf, size)` → restore kernel arrays

**Files to create:**
- `kernel/state_tree.c`
- `kernel/state_tree.h`

**Files to modify:**
- `kernel/checkpoint_mgr.c` — use `state_tree_build()` + `state_tree_serialize()` in the checkpoint flow
- `Makefile` — add object

**Key design decisions:**
- Tree is built in a static buffer (no malloc in this kernel), sized for `PARTITION_MAX * MAX_OBJECTS` worst case
- Serialization format: packed header + node array + data blobs (same freestanding discipline as `simi_ckpt.c`)
- Program immutability hash (same pattern as `simi_ckpt.h`'s `program_hash`) for integrity verification

---

## Step 3: 2-Node Cluster Checkpoint Test

**Goal:** Prove that a checkpoint taken on node 1 can be replicated to node 2 and restored there, using a memory-heavy workload.

**What exists:**
- `run-cluster.sh` boots N nodes with per-node ISOs, dual NICs (mgmt + cluster)
- `tests/cross_node_migration_host_test.c` uses the capture-and-replay technique (real DSPP framing, two simulated nodes)
- `tests/simi_ctx_migrate_host_test.c` proves context migration at every instruction boundary
- DSPP already defines `DSPP_MIGRATE_MAGIC` with opcodes for BEGIN/PAGE/ACK

**What to build:**
1. **Host test** (`tests/checkpoint_cluster_host_test.c`):
   - Same two-simulated-node technique as `cross_node_migration_host_test.c`
   - Populate node 1: create partition, 10+ objects (streams, DB tables), fill with data
   - Trigger checkpoint via IPC (Step 1's `CKPT_OP_TRIGGER`)
   - Serialize state tree (Step 2)
   - Transmit via captured DSPP frames (new opcode `DSPP_CHECKPOINT_MAGIC`)
   - On node 2: receive, deserialize, restore
   - Verify: all objects match, data integrity via checksums
2. **DSPP checkpoint protocol** (extension to `net/dspp.c`):
   - New magic/opcode family for checkpoint transfer (BEGIN, CHUNK, ACK, COMPLETE)
   - Chunked transfer reusing the same pattern as `simi_ctx_migrate.c` (one transfer at a time, reassembly buffer)
3. **Integration test script** (`tests/run_checkpoint_cluster_test.sh`):
   - Boot 2-node cluster via `run-cluster.sh --nodes 2`
   - REST API calls to create objects on node 1
   - Trigger checkpoint, verify replication via node 2's REST API

**Files to create:**
- `tests/checkpoint_cluster_host_test.c`
- `tests/run_checkpoint_cluster_test.sh` (optional, for live cluster)

**Files to modify:**
- `net/dspp.c` / `net/dspp.h` — add checkpoint transfer opcodes + handlers
- `Makefile` — test target

**Depends on:** Steps 1 + 2.

---

## Step 4: Incremental Checkpointing

**Goal:** Reduce NVMe write amplification and network bandwidth by only checkpointing state that changed since the last checkpoint.

**What exists:**
- `kernel/persist.c`: writes full arrays on every mutation (the doc explicitly acknowledges this trade-off)
- `kernel/frame_pool.c`: tracks per-frame `flags` field — could hold a DIRTY bit
- `kernel/simi_ckpt.c`: already uses a hash to detect program changes
- The doc's `AeroSLS-Persist-Write-Amplification-Scoping-v0.1.md` specifically scopes this problem

**What to build:**
1. **Dirty tracking** in the state tree:
   - Add `last_checkpoint_seq` to `state_tree_node_t`
   - `state_tree_diff(current_seq)` returns only nodes whose backing data changed since `last_checkpoint_seq`
   - On commit: stamp `last_checkpoint_seq = current_seq` on all nodes
2. **Per-object mutation flag** in `object_catalog`:
   - Set on any write to the object (stream append, DB insert, etc.)
   - Cleared after checkpoint includes it
   - Avoids re-serializing unchanged objects
3. **Checkpoint delta format**:
   - Header: `base_seq` (what this is relative to) + `delta_seq` (this checkpoint's seq)
   - Body: only the changed nodes + their data
   - Restore: apply deltas in sequence on top of a base snapshot
4. **Periodic full checkpoint**:
   - Every N incremental checkpoints, force a full (compaction)
   - Prevents unbounded delta chain

**Files to create:**
- `kernel/checkpoint_delta.c`
- `kernel/checkpoint_delta.h`

**Files to modify:**
- `kernel/state_tree.c` — add diff/dirty tracking
- `kernel/object_catalog.c` — set dirty flag on mutation
- `kernel/stream.c` — set dirty flag on append
- `kernel/rowstore.c` — set dirty flag on insert/update/delete
- `kernel/checkpoint_mgr.c` — choose full vs. incremental
- `Makefile` — add object

**Depends on:** Steps 1 + 2.

---

## Step 5: Recovery Scenarios

**Goal:** Handle node failure and network partition gracefully, leveraging checkpoints for automatic failover.

**What exists:**
- `backup/backup.sh` — image-level cold backup with retention (Phase 1 of the roadmap, fully built)
- `backup/restore.sh` — image restore with safety copy + health check + spot-check (fully built)
- `persist_restore_all()` — boots from NVMe snapshots (catalog, records, schemas, programs, partitions, rowstore, vecstore)
- `kernel/partition.c` `partition_migrate()` — moves a partition's data + ownership to another node
- `net/consensus.h` / consensus_phase1 — write-lease coordination (single-writer guarantee)
- `simi_ctx_migrate.c` — live context migration and resume

**What to build:**
1. **Node failure detection**:
   - Heartbeat mechanism: each node sends periodic DSPP heartbeat to all peers
   - Timeout detection: if heartbeat missed for N intervals, declare node dead
   - Add to `net/dspp.c` as a new lightweight opcode (`DSPP_HEARTBEAT`)
2. **Automatic failover**:
   - When node X is declared dead, the surviving node with the most recent checkpoint of X's partitions becomes the new owner
   - Execute `state_tree_deserialize()` from the replicated checkpoint
   - Update partition ownership (`partition_set_owner_node()`)
   - Resume serving requests for those partitions
3. **Network partition handling** (split-brain prevention):
   - Leverage existing write-lease mechanism (`net/consensus.h`)
   - A partitioned node that cannot renew its lease stops accepting writes (fences itself)
   - On reconnection: compare checkpoint sequences, the node with the higher seq wins
   - Loser replays from winner's checkpoint (same as failover restore)
4. **Recovery test** (`tests/checkpoint_recovery_host_test.c`):
   - Simulate node failure mid-checkpoint (truncated transfer)
   - Verify surviving node detects and recovers
   - Simulate network partition, verify fencing and reconciliation
5. **Graceful degradation**:
   - REST API returns 503 for partitions whose owner is unreachable
   - Partition auto-pauses when owner node is dead (until failover completes)

**Files to create:**
- `kernel/heartbeat.c` / `kernel/heartbeat.h`
- `kernel/failover.c` / `kernel/failover.h`
- `tests/checkpoint_recovery_host_test.c`

**Files to modify:**
- `net/dspp.c` / `net/dspp.h` — heartbeat opcode + timeout detection
- `kernel/partition.c` — auto-pause on owner-dead detection
- `kernel/secure_api.c` — 503 for unreachable partitions
- `kernel/checkpoint_mgr.c` — failover trigger
- `Makefile` — add objects

**Depends on:** Steps 1 + 2 + 3 + 4 (this is the capstone).

---

## Implementation Order

```
Step 1 ──────────────────────┐
  (IPC port + trigger)       │
                             ├──► Step 3 (2-node cluster test)
Step 2 ──────────────────────┘          │
  (State tree build/serialize)          │
                                        ├──► Step 5 (Recovery scenarios)
Step 4 ────────────────────────────────┘
  (Incremental/dirty tracking)
```

**Recommended sequence:** 1 → 2 → 3 → 4 → 5

Steps 1 and 2 can be developed in parallel (no dependency between them) and tested individually with host tests before combining in Step 3.

---

## What's Already Done (No Work Needed)

These capabilities from the doc's broader roadmap are **already implemented**:

- **Phase 1 (Image-Level Backup):** `backup/backup.sh` + `backup/restore.sh` — full image copy with pm2 stop/start, retention policy, health check, spot-check
- **SIMI Context Checkpointing:** `kernel/simi_ckpt.c` — pure serializer for execution contexts, exhaustively tested
- **Cross-Node Context Migration:** `kernel/simi_ctx_migrate.c` — live computation moves between nodes via DSPP
- **NVMe Persistence:** `kernel/persist.c` — all kernel arrays survive reboots via NVMe snapshots
- **Partition Migration:** `kernel/partition.c` `partition_migrate()` — moves ownership + stream data to another node
- **Cluster Networking:** `net/dspp.c` — real Ethernet-framed protocol with migration, page read/write, consensus
- **N-Node Launcher:** `run-cluster.sh` — boots arbitrary cluster sizes with per-node identity

---

## Estimated Complexity

| Step | New LoC (approx) | New Files | Risk |
|------|-------------------|-----------|------|
| 1 | ~200 | 2 | Low — all primitives exist |
| 2 | ~400 | 2 | Medium — tree design must cover all kernel arrays |
| 3 | ~500 | 1-2 | Medium — DSPP extension + test scaffolding |
| 4 | ~350 | 2 | Medium — dirty tracking touches multiple subsystems |
| 5 | ~600 | 4 | High — distributed consensus edge cases |
