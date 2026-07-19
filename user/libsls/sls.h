/* user/libsls/sls.h — AeroSLS Ring-3 API
 *
 * Single-header library for user-space programs running under the AeroSLS
 * kernel.  Include this file and link start.S; no other runtime is needed.
 *
 * Syscall ABI:
 *   RAX = syscall number
 *   RDI = single argument (pointer to request struct or scalar)
 *   RAX = return value (0 = success, non-zero = error)
 */
#pragma once
#include <stdint.h>
#include <stddef.h>

/* ── Syscall numbers ────────────────────────────────────────────────────── */
#define SLS_SYS_VALLOC          110   /* create a catalog object              */
#define SLS_SYS_VFREE           111   /* delete a catalog object              */
#define SLS_SYS_SELECT          117   /* read a record field                  */
#define SLS_SYS_UPDATE          118   /* overwrite a record field             */
#define SLS_SYS_INSERT          119   /* insert a new record field            */
#define SLS_SYS_DELETE          143   /* delete a record field                */
#define SLS_SYS_EXIT            164   /* terminate process, return to kernel  */
#define SLS_SYS_SERIAL_WRITE    165   /* write string to kernel serial log    */
#define SLS_SYS_IPC_BIND        166   /* bind process to a user IPC port      */
#define SLS_SYS_IPC_SEND        167   /* send message to any port             */
#define SLS_SYS_IPC_RECV        168   /* non-blocking recv from user port     */
#define SLS_SYS_TIMI_INFO       173   /* structured TIMI object introspection */
/* Gap Remediation Phase G: TIMI/partition syscalls below were reachable from
 * the shell (direct C call into the kernel, same address space) but had no
 * binding here for a real Ring-3 program built against this header and
 * trapping in via `syscall` — see this file's own top comment on the ABI
 * difference. Numbers below match kernel/partition.h and kernel/
 * frame_pool.h exactly; kept in sync by hand since this header has no
 * generation step (same posture as every other syscall number above). */
#define SLS_SYS_PARTITION_CREATE     210  /* define a new partition          */
#define SLS_SYS_PARTITION_ASSIGN     211  /* assign a uid to a partition     */
#define SLS_SYS_PARTITION_LIST       212  /* console dump, no struct back    */
#define SLS_SYS_PARTITION_QUOTA_SET  213  /* set a partition's frame quota   */
#define SLS_SYS_PARTITION_DESTROY    214  /* tear down a partition           */
#define SLS_SYS_PARTITION_PAUSE      215  /* stop scheduling a partition     */
#define SLS_SYS_PARTITION_RESUME     216  /* resume scheduling a partition   */
#define SLS_SYS_PARTITION_QUOTA_LIST 217  /* console dump, no struct back    */

/* ── IPC port ranges ────────────────────────────────────────────────────── */
/* Kernel service ports (0x1001-0x1006) — send-only from Ring-3             */
#define SLS_IPC_PORT_VMMGR    0x1001
#define SLS_IPC_PORT_SECMGR   0x1002
#define SLS_IPC_PORT_DBMGR    0x1003
#define SLS_IPC_PORT_TIERMGR  0x1004
#define SLS_IPC_PORT_LOGMGR   0x1005
#define SLS_IPC_PORT_AGENTMGR 0x1006
/* User ports (0x2000-0x200F) — bind + send + recv                          */
#define SLS_IPC_USER_PORT_FIRST  0x2000
#define SLS_IPC_USER_PORT_LAST   0x200F

/* ── Object type codes (must match kernel OBJ_TYPE_* enum) ─────────────── */
#define SLS_OBJ_SYSTEM_METADATA  0
#define SLS_OBJ_DB_TABLE         1
#define SLS_OBJ_PROGRAM          7
#define SLS_OBJ_STREAM           8

/* ── Field size limits (must match kernel/object_catalog.h) ─────────────── */
#define SLS_NAME_LEN    64
#define SLS_KEY_LEN     64
#define SLS_VAL_LEN    256

/* ── Request structs — layout MUST be identical to kernel definitions ────── */
struct sls_valloc_req {
    char     name[SLS_NAME_LEN];
    uint32_t type;
    uint32_t size_pages;
    uint32_t owner_uid;
    uint32_t perm_mask;
};

struct sls_record_req {
    char name[SLS_NAME_LEN];   /* object name  */
    char key [SLS_KEY_LEN ];   /* field key    */
    char val [SLS_VAL_LEN ];   /* field value  */
};

