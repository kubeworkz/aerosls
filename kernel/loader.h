#ifndef LOADER_H
#define LOADER_H

#include <stdint.h>
#include <stddef.h>
#include "process.h"

// ─── Binary store ─────────────────────────────────────────────────────────────
#define LOADER_MAX_BINARY_SIZE  16384   // 16 KiB per service binary (reduced from 64)
#define MAX_SERVICE_BINARIES    16

struct ServiceBinary {
    char     object_name[PROC_NAME_LEN];
    uint64_t object_id;
    uint8_t  data[LOADER_MAX_BINARY_SIZE];
    uint32_t size;        // bytes written so far
    uint8_t  active;
    uint8_t  is_elf;      // detected on first write
};

// ─── Minimal ELF64 structures ─────────────────────────────────────────────────
#define ELF_MAGIC0  0x7F
#define ELF_MAGIC1  'E'
#define ELF_MAGIC2  'L'
#define ELF_MAGIC3  'F'
#define ELF_CLASS64 2
#define PT_LOAD     1
#define PF_X        1       // segment execute permission
#define PF_W        2       // segment write permission
#define PF_R        4       // segment read permission

struct ELF64Header {
    uint8_t  e_ident[16];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint64_t e_entry;       // virtual address of entry point
    uint64_t e_phoff;       // program header table offset
    uint64_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;       // number of program headers
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
} __attribute__((packed));

struct ELF64ProgramHeader {
    uint32_t p_type;        // 1 = PT_LOAD
    uint32_t p_flags;       // PF_X | PF_W | PF_R
    uint64_t p_offset;      // offset of segment in binary
    uint64_t p_vaddr;       // target virtual address
    uint64_t p_paddr;
    uint64_t p_filesz;      // bytes to copy from binary
    uint64_t p_memsz;       // bytes to allocate (p_memsz - p_filesz = BSS)
    uint64_t p_align;
} __attribute__((packed));

// ─── Syscall numbers ──────────────────────────────────────────────────────────
#define SYS_SLS_LOAD           170   // load binary + spawn process
#define SYS_SLS_UPLOAD_BINARY  171   // write a chunk into the binary store

// ─── Upload request (one chunk at a time) ─────────────────────────────────────
#define UPLOAD_CHUNK_MAX 16384  // 16 KiB binary per request; hex = 32 KiB, fits in 64 KiB req_buf

struct SLSUploadRequest {
    char     object_name[PROC_NAME_LEN];
    uint8_t  chunk[UPLOAD_CHUNK_MAX];
    uint32_t chunk_len;
    uint32_t byte_offset;   // position in binary to start writing this chunk
    uint8_t  is_last;       // 1 = final chunk; triggers size finalisation
};

// ─── Public API ───────────────────────────────────────────────────────────────
extern struct ServiceBinary service_binaries[MAX_SERVICE_BINARIES];

void     loader_init(void);

// Find or create a binary slot for an object
struct ServiceBinary* loader_get_or_alloc(const char* object_name, uint64_t obj_id);

// Write a chunk into the binary store
uint64_t sys_sls_upload_binary(struct SLSUploadRequest* req);

// Load the binary for the named SERVICE_PROCESS object into the process's page
// table.  Returns the detected entry point (e_entry for ELF, base_vaddr for flat).
// Returns 0 on failure.
uint64_t loader_load_into_process(const char* object_name,
                                   uint64_t base_vaddr,
                                   uint64_t* pml4);

// Load the demo binary into a named object, then spawn the process
uint64_t sys_sls_load(const char* object_name, uint32_t owner_uid);

// Spawn a process from an OBJ_TYPE_PROGRAM catalog object.
// The object must already have been uploaded via SYS_SLS_UPLOAD_BINARY.
// Returns the new PID on success, 0 on failure.
uint64_t program_load(const char* object_name, uint32_t owner_uid);

// Print info about all loaded service binaries
void     loader_list(void);

// Built-in AeroSLS test binary (calls SYS_SLS_PROC_LIST then exits)
extern const uint8_t  aerosls_demo_bin[];
extern const uint32_t aerosls_demo_bin_size;

#endif /* LOADER_H */
