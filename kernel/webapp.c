#include "webapp.h"
#include "kernel_io.h"

struct WebAsset webapp_store[WEBAPP_MAX_ASSETS];

// ─── String helpers ───────────────────────────────────────────────────────────
static int wa_streq(const char* a, const char* b) {
    while (*a && *b) { if (*a!=*b) return 0; a++; b++; }
    return *a == *b;
}
static void wa_strncpy(char* d, const char* s, int n) {
    int i; for (i=0; i<n-1&&s[i]; i++) d[i]=s[i]; d[i]='\0';
}
static int wa_strlen(const char* s) { int n=0; while(s[n]) n++; return n; }

// ─── MIME detection ───────────────────────────────────────────────────────────
const char* webapp_mime_for(const char* path) {
    // Walk to the last '.' in the path
    const char* ext = 0;
    for (const char* p = path; *p; p++) if (*p == '.') ext = p;
    if (!ext) return "application/octet-stream";
    if (wa_streq(ext, ".html") || wa_streq(ext, ".htm")) return "text/html; charset=utf-8";
    if (wa_streq(ext, ".css"))   return "text/css";
    if (wa_streq(ext, ".js"))    return "application/javascript";
    if (wa_streq(ext, ".json"))  return "application/json";
    if (wa_streq(ext, ".txt"))   return "text/plain; charset=utf-8";
    if (wa_streq(ext, ".png"))   return "image/png";
    if (wa_streq(ext, ".ico"))   return "image/x-icon";
    if (wa_streq(ext, ".svg"))   return "image/svg+xml";
    return "application/octet-stream";
}

// ─── Built-in Navigator welcome page ─────────────────────────────────────────
// Installed automatically by webapp_init() at "/".
// Keep it small enough to fit in WEBAPP_CONTENT_LEN (4 KiB).
static const char BUILTIN_INDEX[] =
    "<!DOCTYPE html>"
    "<html lang=\"en\">"
    "<head>"
    "<meta charset=\"utf-8\">"
    "<title>AeroSLS Navigator</title>"
    "<style>"
    "body{font-family:monospace;background:#0d0d0d;color:#c8c8c8;margin:2rem;}"
    "h1{color:#5fd7ff;} h2{color:#afd7af;border-bottom:1px solid #333;padding-bottom:4px;}"
    "a{color:#87ceeb;} code{background:#1e1e1e;padding:2px 6px;border-radius:3px;}"
    "pre{background:#1a1a1a;padding:1rem;border-left:3px solid #5fd7ff;}"
    "table{border-collapse:collapse;width:100%;}"
    "th,td{border:1px solid #333;padding:6px 12px;text-align:left;}"
    "th{background:#1e1e1e;color:#5fd7ff;}"
    ".badge{display:inline-block;padding:2px 8px;border-radius:3px;"
           "font-size:0.8em;background:#1e4a1e;color:#5fd75f;}"
    "</style>"
    "</head>"
    "<body>"
    "<h1>&#9774; AeroSLS Navigator <span class=\"badge\">4.0-SLS</span></h1>"
    "<p>Single-Level Storage Operating System &mdash; "
    "zero-abstraction, hardware-driven, distributed.</p>"
    "<h2>REST API</h2>"
    "<table>"
    "<tr><th>Endpoint</th><th>Method</th><th>Description</th></tr>"
    "<tr><td><a href=\"/api/health\">/api/health</a></td>"
        "<td>GET</td><td>System health ping</td></tr>"
    "<tr><td><a href=\"/api/scan\">/api/scan</a></td>"
        "<td>GET</td><td>Full catalog manifest (JSON)</td></tr>"
    "</table>"
    "<h2>Shell Quick Reference</h2>"
    "<pre>"
    "valloc MyDB 1 4         Create a 4-page DB_TABLE\n"
    "insert MyDB key value   Add a record field\n"
    "select MyDB             Dump all fields\n"
    "tx begin                Start ACID transaction\n"
    "tx commit               Commit staged writes\n"
    "demo MyApp              Spawn built-in Ring-3 demo\n"
    "query audit ledgers     AI cognitive object scan\n"
    "svc list                Microkernel service health\n"
    "tier list               Storage tier map\n"
    "wal dump                Write-ahead log"
    "</pre>"
    "<p style=\"color:#555;font-size:0.85em\">"
    "AeroSLS &copy; 2026 &mdash; Independent Researcher "
    "(<a href=\"mailto:dave@gridworkz.com\">dave@gridworkz.com</a>)"
    "</p>"
    "</body></html>";

// ─── webapp_init ──────────────────────────────────────────────────────────────
void webapp_init(void) {
    for (int i = 0; i < WEBAPP_MAX_ASSETS; i++) webapp_store[i].active = 0;

    // Install the built-in Navigator welcome page at "/"
    struct WebAsset* root = &webapp_store[0];
    root->active      = 1;
    wa_strncpy(root->obj_name, "_builtin", OBJECT_NAME_LEN);
    wa_strncpy(root->path,     "/",        WEBAPP_PATH_LEN);
    wa_strncpy(root->mime, "text/html; charset=utf-8", WEBAPP_MIME_LEN);
    int len = wa_strlen(BUILTIN_INDEX);
    if (len >= WEBAPP_CONTENT_LEN) len = WEBAPP_CONTENT_LEN - 1;
    wa_strncpy(root->content, BUILTIN_INDEX, WEBAPP_CONTENT_LEN);
    root->content_len = (uint32_t)len;

    // Alias /index.html to "/"
    struct WebAsset* idx = &webapp_store[1];
    *idx = *root;
    wa_strncpy(idx->path, "/index.html", WEBAPP_PATH_LEN);

    kernel_serial_printf(
        "[WEBAPP] Asset store ready. Built-in Navigator page: %u bytes.\n",
        root->content_len);
}

