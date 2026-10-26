#ifndef ENV_CATALOG_H
#define ENV_CATALOG_H

#include <stdint.h>

/* env_catalog.h — POSIX-Environments v0.2 §9's answer: the interop surface.
 *
 * §9 deferred the project's largest architectural question to a later
 * increment: once a POSIX environment exists as a partition, what does it
 * SEE of the object system it is running on top of? Left open, that question
 * admits two bad answers — nothing (an environment is a sealed toy, and the
 * roadmap's own §10.4 "first user" gate stalls on it) or everything (a
 * tenant reading another tenant's rows, which is LPAR Phase 11 undone by a
 * list call). This increment picks a third, and writes down why:
 *
 *   READ-ONLY, PARTITION-SCOPED, MEDIATED BY A SECOND KERNEL-BROKERED
 *   SERVICE — "kernel.env.catalog".
 *
 * WHY A SECOND SERVICE AND NOT A WIDER FIRST ONE.
 * net_socket_service.c proved the shape on the network side: a tenant's
 * manifest names a KERNEL-OWNED peer, cap_create_sidecar() mints the kernel
 * end and registers it with the owning service, and the service answers
 * every verb against the kernel's own state with the caller's identity
 * attached (§6, P2). The catalog reuses that machinery because the property
 * it needs is the same one P2 already established — the boundary is the
 * channel, not the API. A manifest cannot point at another partition's
 * catalog, because the catalog is not a thing a manifest can name: it is
 * kernel-owned, and cap.c wires it only for the partition the caller is
 * already in. Widening net_socket_service itself to carry catalog verbs
 * would instead mean one channel answering two specs, and the socket
 * service's admission path (tcp_quota, attribution) is a different question
 * from a name lookup. Two small services with one wire discipline beat one
 * service with two jobs.
 *
 * WHAT THE SURFACE EXPOSES — and, more importantly, what it does not:
 *
 *   CAT_INFO  the handshake, answered honestly: the real object ceiling,
 *             the real name/value caps, and the partition the KERNEL thinks
 *             this caller is in (a tenant can verify the identity the
 *             boundary is being computed from, rather than trusting it).
 *   CAT_LIST  the names in this partition's namespace, paged.
 *   CAT_STAT  one name -> type, tier, page size, owner uid.
 *   CAT_READ  one (object, key) -> the value from the legacy KV record path.
 *
 * There is no write verb. Not "write is refused" — the verb does not exist
 * in the table, and a type outside the table is refused BY NAME with its
 * number, which is how P2's increment 1 already handles the verbs no
 * increment builds. A read-only surface has no write-path permission
 * question to get wrong, which is exactly why it is the right first cut:
 * the hard part of interop is the ISOLATION, and isolation is fully
 * exercised by reading.
 *
 * THE BOUNDARY: partition_id, and absence rather than denial.
 * Every verb is answered against object_catalog[] entries whose
 * SLSObjectEntry.partition_id equals the partition this channel registered
 * with. An entry in another partition is NOT REPRESENTABLE in the reply:
 * it does not appear in CAT_LIST, and CAT_STAT/CAT_READ on its name answer
 * the same CAT_ERR_NOENT as a name that never existed anywhere. That is a
 * deliberate choice of answer, not an omission — a CAT_ERR_PERM here would
 * confirm to a tenant that a name exists in some partition it cannot see,
 * turning a list call into an existence oracle across the LPAR boundary.
 * P2's own summary already states the property for sockets ("a
 * cross-partition socket is not representable at the wire"); this is the
 * same statement applied to a catalog entry.
 *
 * The corollary, which the host test pins: the NOENT answer and the PERM
 * answer are both real, and they mean different things. NOENT means "not in
 * your namespace" — foreign, or never existed; the caller cannot tell, and
 * that is the point. PERM means "in your namespace, and this surface still
 * will not serve it" — see below.
 *
 * WHAT IS REFUSED INSIDE THE PARTITION, BY NAME.
 * Two object kinds answer a refusal instead of data, each for a reason the
 * tree already states elsewhere:
 *
 *   OBJ_TYPE_SYSTEM_METADATA -> CAT_ERR_PERM. object_catalog.h's own role
 *     table gives APP_USER no read on SYSTEM_METADATA. The catalog does not
 *     re-derive that policy; it refuses by name and lets the tenant see the
 *     refusal, so the answer is visible in a transcript instead of silent.
 *
 *   uses_rowstore == 1 -> CAT_ERR_TYPE on CAT_READ. Phase 24's guard in
 *     sys_sls_select() refuses the legacy record API for a row-set table
 *     and names the SQL path instead; CAT_READ IS the legacy record path,
 *     so it inherits that refusal verbatim. Routing SQL through a mediated
 *     byte channel is a larger design than §9's first cut, and saying so
 *     by name is better than silently returning a legacy view of a table
 *     that no longer keeps its rows there. CAT_STAT still answers for such
 *     an object — its type and size are real metadata, and hiding them
 *     would make the refusal unexplainable.
 *
 * WHAT THIS INCREMENT IS NOT.
 * There is no write path, no schema mutation, no cross-partition federation
 * and no per-object ACL keyed on a uid — a registered channel carries a
 * partition and a pid, not a uid, and inventing an ACL the caller's
 * identity cannot support would be worse than not having one. The isolation
 * this service enforces is the partition, which is the boundary the rest of
 * the project already enforces. Row-level policy is the SQL path's job
 * (rowstore.c's catalog_check_access), and stays there.
 */

