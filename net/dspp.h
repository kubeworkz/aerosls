/*
 * dspp.h — DSPP (Distributed Shared Page Protocol) wire format and, as of
 * Multi-Node Partition Scaling Roadmap Phase 5, real partition-routing
 * logic (net/dspp.c — this header previously had no corresponding .c at
 * all, just struct/enum definitions consumed directly by net/consensus.c
 * and net/prefetch.c).
 *
 * Phase 5 findings note this file had no include guard before this phase
 * — worked around rather than fixed by net/consensus.h's own comment
 * ("dspp.h has no include guard of its own, so pulling it in here would
 * double-define every struct in it for any .c file that... already
 * includes both headers separately"). Added here as a small, low-risk,
 * in-passing fix while touching this file for a real reason: a proper
 * guard makes any future double-#include (direct or transitive) a safe
 * no-op instead of a hard redefinition error. net/consensus.h's own
 * forward-declaration-only approach for `struct DSPPFullPagePacket` still
 * works fine either way and is left untouched — not this phase's scope to
 * revisit.
 */
#ifndef DSPP_H
#define DSPP_H

#include <stdint.h>

/* Multi-Node Partition Scaling Roadmap Phase 5: struct DSPPPacketHeader's
 * wire layout changed below (partition_id field added) -- the magic value
 * is bumped so a pre-Phase-5 receiver (if one ever exists) recognizes an
 * unfamiliar layout instead of silently misparsing shifted-by-4-bytes
 * garbage as system_object_id/virtual_address/etc. Same "a distinct magic
 * value signals an incompatible on-disk/on-wire layout" convention
 * kernel/persist.c's PERSIST_MAGIC_* constants already use for snapshot
 * version detection -- reused here, not invented fresh, for the identical
 * problem one layer over (wire instead of disk).
 *
 * DSPP_MAGIC_V1 is kept as a named constant (not just deleted) specifically
 * so any future real RX handler for these opcodes has something concrete
 * to compare an incoming header.magic against and reject cleanly as
 * "pre-Phase-5, no partition_id available" rather than silently trusting
 * whatever bytes happen to sit at the new field's offset. No code in this
 * codebase currently sends DSPP_MAGIC_V1 -- every packet-construction call
 * site (net/consensus.c, net/prefetch.c) is part of this same Phase 5
 * commit and uses the new DSPP_MAGIC value below. */
#define DSPP_MAGIC_V1 0x534c534e45544d41ULL // "SLSNETMA" -- pre-Phase-5 layout, no partition_id
#define DSPP_MAGIC    0x534c534e45544d42ULL // "SLSNETMB" -- Phase 5+: DSPPPacketHeader carries partition_id

enum DSPPOpcode {
    DSPP_PAGE_READ_REQ  = 1, // Request a page frame from a remote node's RAM
    DSPP_PAGE_READ_ACK  = 2, // Contains the requested 4KB data frame payload
    DSPP_PAGE_WRITE_REQ = 3, // Push a dirty page replication packet to a mirror backup node
    DSPP_PAGE_WRITE_ACK = 4  // Replication acknowledgment confirmation
};

/* Byte offsets/sizes below, spelled out explicitly per this phase's own
 * verification plan ("logic review of the packet size/offset math given
 * the packed-struct field addition") -- __attribute__((packed)) means no
 * compiler-inserted alignment padding either before or after the new
 * field, so these are exact, not just documentation. */
struct DSPPPacketHeader {
    uint64_t magic;             // offset  0, 8 bytes
    uint64_t system_object_id;  // offset  8, 8 bytes
    uint64_t virtual_address;   // offset 16, 8 bytes
    uint32_t transaction_id;    // offset 24, 4 bytes
    uint16_t opcode;            // offset 28, 2 bytes -- DSPPOpcode
    uint16_t node_source_id;    // offset 30, 2 bytes
    uint32_t partition_id;      // offset 32, 4 bytes -- Multi-Node Partition
                                 // Scaling Roadmap Phase 5: NEW. Which
                                 // partition system_object_id belongs to,
                                 // resolved from object_catalog[] at
                                 // packet-construction time
                                 // (dspp_resolve_partition_id(), net/
                                 // dspp.c) -- mirrors how catalog_check_
                                 // access() (kernel/object_catalog.c)
                                 // already resolves partition_id from
                                 // object_catalog[] rather than re-
                                 // deriving it some other way.
} __attribute__((packed));      // total: 36 bytes (was 32 bytes pre-Phase-5)

struct DSPPFullPagePacket {
    struct DSPPPacketHeader header;
    uint8_t                 payload_4kb[4096]; // The actual page contents
} __attribute__((packed));      // total: 4132 bytes (was 4128 bytes pre-Phase-5)

/* Multi-Node Partition Scaling Roadmap Phase 5: real DSPP page-routing
 * logic (net/dspp.c). See that file's own header comment for the honest
 * caveat this phase's findings addendum also names: no RX dispatcher for
 * ANY DSPP opcode exists anywhere in this codebase yet (confirmed by a
 * repo-wide grep before this phase started) -- these are real, callable,
 * host-tested functions, not yet wired into a live receive path. */

/* Scans object_catalog[] (kernel/object_catalog.h) for an active entry
 * whose object_id matches system_object_id and returns its partition_id.
 * Returns 0 (PARTITION_SYSTEM's numeric value, though this file
 * deliberately doesn't #include kernel/partition.h just to name that
 * constant -- 0 is already this whole project's "honest absence" default)
 * if no matching object is found. Mirrors the identical inline scan
 * pattern already repeated at several call sites in this codebase
 * (kernel/microkernel.c, kernel/row_index.c, kernel/syscall_dispatch.c,
 * kernel/stubs.c) rather than inventing a new lookup mechanism. */
