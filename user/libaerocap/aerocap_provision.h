/* user/libaerocap/aerocap_provision.h — AeroSLS Ring-3 provisioning SDK
 *
 * The skin over the object-catalog / program-upload syscalls a ring-3
 * program needs to provision children INSIDE a fresh partition. It
 * complements libsls/sls.h, which already owns the partition LIFECYCLE
 * wrappers (sls_partition_create / sls_partition_assign /
 * sls_partition_destroy / sls_partition_pause / sls_partition_resume) and
 * the request structs they use (sls_partition_create_req /
 * sls_partition_assign_req / sls_valloc_req — see sls.h's
 * "Partitions / LPAR" section). This header adds the two pieces sls.h does
 * NOT have:
 *
 *   sls_obj_valloc()    — a partition-AWARE object creation (the sls.h
 *                         sls_valloc() can only create uid-0, default-
 *                         partition objects; it cannot set owner_uid,
 *                         perm_mask, or partition_id).
 *   sls_upload_binary() — write a program's binary bytes into an object.
 *   sls_obj_vfree()     — delete an object AND its binary-store slot (the
 *                         destroy-time companion to sls_obj_valloc(); the
 *                         slot free is what makes a later valloc + upload
 *                         of the same name start fresh).
 *
 * A program that provisions partitions and children includes this header
 * (which pulls in sls.h) and calls sls_partition_create / assign / destroy
 * (sls.h) plus sls_obj_valloc / sls_upload_binary / sls_obj_vfree (here);
 * it never sees a request struct or a raw syscall number.
 *
 * Conventions (matching kernel/object_catalog.h and kernel/loader.h — kept
 * in sync by hand, same posture as aerocap.h):
 *   return value = 0 on success (out-params filled), -1 on failure
 *   sls_obj_valloc returns the new object id via *out_obj_id
 *
 * Every request struct is byte-for-byte identical to the kernel's
 * SLSVallocRequest / SLSUploadRequest. tests/aerocap_abi_host_test.c pins
 * that identity (plus sls.h's partition structs) with sizeof/offsetof
 * comparisons against the kernel headers, exactly as it does for
 * aerocap.h vs kernel/cap.h.
 *
 * Why this exists (the constraint that forced ring-3 object creation):
 * HTTP-uploaded program objects are always owned by uid 0, so they live
 * in PARTITION_SYSTEM — and catalog_check_access()'s partition boundary
 * denies every spawn from inside a non-zero partition. The ONLY way to get
 * a spawnable object in a fresh partition is to create it from ring-3 with
 * owner_uid = the uid assigned to that partition (partition_id 0 defaults
 * to the owner's current partition) and upload its binary from ring-3.
 *
 * Ring-3 caveat (flat-binary memory model): the flat program image is
 * mapped WITHOUT the WRITE bit, so ring-3 statics/globals are read-only —
 * a global write faults. sls_upload_binary() builds its 16,457-byte
 * request on the CALLER'S stack (never a global), which is why the ring-3
 * stack must be >= 32 KiB (PROC_USER_STACK_PAGES = 8; the 16 KiB stack
 * could not hold SLSUploadRequest — see loader.h).
 *
 * Build:
 *   make user-programs          (USER_CFLAGS gains -Iuser/libaerocap)
 */

#ifndef AEROCAP_PROVISION_H
#define AEROCAP_PROVISION_H

#include <sls.h>   /* _sls_syscall, sls_memset, sls_strncpy, SLS_SYS_*,
                    * SLS_OBJ_PROGRAM, sls_valloc_req, sls_partition_* */

