#include "../include/sls_mmu.h"
    #define HASH_BUCKETS 2048
    struct SLSObjectNode { uint64_t unique_object_id; uint64_t global_virtual_address; struct SLSObjectNode* next; };
    static struct SLSObjectNode* volatile concurrent_object_directory[HASH_BUCKETS];
    struct SLSObjectNode* lockfree_lookup_object(uint64_t object_id) {
        uint32_t bucket = object_id % HASH_BUCKETS;
        struct SLSObjectNode* current = concurrent_object_directory[bucket];
        while (current != NULL) {
            if (current->unique_object_id == object_id) return current;
            current = current->next;
        }
        return 0;
    }