uint32_t dspp_resolve_partition_id(uint64_t system_object_id);

/* True if this node may service a READ request for a page belonging to
 * partition_id -- Phase 2's local/remote ownership check (kernel/
 * partition.c's partition_is_local()) only. Reading a page this node
 * locally holds does not require also holding partition_id's WRITE lease
 * (Phase 4) -- that is specifically a write-authorization concept. */
int dspp_page_read_allowed(uint32_t partition_id);

/* True if this node may accept a WRITE request for partition_id --
 * requires BOTH Phase 2's local ownership (the partition's data lives on
 * this node) AND Phase 4's write lease (this node currently holds write
 * authorization for partition_id, not just physical residency).
 * Ownership and lease are deliberately separate concepts (Phase 2 vs
 * Phase 4) -- a node can locally own a partition's data while a different
 * node holds the actual write lease (mid-migration, or during a
 * split-brain window), and a real write must check both, not just one. */
int dspp_page_write_allowed(uint32_t partition_id);

/* Real per-packet gate for DSPP_PAGE_READ_REQ/DSPP_PAGE_WRITE_REQ: resolves
 * partition_id from the packet's system_object_id via dspp_resolve_
 * partition_id() above, then checks dspp_page_read_allowed()/_write_
 * allowed() as appropriate. Deliberately minimal -- does NOT move any
 * actual page data (no real object-to-physical-frame resolution plumbing
 * exists anywhere in this codebase to move; that is a separate, larger
 * integration this phase does not attempt). Returns 1 if the request
 * would be serviced (an ACK would be queued), 0 if denied, 0 for any
 * opcode this function doesn't handle. */
int process_dspp_page_packet(struct DSPPFullPagePacket* packet);

/* ─── Multi-Node Partition Scaling Roadmap Phase 7: real cross-node data
 * movement ───────────────────────────────────────────────────────────────
 *
 * Everything above this point (Phase 5) closed the "routing/gating" gap for
 * RAM-page opcodes that nothing has ever actually moved bytes for (no
 * object-to-physical-frame plumbing exists -- see process_dspp_page_
 * packet()'s own comment). This phase closes a DIFFERENT, more foundational
 * gap named three times across this codebase's docs (Phase 5's own
 * findings, the Phase 6 addendum, Multitenant Isolation Gap Analysis §13):
 * there was no RX dispatcher for ANY DSPP opcode at all, AND (a deeper
 * problem discovered while fixing that) every existing DSPP send site
 * transmitted a raw struct with zero Ethernet framing -- no dst/src MAC, no
 * ethertype -- so there was nothing for a real RX dispatcher to even
 * recognize as a DSPP frame in the first place. Both are fixed together
 * here: dspp_transmit_raw() (net/dspp.c) is the one real framing helper
 * every DSPP send site now goes through, and net/net.c's net_rx_dispatch()
 * gained a real ETHERTYPE_DSPP branch calling dspp_rx_dispatch() below --
 * the first genuinely live DSPP receive path this codebase has ever had.
 *
 * Scope: this phase moves kernel/stream.c's stream/blob data across a real
 * wire to a real (if simulated, in this dev environment) second node's own
 * storage -- the same scope stream_relocate_partition() (Phase 6 addendum)
 * chose for the identical reasons (streams already carry owner_uid/
 * partition_id per self-contained slot; rowstore/vecstore have no
 * per-partition page index yet, Storage Isolation Roadmap Phase 1's job).
 * Deliberately fire-and-forget, not a full reliable-transport state
 * machine: BEGIN/PAGE requests are sent once, without blocking on their
 * ACKs or retrying a dropped one -- ACKs exist in the wire format (the
 * receiver genuinely sends them) so a future phase can build retry logic
 * on top without another wire-format change, the same "protocol has an ACK
 * opcode nothing currently blocks on yet" posture DSPP_PAGE_WRITE_ACK
 * already had before this phase. Named honestly as a first cut, the same
 * disclosed-scope discipline this project applies throughout (e.g.
 * HTTP_PARTITION_RATE_LIMIT's own "deliberately generous first cut, not a
 * measured production value").
 */

/* A dedicated magic and packet family, entirely separate from struct
 * DSPPPacketHeader/DSPPFullPagePacket above -- NOT a repurposing of the
 * existing page-replication opcodes (DSPP_PAGE_READ_REQ/WRITE_REQ are a
 * different concept: RAM-page replication with no move plumbing behind
 * them yet, per process_dspp_page_packet()'s own comment). dspp_rx_
 * dispatch() (below) decides which family an incoming frame belongs to by
 * checking its magic value FIRST, before touching opcode -- the two
 * families' opcode numbers deliberately overlap (both start at 1) since
 * they are different wire "namespaces," identified by magic, not by a
 * single global opcode space. */
#define DSPP_MIGRATE_MAGIC 0x534c534e4d494752ULL // "SLSNMIGR"

enum DSPPMigrateOpcode {
    DSPP_MIGRATE_BEGIN_REQ = 1, // sender -> receiver: a new stream's metadata; allocate a local slot for it
    DSPP_MIGRATE_BEGIN_ACK = 2, // receiver -> sender: slot allocated (status=0) or denied/out-of-slots (status!=0)
    DSPP_MIGRATE_PAGE_REQ  = 3, // sender -> receiver: one 4KiB page of transfer_id's data, at page_index
    DSPP_MIGRATE_PAGE_ACK  = 4  // receiver -> sender: page_index written and verified (status=0) or failed (status!=0)
};