#include <stdint.h>

/* ─── the wire ────────────────────────────────────────────────────────────
 * Same 16-byte header shape as user/proto's NetFrame, with a distinct
 * magic: "AEROSCT\x01". The magic is distinct so a NET_* frame arriving on
 * a catalog channel (or a CAT_* frame on a socket channel) is dropped as
 * not-a-frame rather than answered as a verb — the two services can never
 * parse each other's traffic, which is the same reason nss_is_net_frame()
 * exists. user/proto's source of truth for these bytes would be a
 * matching CatFrame; the kernel does not link Rust, so they are written
 * here and the host test pins the layout. */
#define CAT_MAGIC_LEN 8
#define CAT_HDR 16u

/* Every reply is {header[16], status u16, body[...]} — the same shape
 * nss_reply_ok() gives a NET_* reply. status is at CAT_STATUS on EVERY
 * reply, clean or refused (CAT_OK on a clean answer), so a client reads it
 * without first branching on the error flag; the body always starts at
 * CAT_BODY, a verb's own payload on success and the rendered refusal text
 * on an error. A body written over the status word instead would make a
 * two-object CAT_LIST read as status 2 (CAT_ERR_PERM) — the clause
 * env_catalog_host_test.c pins, as literals, for each verb. */
#define CAT_STATUS 16u
#define CAT_BODY   18u

/* Channels with a live catalog registration. M1 targets four concurrent
 * environments; this is headroom, not a design limit — the same arithmetic
 * net_socket_service.h makes for NET_SOCKET_SERVICE_MAX. */
#define ENV_CATALOG_SERVICE_MAX 8

#define CAT_VERB_INFO 1u
#define CAT_VERB_LIST 2u
#define CAT_VERB_STAT 3u
#define CAT_VERB_READ 4u

#define CAT_FLAG_ERROR 0x0001u

#define CAT_OK 0u
/* Not in this partition's namespace. Deliberately covers BOTH "that name
 * never existed" and "that name exists in a partition that is not yours" —
 * see the header's boundary section; distinguishing them would be an
 * existence oracle. */
#define CAT_ERR_NOENT 1u
/* In this partition's namespace, and refused anyway: SYSTEM_METADATA. */
#define CAT_ERR_PERM 2u
/* The frame is malformed by name (short body, an over-long length field). */
#define CAT_ERR_SHAPE 3u
/* The verb is known and the object is in the namespace, but not the kind
 * this verb serves: CAT_READ on a rowstore-backed table. */
#define CAT_ERR_TYPE 4u
/* The verb is outside the table entirely: refused by name, by number. */
#define CAT_ERR_UNKNOWN 9u

#define CAT_MAX_NAME 64u    /* OBJECT_NAME_LEN */
#define CAT_MAX_KEY 64u     /* RECORD_KEY_LEN  */
#define CAT_MAX_VALUE 256u  /* RECORD_VAL_LEN  */

/* CAT_LIST pages, so a reply can never approach the channel's payload
 * bound no matter how full the catalog gets. 8 entries of a 64-byte name
 * is a ~550-byte reply against a 4 KiB receive buffer. */
#define CAT_LIST_PAGE_MAX 8u

#define CAT_TEXT_CAP 192u

/* ─── the service's API ───────────────────────────────────────────────────
 * The registration is the same shape as net_socket_service_register()'s:
 * called from cap_create_sidecar() the moment the kernel end of the
 * tenant's catalog cap is minted, carrying the caller's REAL partition,
 * pid and name from the process descriptor — the caller cannot state its
 * own identity on the wire, so the boundary is computed from one the
 * kernel already verified. Returns 0 if the registry is full. */
int env_catalog_service_register(uint16_t k_rd, uint16_t k_wr,
                                 uint32_t partition, uint32_t pid,
                                 const char* name);

/* console_service_tick() asks this before draining a kernel-held CHAN_R
 * slot: this channel carries CAT_* frames, not console text. Same exclusion
 * shape as env_console_kernel_slot() and net_socket_service_kernel_slot(). */
int env_catalog_service_kernel_slot(uint16_t slot);

/* Drain every registered channel once. Called from
 * microkernel_service_poll(), next to net_socket_service_tick(). Same
 * single-writer argument as that tick: one non-IRQ kernel context. */
void env_catalog_service_tick(void);

uint32_t env_catalog_service_count(void);
uint32_t env_catalog_refusals(void);   /* every non-CAT_OK answer */
uint32_t env_catalog_dropped(void);    /* frames that were not CAT_* at all */

/* The verb-name table — the refusal's "by name" source, and the test's
 * way of asserting that an unknown verb really is unknown. Returns 0 for
 * a type outside the table. */
const char* env_catalog_verb_name(uint16_t ty);

/* The refusal renderer, exported for the host test — same reason
 * net_socket_refusal_text() is: the guard pins the exact string the tick
 * logs and the live clause greps. Writes at most cap-1 bytes and always
 * terminates. */
uint32_t env_catalog_refusal_text(uint16_t ty, char* out, uint32_t cap);

#endif /* ENV_CATALOG_H */