/* ── TIMI introspection (Gap Remediation Phase G) ────────────────────────── */
/* Layout MUST be identical to kernel/loader.h's TimiEntryRec/TimiNameRec/
 * TimiActivationStatus/TimiInfoResult/SLSTimiInfoRequest. TIMI_ENTRY_NAME_LEN
 * (32) and the TIMI_INFO_MAX_ENTRIES/NAMES cap (16) are inlined as literals
 * here rather than re-#defined, to avoid a second set of names for the same
 * constants in a header with no shared-generation step against the kernel. */
#define SLS_TIMI_INFO_STATUS_OK        0
#define SLS_TIMI_INFO_STATUS_NOT_FOUND 1
#define SLS_TIMI_INFO_STATUS_NOT_TIMI  2
#define SLS_TIMI_INFO_STATUS_CORRUPT   3

struct sls_timi_entry_rec {
    char     name[32];
    uint32_t offset;
} __attribute__((packed));

struct sls_timi_name_rec {
    char name[32];
} __attribute__((packed));

struct sls_timi_activation_status {
    uint8_t  cached;
    uint32_t code_pages;
    uint32_t entry_offset;
    uint32_t content_hash;
};

struct sls_timi_info_result {
    uint32_t status;
    char     format_name[16];
    uint32_t num_instr, num_literals, num_entries, num_names;
    struct sls_timi_entry_rec entries[16];
    uint32_t entries_returned;
    uint8_t  entries_truncated;
    struct sls_timi_name_rec  names[16];
    uint32_t names_returned;
    uint8_t  names_truncated;
    struct sls_timi_activation_status activation;
};

struct sls_timi_info_req {
    char object_name[SLS_NAME_LEN];   /* [in]  must fit PROC_NAME_LEN (64) */
    struct sls_timi_info_result result; /* [out] */
};

/* ── Partitions / LPAR (Gap Remediation Phase G) ─────────────────────────── */
/* Layout MUST be identical to kernel/partition.h / kernel/frame_pool.h. */
#define SLS_PARTITION_INVALID_ID 0xFFFFFFFFu  /* sentinel returned by create on failure */

struct sls_partition_create_req {
    char name[32];   /* PARTITION_NAME_LEN */
};

struct sls_partition_assign_req {
    uint32_t uid;
    uint32_t partition_id;
};

struct sls_partition_quota_set_req {
    uint32_t partition_id;
    uint64_t frame_quota;
};

/* ── Minimal freestanding string utilities ──────────────────────────────── */
static inline size_t sls_strlen(const char *s) {
    size_t n = 0;
    while (s[n]) n++;
    return n;
}

static inline void sls_strncpy(char *dst, const char *src, size_t n) {
    size_t i;
    for (i = 0; i < n && src[i]; i++) dst[i] = src[i];
    for (; i <= n; i++) dst[i] = '\0';
}

static inline void sls_memset(void *dst, int val, size_t n) {
    unsigned char *p = (unsigned char *)dst;
    while (n--) *p++ = (unsigned char)val;
}

/* ── Raw syscall ────────────────────────────────────────────────────────── */
/* Clobber list tells GCC which registers the kernel may modify.
 * RAX = return value (output).  RCX and R11 are trashed by the CPU's
 * SYSCALL/SYSRETQ mechanism.  R8-R10, RSI, RDX are caller-saved in the C
 * ABI and not preserved by our kernel stub — list them so GCC spills any
 * live values around each syscall. */
static inline uint64_t _sls_syscall(uint64_t num, void *arg) {
    uint64_t ret;
    __asm__ volatile (
        "syscall"
        : "=a" (ret)
        : "a"  (num), "D" (arg)
        : "rcx", "r11",
          "r8", "r9", "r10",
          "rsi", "rdx",
          "memory"
    );
    return ret;
}

/* ── Process control ─────────────────────────────────────────────────────── */

/* Terminate the current process and return control to the kernel. */
static inline void __attribute__((noreturn)) sls_exit(int code) {
    _sls_syscall(SLS_SYS_EXIT, (void *)(uintptr_t)(unsigned int)code);
    for (;;) __asm__ volatile ("hlt");
}

/* ── Debug output ────────────────────────────────────────────────────────── */

/* Write a null-terminated string to the kernel serial debug log. */
static inline void sls_puts(const char *s) {
    _sls_syscall(SLS_SYS_SERIAL_WRITE, (void *)s);
}

/* Write a string followed by a newline. */
static inline void sls_println(const char *s) {
    sls_puts(s);
    sls_puts("\n");
}

/* ── Object catalog ──────────────────────────────────────────────────────── */