/* Fixed-size control header, carrying everything BEGIN/PAGE/their ACKs need
 * except page bytes themselves. Sized and transmitted alone (sizeof(struct
 * DSPPMigrateHeader)) for every opcode except DSPP_MIGRATE_PAGE_REQ, which
 * appends the page payload (struct DSPPMigratePagePacket, below) -- the
 * same "header-only vs. header+payload, sized per opcode" convention
 * net/consensus.c's own existing sends already established (e.g.
 * check_consensus_heartbeat_tick() transmits sizeof(struct
 * DSPPPacketHeader) alone; trigger_kernel_election_campaign() transmits
 * the full sizeof(struct DSPPFullPagePacket)). transfer_id correlates every
 * packet belonging to one stream's migration (sender-chosen, see kernel/
 * stream.c's stream_migrate_send_partition() for how); node_dest_id is a
 * genuine point-to-point destination (0 would mean "broadcast," but this
 * protocol family never legitimately broadcasts -- migration always has
 * exactly one intended recipient) checked by dspp_rx_dispatch() below
 * before this packet is acted on at all, since this codebase has no
 * node-id-to-MAC resolution table (every DSPP frame, old and new, is L2
 * broadcast -- see dspp_transmit_raw()'s own comment) and point-to-point
 * delivery must therefore be self-filtered one layer up. */
struct DSPPMigrateHeader {
    uint64_t magic;             // DSPP_MIGRATE_MAGIC
    uint16_t opcode;            // enum DSPPMigrateOpcode
    uint16_t node_source_id;
    uint32_t node_dest_id;      // the one real recipient this packet is for -- never 0/broadcast
    uint64_t transfer_id;       // correlates every packet for one stream's migration
    uint32_t partition_id;
    uint32_t page_index;        // meaningful only for PAGE_REQ/PAGE_ACK
    uint8_t  status;            // meaningful only for *_ACK opcodes: 0 = ok, nonzero = denied/failed
    char     stream_name[64];        // meaningful only for BEGIN_REQ (mirrors STREAM_NAME_LEN, kernel/stream.h)
    char     stream_mime_type[64];   // meaningful only for BEGIN_REQ (mirrors STREAM_MIME_LEN)
    uint64_t stream_size;             // meaningful only for BEGIN_REQ
    uint32_t stream_frames_used;      // meaningful only for BEGIN_REQ
    uint32_t stream_owner_uid;        // meaningful only for BEGIN_REQ
    uint32_t frag_index;              // which slice of page_index this is (PAGE_REQ/PAGE_ACK)
} __attribute__((packed));

/* ─── A 4 KiB page does not fit an Ethernet frame, so it goes in slices ───
 *
 * This packet used to carry page_data[4096], making it 4273 bytes -- almost
 * three times the link payload. Every page frame was therefore dropped, and
 * cross-node stream migration transferred nothing while appearing to start
 * (the 177-byte BEGIN header fits, so a migration announced itself and then
 * went quiet).
 *
 * 1024 matches DSPP_CTX_CHUNK_BYTES deliberately: two families slicing at
 * different sizes for no reason is a thing to get wrong later. It divides
 * 4096 exactly, which is what lets frag_index alone locate a slice -- no
 * per-fragment length field is needed, unlike the context family, whose
 * checkpoints are of arbitrary length and so must carry chunk_bytes.
 *
 * The resulting packet is 181 + 1024 = 1205 bytes. net/dspp.c static-asserts
 * both that it fits the link and that the division is exact. */
#define DSPP_MIGRATE_FRAG_BYTES 1024
#define DSPP_MIGRATE_FRAGS_PER_PAGE (4096 / DSPP_MIGRATE_FRAG_BYTES)

struct DSPPMigratePagePacket {
    struct DSPPMigrateHeader header;
    uint8_t                  page_data[DSPP_MIGRATE_FRAG_BYTES];
} __attribute__((packed));

/* ─── Live execution-context migration (PEC Phase 3) ──────────────────
 * Moving a RUNNING computation to another node, not just its data. The
 * stream family above moves bytes at rest; this moves a checkpoint of a
 * mid-execution SIMI context (kernel/simi_ckpt.h), so the receiving node
 * resumes the program at the exact instruction the sender stopped at.
 *
 * ─── Why a separate header struct rather than new fields ──────────────
 * DSPPMigrateHeader's tail is stream-specific (name, mime type, size,
 * frames_used, owner_uid) and means nothing for a context; a context needs
 * different metadata (total length, chunk count, program hash). Bolting
 * both sets onto one struct would grow every migrate packet and invite
 * reading a field that is meaningless for the opcode in hand.
 *
 * These opcodes therefore share DSPP_MIGRATE_MAGIC -- so dspp_rx_dispatch()
 * still routes them as one family -- but carry their own header.
 *
 * THE CONTRACT THAT MAKES THAT SAFE: the dispatcher must read magic,
 * opcode and node_dest_id BEFORE it can know which header struct it is
 * looking at. So the leading fields of both headers are laid out
 * identically, up to and including `status`. That is not a coincidence to
 * be preserved by memory -- net/dspp.c static-asserts every shared
 * offset, so changing either struct's prefix is a compile error rather
 * than a wire-format bug that only shows up as misrouted packets. */
