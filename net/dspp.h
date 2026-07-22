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

#endif /* DSPP_H */
