#include "loader.h"
#include "object_catalog.h"
#include "kernel_io.h"
#include "process.h"
#include "../arch/x86/user_paging.h"

// frame_pool.c has no header — declare the allocator directly
extern void* allocate_physical_ram_frame(void);

// ─── Binary store ─────────────────────────────────────────────────────────────
struct ServiceBinary service_binaries[MAX_SERVICE_BINARIES];

// ─── Built-in AeroSLS demo binary ─────────────────────────────────────────────
// This flat x86-64 binary calls SYS_SLS_PROC_LIST (162) to print the process
// table via the kernel, then calls SYS_SLS_PROC_KILL (161) with RDI=0 (self)
// to terminate cleanly.
//
// Assembled instructions:
//   mov rax, 162    ; SYS_SLS_PROC_LIST
//   xor rdi, rdi
//   syscall
//   mov rax, 161    ; SYS_SLS_PROC_KILL (pid=0 = self)
//   xor rdi, rdi
//   syscall
//   .spin: jmp .spin
const uint8_t aerosls_demo_bin[] = {
    0x48, 0xC7, 0xC0, 0xA2, 0x00, 0x00, 0x00,  // mov rax, 162
    0x48, 0x31, 0xFF,                            // xor rdi, rdi
    0x0F, 0x05,                                  // syscall
    0x48, 0xC7, 0xC0, 0xA1, 0x00, 0x00, 0x00,  // mov rax, 161
    0x48, 0x31, 0xFF,                            // xor rdi, rdi
    0x0F, 0x05,                                  // syscall
    0xEB, 0xFE                                   // .spin: jmp .spin
};
const uint32_t aerosls_demo_bin_size =
    (uint32_t)sizeof(aerosls_demo_bin);

// ─── String helpers ───────────────────────────────────────────────────────────
static size_t ld_strlen(const char* s) { size_t n=0; while(s[n]) n++; return n; }
static int    ld_streq(const char* a, const char* b) {
    while (*a && *b) { if (*a!=*b) return 0; a++; b++; }
    return *a == *b;
}
static void ld_strncpy(char* d, const char* s, size_t n) {
    size_t i; for (i=0; i<n-1&&s[i]; i++) d[i]=s[i]; d[i]='\0';
}
static void ld_memset(void* p, uint8_t v, size_t n) {
    uint8_t* b = (uint8_t*)p;
    while (n--) *b++ = v;
}
static void ld_memcpy(void* d, const void* s, size_t n) {
    uint8_t* dd = (uint8_t*)d; const uint8_t* ss = (const uint8_t*)s;
    while (n--) *dd++ = *ss++;
}

// ─── loader_init ──────────────────────────────────────────────────────────────
void loader_init(void) {
    for (int i = 0; i < MAX_SERVICE_BINARIES; i++) {
        service_binaries[i].active = 0;
        service_binaries[i].size   = 0;
    }
    kernel_serial_printf(
        "[LOADER] Service binary store ready. "
        "Built-in demo: %u bytes.\n", aerosls_demo_bin_size);
}

// ─── loader_get_or_alloc ─────────────────────────────────────────────────────
struct ServiceBinary* loader_get_or_alloc(const char* name, uint64_t obj_id) {
    // Check for existing slot
    for (int i = 0; i < MAX_SERVICE_BINARIES; i++) {
        if (service_binaries[i].active &&
            ld_streq(service_binaries[i].object_name, name))
            return &service_binaries[i];
    }
    // Allocate a new slot
    for (int i = 0; i < MAX_SERVICE_BINARIES; i++) {
        if (!service_binaries[i].active) {
            service_binaries[i].active    = 1;
            service_binaries[i].size      = 0;
            service_binaries[i].is_elf    = 0;
            service_binaries[i].object_id = obj_id;
            ld_strncpy(service_binaries[i].object_name, name, PROC_NAME_LEN);
            return &service_binaries[i];
        }
    }
    return 0;
}

