#include "secure_api.h"
#include "../drivers/nvme.h"

struct SecuredObjectKeyEntry {
    uint64_t object_id;
    uint32_t derived_key[8]; // 256-bit derived cipher key
    uint32_t is_active;
};

#define MAX_SECURE_OBJECTS 256
static struct SecuredObjectKeyEntry secure_key_directory[MAX_SECURE_OBJECTS];

// Derives a 256-bit key from a password string using an iterative avalanche scheme
void derive_user_key(const char* password, uint32_t len, uint32_t* out_key) {
    uint64_t h1 = 0xcbf29ce484222325ULL;
    uint64_t h2 = 0x84222325cbf29ce4ULL;

    // Run 1000 mixing iterations to increase complexity
    for (int iter = 0; iter < 1000; iter++) {
        for (uint32_t i = 0; i < len; i++) {
            h1 ^= (uint8_t)password[i] + iter;
            h1 *= 0x00000100000001B3ULL;
            h2 ^= (uint8_t)password[len - 1 - i] ^ h1;
            h2 *= 0x00000100000001B3ULL;
        }
    }

    out_key[0] = (uint32_t)h1;       out_key[1] = (uint32_t)(h1 >> 32);
    out_key[2] = (uint32_t)h2;       out_key[3] = (uint32_t)(h2 >> 32);
    out_key[4] = (uint32_t)(h1 ^ h2); out_key[5] = (uint32_t)((h1 ^ h2) >> 32);
    out_key[6] = 0xDEADBEEF ^ out_key[0];
    out_key[7] = 0xCAFEBABE ^ out_key[1];
}

// Gap Remediation Phase E: sys_sls_secure_seal() derives a password-based
// key (via derive_user_key() above) and stores it in the kernel's in-memory
// key directory, keyed by system_object_id. That is ALL it does. It does
// NOT read, transform, or write back the target object's actual stored
// data -- nothing in this function ever touches the object's own bytes on
// NVMe or in RAM, so calling this on an object leaves that object's
// content exactly as readable as it was before the call. The struct
// SLSSealRequest's own encryption_algorithm_flags field (secure_api.h,
// e.g. "1 = ChaCha20-User") is accepted but never read below -- no cipher
// is ever applied, ChaCha20 or otherwise. This function previously logged
// "successfully encrypted," which was false and actively misleading to
// anyone reading the kernel log; the roadmap's own gap analysis named this
// specifically. Per the roadmap's own recommendation, this pass relabels
// rather than implements real encryption (a much larger project: it would
// need a real cipher, and hooks into every object-storage read/write path
// this kernel has -- rowstore, vecstore, legacy KV records -- to actually
// transform data at rest, not just register a key nobody uses yet).
uint64_t sys_sls_secure_seal(struct SLSSealRequest* req) {
    if (!req || req->password_len == 0) return -1;

    // Verify user owns the object via the Memory Protection Matrix
    uint32_t caller_uid = kernel_get_current_thread_id(); // Using thread context as placeholder UID
    if (!verify_expanded_matrix_access(caller_uid, 0, req->system_object_id, 0x08)) { // 0x08 = OWNER
        return -1; // Privilege violation
    }

    // Find a free slot in the kernel's encryption directory
    int target_slot = -1;
    for (int i = 0; i < MAX_SECURE_OBJECTS; i++) {
        if (!secure_key_directory[i].is_active || secure_key_directory[i].object_id == req->system_object_id) {
            target_slot = i;
            break;
        }
    }

    if (target_slot == -1) return -2; // Directory saturated

    secure_key_directory[target_slot].object_id = req->system_object_id;
    derive_user_key(req->user_password, req->password_len, secure_key_directory[target_slot].derived_key);
    secure_key_directory[target_slot].is_active = 1;

    // Gap Remediation Phase E: this log previously claimed the object was
    // "successfully encrypted" -- it wasn't (see this function's own header
    // comment). Relabeled to describe exactly what happened: a key was
    // derived and stored, nothing more.
    kernel_serial_printf("[SECURITY] Key derived and stored for object %d (data NOT re-encrypted -- key registration only).\n", req->system_object_id);
    return 0;
}