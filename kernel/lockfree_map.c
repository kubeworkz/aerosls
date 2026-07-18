#include <stdint.h>
#include <stddef.h>

#define HASH_MATRIX_BUCKETS 2048

struct SLSObject {
    uint64_t start_virtual_address;
    uint64_t starting_disk_block;
};

struct SLSObjectNode {
    uint64_t unique_object_id;
    uint64_t global_virtual_address;
    uint64_t assigned_disk_sector;
    size_t   allocated_bytes;
    struct SLSObjectNode* next;
};

// Our multi-core concurrent bucket lookup matrix index array
static struct SLSObjectNode* volatile concurrent_object_directory[HASH_MATRIX_BUCKETS];

extern uint64_t generate_unique_object_id(const char* key, size_t length);
extern struct SLSObject create_persistent_region(size_t size);
extern void* allocate_physical_ram_frame(void);

struct SLSObjectNode* lockfree_lookup_object(uint64_t object_id) {
    uint32_t bucket = object_id % HASH_MATRIX_BUCKETS;
    struct SLSObjectNode* current = concurrent_object_directory[bucket];

    while (current != NULL) {
        if (current->unique_object_id == object_id) {
            return current; // Found active record match safely without locking
        }
        current = current->next;
    }
    return NULL; // Object handle not active or unmapped
}

// Atomically registers or resolves an object across concurrent execution barriers
uint64_t concurrent_get_or_create_object(const char* name, size_t name_len, size_t size_bytes) {
    uint64_t obj_id = generate_unique_object_id(name, name_len);
    uint32_t bucket = obj_id % HASH_MATRIX_BUCKETS;

    // Check if the object already exists in the system
    struct SLSObjectNode* existing = lockfree_lookup_object(obj_id);
    if (existing != NULL) {
        return existing->global_virtual_address;
    }

    // Allocate a temporary node descriptor space
    struct SLSObjectNode* new_node = (struct SLSObjectNode*)allocate_physical_ram_frame();
    new_node->unique_object_id = obj_id;
    new_node->allocated_bytes = size_bytes;
    
    // Allocate the underlying physical persistent sector ranges
    struct SLSObject new_region = create_persistent_region(size_bytes);
    new_node->global_virtual_address = new_region.start_virtual_address;
    new_node->assigned_disk_sector   = new_region.starting_disk_block;

    // Attempt an Atomic Lock-Free Head Insertion into the linked chain
    while (1) {
        struct SLSObjectNode* current_head = concurrent_object_directory[bucket];
        
        // Secondary protection sweep: check if a separate core snuck an entry in mid-allocation
        struct SLSObjectNode* verification_sweep = current_head;
        while (verification_sweep != NULL) {
            if (verification_sweep->unique_object_id == obj_id) {
                // Core collision confirmed. Deallocate the redundant node space and return the winner's pointer
                free_kernel_memory(new_node); 
                return verification_sweep->global_virtual_address;
            }
            verification_sweep = verification_sweep->next;
        }

        // Point the new node's next field to the current head of the bucket
        new_node->next = current_head;

        // Compare-and-Swap (CAS) execution:
        // If concurrent_object_directory[bucket] is still equal to current_head, 
        // atomically swap it with new_node and return true. If another core modified it 
        // in the last fraction of a microsecond, the CAS operation fails, and we loop to retry safely.
        if (__sync_bool_compare_and_swap(&concurrent_object_directory[bucket], current_head, new_node)) {
            break; // Node successfully committed to the global dictionary trace
        }
    }

    return new_node->global_virtual_address;
}
