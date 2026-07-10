#include <stdint.h>
#include "permissions.h"

struct SLSAllocationRequest {
    uint64_t system_object_id;
    uint64_t size_requested;
    uint32_t access_flags;
};

#define SYS_SLS_ALLOCATE   105
#define SYS_SLS_CHMOD      107
#define SYS_SLS_SET_USER   108

static uint32_t current_session_uid = 1000; // Default logged-in guest shell user
static uint32_t current_session_gid = 1000;

void sls_shell_loop(void) {
    char input_buffer[128];
    print("\n--- Multi-User SLS Secure Shell Active ---\n");

    while (1) {
        kernel_serial_printf("uid:%d> ", current_session_uid);
        read_line(input_buffer);

        if (string_starts_with(input_buffer, "login ")) {
            // Command: login <uid> <gid>
            uint32_t target_uid = parse_int(input_buffer + 6);
            uint32_t target_gid = parse_next_int(input_buffer + 6);
            
            // Invoke kernel system call to flip active security credentials for this thread
            do_syscall(SYS_SLS_SET_USER, (void*)((uint64_t)target_uid << 32 | target_gid));
            current_session_uid = target_uid;
            current_session_gid = target_gid;
            print("Session privilege level altered.\n");
        }
        else if (string_starts_with(input_buffer, "chmod ")) {
            // Command: chmod <name> <bitmask>
            char* name = input_buffer + 6;
            uint64_t obj_id = hash_string(name);
            uint32_t target_mask = parse_int(find_next_argument(name));

            uint64_t args[2] = { obj_id, target_mask };
            uint64_t status = do_syscall(SYS_SLS_CHMOD, args);
            
            if (status == 0) print("Permissions matrix successfully updated.\n");
            else print("Security Violation: Only the object owner can alter permissions.\n");
        }
        else if (string_starts_with(input_buffer, "write ")) {
            char* name = input_buffer + 6;
            uint64_t obj_id = hash_string(name);
            
            struct SLSAllocationRequest req = {.system_object_id = obj_id, .size_requested = 4096, .access_flags = PERM_WRITE};
            char* persistent_ptr = (char*)do_syscall(SYS_SLS_ALLOCATE, &req);

            if (persistent_ptr) {
                char* payload = find_next_argument(name);
                
                // KERNEL HANDSHAKE EVALUATION:
                // If this object was marked FLAG_APPEND_ONLY by an administrator, the 
                // kernel's hardware page table entry will map this page as READ-ONLY. 
                // When the loop below tries to modify a byte that isn't at the end, 
                // a page fault triggers, and the matrix engine will instantly kill this thread.
                string_copy(persistent_ptr, payload);
                print("Direct memory mutation verified.\n");
            } else {
                print("Access Denied: Your UID/GID lacks clearance permissions.\n");
            }
        }
    }
}