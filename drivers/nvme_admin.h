#ifndef NVME_ADMIN_H
    #define NVME_ADMIN_H
    #include <stdint.h>
    struct NVMeCmd { uint8_t opcode; uint8_t flags; uint16_t command_id; uint32_t nsid; 
    uint64_t rsvd; uint64_t metadata; uint64_t prp1; uint64_t prp2; uint32_t cdw; } 
    attribute((packed));
    #define IO_QUEUE_SIZE 256
    #endif