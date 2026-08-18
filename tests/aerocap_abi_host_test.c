/* tests/aerocap_abi_host_test.c — pins user/libaerocap/aerocap.h to
 * kernel/cap.h, and user/libaerocap/aerocap_provision.h to
 * kernel/partition.h + kernel/object_catalog.h + kernel/loader.h
 *
 * The Ring-3 SDK (user/libaerocap/aerocap.h) mirrors the kernel's syscall
 * ABI by hand: request structs byte-for-byte identical to the kernel's
 * SLSCap*Request, plus the syscall numbers (289-297), CAP_PERM_* masks,
 * error codes, and CAP_NONE. The provisioning skin
 * (user/libaerocap/aerocap_provision.h) does the same for the partition-
 * lifecycle and object-catalog syscalls. There is no generation step
 * between the headers, so this test holds them together: it includes BOTH
 * sides and asserts sizeof/offsetof equality for every field of every
 * request struct, and numeric equality for every shared constant.
 *
 * The capability headers cannot coexist in one TU with their macro names
 * intact (both define CAP_NONE / CAP_PERM_* / CAP_E* — identical by
 * design), so the kernel's copies are captured into consts first, then
 * #undef'd before the SDK header is included; the asserts then prove the
 * SDK values equal the captured kernel values. The provisioning headers
 * use distinct identifiers (SLS_SYS_*, SLS_OBJ_PROGRAM, SLS_PERM_* vs the
 * kernel's SYS_SLS_*, OBJ_TYPE_PROGRAM, PERM_*), so both sides coexist
 * without any renaming — the partition lifecycle structs and sls_valloc_req
 * come from libsls/sls.h, and the upload struct from aerocap_provision.h.
 *
 * Build and run:
 *   gcc -std=c11 -Wall -Wextra -I. -Ikernel -Iuser/libsls -Iuser/libaerocap \
 *       tests/aerocap_abi_host_test.c -o /tmp/aerocap_abi
 *   /tmp/aerocap_abi
 *
 * (The run step is on its own line on purpose: tests/run_all.sh folds the
 * gcc continuation lines into one compile command, so a "&& /tmp/..." on
 * the last continuation line makes the suite depend on a stale /tmp
 * binary existing -- same convention as tests/sls_alloc_host_test.c.)
 */
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

/* Both headers legitimately define functions named cap_* (the kernel's take
 * an explicit pid; the SDK's hide it). They never coexist in a real program
 * — user code includes only aerocap.h — but this test includes both to
 * compare struct layouts, so the kernel-side declarations are renamed out of
 * the way first. */
#define cap_create_mem  kern_cap_create_mem
#define cap_arena_alloc kern_cap_arena_alloc
#define cap_chan_create kern_cap_chan_create
#define cap_send        kern_cap_send
#define cap_recv        kern_cap_recv
#define cap_revoke      kern_cap_revoke
#define cap_map         kern_cap_map
#define cap_unmap       kern_cap_unmap
#define cap_list        kern_cap_list
#include "../kernel/cap.h"
#undef cap_create_mem
#undef cap_arena_alloc
#undef cap_chan_create
#undef cap_send
#undef cap_recv
#undef cap_revoke
#undef cap_map
#undef cap_unmap
#undef cap_list

/* Capture the kernel's values before the SDK header redefines the names. */
static const int    kern_cap_none   = CAP_NONE;
static const int    kern_perm_r     = CAP_PERM_R;
static const int    kern_perm_w     = CAP_PERM_W;
static const int    kern_perm_x     = CAP_PERM_X;
static const int    kern_perm_map   = CAP_PERM_MAP;
static const int    kern_e_inval    = CAP_EINVAL;
static const int    kern_e_badf     = CAP_EBADF;
static const int    kern_e_again    = CAP_EAGAIN;
static const int    kern_e_tableful = CAP_ETABLEFULL;
static const int    kern_e_revoked  = CAP_ECAPREVOKED;
static const int    kern_e_already  = CAP_EALREADY;
static const int    kern_e_nomem    = CAP_ENOMEM;
static const int    kern_e_nospc    = CAP_ENOSPC;
static const int    kern_e_range    = CAP_ERANGE;
static const int    kern_e_nosys    = CAP_ENOSYS;
static const int    kern_e_conflict = CAP_ECONFLICT;

