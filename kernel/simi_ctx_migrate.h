#ifndef SIMI_CTX_MIGRATE_H
#define SIMI_CTX_MIGRATE_H

#include <stdint.h>
#include "simi_interp.h"
#include "simi_ckpt.h"

/*
 * simi_ctx_migrate.h — move a RUNNING computation between nodes
 * (Persistent Execution Contexts, Phase 3).
 *
 * This is the capability the whole PEC track exists for. Phase 2 made a
 * live context serialisable; this puts those exact bytes on the wire and
 * resumes the computation on another machine at the instruction it was
 * stopped at.
 *
 * ─── Why it is small ──────────────────────────────────────────────────
 * Almost nothing here is new. The transport (DSPP with real Ethernet
 * framing), the receive dispatcher, the self-filtering by node id, the
 * fire-and-forget send discipline and the two-simulated-node test
 * technique were all built for stream migration. Phase 2 deliberately
 * produced a flat byte buffer rather than writing to storage precisely so
 * this phase would move the same bytes instead of needing a second,
 * parallel serialiser. So this file is: chunk a buffer on the way out,
 * reassemble it on the way in, and hand the result to simi_ckpt_load().
 *
 * This mirrors kernel/stream.c's role in stream migration exactly --
 * net/dspp.c owns the wire encoding, this owns what the bytes mean.
 *
 * ─── Concurrency scope, stated rather than discovered ─────────────────
 * ONE inbound transfer at a time. A reassembly buffer must hold a whole
 * checkpoint (up to ~360 KiB at SIMI_MAX_FRAMES=32), so N concurrent
 * inbound transfers cost N of those. One is enough to prove the
 * capability and to serve the realistic case (a partition migrating to a
 * chosen destination); a second BEGIN arriving while one is in flight is
 * refused with a distinct status rather than silently corrupting the
 * first. Raising the limit is adding an array, not redesigning anything.
 */

typedef enum {
    SIMI_CTXMIG_OK = 0,
    SIMI_CTXMIG_ERR_BUSY,          /* another inbound transfer is already in flight */
    SIMI_CTXMIG_ERR_TOO_LARGE,     /* checkpoint exceeds the reassembly buffer */
    SIMI_CTXMIG_ERR_NO_TRANSFER,   /* chunk arrived for an unknown transfer_id */
    SIMI_CTXMIG_ERR_BAD_CHUNK,     /* chunk index out of range, or oversized */
    SIMI_CTXMIG_ERR_NO_PROGRAM,    /* no local image matches the sender's program hash */
    SIMI_CTXMIG_ERR_RESTORE,       /* reassembled, but simi_ckpt_load() refused it */
} SimiCtxMigStatus;

const char* simi_ctx_mig_status_name(SimiCtxMigStatus s);

/* ─── Sender side ─────────────────────────────────────────────────────
 * Checkpoints `ctx` and transmits it as BEGIN + N chunks to `dest_node`.
 * Returns the number of chunks sent, or 0 on failure (checkpoint too
 * large for the caller's staging buffer, or serialisation refused).
 *
 * Fire-and-forget: does not wait for ACKs. The receiver does send them --
 * so a later phase can add retry without another wire-format change --
 * but nothing reads them yet, exactly as with stream migration. */
uint32_t simi_ctx_migrate_send(const struct SimiContext* ctx,
                               uint64_t transfer_id, uint32_t dest_node,
                               uint32_t partition_id, const char* ctx_name);

/* ─── Receiver side ───────────────────────────────────────────────────
 * Called by net/dspp.c when the matching opcodes arrive. */
SimiCtxMigStatus simi_ctx_migrate_recv_begin(uint64_t transfer_id,
                                             const char* ctx_name,
                                             uint64_t total_bytes,
                                             uint32_t total_chunks,
                                             uint64_t program_hash);

SimiCtxMigStatus simi_ctx_migrate_recv_chunk(uint64_t transfer_id,
                                             uint32_t chunk_index,
                                             const uint8_t* data,
                                             uint32_t chunk_bytes);

/* ─── Program-image binding ───────────────────────────────────────────
 * A checkpoint does not carry its program (see simi_ckpt.h). On restore
 * the receiver must supply an image whose hash matches, or the restore is
 * refused -- `pc` is an instruction index, so resuming against the wrong
 * image would execute unrelated code at a plausible-looking offset.
 *
 * The receiving node therefore needs a way to find "the local image with
 * this hash". Registering images explicitly keeps this file independent
 * of the loader and the object catalog, and keeps it host-testable. */
void simi_ctx_migrate_register_image(const SimiObject* obj);
void simi_ctx_migrate_clear_images(void);

/* ─── Live-context registry ───────────────────────────────────────────
 * partition_migrate() moves everything a partition owns. To move its
 * RUNNING computations it needs to know which they are, and nothing in
 * this kernel currently tracks that: the interpreter (Phase 1) executes a
 * context the caller owns, and no scheduler holds a set of live ones.
 *
 * STATE OF THIS, PLAINLY: the registry below is real and
 * partition_migrate() really iterates it, but NO subsystem registers
 * anything into it yet, so on every current boot it is empty and the
 * partition path moves zero contexts. That is a genuine "the owner of
 * live contexts has not been built yet" gap, not a stub -- the moment a
 * scheduler or service runtime calls simi_ctx_register(), migration
 * starts moving real work with no further changes here. It is wired now
 * rather than later so the capability is reachable from the system
 * instead of only from a test. */
#define SIMI_CTX_REGISTRY_MAX 32

int  simi_ctx_register(uint32_t partition_id, const char* name, struct SimiContext* ctx);
void simi_ctx_unregister(const struct SimiContext* ctx);
uint32_t simi_ctx_registered_count(uint32_t partition_id);

/* Checkpoints and transmits every registered context belonging to
 * `partition_id`. Returns the count actually sent. Mirrors
 * stream_migrate_send_partition()'s contract, including its return
 * convention, so partition_migrate() treats both the same way. */
uint32_t simi_ctx_migrate_send_partition(uint32_t partition_id, uint32_t dest_node);

/* Where a completed transfer lands. Valid once the final chunk has been
 * received and restored; simi_ctx_migrate_arrived() reports whether that
 * has happened, so a caller can poll rather than being called back. */
struct SimiContext* simi_ctx_migrate_landed(void);
int                 simi_ctx_migrate_arrived(void);
void                simi_ctx_migrate_reset(void);

#endif /* SIMI_CTX_MIGRATE_H */
