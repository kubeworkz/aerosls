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
