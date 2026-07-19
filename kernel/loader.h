#ifndef LOADER_H
#define LOADER_H

#include <stdint.h>
#include <stddef.h>
#include "process.h"
#include "timi_translate.h"   // Gap Remediation Phase G -- struct TimiActivationStatus

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
    uint8_t  is_timi;     // detected on first write — see TIMI section below
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

// ─── TIMI object format (AeroSLS-TIMI-ISA-v0.1.md, Phase 2; v0.3 Phase 6) ────
// A TIMI-format binary is the flat .tmo container produced by the Phase 1
// host toolchain (timi-asm): a header of five little-endian u32 fields,
// followed by the instruction stream, the literal pool, the entry-point
// table, and (v0.3) the object-name pool, back to back. This struct layout
// is byte-for-byte identical to `TimiObject`'s on-disk form in the host
// tool's timi_isa.h/timi_obj.c — a .tmo produced there can be uploaded via
// SYS_SLS_UPLOAD_BINARY unmodified.
//
// v0.3 (Phase 6) adds num_names + TimiNameRec, breaking the v0.2 16-byte
// header (see timi_isa.h's top comment for why this was a clean bump
// rather than a back-compat shim) — every .tmo in this project is
// regenerated from source by the same toolchain, so there is nothing else
// to keep compatible with.
//
// TIMI bytecode is never executed directly (ISA spec design principle #1:
// "never interpreted, always translated"). loader_load_into_process()
// detects and validates a TIMI payload, then hands it to
// timi_translate_and_map() (kernel/timi_translate.c, Phase 3) to be
// translated to real x86-64 machine code and mapped executable — the
// bytecode words themselves are never mapped or run as-is.
#define TIMI_MAGIC          0x314D4954u   // "TIM1", matches timi_isa.h TIMI_MAGIC
#define TIMI_ENTRY_NAME_LEN 32            // matches host tool's TIMI_MAX_NAME

struct TimiObjectHeader {
    uint32_t magic;
    uint32_t num_instr;
    uint32_t num_literals;
    uint32_t num_entries;
    uint32_t num_names;    // v0.3
} __attribute__((packed));

struct TimiEntryRec {
    char     name[TIMI_ENTRY_NAME_LEN];
    uint32_t offset;   // instruction index
} __attribute__((packed));

// v0.3 (Phase 6): object-name pool entry, referenced by RESOLVE's operand.
// Same 32-byte fixed-slot shape as TimiEntryRec, matches timi_isa.h's
// TimiName / timi_x86.h's TxNameRec.
struct TimiNameRec {
    char name[TIMI_ENTRY_NAME_LEN];
} __attribute__((packed));

// Validates a TIMI payload sitting in `data` (size `size` bytes): checks the
// magic number and that the header's declared instr/literal/entry counts
// actually fit inside `size` (a corrupt or truncated header could otherwise
// claim a huge instruction count and walk off the end of the buffer).
// On success, fills `out_hdr` and returns 1. Returns 0 and leaves `out_hdr`
// untouched on any failure.
int  timi_validate(const uint8_t* data, uint32_t size, struct TimiObjectHeader* out_hdr);

// Prints a loader_list()-style report for one TIMI object: header counts,
// exported entry-point names, and (since Phase 3 doesn't exist yet) a
// reminder that it can't be spawned. Gap Remediation Phase G: now a thin
// wrapper over loader_timi_info_query() below (single source of truth)
// rather than its own independent parse.
void loader_timi_info(const char* object_name);

// Shared by loader_list() and the three net/http.c format-string sites so
// TIMI uploads report correctly everywhere instead of falling through to
// the ELF/flat ternary and showing up mislabeled as "flat".
const char* binary_format_name(const struct ServiceBinary* sb);

// ─── Gap Remediation Phase G: structured TIMI introspection ───────────────────
// Before this phase, loader_timi_info() only ever kernel_serial_printf'd its
// findings -- no caller (HTTP, syscall, or otherwise) could get the data back
// structurally. TIMI_INFO_MAX_ENTRIES/NAMES caps this project's usual "16"
// fixed-array convention (matches ROW_JOURNAL_MAX_ENTRIES, BTREE_MAX_DUPES_
// PER_KEY, etc.) -- a real object with more than 16 entries or names is
// reported truncated (entries_truncated/names_truncated), not silently cut
// off with no signal, same "denial looks like absence" carefulness as every
// other capped-array result in this project (see row_index.h's
// row_index_lookup_checked() for the precedent this mirrors).
#define TIMI_INFO_STATUS_OK        0
#define TIMI_INFO_STATUS_NOT_FOUND 1   // no such uploaded object
#define TIMI_INFO_STATUS_NOT_TIMI  2   // object exists but isn't a TIMI upload
#define TIMI_INFO_STATUS_CORRUPT   3   // TIMI magic present but header validation failed

#define TIMI_INFO_MAX_ENTRIES 16
#define TIMI_INFO_MAX_NAMES   16

struct TimiInfoResult {
    uint32_t status;                 // TIMI_INFO_STATUS_*
    char     format_name[16];        // filled on NOT_TIMI (binary_format_name()'s string)
    uint32_t num_instr, num_literals, num_entries, num_names;   // raw header counts
    struct TimiEntryRec entries[TIMI_INFO_MAX_ENTRIES];
    uint32_t entries_returned;
    uint8_t  entries_truncated;      // 1 if num_entries > TIMI_INFO_MAX_ENTRIES
    struct TimiNameRec  names[TIMI_INFO_MAX_NAMES];
    uint32_t names_returned;
    uint8_t  names_truncated;        // 1 if num_names > TIMI_INFO_MAX_NAMES
    struct TimiActivationStatus activation;
};

// Fills *out with the same data loader_timi_info() prints, structurally.
// Always fills *out (status tells the caller what happened) -- never
// leaves it uninitialized, same "no partial/garbage result" posture as
// every other query function in this project. Returns 1 if status ==
// TIMI_INFO_STATUS_OK, 0 otherwise (mirrors timi_activation_query()'s
// own 1/0-means-"was there real data" convention).
int loader_timi_info_query(const char* object_name, struct TimiInfoResult* out);

struct SLSTimiInfoRequest {
    char object_name[PROC_NAME_LEN];   // [in]
    struct TimiInfoResult result;      // [out]
};

// Thin syscall wrapper -- returns 0 if result.status == TIMI_INFO_STATUS_OK,
// 1 otherwise, matching this project's usual 0-success/1-failure convention
// (the full detail is always in req->result.status regardless).
uint64_t sys_sls_timi_info(struct SLSTimiInfoRequest* req);

#define SYS_SLS_TIMI_INFO 173   // Gap Remediation Phase G: now struct-based
                                 // (struct SLSTimiInfoRequest*), not a raw
                                 // const char* object name -- confirmed via
                                 // direct repo-wide grep that nothing else
                                 // (no host test, no other kernel file, no
                                 // shell command) depended on the old raw-
                                 // string wire format before changing it.

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
//
// partition_id (Phase 13, LPAR): the spawning process's partition, used to
// quota-check the ELF64/flat segment frames this function allocates —
// these are the per-binary, potentially-large frame allocations Phase 13
// exists to cap. Unused for the TIMI branch (timi_translate_and_map()'s
// frames come from the shared, deliberately partition-agnostic activation
// cache — see frame_pool.h's header comment).
uint64_t loader_load_into_process(const char* object_name,
                                   uint64_t base_vaddr,
                                   uint64_t* pml4,
                                   uint32_t partition_id);

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
