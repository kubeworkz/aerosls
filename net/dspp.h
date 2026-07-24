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
} __attribute__((packed));

struct DSPPMigratePagePacket {
    struct DSPPMigrateHeader header;
    uint8_t                  page_data[4096];
} __attribute__((packed));

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
 * overflowing that buffer. */
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

/* Sender-side: builds and transmits one DSPP_MIGRATE_PAGE_REQ carrying one
 * 4KiB page. Called once per page by kernel/stream.c's stream_migrate_
 * send_partition(). Fire-and-forget, same as dspp_migrate_send_begin(). */
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

#endif /* DSPP_H */