// ─── webapp_find ─────────────────────────────────────────────────────────────
struct WebAsset* webapp_find(const char* path) {
    for (int i = 0; i < WEBAPP_MAX_ASSETS; i++) {
        if (webapp_store[i].active && wa_streq(webapp_store[i].path, path))
            return &webapp_store[i];
    }
    return 0;
}

struct WebAsset* webapp_find_in(const char* obj_name, const char* path) {
    for (int i = 0; i < WEBAPP_MAX_ASSETS; i++) {
        if (!webapp_store[i].active) continue;
        if (wa_streq(webapp_store[i].obj_name, obj_name) &&
            wa_streq(webapp_store[i].path, path))
            return &webapp_store[i];
    }
    return 0;
}

// ─── sys_sls_webapp_set ───────────────────────────────────────────────────────
uint64_t sys_sls_webapp_set(struct WebAppSetRequest* req) {
    if (!req || !req->path[0]) return 1;

    // Check that the parent object exists and is WEB_APP (or builtin)
    int found_obj = 0;
    for (uint32_t i = 0; i < object_catalog_count; i++) {
        if (!object_catalog[i].active) continue;
        if (!wa_streq(object_catalog[i].name, req->obj_name)) continue;
        if (object_catalog[i].type == OBJ_TYPE_WEB_APP) { found_obj = 1; break; }
        kernel_serial_printf("[WEBAPP] '%s' is not a WEB_APP object.\n",
                             req->obj_name);
        return 1;
    }
    if (!found_obj && !wa_streq(req->obj_name, "_builtin")) {
        kernel_serial_printf("[WEBAPP] WEB_APP object '%s' not found. "
                             "Create with: valloc %s 5 4\n",
                             req->obj_name, req->obj_name);
        return 1;
    }

    // Find existing asset or allocate a new slot
    struct WebAsset* asset = webapp_find_in(req->obj_name, req->path);
    if (!asset) {
        for (int i = 0; i < WEBAPP_MAX_ASSETS; i++) {
            if (!webapp_store[i].active) { asset = &webapp_store[i]; break; }
        }
    }
    if (!asset) {
        kernel_serial_print("[WEBAPP] Asset store full.\n");
        return 1;
    }

    if (!asset->active) {
        // New asset
        asset->active = 1;
        wa_strncpy(asset->obj_name, req->obj_name, OBJECT_NAME_LEN);
        wa_strncpy(asset->path, req->path, WEBAPP_PATH_LEN);
        wa_strncpy(asset->mime, webapp_mime_for(req->path), WEBAPP_MIME_LEN);
        asset->content_len = 0;
    }

    if (req->append && asset->content_len > 0) {
        // Append mode: add content after the current end
        uint32_t avail = WEBAPP_CONTENT_LEN - asset->content_len - 1;
        uint32_t add   = req->content_len < avail ? req->content_len : avail;
        for (uint32_t i = 0; i < add; i++)
            asset->content[asset->content_len + i] = req->content[i];
        asset->content_len += add;
        asset->content[asset->content_len] = '\0';
    } else {
        // Replace mode
        uint32_t len = req->content_len < WEBAPP_CONTENT_LEN - 1
                       ? req->content_len : WEBAPP_CONTENT_LEN - 1;
        for (uint32_t i = 0; i < len; i++) asset->content[i] = req->content[i];
        asset->content[len] = '\0';
        asset->content_len = len;
    }

    kernel_serial_printf(
        "[WEBAPP] %s '%s' @ '%s'  MIME: %s  len=%u\n",
        req->append ? "Appended to" : "Set",
        req->obj_name, req->path, asset->mime, asset->content_len);
    return 0;
}

// ─── sys_sls_webapp_list ──────────────────────────────────────────────────────
void sys_sls_webapp_list(const char* obj_name) {
    int all = wa_streq(obj_name, "*") || obj_name[0] == '\0';
    kernel_serial_print(
        "\n[WEBAPP] Asset Store\n"
        " %-20s  %-28s  %-30s  %s\n"
        " --------------------  ----------------------------  "
        "------------------------------  ----\n",
        "Object", "Path", "MIME", "Len");
    int shown = 0;
    for (int i = 0; i < WEBAPP_MAX_ASSETS; i++) {
        if (!webapp_store[i].active) continue;
        if (!all && !wa_streq(webapp_store[i].obj_name, obj_name)) continue;
        kernel_serial_printf(
            " %-20s  %-28s  %-30s  %u\n",
            webapp_store[i].obj_name,
            webapp_store[i].path,
            webapp_store[i].mime,
            webapp_store[i].content_len);
        shown++;
    }
    if (!shown) kernel_serial_print(" (no assets)\n");
    kernel_serial_printf(" %d asset(s).\n\n", shown);
}