enum DSPPCtxMigrateOpcode {
    DSPP_MIGRATE_CTX_BEGIN_REQ = 5,  // sender -> receiver: a context is arriving; here is its size and program
    DSPP_MIGRATE_CTX_BEGIN_ACK = 6,  // receiver -> sender: ready (status=0) or refused (status!=0)
    DSPP_MIGRATE_CTX_CHUNK_REQ = 7,  // sender -> receiver: one chunk of the checkpoint byte stream
    DSPP_MIGRATE_CTX_CHUNK_ACK = 8   // receiver -> sender: chunk stored; status!=0 on the final chunk means the restore failed
};

/* ─── Chunk size: chosen to fit an Ethernet frame, not a memory page ──────
 *
 * This was 4096, which made struct DSPPCtxMigrateChunkPacket 4217 bytes --
 * nearly three times what the link carries, so not one chunk of a live
 * context ever arrived anywhere. Live context migration appeared to start
 * (the 121-byte BEGIN header fits) and then moved nothing.
 *
 * 4096 was never a requirement. It matched the page size out of habit; a
 * checkpoint is a flat byte stream and this protocol already carries
 * chunk_index/total_chunks/chunk_bytes and reassembles on the far side, so
 * the size is free to be whatever the wire prefers.
 *
 * 1024 rather than the 1365 that would just fit (1486 wire payload minus a
 * 121-byte header): a power of two divides a 4 KiB checkpoint evenly, and
 * the ~340 bytes of slack means adding a field to the header below cannot
 * silently push the packet over the limit. The _Static_assert in net/dspp.c
 * enforces that regardless. */
#define DSPP_CTX_CHUNK_BYTES 1024
#define DSPP_CTX_NAME_LEN    64

struct DSPPCtxMigrateHeader {
    /* ── Prefix: byte-identical layout to struct DSPPMigrateHeader ──── */
    uint64_t magic;             // DSPP_MIGRATE_MAGIC
    uint16_t opcode;            // enum DSPPCtxMigrateOpcode
    uint16_t node_source_id;
    uint32_t node_dest_id;      // self-filtered on receipt, as with the stream family
    uint64_t transfer_id;
    uint32_t partition_id;
    uint32_t chunk_index;       // aligns with DSPPMigrateHeader::page_index
    uint8_t  status;
    /* ── Context-specific tail ──────────────────────────────────────── */
    char     ctx_name[DSPP_CTX_NAME_LEN];  // meaningful on BEGIN_REQ
    uint64_t total_bytes;                  // full checkpoint length (BEGIN_REQ)
    uint32_t total_chunks;                 // ceil(total_bytes / DSPP_CTX_CHUNK_BYTES) (BEGIN_REQ)
    uint32_t chunk_bytes;                  // live bytes in THIS chunk (CHUNK_REQ; last one is short)
    uint64_t program_hash;                 // simi_ckpt_program_hash() -- receiver must hold the same image
} __attribute__((packed));

struct DSPPCtxMigrateChunkPacket {
    struct DSPPCtxMigrateHeader header;
    uint8_t                     chunk_data[DSPP_CTX_CHUNK_BYTES];
} __attribute__((packed));

/* Sender-side. Both are fire-and-forget, exactly like their stream
 * counterparts and for the same reason (kernel/net_event.h's blocking
 * wait contains privileged `sti; hlt` and cannot be used here). */
void dspp_ctx_migrate_send_begin(uint64_t transfer_id, uint32_t node_dest_id,
                                 uint32_t partition_id, const char* ctx_name,
                                 uint64_t total_bytes, uint32_t total_chunks,
                                 uint64_t program_hash);

void dspp_ctx_migrate_send_chunk(uint64_t transfer_id, uint32_t node_dest_id,
                                 uint32_t partition_id, uint32_t chunk_index,
                                 const uint8_t* data, uint32_t chunk_bytes);

/* Receiver-side, reached from dspp_rx_dispatch() for the CTX opcodes.
 * Self-filters on node_dest_id first, then routes BEGIN_REQ/CHUNK_REQ into
 * kernel/simi_ctx_migrate.c and ACKs each. */
void dspp_ctx_migrate_rx(struct DSPPCtxMigrateChunkPacket* packet, uint16_t len);

/* ─── Service-registry replication (Orchestration Plan Phase 4 gap) ────
 * Phase 4 shipped a per-node registry: a name registered on node 1 did
 * not resolve on node 2. This announces registrations to the cluster so
 * it does.
 *
 * ─── Announce, do not query ───────────────────────────────────────────
 * A request/response "who has this name?" would need a reply timeout, and
 * every blocking wait in this codebase routes through kernel/net_event.h,
 * whose privileged `sti; hlt` cannot be used from the paths that would
 * call it. So this is the same fire-and-forget shape the two migrate
 * families already use: each node ANNOUNCES what it owns, and every other
 * node caches what it hears. A resolve is then always a local lookup,
 * with no network round trip on the hot path at all.
 *
 * ─── Why remote entries are a separate table ──────────────────────────
 * A cached remote registration is not the same kind of fact as a local
 * one. It is another node's truth, it is not authoritative here, and it
 * must never be persisted -- restoring a stale cache from disk would
 * resurrect services that may have moved or vanished while this node was
 * down. Keeping them apart makes "local wins, and only local persists" a
 * property of the data structure rather than a rule to remember.
 *
 * Broadcast to the whole segment (node_dest_id 0 == "everyone"), unlike
 * the migrate families which are point-to-point and self-filter. */
enum DSPPServiceOpcode {
    DSPP_SVC_ANNOUNCE = 9,   /* "I own this name, here is where it lives" */
    DSPP_SVC_WITHDRAW = 10,  /* "I no longer own this name" */
};

