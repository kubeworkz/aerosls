#ifndef NVME_ADMIN_H
#define NVME_ADMIN_H

#include <stdint.h>

// Admin Opcodes
#define NVME_ADMIN_CMD_CREATE_CQ 0x05
#define NVME_ADMIN_CMD_CREATE_SQ 0x01
#define NVME_ADMIN_CMD_IDENTIFY  0x06

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

// ─── Navigator-Parity Gap Roadmap Phase 2 ─────────────────────────────────────
// Issues Identify Namespace (CNS=0x1, nsid=1 -- this driver only ever brings
// up a single namespace, matching nvme_io.c's own single-namespace scope) and
// caches NSZE (namespace size, in logical blocks) and NCAP (namespace
// capacity, in logical blocks) from the returned 4KB Identify data structure
// (NSZE at byte offset 0, NCAP at byte offset 8 -- NVMe base spec Figure
// "Identify Namespace Data Structure"). Returns 1 on success (queries the
// controller and populates the cache), 0 on failure (admin queue not ready,
// command timeout/error, or scratch frame allocation failure -- cache is left
// at its prior value, 0 before the first successful call).
//
// Deliberately does not parse the LBA Format table (offset 128+) to get the
// real per-namespace sector size -- nvme_get_capacity_bytes() below assumes
// the overwhelmingly common 512-byte logical block, the same simplifying
// assumption nvme_io.c's own sector arithmetic already makes elsewhere in
// this driver. Named here rather than silently assumed.
int nvme_identify_namespace(uint32_t nsid);

// Returns the cached namespace capacity in bytes (NCAP * 512), or 0 if
// nvme_identify_namespace() was never called or last failed.
uint64_t nvme_get_capacity_bytes(void);

#endif