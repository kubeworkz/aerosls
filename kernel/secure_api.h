#pragma once
#include <stdint.h>

#define SYS_SLS_SECURE_SEAL 109

// Gap Remediation Phase E: sys_sls_secure_seal() (kernel/secure_api.c) only
// derives and stores a password-based key against system_object_id -- it
// does not encrypt the object's actual stored data, and
// encryption_algorithm_flags below is accepted but not yet acted on by
// anything. See that function's own header comment for the full rationale.
struct SLSSealRequest {
    uint64_t system_object_id; // Target single-level object to bind
    char     user_password[32]; // Plaintext input array
    uint32_t password_len;
    uint32_t encryption_algorithm_flags; // accepted, not yet consulted -- see note above
};