/* Allocate a new catalog object.  Returns 0 on failure (object_id != 0). */
static inline uint64_t sls_valloc(const char *name, uint32_t type, uint32_t pages) {
    struct sls_valloc_req req;
    sls_memset(&req, 0, sizeof(req));
    sls_strncpy(req.name, name, SLS_NAME_LEN - 1);
    req.type       = type;
    req.size_pages = pages ? pages : 4;
    return _sls_syscall(SLS_SYS_VALLOC, &req);
}

/* ── DB_TABLE record operations ──────────────────────────────────────────── */

/* Insert a new key-value field into a DB_TABLE object.
 * Returns 0 on success. */
static inline int sls_insert(const char *object, const char *key, const char *val) {
    struct sls_record_req req;
    sls_memset(&req, 0, sizeof(req));
    sls_strncpy(req.name, object, SLS_NAME_LEN - 1);
    sls_strncpy(req.key,  key,    SLS_KEY_LEN  - 1);
    sls_strncpy(req.val,  val,    SLS_VAL_LEN  - 1);
    return (int)_sls_syscall(SLS_SYS_INSERT, &req);
}

/* Update an existing key-value field in a DB_TABLE object.
 * Returns 0 on success. */
static inline int sls_update(const char *object, const char *key, const char *val) {
    struct sls_record_req req;
    sls_memset(&req, 0, sizeof(req));
    sls_strncpy(req.name, object, SLS_NAME_LEN - 1);
    sls_strncpy(req.key,  key,    SLS_KEY_LEN  - 1);
    sls_strncpy(req.val,  val,    SLS_VAL_LEN  - 1);
    return (int)_sls_syscall(SLS_SYS_UPDATE, &req);
}

/* Read a key-value field from a DB_TABLE object.
 * The value is written into 'out' (up to outlen-1 bytes).
 * Returns 0 on success. */
static inline int sls_select(const char *object, const char *key,
                              char *out, size_t outlen) {
    struct sls_record_req req;
    sls_memset(&req, 0, sizeof(req));
    sls_strncpy(req.name, object, SLS_NAME_LEN - 1);
    sls_strncpy(req.key,  key,    SLS_KEY_LEN  - 1);
    int rc = (int)_sls_syscall(SLS_SYS_SELECT, &req);
    if (rc == 0 && out && outlen > 0) {
        sls_strncpy(out, req.val, outlen - 1);
        out[outlen - 1] = '\0';
    }
    return rc;
}

/* Delete a key-value field from a DB_TABLE object.
 * Returns 0 on success. */
static inline int sls_delete(const char *object, const char *key) {
    struct sls_record_req req;
    sls_memset(&req, 0, sizeof(req));
    sls_strncpy(req.name, object, SLS_NAME_LEN - 1);
    sls_strncpy(req.key,  key,    SLS_KEY_LEN  - 1);
    return (int)_sls_syscall(SLS_SYS_DELETE, &req);
}

/* ── IPC ────────────────────────────────────────────────────────────────── */

/* Message struct — layout MUST match kernel struct IPCMessage in ipc.h */
struct sls_ipc_msg {
    uint16_t src_port;
    uint16_t dst_port;
    uint32_t opcode;
    uint64_t payload[4];
    uint32_t reply_token;
    uint8_t  consumed;
    uint8_t  _pad[3];
};

/* Send-request struct — layout matches kernel struct IPCUserSendReq */
struct sls_ipc_send_req {
    uint16_t  dst_port;
    uint16_t  _pad0;
    uint32_t  opcode;
    uint64_t  payload[4];
};

/* Recv-request struct — layout matches kernel struct IPCUserRecvReq */
struct sls_ipc_recv_req {
    uint16_t          port;    /* [in]  user port to receive from */
    uint8_t           _pad[6];
    struct sls_ipc_msg msg;    /* [out] filled on successful recv */
};

/* Bind the calling process to a user IPC port (0x2000–0x200F).
 * Only one process per port.  Returns 0 on success. */
static inline int sls_ipc_bind(uint16_t port) {
    return (int)_sls_syscall(SLS_SYS_IPC_BIND, (void *)(uintptr_t)port);
}

/* Send a message to any port.
 * dst 0x1001-0x1006 = kernel service; 0x2000-0x200F = user process.
 * Returns 0 on success, non-zero on queue-full or bad port. */
static inline int sls_ipc_send(uint16_t dst, uint32_t opcode,
                                uint64_t a0, uint64_t a1,
                                uint64_t a2, uint64_t a3) {
    struct sls_ipc_send_req req;
    sls_memset(&req, 0, sizeof(req));
    req.dst_port   = dst;
    req.opcode     = opcode;
    req.payload[0] = a0; req.payload[1] = a1;
    req.payload[2] = a2; req.payload[3] = a3;
    return (int)_sls_syscall(SLS_SYS_IPC_SEND, &req);
}