/* Syscall numbers: kernel names them SYS_SLS_CAP_*, the SDK SLS_SYS_CAP_*
 * (different identifiers, same numbers — no undef needed, only equality). */
static const int    kern_sys_create = SYS_SLS_CAP_CREATE_MEM;
static const int    kern_sys_arena  = SYS_SLS_CAP_ARENA_ALLOC;
static const int    kern_sys_chan   = SYS_SLS_CHAN_CREATE;
static const int    kern_sys_send   = SYS_SLS_CAP_SEND;
static const int    kern_sys_recv   = SYS_SLS_CAP_RECV;
static const int    kern_sys_revoke = SYS_SLS_CAP_REVOKE;
static const int    kern_sys_map    = SYS_SLS_CAP_MAP;
static const int    kern_sys_unmap  = SYS_SLS_CAP_UNMAP;
static const int    kern_sys_list   = SYS_SLS_CAP_LIST;

/* The SDK redefines these names with the same values by design. */
#undef CAP_NONE
#undef CAP_PERM_R
#undef CAP_PERM_W
#undef CAP_PERM_X
#undef CAP_PERM_MAP
#undef CAP_EINVAL
#undef CAP_EBADF
#undef CAP_EAGAIN
#undef CAP_ETABLEFULL
#undef CAP_ECAPREVOKED
#undef CAP_EALREADY
#undef CAP_ENOMEM
#undef CAP_ENOSPC
#undef CAP_ERANGE
#undef CAP_ENOSYS
#undef CAP_ECONFLICT

#include "../user/libaerocap/aerocap.h"

/* ── Provisioning ABI: kernel headers first, then the SDK skin ──────────────
 * The identifiers do not collide with anything above (SLS_SYS_* / SLS_OBJ_
 * TYPE_* / SLS_PERM_* vs SYS_SLS_* / OBJ_TYPE_* / PERM_*), so both can sit
 * in the same TU. loader.h pulls process.h (PROC_NAME_LEN) and
 * simi_translate.h — all header-only, fine for a host compile. */
#include "../kernel/partition.h"
#include "../kernel/object_catalog.h"
#include "../kernel/loader.h"
#include "../user/permissions.h"
#include "../user/libaerocap/aerocap_provision.h"

#define CHECK(cond, msg) do {                                   \
    if (!(cond)) {                                              \
        printf("FAIL: %s\n", msg);                              \
        failures++;                                             \
    }                                                           \
} while (0)