// ─── sys_sls_upload_binary ────────────────────────────────────────────────────
// Write one chunk of binary data into the binary store for a SERVICE_PROCESS.
uint64_t sys_sls_upload_binary(struct SLSUploadRequest* req) {
    if (!req || !req->chunk_len) return 1;

    // Resolve object_id from the catalog
    uint64_t obj_id = 0;
    for (uint32_t i = 0; i < object_catalog_count; i++) {
        if (!object_catalog[i].active) continue;
        if (ld_streq(object_catalog[i].name, req->object_name)) {
            obj_id = object_catalog[i].object_id;
            break;
        }
    }

    struct ServiceBinary* sb = loader_get_or_alloc(req->object_name, obj_id);
    if (!sb) {
        kernel_serial_print("[LOADER] upload: binary store full.\n");
        return 1;
    }

    uint32_t end = req->byte_offset + req->chunk_len;
    if (end > LOADER_MAX_BINARY_SIZE) {
        kernel_serial_printf("[LOADER] upload: binary exceeds %u KB limit.\n",
                             LOADER_MAX_BINARY_SIZE / 1024);
        return 1;
    }

    ld_memcpy(sb->data + req->byte_offset, req->chunk, req->chunk_len);

    if (end > sb->size) sb->size = end;

    // Detect ELF magic on the first chunk
    if (req->byte_offset == 0 && req->chunk_len >= 4) {
        sb->is_elf = (sb->data[0] == ELF_MAGIC0 &&
                      sb->data[1] == ELF_MAGIC1 &&
                      sb->data[2] == ELF_MAGIC2 &&
                      sb->data[3] == ELF_MAGIC3);
    }

    kernel_serial_printf(
        "[LOADER] '%s': wrote %u bytes at offset %u (total=%u, %s)\n",
        req->object_name, req->chunk_len, req->byte_offset, sb->size,
        sb->is_elf ? "ELF64" : "flat");

    (void)ld_strlen;  // suppress unused warning
    return 0;
}

// ─── loader_load_into_process ─────────────────────────────────────────────────
// Core loader: maps the binary into the process's page table.
// Returns the entry point vaddr, or 0 on failure.
uint64_t loader_load_into_process(const char* object_name,
                                   uint64_t base_vaddr,
                                   uint64_t* pml4) {
    // Find the binary
    struct ServiceBinary* sb = 0;
    for (int i = 0; i < MAX_SERVICE_BINARIES; i++) {
        if (service_binaries[i].active &&
            ld_streq(service_binaries[i].object_name, object_name)) {
            sb = &service_binaries[i];
            break;
        }
    }
    if (!sb || sb->size == 0) {
        kernel_serial_printf(
            "[LOADER] '%s': no binary — use 'upload' or 'demo' first.\n",
            object_name);
        return 0;
    }

    // ── ELF64 loader ─────────────────────────────────────────────────────────
    if (sb->is_elf && sb->size >= sizeof(struct ELF64Header)) {
        struct ELF64Header* ehdr = (struct ELF64Header*)sb->data;

        if (ehdr->e_ident[4] != ELF_CLASS64) {
            kernel_serial_print("[LOADER] ELF: not 64-bit, aborting.\n");
            return 0;
        }
        if (ehdr->e_phnum == 0 || ehdr->e_phoff == 0) {
            kernel_serial_print("[LOADER] ELF: no program headers.\n");
            return 0;
        }

        struct ELF64ProgramHeader* phdr =
            (struct ELF64ProgramHeader*)(sb->data + ehdr->e_phoff);

        for (uint16_t ph = 0; ph < ehdr->e_phnum; ph++) {
            if (phdr[ph].p_type != PT_LOAD) continue;
            if (phdr[ph].p_filesz == 0) continue;

            uint64_t vaddr_base = phdr[ph].p_vaddr;
            uint32_t n_pages    = (uint32_t)
                ((phdr[ph].p_memsz + 4095) / 4096);

            // Determine page flags
            uint64_t flags = USER_PTE_PRESENT | USER_PTE_USER;
            if (phdr[ph].p_flags & PF_W) flags |= USER_PTE_WRITE;
            if (!(phdr[ph].p_flags & PF_X)) flags |= USER_PTE_NOEXEC;

            kernel_serial_printf(
                "[LOADER] ELF segment: vaddr=0x%016lx  filesz=%lu  memsz=%lu  "
                "flags=%c%c%c\n",
                vaddr_base, phdr[ph].p_filesz, phdr[ph].p_memsz,
                (phdr[ph].p_flags & PF_R) ? 'R' : '-',
                (phdr[ph].p_flags & PF_W) ? 'W' : '-',
                (phdr[ph].p_flags & PF_X) ? 'X' : '-');

            for (uint32_t p = 0; p < n_pages; p++) {
                void* frame = allocate_physical_ram_frame();
                if (!frame) return 0;
                ld_memset(frame, 0, 4096);  // zero (BSS) fill

                // Copy file content into this frame
                uint64_t byte_off = (uint64_t)p * 4096;
                if (byte_off < phdr[ph].p_filesz) {
                    uint64_t copy_len = phdr[ph].p_filesz - byte_off;
                    if (copy_len > 4096) copy_len = 4096;
                    ld_memcpy(frame,
                              sb->data + phdr[ph].p_offset + byte_off,
                              (size_t)copy_len);
                }

                user_map_page(pml4, vaddr_base + (uint64_t)p * 4096,
                              (uint64_t)(uintptr_t)frame, flags);
            }
        }

        kernel_serial_printf("[LOADER] ELF64 loaded. Entry: 0x%016lx\n",
                             ehdr->e_entry);
        return ehdr->e_entry;
    }

    // ── Flat binary loader ────────────────────────────────────────────────────
    uint32_t n_pages = (sb->size + 4095) / 4096;
    for (uint32_t p = 0; p < n_pages; p++) {
        void* frame = allocate_physical_ram_frame();
        if (!frame) return 0;
        ld_memset(frame, 0, 4096);

        uint32_t off = p * 4096;
        uint32_t len = sb->size - off;
        if (len > 4096) len = 4096;
        ld_memcpy(frame, sb->data + off, (size_t)len);

        user_map_page(pml4, base_vaddr + (uint64_t)p * 4096,
                      (uint64_t)(uintptr_t)frame,
                      USER_PTE_PRESENT | USER_PTE_USER | USER_PTE_EXEC);
    }

    kernel_serial_printf("[LOADER] Flat binary loaded: %u bytes at 0x%016lx\n",
                         sb->size, base_vaddr);
    return base_vaddr;   // flat binary entry = first byte
}

