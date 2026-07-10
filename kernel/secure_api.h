#define SYS_SLS_SECURE_SEAL 109

struct SLSSealRequest {
    uint64_t system_object_id; // Target single-level object to bind
    char     user_password[32]; // Plaintext input array
    uint32_t password_len;
    uint32_t encryption_algorithm_flags; // e.g., 1 = ChaCha20-User
};