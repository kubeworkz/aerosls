#ifndef NVME_ADMIN_H
#define NVME_ADMIN_H

#include <stdint.h>

// Admin Opcodes
#define NVME_ADMIN_CMD_CREATE_CQ 0x05
#define NVME_ADMIN_CMD_CREATE_SQ 0x01

// 64-byte generic layout for NVMe Admin/IO commands
struct NVMeCmd {
    uint8_t  opcode;
    uint8_t  flags;
    uint16_t command_id;
    uint32_t nsid;
    uint64_t reserved0;
    uint64_t metadata;
    uint64_t prp1;
    uint64_t prp2;
    uint32_t cdw10; // Command Dword 10 (Command specific parameters)
    uint32_t cdw11; // Command Dword 11
    uint32_t cdw12;
    uint32_t cdw13;
    uint32_t cdw14;
    uint32_t cdw15;
} __attribute__((packed));

// 16-byte NVMe Completion Queue entry layout
struct NVMeCqe {
    uint32_t result;
    uint32_t reserved;
    uint16_t sq_head;
    uint16_t sq_id;
    uint16_t command_id;
    uint16_t status; // Bit 0 is the Phase Tag (P) matching execution status
} __attribute__((packed));

// Submit an Admin command synchronously and return the completion entry.
struct NVMeCqe nvme_submit_admin_cmd(struct NVMeCmd cmd);

#endif