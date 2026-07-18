#include "lock_mgr.h"
#include "kernel_io.h"

// ─── Globals ──────────────────────────────────────────────────────────────────
struct LockEntry lock_table[LOCK_MAX_ENTRIES];
uint32_t         lock_entry_count = 0;

// ─── Internal helpers ─────────────────────────────────────────────────────────
static void lk_copy(char* dst, const char* src, int n) {
    int i = 0;
    for (; i < n - 1 && src[i]; i++) dst[i] = src[i];
    dst[i] = '\0';
}

static int lk_eq(const char* a, const char* b) {
    while (*a && *b) { if (*a != *b) return 0; a++; b++; }
    return *a == *b;
}

// ─── lock_mgr_init ────────────────────────────────────────────────────────────
void lock_mgr_init(void) {
    for (int i = 0; i < LOCK_MAX_ENTRIES; i++) lock_table[i].active = 0;
    lock_entry_count = 0;
    kernel_serial_print("[LOCK] Row-level lock manager initialised.\n");
}

// ─── lock_acquire ─────────────────────────────────────────────────────────────
int lock_acquire(uint64_t tx_id, uint64_t object_id, const char* key,
                 LockType type)
{
    // Check for conflicts: scan active locks on the same (object_id, key)
    for (int i = 0; i < LOCK_MAX_ENTRIES; i++) {
        struct LockEntry* e = &lock_table[i];
        if (!e->active) continue;
        if (e->object_id != object_id) continue;
        if (!lk_eq(e->key, key)) continue;

        // Same key locked by another TX — conflict
        if (e->tx_id != tx_id && e->type == (uint8_t)LOCK_EXCLUSIVE) {
            kernel_serial_print("[LOCK] CONFLICT: key locked by tx ");
            kernel_serial_print_hex64(e->tx_id);
            kernel_serial_print("\n");
            return 1;  // conflict
        }

        // Same TX re-acquiring the same lock (upgrade to EXCLUSIVE if needed)
        if (e->tx_id == tx_id) {
            if (type == LOCK_EXCLUSIVE) e->type = (uint8_t)LOCK_EXCLUSIVE;
            return 0;  // idempotent
        }
    }

    // Find a free slot
    for (int i = 0; i < LOCK_MAX_ENTRIES; i++) {
        struct LockEntry* e = &lock_table[i];
        if (e->active) continue;
        e->tx_id     = tx_id;
        e->object_id = object_id;
        lk_copy(e->key, key, (int)sizeof(e->key));
        e->type      = (uint8_t)type;
        e->active    = 1;
        lock_entry_count++;
        return 0;
    }

    kernel_serial_print("[LOCK] lock table full\n");
    return 2;  // table full
}

// ─── lock_release_tx ─────────────────────────────────────────────────────────
void lock_release_tx(uint64_t tx_id) {
    uint32_t released = 0;
    for (int i = 0; i < LOCK_MAX_ENTRIES; i++) {
        if (lock_table[i].active && lock_table[i].tx_id == tx_id) {
            lock_table[i].active = 0;
            released++;
        }
    }
    if (released)
        kernel_serial_print("[LOCK] released locks for tx\n");
}

// ─── lock_owned ───────────────────────────────────────────────────────────────
int lock_owned(uint64_t tx_id, uint64_t object_id, const char* key) {
    for (int i = 0; i < LOCK_MAX_ENTRIES; i++) {
        struct LockEntry* e = &lock_table[i];
        if (e->active && e->tx_id == tx_id &&
            e->object_id == object_id && lk_eq(e->key, key))
            return 1;
    }
    return 0;
}

// ─── lock_to_json ─────────────────────────────────────────────────────────────
int lock_to_json(char* buf, int max) {
    int n = 0;
    #define JW(s)  do { const char* _s=(s); while(*_s && n<max-2) buf[n++]=*_s++; } while(0)
    #define JWC(c) do { if (n<max-2) buf[n++]=(c); } while(0)
    #define JWU64(v) do { \
        char _t[20]; int _l=0; uint64_t _v=(v); \
        if(!_v){_t[_l++]='0';} else{while(_v){_t[_l++]=(char)('0'+_v%10);_v/=10;}} \
        for(int _i=_l-1;_i>=0&&n<max-2;_i--) buf[n++]=_t[_i]; \
    } while(0)

    JWC('[');
    int first = 1;
    for (int i = 0; i < LOCK_MAX_ENTRIES; i++) {
        struct LockEntry* e = &lock_table[i];
        if (!e->active) continue;
        if (!first) JWC(',');
        first = 0;
        JWC('{');
        JW("\"tx\":"); JWU64(e->tx_id);
        JW(",\"type\":\""); JW(e->type == (uint8_t)LOCK_EXCLUSIVE ? "X" : "S");
        JWC('"');
        JW(",\"key\":\""); JW(e->key[0] ? e->key : "(object)"); JWC('"');
        JWC('}');
    }
    JWC(']');
    if (n < max) buf[n] = '\0';

    #undef JW
    #undef JWC
    #undef JWU64
    return n;
}