struct DSPPServiceHeader {
    /* Same prefix layout as both migrate headers -- static-asserted in
     * dspp.c, for the same dispatcher reason. */
    uint64_t magic;             /* DSPP_MIGRATE_MAGIC (shared family) */
    uint16_t opcode;
    uint16_t node_source_id;
    uint32_t node_dest_id;      /* 0 == broadcast to the segment */
    uint64_t transfer_id;       /* unused; keeps the prefix identical */
    uint32_t partition_id;
    uint32_t chunk_index;       /* unused */
    uint8_t  status;
    /* ── Service-specific tail ─────────────────────────────────────── */
    char     service_name[DSPP_CTX_NAME_LEN];
    uint32_t endpoint_port;
    uint8_t  endpoint_kind;
    uint32_t owner_uid;
    /* SLSServiceServing, as the ANNOUNCING node probed its own endpoint.
     * Riding the heartbeat rather than being asked for: a cross-node probe
     * would need a request/response with a timeout, and no blocking wait
     * is usable from these paths (see this file's own note on
     * net_event.h). One byte and no extra round trip. */
    uint8_t  serving;
} __attribute__((packed));

void dspp_service_announce(const char* name, uint32_t partition_id,
                           uint8_t endpoint_kind, uint32_t endpoint_port,
                           uint32_t owner_uid, uint8_t serving);
void dspp_service_withdraw(const char* name);
void dspp_service_rx(struct DSPPServiceHeader* h, uint16_t len);

/* ─── Partition-table replication (Multi-Node Phase 2 gap) ─────────────
 * Partition tables were local-only: a partition created on node 1 did
 * not exist on node 2, and the failover doc row's honest boundary said
 * so. This announces partition rows (id, name, owner) to the segment so
 * every node's table converges on change -- create, migrate/adopt (both
 * go through partition_set_owner_node()), and destroy.
 *
 * The same "announce, do not query" shape as the service family above,
 * and for the same reason: each node ANNOUNCES what it owns and every
 * other node applies what it hears. Broadcast (node_dest_id 0), exactly
 * like the service family -- partition rows are cluster state, not a
 * point-to-point transfer.
 *
 * ─── What is NOT replicated ───────────────────────────────────────────
 * Only the table rows. The catalog objects and stream bytes inside a
 * partition still move only via the migrate families; the checkpoint
 * broadcast (failover) still carries the leader's tree. And, matching
 * the service family's "must never be persisted" rule, a row learned
 * from the wire is RUNTIME state on this node -- the RX path runs in
 * the timer ISR, where persist_partitions()'s NVMe write must not run.
 * A reboot re-converges on the next announce.
 *
 * ─── Last announce wins ───────────────────────────────────────────────
 * Same conflict rule as service_remote_learn(): two nodes claiming one
 * partition id is an operator error (ids are local slot numbers), and
 * the only thing worse than picking one is picking one quietly -- so it
 * is logged and the newer announce is taken. A withdraw applies only if
 * this node's current owner row says the announcing node owned it (the
 * service family's own source-scoped forget rule). */
enum DSPPPartitionOpcode {
    DSPP_PARTITION_ANNOUNCE = 15,  /* "partition <id> is named <name>, owned by <node>" */
    DSPP_PARTITION_WITHDRAW = 16,  /* "partition <id> no longer exists" */
};

/* Mirrors PARTITION_NAME_LEN (kernel/partition.h) -- the same
 * wire-vs-kernel constant pair DSPP_CTX_NAME_LEN already is. */
#define DSPP_PARTITION_NAME_LEN 32

struct DSPPPartitionSyncHeader {
    /* Same prefix layout as the migrate/service headers -- static-asserted
     * in dspp.c, for the same dispatcher reason. */
    uint64_t magic;             /* DSPP_MIGRATE_MAGIC (shared family) */
    uint16_t opcode;
    uint16_t node_source_id;
    uint32_t node_dest_id;      /* 0 == broadcast to the segment */
    uint64_t transfer_id;       /* unused; keeps the prefix identical */
    uint32_t partition_id;
    uint32_t chunk_index;       /* unused */
    uint8_t  status;
    /* ── Partition-specific tail ──────────────────────────────────── */
    uint32_t owner_node_id;     /* ANNOUNCE: who owns the partition now */
    char     partition_name[DSPP_PARTITION_NAME_LEN];
} __attribute__((packed));

/* Sender side, called by kernel/partition.c's mutators. Silent on a node
 * with no cluster identity (node id 0 is the Phase 1 uninitialized
 * sentinel), same rule as dspp_service_announce(). */
void dspp_partition_announce(uint32_t partition_id, const char* name,
                             uint32_t owner_node_id);
void dspp_partition_withdraw(uint32_t partition_id);
/* Receiver side: called from dspp_rx_dispatch() for DSPP_PARTITION_*;
 * applies the row via kernel/partition.c's partition_sync_upsert()/
 * partition_sync_withdraw(). */
void dspp_partition_rx(struct DSPPPartitionSyncHeader* h, uint16_t len);