#ifdef __cplusplus
extern "C" {
#endif

/* ── Syscall numbers not already in sls.h (MUST match kernel/loader.h) ─────── */
#define SLS_SYS_UPLOAD_BINARY     171

/* ── Limits (MUST match kernel/loader.h UPLOAD_CHUNK_MAX) ──────────────────── */
#define SLS_UPLOAD_CHUNK_MAX      16384

/* ── Permission bits (MUST match user/permissions.h PERM_*) ────────────────── */
#define SLS_PERM_READ            (1u << 0)
#define SLS_PERM_WRITE           (1u << 1)
#define SLS_PERM_EXECUTE         (1u << 2)
#define SLS_PERM_OWNER           (1u << 3)

/* ── Request struct — layout MUST be identical to kernel/loader.h's
 *    SLSUploadRequest. (sls_valloc_req comes from sls.h.) ──────────────────── */
struct sls_upload_req {
    char     object_name[SLS_NAME_LEN];          /* PROC_NAME_LEN = 64 */
    uint8_t  chunk[SLS_UPLOAD_CHUNK_MAX];        /* 16 KiB inline — see header note */
    uint32_t chunk_len;
    uint32_t byte_offset;     /* position in the binary this chunk starts at */
    uint8_t  is_last;         /* 1 = final chunk; finalises the binary size */
};

/* ── Wrappers ──────────────────────────────────────────────────────────────── */

/* Create a catalog object with full control over ownership and partition —
 * the operation sls.h's sls_valloc() cannot express. partition_id 0
 * defaults to owner_uid's CURRENT partition (partition_get_for_uid) — THE
 * way to get an object inside a fresh partition (see the header comment).
 * Returns 0 with *out_obj_id set, or -1 (name invalid or already exists /
 * catalog full / size_pages 0). */
static inline int sls_obj_valloc(const char* name, uint32_t type,
                                 uint32_t size_pages, uint32_t owner_uid,
                                 uint32_t perm_mask, uint32_t partition_id,
                                 uint64_t* out_obj_id) {
    struct sls_valloc_req req;
    sls_memset(&req, 0, sizeof(req));
    sls_strncpy(req.name, name, sizeof(req.name) - 1);
    req.type         = type;
    req.size_pages   = size_pages;
    req.owner_uid    = owner_uid;
    req.perm_mask    = perm_mask;
    req.partition_id = partition_id;
    req.database_id  = 0;
    uint64_t r = _sls_syscall(SLS_SYS_VALLOC, &req);
    if (r == 0) return -1;
    if (out_obj_id) *out_obj_id = r;
    return 0;
}

/* Delete a catalog object and its binary-store slot (SYS_SLS_VFREE) — the
 * destroy-time companion to sls_obj_valloc(). Phase 14a: the slot free is
 * what guarantees a later valloc + upload of the same name starts
 * byte-for-byte fresh (without it, a smaller re-upload would inherit the
 * previous binary's stale size). Returns 0 on success, -1 if no such
 * object. */
static inline int sls_obj_vfree(const char* name) {
    if (!name || !name[0]) return -1;
    return (int)(int64_t)_sls_syscall(SLS_SYS_VFREE, (void*)name);
}

/* Write one chunk of binary data into the object's store. A single chunk
 * (offset 0, is_last 1) suffices for any binary <= 16 KiB. Returns 0, or -1
 * (len == 0 / len > SLS_UPLOAD_CHUNK_MAX / store full / no such object).
 *
 * Stack note: the 16,457-byte request struct is allocated HERE, on the
 * caller's stack — never as a global (flat binaries map read-only). The
 * ring-3 stack must be >= 32 KiB (PROC_USER_STACK_PAGES = 8). */
static inline int sls_upload_binary(const char* object_name,
                                    const uint8_t* data, uint32_t len,
                                    uint32_t byte_offset, int is_last) {
    if (!data || len == 0 || len > SLS_UPLOAD_CHUNK_MAX) return -1;
    struct sls_upload_req req;
    sls_memset(&req, 0, sizeof(req));
    sls_strncpy(req.object_name, object_name, sizeof(req.object_name) - 1);
    for (uint32_t i = 0; i < len; i++) req.chunk[i] = data[i];
    req.chunk_len   = len;
    req.byte_offset = byte_offset;
    req.is_last     = is_last ? 1 : 0;
    int64_t r = (int64_t)_sls_syscall(SLS_SYS_UPLOAD_BINARY, &req);
    return r == 0 ? 0 : -1;
}

#ifdef __cplusplus
}
#endif

#endif /* AEROCAP_PROVISION_H */
