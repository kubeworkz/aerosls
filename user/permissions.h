#pragma once

#define PERM_READ            (1 << 0)
#define PERM_WRITE           (1 << 1)
#define PERM_EXECUTE         (1 << 2)
#define PERM_OWNER           (1 << 3)

// Advanced Matrix Modifiers
#define FLAG_APPEND_ONLY     (1 << 4)  // Can add bytes, but cannot modify existing values
#define FLAG_SETUID          (1 << 5)  // Runs application with the object owner's privileges
#define FLAG_STICKY_OBJECT   (1 << 6)  // Only the owner can delete or unmap the segment

// Permission shorthand used in grant/revoke shell commands
#define PERM_RWX             (PERM_READ | PERM_WRITE | PERM_EXECUTE)
#define PERM_RW              (PERM_READ | PERM_WRITE)
#define PERM_RO              (PERM_READ)

struct ExpandedMatrixEntry {
    uint32_t uid;              // User ID claiming authority
    uint32_t gid;              // Group ID claiming authority
    uint64_t system_object_id; // Universal SLS object target
    uint32_t permission_mask;  // Combines PERM and FLAG bits
};