/* Wraps dspp_len bytes at dspp_payload in a broadcast Ethernet frame
 * (ethertype ETHERTYPE_DSPP, net/net.h) and transmits it -- the one real
 * framing helper every DSPP send site (old and new) now goes through
 * instead of calling e1000_transmit_packet() directly on a bare struct
 * with no L2 framing at all (see this file's own Phase 7 header comment
 * for why that was a real gap). Broadcast destination MAC, not a specific
 * peer's, because this codebase has no node-id-to-MAC resolution table --
 * point-to-point delivery, where it matters (migration), is self-filtered
 * one layer up via struct DSPPMigrateHeader's own node_dest_id field, the
 * same way IP-layer protocols self-filter above a shared L2 broadcast
 * domain when no more specific addressing exists yet. dspp_len must not
 * exceed the larger of sizeof(struct DSPPFullPagePacket) or sizeof(struct
 * DSPPMigratePagePacket) (whichever DSPP payload family is larger -- the
 * internal static frame buffer is sized off both, not just one, after a
 * real bug where sizing off only the older, smaller struct silently
 * dropped every migrate-family page packet); a caller passing more still
 * is a programming error and the packet is dropped rather than
 * overflowing that buffer.
 *
 * ─── AND it must fit the link, which is a separate, smaller limit ────────
 * The buffer-overflow check above was the only size guard for several
 * phases, and it let every 4 KB DSPP message through -- to an Ethernet
 * segment that cannot carry one. A standard frame tops out at 1500 bytes of
 * payload; e1000 will not receive more than 1522 total unless RCTL.LPE is
 * set (it is not, net/e1000.c), and the RX buffers are 2048 bytes with no
 * descriptor chaining in e1000_poll_rx(), so a long frame has nowhere to
 * land even if the MAC accepted it.
 *
 * The symptom was a four-node cluster whose nodes campaigned forever: a
 * REQUEST_VOTE is a struct DSPPFullPagePacket (4132 bytes) purely because
 * struct ConsensusMessage was placed in its payload_4kb field, so every
 * vote request left the NIC and landed nowhere, and every node sat at
 * CANDIDATE with a term climbing once per election timeout.
 *
 * No host test caught it because every one of them calls
 * dspp_rx_dispatch() with a buffer already in memory. The wire is the exact
 * part the test strategy structurally cannot reach, so the guard has to be
 * in the code rather than in a test of the code. */
#define DSPP_LINK_MTU          1500u
/* 1500 - ETH_HDR_LEN(14). Spelled as a literal rather than derived, because
 * dspp.h deliberately does not #include net.h -- it has no include guard of
 * its own (see the note on struct DSPPFullPagePacket), so pulling headers in
 * here breaks any translation unit that includes both. net/dspp.c carries a
 * _Static_assert that this stays equal to DSPP_LINK_MTU - ETH_HDR_LEN, so
 * the two cannot drift apart silently. */
#define DSPP_MAX_WIRE_PAYLOAD  1486u

/* Frames refused for exceeding DSPP_MAX_WIRE_PAYLOAD. Non-zero means some
 * DSPP message is structurally undeliverable on this link -- as of today
 * that is the 4 KB page-transfer family (DSPPMigratePagePacket,
 * DSPPCtxMigrateChunkPacket, DSPP_PAGE_*_REQ), which needs either jumbo
 * frames or protocol-level fragmentation. Counted and logged rather than
 * dropped quietly: this failure previously presented as a healthy-looking
 * cluster that simply never converged. */
extern uint64_t dspp_tx_oversize_dropped;

/* The limit dspp_transmit_raw() actually enforces. Initialised to
 * DSPP_MAX_WIRE_PAYLOAD and NEVER written by kernel code -- there is no
 * code path in the kernel that assigns to it, deliberately.
 *
 * It exists as a variable solely so a host test exercising a protocol layer
 * ABOVE the link (chunk reassembly, ordering, duplicate rejection -- all of
 * which are link-independent) can simulate a link that could carry its
 * frames, instead of rewriting the test to hand-build every packet.
 *
 * The hazard with a seam like this is that it quietly removes the property
 * it was added around, so: the two tests that assert the link limit itself
 * -- dspp_phase5_host_test.c Scenarios 9-10 and cross_node_migration_host_
 * test.c Scenario 1 -- must never touch it, and say so in place. If this
 * ever gains a writer outside tests/, that is a bug. */
extern uint16_t dspp_max_wire_payload;

/* Frames dropped because the timer ISR re-entered dspp_transmit_raw() while
 * the BSP was building one. Always an ACK (that is the only thing the RX
 * path transmits), and a lost ACK is covered by the sender's retransmit
 * timer -- so this is informational, not a fault. See the guard in
 * net/dspp.c for why dropping beats buffering. */
extern uint64_t dspp_tx_reentrant_dropped;

void dspp_transmit_raw(const void* dspp_payload, uint16_t dspp_len);

/* The real DSPP receive entry point, called from net/net.c's
 * net_rx_dispatch() once an ETHERTYPE_DSPP frame's Ethernet header has
 * already been stripped -- buf/len here are the DSPP payload only. Checks
 * the leading magic value first to decide which packet family (existing
 * consensus/page-routing vs. this phase's new migrate family) the frame
 * belongs to, then routes by opcode within that family: DSPP_CMD_REQUEST_
 * VOTE/_VOTE_REPLY/_HEARTBEAT to process_consensus_packet() (net/
 * consensus.h), DSPP_CMD_PARTITION_* to process_partition_consensus_
 * packet(), DSPP_PAGE_READ_REQ/WRITE_REQ to process_dspp_page_packet()
 * above, DSPP_MIGRATE_* to dspp_migrate_rx() below -- exactly the routing
 * net/consensus.h's own process_partition_consensus_packet() comment
 * already anticipated ("the same way any future RX dispatcher would need
 * to distinguish them"). DSPP_PAGE_READ_ACK/WRITE_ACK/DSPP_MIGRATE_
 * BEGIN_ACK/PAGE_ACK are honestly no-ops here -- nothing in this codebase
 * yet blocks on or retries against an incoming ACK (see this file's own
 * "fire-and-forget, not a full reliable-transport state machine" scope
 * note), so an ACK arriving is correctly received and silently discarded,
 * not misrouted. Any unrecognized magic is silently dropped -- the same
 * "denial looks like absence" carefulness kernel/persist.c's own magic-
 * mismatch handling already established for a different layer (disk
 * instead of wire). */