// ─── loader_list ─────────────────────────────────────────────────────────────
void loader_list(void) {
    kernel_serial_printf(
        "\n[LOADER] Service Binary Store\n"
        " %-24s  %-8s  %-6s  %s\n"
        " ------------------------  --------  ------  ----\n",
        "Object", "Size", "Format", "Active");
    int found = 0;
    for (int i = 0; i < MAX_SERVICE_BINARIES; i++) {
        if (!service_binaries[i].active) continue;
        kernel_serial_printf(
            " %-24s  %-8u  %-6s  yes\n",
            service_binaries[i].object_name,
            service_binaries[i].size,
            service_binaries[i].is_elf ? "ELF64" : "flat");
        found++;
    }
    if (!found) kernel_serial_print(" (no binaries loaded)\n");
    kernel_serial_print("\n");
}

// ─── sys_sls_load ─────────────────────────────────────────────────────────────
// Ensure a binary exists for the object, then spawn the process.
uint64_t sys_sls_load(const char* object_name, uint32_t owner_uid) {
    // Verify the object exists and is of the right type
    int found = 0;
    for (uint32_t i = 0; i < object_catalog_count; i++) {
        if (!object_catalog[i].active) continue;
        if (!ld_streq(object_catalog[i].name, object_name)) continue;
        if (object_catalog[i].type != OBJ_TYPE_SERVICE_PROCESS) {
            kernel_serial_printf(
                "[LOADER] '%s' is not a SERVICE_PROCESS object.\n",
                object_name);
            return 1;
        }
        found = 1;
        break;
    }
    if (!found) {
        kernel_serial_printf(
            "[LOADER] Object '%s' not found. "
            "Create it with: valloc %s 4 4\n",
            object_name, object_name);
        return 1;
    }

    struct ProcCreateRequest req;
    ld_strncpy(req.object_name, object_name, PROC_NAME_LEN);
    req.owner_uid = owner_uid;
    return process_create(&req);
}

// ─── program_load ─────────────────────────────────────────────────────────────
// Spawn a process from an OBJ_TYPE_PROGRAM catalog object.
// Unlike sys_sls_load (which expects OBJ_TYPE_SERVICE_PROCESS), this function
// accepts the new OBJ_TYPE_PROGRAM type that represents a named executable in
// the SLS object store — the OS-native replacement for filesystem executables.
uint64_t program_load(const char* object_name, uint32_t owner_uid) {
    // Validate type before handing off to program_spawn()
    int found = 0;
    for (uint32_t i = 0; i < object_catalog_count; i++) {
        if (!object_catalog[i].active) continue;
        if (!ld_streq(object_catalog[i].name, object_name)) continue;
        if (object_catalog[i].type != OBJ_TYPE_PROGRAM) {
            kernel_serial_printf(
                "[LOADER] program_load: '%s' is not an OBJ_TYPE_PROGRAM object "
                "(type=%s). Use sys_sls_load for SERVICE_PROCESS objects.\n",
                object_name,
                obj_type_name(object_catalog[i].type));
            return 0;
        }
        found = 1;
        break;
    }
    if (!found) {
        kernel_serial_printf(
            "[LOADER] program_load: object '%s' not found.\n", object_name);
        return 0;
    }

    // Delegate to program_spawn() which owns the page-table mapping pipeline
    return (uint64_t)program_spawn(object_name, owner_uid);
}
