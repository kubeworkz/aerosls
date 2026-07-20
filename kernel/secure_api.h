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

// Derives a 256-bit key from a password string (kernel/secure_api.c).
// Previously used only within that file with no header declaration, so
// every caller outside it (this used to mean none) would need its own
// implicit/duplicate declaration. Architectural Phase 4 (see
// docs/AeroSLS-Architectural-MVP-Roadmap-v0.1.md) reuses this in
// kernel/auth.c for account password verification -- deliberately the
// same primitive `seal` already uses, not a second scheme -- which is
// what surfaced the missing prototype.
void derive_user_key(const char* password, uint32_t len, uint32_t* out_key);