void dspp_rx_dispatch(void* buf, uint16_t len);

/* Sender-side: builds and transmits one DSPP_MIGRATE_BEGIN_REQ for a stream
 * about to be migrated. Called by kernel/stream.c's stream_migrate_send_
 * partition() once per stream slot being migrated, before any of that
 * slot's pages are sent. Fire-and-forget (see this file's own scope note)
 * -- does not wait for or check DSPP_MIGRATE_BEGIN_ACK. */
void dspp_migrate_send_begin(uint64_t transfer_id, uint32_t node_dest_id,
                              uint32_t partition_id, const char* name,
                              const char* mime_type, uint64_t size,
                              uint32_t frames_used, uint32_t owner_uid);

/* ─── Sender-side ACK tracking (retransmission) ───────────────────────────
 *
 * DSPP_MIGRATE_BEGIN_ACK / PAGE_ACK used to be received and discarded, and
 * dspp.h said so honestly under "fire-and-forget". They are now recorded, so
 * a sender can tell which fragments landed and resend only the ones that did
 * not.
 *
 * ─── How the waiting works, since it looks impossible at first glance ────
 * kernel/stream.c's sender is synchronous: it spins waiting for ACKs. The
 * ACKs are delivered by the TIMER ISR -- kernel/timer.c's timer_irq_handler()
 * calls net_poll_tick(), which drains the NIC and lands in
 * dspp_migrate_note_ack() below. So the waiting thread is not the thread
 * making progress, and a spin with interrupts enabled is not a deadlock.
 * That is the same arrangement kernel_sleep_ticks() relies on.
 *
 * Exactly one page is ever in flight (partition_migrate() is synchronous),
 * so this is a single record rather than a table. Arm it, send, wait, read.
 *
 * A late ACK for a page no longer being awaited is ignored -- otherwise a
 * duplicate from a previous retransmit round could satisfy the current
 * page's completion check with fragments that were never sent for it.
 *
 * ─── The BEGIN is waited on too, and originally was not ──────────────────
 * This header used to argue that no separate BEGIN_ACK wait was needed: a
 * PAGE_ACK with status 0 already proves the BEGIN landed, because
 * stream_migrate_recv_page() refuses a page for a transfer it has no
 * inflight row for.
 *
 * That reasoning holds only when there is at least one page. A stream with
 * frames_used == 0 sends no pages, collects no ACKs, and -- because the
 * sender's `stream_confirmed` flag started out true and the page loop simply
 * never ran -- was retired having confirmed nothing whatsoever. Exactly the
 * fire-and-forget deletion that waiting for ACKs was introduced to prevent,
 * surviving in the empty case. Found by migrating a freshly created stream
 * on a real cluster and noticing "0 page(s) ... every page acknowledged".
 *
 * Waiting on the BEGIN unconditionally costs one round trip per STREAM (not
 * per page), removes that special case entirely, and closes the other gap
 * the old note named: a lost BEGIN is now retransmitted rather than causing
 * every subsequent page to be refused. */
void dspp_migrate_arm_begin(uint64_t transfer_id);
int  dspp_migrate_begin_acked(void);
void dspp_migrate_arm_page(uint64_t transfer_id, uint32_t page_index);
void dspp_migrate_note_ack(uint64_t transfer_id, uint16_t opcode,
                           uint32_t page_index, uint32_t frag_index,
                           uint8_t status);
int  dspp_migrate_page_acked(void);
int  dspp_migrate_frag_acked(uint32_t frag_index);
/* 1 if any ACK for the armed transfer reported a non-zero status. That is a
 * REFUSAL, not a loss -- the destination has no slot, or the page index was
 * out of range. Retrying cannot help, so the sender aborts instead. */
int  dspp_migrate_nacked(void);
void dspp_migrate_disarm(void);

/* ─── Retransmission budget ───────────────────────────────────────────────
 * Timeouts are in kernel_tick_counter units (~100 Hz, kernel/timer.c), the
 * same clock net/consensus.h's election timing uses and for the same reason:
 * the waiting loop's iteration count says nothing about elapsed time.
 *
 * 25 ticks is 250 ms per attempt -- generous for a local segment where a
 * round trip is sub-millisecond, and deliberately so: the cost of waiting
 * too long is a slow migration, while the cost of giving up too early is a
 * retransmit storm on a link that is already struggling.
 *
 * 4 attempts, so a page has ~1 second to land before the transfer is
 * declared failed. A transfer that cannot get one page across in a second
 * on a local segment has something wrong with it that more retries will not
 * fix. */
#define DSPP_MIGRATE_ACK_TIMEOUT_TICKS 25u
#define DSPP_MIGRATE_MAX_ATTEMPTS       4u

