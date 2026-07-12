#ifndef WEBAPP_H
#define WEBAPP_H

#include <stdint.h>
#include "../kernel/object_catalog.h"

// ─── Asset store limits ───────────────────────────────────────────────────────
#define WEBAPP_MAX_ASSETS    32
#define WEBAPP_PATH_LEN     128    // URL path, e.g. "/index.html"
#define WEBAPP_CONTENT_LEN 2048   // 2 KiB per asset (reduced for BSS budget)
#define WEBAPP_MIME_LEN      48

// ─── One stored web asset ─────────────────────────────────────────────────────
struct WebAsset {
    char     obj_name[OBJECT_NAME_LEN];  // parent WEB_APP object name
    char     path[WEBAPP_PATH_LEN];      // URL path served at
    char     mime[WEBAPP_MIME_LEN];
    char     content[WEBAPP_CONTENT_LEN];
    uint32_t content_len;
    uint8_t  active;
};

// ─── Syscall argument ─────────────────────────────────────────────────────────
struct WebAppSetRequest {
    char     obj_name[OBJECT_NAME_LEN];
    char     path[WEBAPP_PATH_LEN];
    char     content[WEBAPP_CONTENT_LEN];
    uint32_t content_len;
    uint8_t  append;   // 1 = append to existing, 0 = replace
};

// ─── Syscall numbers ──────────────────────────────────────────────────────────
#define SYS_SLS_WEBAPP_SET  180
#define SYS_SLS_WEBAPP_LIST 182

// ─── Public API ───────────────────────────────────────────────────────────────
extern struct WebAsset webapp_store[WEBAPP_MAX_ASSETS];

void             webapp_init(void);

// Find an asset by URL path across all WEB_APP objects.
struct WebAsset* webapp_find(const char* path);

// Find an asset by path within a specific WEB_APP object.
struct WebAsset* webapp_find_in(const char* obj_name, const char* path);

// Set or append asset content.
uint64_t sys_sls_webapp_set(struct WebAppSetRequest* req);

// Print all assets for a given WEB_APP object (or all objects if obj_name="*").
void sys_sls_webapp_list(const char* obj_name);

// Infer MIME type from a file extension in path.
const char* webapp_mime_for(const char* path);

#endif /* WEBAPP_H */