/* Non-blocking receive from a user port.
 * Returns 1 and fills *out if a message is available; returns 0 if empty.
 * Typical usage: spin until sls_ipc_recv() returns 1 (preemptive timer
 * will switch to other processes while spinning). */
static inline int sls_ipc_recv(uint16_t port, struct sls_ipc_msg *out) {
    struct sls_ipc_recv_req req;
    sls_memset(&req, 0, sizeof(req));
    req.port = port;
    int rc = (int)_sls_syscall(SLS_SYS_IPC_RECV, &req);
    if (rc && out) {
        uint8_t *src = (uint8_t *)&req.msg;
        uint8_t *dst = (uint8_t *)out;
        for (size_t i = 0; i < sizeof(*out); i++) dst[i] = src[i];
    }
    return rc;
}

/* ── TIMI introspection (Gap Remediation Phase G) ────────────────────────── */

/* Query a TIMI object's header, entries, exported names, and activation-
 * cache status. Fills *out (always -- same "no partial result" contract as
 * the kernel's own loader_timi_info_query()) and returns 0 if
 * out->status == SLS_TIMI_INFO_STATUS_OK, 1 otherwise (the full detail is
 * always in out->status regardless of the return value). */
static inline int sls_timi_info(const char *object_name,
                                 struct sls_timi_info_result *out) {
    struct sls_timi_info_req req;
    sls_memset(&req, 0, sizeof(req));
    sls_strncpy(req.object_name, object_name, SLS_NAME_LEN - 1);
    int rc = (int)_sls_syscall(SLS_SYS_TIMI_INFO, &req);
    if (out) *out = req.result;
    return rc;
}

/* ── Partitions / LPAR (Gap Remediation Phase G) ─────────────────────────── */

/* Define a new partition. Returns its id, or SLS_PARTITION_INVALID_ID if
 * the partition table is full (mirrors kernel/partition.h's own sentinel;
 * see this file's own top comment on why it isn't a named kernel-side
 * constant either). */
static inline uint64_t sls_partition_create(const char *name) {
    struct sls_partition_create_req req;
    sls_memset(&req, 0, sizeof(req));
    sls_strncpy(req.name, name, sizeof(req.name) - 1);
    return _sls_syscall(SLS_SYS_PARTITION_CREATE, &req);
}

/* Assign a uid to a partition. Returns 0 on success. */
static inline int sls_partition_assign(uint32_t uid, uint32_t partition_id) {
    struct sls_partition_assign_req req;
    req.uid          = uid;
    req.partition_id = partition_id;
    return (int)_sls_syscall(SLS_SYS_PARTITION_ASSIGN, &req);
}

/* Tear down a partition (kills its processes, vfrees its objects).
 * Returns 0 on success. */
static inline int sls_partition_destroy(uint32_t partition_id) {
    return (int)_sls_syscall(SLS_SYS_PARTITION_DESTROY,
                              (void *)(uintptr_t)partition_id);
}

/* Stop scheduling a partition. Returns 0 on success. */
static inline int sls_partition_pause(uint32_t partition_id) {
    return (int)_sls_syscall(SLS_SYS_PARTITION_PAUSE,
                              (void *)(uintptr_t)partition_id);
}

/* Resume scheduling a partition. Returns 0 on success. */
static inline int sls_partition_resume(uint32_t partition_id) {
    return (int)_sls_syscall(SLS_SYS_PARTITION_RESUME,
                              (void *)(uintptr_t)partition_id);
}

/* Set a partition's frame quota (0 = unlimited). Returns 0 on success. */
static inline int sls_partition_quota_set(uint32_t partition_id,
                                           uint64_t frame_quota) {
    struct sls_partition_quota_set_req req;
    req.partition_id = partition_id;
    req.frame_quota  = frame_quota;
    return (int)_sls_syscall(SLS_SYS_PARTITION_QUOTA_SET, &req);
}

/* List all defined partitions / per-partition usage+quota. Neither syscall
 * returns structured data to the caller (console-dump only, same as
 * SYS_SLS_PROC_LIST/SYS_SLS_OBJ_LIST) -- see loader.h's own comment on why
 * only loader_timi_info() got the structured-data treatment in Phase G,
 * not this pair. Kept here anyway so a native program can at least trigger
 * the dump without hand-rolling the trap. */
static inline void sls_partition_list(void) {
    _sls_syscall(SLS_SYS_PARTITION_LIST, 0);
}
static inline void sls_partition_quota_list(void) {
    _sls_syscall(SLS_SYS_PARTITION_QUOTA_LIST, 0);
}