int main(void) {
    int failures = 0;

    /* ── request struct layout: size + every field's offset ─────────────── */
    CHECK(sizeof(struct sls_cap_create_mem_req) == sizeof(struct SLSCapCreateMemRequest),
          "create_mem req size");
    CHECK(offsetof(struct sls_cap_create_mem_req, phys_base) == offsetof(struct SLSCapCreateMemRequest, phys_base),
          "create_mem.phys_base offset");
    CHECK(offsetof(struct sls_cap_create_mem_req, npages) == offsetof(struct SLSCapCreateMemRequest, npages),
          "create_mem.npages offset");
    CHECK(offsetof(struct sls_cap_create_mem_req, perm) == offsetof(struct SLSCapCreateMemRequest, perm),
          "create_mem.perm offset");

    CHECK(sizeof(struct sls_cap_arena_alloc_req) == sizeof(struct SLSCapArenaAllocRequest),
          "arena_alloc req size");
    CHECK(offsetof(struct sls_cap_arena_alloc_req, npages) == offsetof(struct SLSCapArenaAllocRequest, npages),
          "arena_alloc.npages offset");
    CHECK(offsetof(struct sls_cap_arena_alloc_req, perm) == offsetof(struct SLSCapArenaAllocRequest, perm),
          "arena_alloc.perm offset");

    CHECK(sizeof(struct sls_cap_chan_create_req) == sizeof(struct SLSCapChanCreateRequest),
          "chan_create req size");
    CHECK(offsetof(struct sls_cap_chan_create_req, far_pid) == offsetof(struct SLSCapChanCreateRequest, far_pid),
          "chan_create.far_pid offset");
    CHECK(offsetof(struct sls_cap_chan_create_req, out_rd) == offsetof(struct SLSCapChanCreateRequest, out_rd),
          "chan_create.out_rd offset");
    CHECK(offsetof(struct sls_cap_chan_create_req, out_wr) == offsetof(struct SLSCapChanCreateRequest, out_wr),
          "chan_create.out_wr offset");
    CHECK(offsetof(struct sls_cap_chan_create_req, out_far_rd) == offsetof(struct SLSCapChanCreateRequest, out_far_rd),
          "chan_create.out_far_rd offset");
    CHECK(offsetof(struct sls_cap_chan_create_req, out_far_wr) == offsetof(struct SLSCapChanCreateRequest, out_far_wr),
          "chan_create.out_far_wr offset");

    CHECK(sizeof(struct sls_cap_send_req) == sizeof(struct SLSCapSendRequest),
          "send req size");
    CHECK(offsetof(struct sls_cap_send_req, ch_w_idx) == offsetof(struct SLSCapSendRequest, ch_w_idx),
          "send.ch_w_idx offset");
    CHECK(offsetof(struct sls_cap_send_req, cap_idx) == offsetof(struct SLSCapSendRequest, cap_idx),
          "send.cap_idx offset");
    CHECK(offsetof(struct sls_cap_send_req, cookie) == offsetof(struct SLSCapSendRequest, cookie),
          "send.cookie offset");

    CHECK(sizeof(struct sls_cap_recv_req) == sizeof(struct SLSCapRecvRequest),
          "recv req size");
    CHECK(offsetof(struct sls_cap_recv_req, ch_r_idx) == offsetof(struct SLSCapRecvRequest, ch_r_idx),
          "recv.ch_r_idx offset");
    CHECK(offsetof(struct sls_cap_recv_req, block) == offsetof(struct SLSCapRecvRequest, block),
          "recv.block offset");
    CHECK(offsetof(struct sls_cap_recv_req, cookie) == offsetof(struct SLSCapRecvRequest, cookie),
          "recv.cookie offset");

    CHECK(sizeof(struct sls_cap_revoke_req) == sizeof(struct SLSCapRevokeRequest),
          "revoke req size");
    CHECK(offsetof(struct sls_cap_revoke_req, cap_idx) == offsetof(struct SLSCapRevokeRequest, cap_idx),
          "revoke.cap_idx offset");

    CHECK(sizeof(struct sls_cap_map_req) == sizeof(struct SLSCapMapRequest),
          "map req size");
    CHECK(offsetof(struct sls_cap_map_req, cap_idx) == offsetof(struct SLSCapMapRequest, cap_idx),
          "map.cap_idx offset");
    CHECK(offsetof(struct sls_cap_map_req, vaddr) == offsetof(struct SLSCapMapRequest, vaddr),
          "map.vaddr offset");
    CHECK(offsetof(struct sls_cap_map_req, flags) == offsetof(struct SLSCapMapRequest, flags),
          "map.flags offset");

    CHECK(sizeof(struct sls_cap_unmap_req) == sizeof(struct SLSCapUnmapRequest),
          "unmap req size");
    CHECK(offsetof(struct sls_cap_unmap_req, cap_idx) == offsetof(struct SLSCapUnmapRequest, cap_idx),
          "unmap.cap_idx offset");
    CHECK(offsetof(struct sls_cap_unmap_req, vaddr) == offsetof(struct SLSCapUnmapRequest, vaddr),
          "unmap.vaddr offset");

    /* ── shared constants ───────────────────────────────────────────────── */
    CHECK(CAP_NONE        == kern_cap_none,   "CAP_NONE");
    CHECK(CAP_PERM_R      == kern_perm_r,     "CAP_PERM_R");
    CHECK(CAP_PERM_W      == kern_perm_w,     "CAP_PERM_W");
    CHECK(CAP_PERM_X      == kern_perm_x,     "CAP_PERM_X");
    CHECK(CAP_PERM_MAP    == kern_perm_map,   "CAP_PERM_MAP");
    CHECK(CAP_EINVAL      == kern_e_inval,    "CAP_EINVAL");
    CHECK(CAP_EBADF       == kern_e_badf,     "CAP_EBADF");
    CHECK(CAP_EAGAIN      == kern_e_again,    "CAP_EAGAIN");
    CHECK(CAP_ETABLEFULL  == kern_e_tableful, "CAP_ETABLEFULL");
    CHECK(CAP_ECAPREVOKED == kern_e_revoked,  "CAP_ECAPREVOKED");
    CHECK(CAP_EALREADY    == kern_e_already,  "CAP_EALREADY");
    CHECK(CAP_ENOMEM      == kern_e_nomem,    "CAP_ENOMEM");
    CHECK(CAP_ENOSPC      == kern_e_nospc,    "CAP_ENOSPC");
    CHECK(CAP_ERANGE      == kern_e_range,    "CAP_ERANGE");
    CHECK(CAP_ENOSYS      == kern_e_nosys,    "CAP_ENOSYS");
    CHECK(CAP_ECONFLICT   == kern_e_conflict, "CAP_ECONFLICT");

    /* ── syscall numbers ────────────────────────────────────────────────── */
    CHECK(SLS_SYS_CAP_CREATE_MEM  == kern_sys_create, "SYS_SLS_CAP_CREATE_MEM");
    CHECK(SLS_SYS_CAP_ARENA_ALLOC == kern_sys_arena,  "SYS_SLS_CAP_ARENA_ALLOC");
    CHECK(SLS_SYS_CHAN_CREATE     == kern_sys_chan,   "SYS_SLS_CHAN_CREATE");
    CHECK(SLS_SYS_CAP_SEND        == kern_sys_send,   "SYS_SLS_CAP_SEND");
    CHECK(SLS_SYS_CAP_RECV        == kern_sys_recv,   "SYS_SLS_CAP_RECV");
    CHECK(SLS_SYS_CAP_REVOKE      == kern_sys_revoke, "SYS_SLS_CAP_REVOKE");
    CHECK(SLS_SYS_CAP_MAP         == kern_sys_map,    "SYS_SLS_CAP_MAP");
    CHECK(SLS_SYS_CAP_UNMAP       == kern_sys_unmap,  "SYS_SLS_CAP_UNMAP");
    CHECK(SLS_SYS_CAP_LIST        == kern_sys_list,   "SYS_SLS_CAP_LIST");

    /* ── provisioning request structs (aerocap_provision.h) ────────────── */
    CHECK(sizeof(struct sls_partition_create_req) == sizeof(struct SLSPartitionCreateRequest),
          "partition_create req size");
    CHECK(offsetof(struct sls_partition_create_req, name) == offsetof(struct SLSPartitionCreateRequest, name),
          "partition_create.name offset");

    CHECK(sizeof(struct sls_partition_assign_req) == sizeof(struct SLSPartitionAssignRequest),
          "partition_assign req size");
    CHECK(offsetof(struct sls_partition_assign_req, uid) == offsetof(struct SLSPartitionAssignRequest, uid),
          "partition_assign.uid offset");
    CHECK(offsetof(struct sls_partition_assign_req, partition_id) == offsetof(struct SLSPartitionAssignRequest, partition_id),
          "partition_assign.partition_id offset");

    CHECK(sizeof(struct sls_valloc_req) == sizeof(struct SLSVallocRequest),
          "valloc req size");
    CHECK(offsetof(struct sls_valloc_req, name) == offsetof(struct SLSVallocRequest, name),
          "valloc.name offset");
    CHECK(offsetof(struct sls_valloc_req, type) == offsetof(struct SLSVallocRequest, type),
          "valloc.type offset");
    CHECK(offsetof(struct sls_valloc_req, size_pages) == offsetof(struct SLSVallocRequest, size_pages),
          "valloc.size_pages offset");
    CHECK(offsetof(struct sls_valloc_req, owner_uid) == offsetof(struct SLSVallocRequest, owner_uid),
          "valloc.owner_uid offset");
    CHECK(offsetof(struct sls_valloc_req, perm_mask) == offsetof(struct SLSVallocRequest, perm_mask),
          "valloc.perm_mask offset");
    CHECK(offsetof(struct sls_valloc_req, partition_id) == offsetof(struct SLSVallocRequest, partition_id),
          "valloc.partition_id offset");
    CHECK(offsetof(struct sls_valloc_req, database_id) == offsetof(struct SLSVallocRequest, database_id),
          "valloc.database_id offset");

    CHECK(sizeof(struct sls_upload_req) == sizeof(struct SLSUploadRequest),
          "upload req size");
    CHECK(offsetof(struct sls_upload_req, object_name) == offsetof(struct SLSUploadRequest, object_name),
          "upload.object_name offset");
    CHECK(offsetof(struct sls_upload_req, chunk) == offsetof(struct SLSUploadRequest, chunk),
          "upload.chunk offset");
    CHECK(offsetof(struct sls_upload_req, chunk_len) == offsetof(struct SLSUploadRequest, chunk_len),
          "upload.chunk_len offset");
    CHECK(offsetof(struct sls_upload_req, byte_offset) == offsetof(struct SLSUploadRequest, byte_offset),
          "upload.byte_offset offset");
    CHECK(offsetof(struct sls_upload_req, is_last) == offsetof(struct SLSUploadRequest, is_last),
          "upload.is_last offset");

    /* ── provisioning shared constants ──────────────────────────────────── */
    CHECK(SLS_SYS_PARTITION_CREATE  == SYS_SLS_PARTITION_CREATE,  "SYS_SLS_PARTITION_CREATE");
    CHECK(SLS_SYS_PARTITION_ASSIGN  == SYS_SLS_PARTITION_ASSIGN,  "SYS_SLS_PARTITION_ASSIGN");
    CHECK(SLS_SYS_PARTITION_DESTROY == SYS_SLS_PARTITION_DESTROY, "SYS_SLS_PARTITION_DESTROY");
    CHECK(SLS_SYS_VALLOC            == SYS_SLS_VALLOC,            "SYS_SLS_VALLOC");
    CHECK(SLS_SYS_UPLOAD_BINARY     == SYS_SLS_UPLOAD_BINARY,     "SYS_SLS_UPLOAD_BINARY");
    /* The partition name length lives inline in sls.h's create_req
     * (char name[32]); its struct-size pin above covers the layout. */
    CHECK(SLS_NAME_LEN              == OBJECT_NAME_LEN,           "OBJECT_NAME_LEN");
    CHECK(SLS_UPLOAD_CHUNK_MAX      == UPLOAD_CHUNK_MAX,          "UPLOAD_CHUNK_MAX");
    CHECK(SLS_OBJ_PROGRAM           == OBJ_TYPE_PROGRAM,          "OBJ_TYPE_PROGRAM");
    CHECK(SLS_PERM_READ             == PERM_READ,                 "PERM_READ");
    CHECK(SLS_PERM_WRITE            == PERM_WRITE,                "PERM_WRITE");
    CHECK(SLS_PERM_EXECUTE          == PERM_EXECUTE,              "PERM_EXECUTE");
    CHECK(SLS_PERM_OWNER            == PERM_OWNER,                "PERM_OWNER");

    if (failures == 0) {
        printf("ALL PASS: libaerocap ABI matches kernel/cap.h and the provisioning headers\n");
        return 0;
    }
    printf("%d FAILURE(S): libaerocap ABI drifted from the kernel headers\n", failures);
    return 1;
}