/* ─── The second bound: what if the clock itself stops? ───────────────────
 * Waiting on kernel_tick_counter assumes the timer ISR is running. If it is
 * not -- interrupts disabled by a caller, the LAPIC timer not yet
 * calibrated, a fault in the handler -- then `while (tick < deadline)` never
 * terminates and the migration hangs the node. This kernel has already been
 * bitten by exactly that shape once: boot_application_processors() spun
 * unbounded waiting for an AP that never came up, and hung every `-smp 1`
 * boot until it was given a bound.
 *
 * A plain iteration cap cannot serve, because any value short enough to be
 * useful in a host test is far shorter than 25 real ticks and would fire
 * first on real hardware, silently converting the timeout into "spin 100000
 * times". So the bound is on the clock being STALLED rather than on
 * iterations outright: if this many spins pass without kernel_tick_counter
 * changing AT ALL, the clock is not running and no amount of further waiting
 * will help.
 *
 * On real hardware the tick advances every ~10 ms, so the counter resets
 * long before this trips -- it is a safety net, not part of the timing. In a
 * host test with a static clock it trips at once, which is what makes the
 * timeout path testable without waiting real seconds. */
#define DSPP_MIGRATE_STALL_SPINS   200000u

/* Sender-side: transmits ONE fragment of a page. Used by the retransmit
 * path to resend just the slices that were not acknowledged, rather than the
 * whole page. */
void dspp_migrate_send_frag(uint64_t transfer_id, uint32_t node_dest_id,
                            uint32_t partition_id, uint32_t page_index,
                            uint32_t frag_index, const uint8_t* page_data);

/* Sender-side: transmits one 4 KiB page as DSPP_MIGRATE_FRAGS_PER_PAGE
 * separate DSPP_MIGRATE_PAGE_REQ frames, each carrying
 * DSPP_MIGRATE_FRAG_BYTES of it and its own frag_index. Called once per
 * page by kernel/stream.c's stream_migrate_send_partition(); the slicing is
 * this function's business, so that caller still thinks in whole pages.
 * Fire-and-forget, same as dspp_migrate_send_begin(). */
void dspp_migrate_send_page(uint64_t transfer_id, uint32_t node_dest_id,
                             uint32_t partition_id, uint32_t page_index,
                             const uint8_t* page_data);

/* Receiver-side: the real handler dspp_rx_dispatch() routes DSPP_MIGRATE_*
 * opcodes to. Checks header->node_dest_id against cluster_local_node_id()
 * (net/consensus.h) first and silently drops anything not addressed to
 * this node -- the self-filtering this protocol family relies on in place
 * of real L2 addressing (see struct DSPPMigrateHeader's own comment).
 * BEGIN_REQ calls kernel/stream.c's stream_migrate_recv_begin() to
 * allocate a local slot and transmits DSPP_MIGRATE_BEGIN_ACK reporting the
 * result; PAGE_REQ calls stream_migrate_recv_page() to write+verify the
 * page against this node's own storage and transmits DSPP_MIGRATE_PAGE_ACK.
 * `packet` is a struct DSPPMigratePagePacket* for PAGE_REQ (page_data must
 * be present and len must cover it) or a struct DSPPMigrateHeader* for
 * every other opcode (page_data is neither present nor read). */
void dspp_migrate_rx(struct DSPPMigratePagePacket* packet, uint16_t len);

/* ─── Checkpoint transfer (Core Backup Strategies, Step 3) ─────────────────
 * Transfers a serialized state tree (kernel/state_tree.h) from one node to
 * another via DSPP. Same chunked protocol shape as context migration:
 * BEGIN announces the transfer, CHUNKs carry the payload, receiver ACKs
 * each. Fire-and-forget from the sender's perspective (no retransmit).
 *
 * Shares DSPP_MIGRATE_MAGIC, routed by opcode in dspp_rx_dispatch(). */
enum DSPPCkptOpcode {
    DSPP_CKPT_BEGIN_REQ  = 11,
    DSPP_CKPT_BEGIN_ACK  = 12,
    DSPP_CKPT_CHUNK_REQ  = 13,
    DSPP_CKPT_CHUNK_ACK  = 14,
};

#define DSPP_CKPT_CHUNK_BYTES 1024

struct DSPPCkptHeader {
    /* Prefix: byte-identical to DSPPMigrateHeader's leading fields */
    uint64_t magic;             /* DSPP_MIGRATE_MAGIC */
    uint16_t opcode;            /* DSPPCkptOpcode */
    uint16_t node_source_id;
    uint32_t node_dest_id;
    uint64_t transfer_id;
    uint32_t partition_id;      /* unused for checkpoint; kept for prefix compat */
    uint32_t chunk_index;
    uint8_t  status;
    /* Checkpoint-specific tail */
    uint64_t sequence;          /* checkpoint sequence number (BEGIN_REQ) */
    uint32_t total_bytes;       /* total serialized tree size (BEGIN_REQ) */
    uint32_t total_chunks;      /* ceil(total_bytes / DSPP_CKPT_CHUNK_BYTES) */
    uint32_t chunk_bytes;       /* live bytes in THIS chunk (CHUNK_REQ) */
} __attribute__((packed));

struct DSPPCkptChunkPacket {
    struct DSPPCkptHeader header;
    uint8_t               chunk_data[DSPP_CKPT_CHUNK_BYTES];
} __attribute__((packed));

/* Sender: transmit a full serialized state tree to node_dest_id */
void dspp_ckpt_send(uint64_t transfer_id, uint32_t node_dest_id,
                    uint64_t sequence, const uint8_t* data, uint32_t data_size);

/* Receiver: called from dspp_rx_dispatch() for DSPP_CKPT_* opcodes */
void dspp_ckpt_rx(struct DSPPCkptChunkPacket* packet, uint16_t len);

/* Receiver state: query whether a complete checkpoint was received */
int      dspp_ckpt_recv_ready(void);
uint32_t dspp_ckpt_recv_size(void);
void     dspp_ckpt_recv_copy(uint8_t* out, uint32_t max);
void     dspp_ckpt_recv_reset(void);

#endif /* DSPP_H */
