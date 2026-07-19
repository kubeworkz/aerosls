/*
 * row_index.c — Phase 17 (relational layer) B-tree indexing for row-set
 * tables. See row_index.h for the full design writeup.
 */
#include "row_index.h"
#include "object_catalog.h"
#include "kernel_io.h"
#include "persist.h"   // Gap Remediation Phase D -- persist_row_index_defs()
#include "../user/permissions.h"
#include <stddef.h>

struct RowIndex row_indexes[ROW_INDEX_MAX];

// ─── Node pool — shared bump allocator across every index (no reclaim in
// this first cut, matching rowstore.c's page pool). ──────────────────────────
static struct BTreeNode btree_nodes[BTREE_MAX_NODES];
static uint32_t         btree_next_free_node = 0;

// ─── String / parsing helpers (no libc — each kernel source file keeps its
// own small copies, matching this codebase's established convention). ───────
static uint32_t ri_strlen(const char* s) { uint32_t n = 0; while (s[n]) n++; return n; }
static int ri_streq(const char* a, const char* b) {
    while (*a && *b) { if (*a != *b) return 0; a++; b++; }
    return *a == *b;
}
static void ri_strcpy(char* dst, const char* src, uint32_t max) {
    uint32_t i = 0; for (; i < max - 1 && src[i]; i++) dst[i] = src[i]; dst[i] = '\0';
}
static void ri_memcpy(void* d, const void* s, uint32_t n) {
    uint8_t* dd = (uint8_t*)d; const uint8_t* ss = (const uint8_t*)s;
    while (n--) *dd++ = *ss++;
}
static void ri_memset(void* d, uint8_t v, uint32_t n) {
    uint8_t* p = (uint8_t*)d; while (n--) *p++ = v;
}
static int ri_memcmp(const void* a, const void* b, uint32_t n) {
    const uint8_t* aa = (const uint8_t*)a; const uint8_t* bb = (const uint8_t*)b;
    for (uint32_t i = 0; i < n; i++) if (aa[i] != bb[i]) return (int)aa[i] - (int)bb[i];
    return 0;
}
static int ri_parse_u64(const char* s, uint64_t* out) {
    if (!s || !s[0]) return 1;
    uint64_t v = 0;
    for (uint32_t i = 0; s[i]; i++) {
        if (s[i] < '0' || s[i] > '9') return 1;
        v = v * 10 + (uint64_t)(s[i] - '0');
    }
    *out = v;
    return 0;
}
static int ri_parse_f64(const char* s, double* out) {
    if (!s || !s[0]) return 1;
    uint32_t i = 0;
    int neg = 0;
    if (s[i] == '-') { neg = 1; i++; }
    if (!s[i]) return 1;
    double v = 0.0;
    int saw_digit = 0;
    for (; s[i] >= '0' && s[i] <= '9'; i++) { v = v * 10.0 + (double)(s[i] - '0'); saw_digit = 1; }
    if (s[i] == '.') {
        i++;
        double frac = 0.1;
        for (; s[i] >= '0' && s[i] <= '9'; i++) { v += (double)(s[i] - '0') * frac; frac *= 0.1; saw_digit = 1; }
    }
    if (s[i] != '\0' || !saw_digit) return 1;
    *out = neg ? -v : v;
    return 0;
}
static int ri_parse_bool(const char* s, uint8_t* out) {
    if (ri_streq(s, "true") || ri_streq(s, "1"))  { *out = 1; return 0; }
    if (ri_streq(s, "false") || ri_streq(s, "0")) { *out = 0; return 0; }
    return 1;
}

// ─── Canonical, memcmp-comparable key encoding ──────────────────────────────
// Always fills the full BTREE_MAX_KEY_BYTES buffer (zero-padded); every
// comparison memcmp's the full buffer, so no per-key length tracking is
// needed — see row_index.h's header comment for why this is correct.
static int encode_key(SLSFieldType type, const char* text, uint8_t out[BTREE_MAX_KEY_BYTES]) {
    ri_memset(out, 0, BTREE_MAX_KEY_BYTES);
    switch (type) {
        case FIELD_TYPE_UINT64: {
            uint64_t v; if (ri_parse_u64(text, &v)) return 1;
            for (int i = 0; i < 8; i++) out[i] = (uint8_t)(v >> (56 - i * 8));
            return 0;
        }
        case FIELD_TYPE_FLOAT: {
            double v; if (ri_parse_f64(text, &v)) return 1;
            uint64_t bits; ri_memcpy(&bits, &v, 8);
            // Standard IEEE-754-to-memcmp-orderable transform: negative
            // values flip every bit (reverses order within negatives, and
            // places them below all positives); non-negative values just
            // get the sign bit set (so they sort above all negatives).
            if (bits & 0x8000000000000000ULL) bits = ~bits;
            else                                bits |= 0x8000000000000000ULL;
            for (int i = 0; i < 8; i++) out[i] = (uint8_t)(bits >> (56 - i * 8));
            return 0;
        }
        case FIELD_TYPE_BOOL: {
            uint8_t v; if (ri_parse_bool(text, &v)) return 1;
            out[0] = v;
            return 0;
        }
        case FIELD_TYPE_STRING:
        default: {
            uint32_t len = ri_strlen(text);
            if (len >= BTREE_MAX_KEY_BYTES) return 1;   // reject, never silently truncate
            ri_memcpy(out, text, len);
            return 0;
        }
    }
}

static int key_cmp(const uint8_t* a, const uint8_t* b) {
    return ri_memcmp(a, b, BTREE_MAX_KEY_BYTES);
}

// ─── Node pool ───────────────────────────────────────────────────────────────
static uint32_t alloc_node(int is_leaf) {
    if (btree_next_free_node >= BTREE_MAX_NODES) return BTREE_INVALID_NODE;
    uint32_t id = btree_next_free_node++;
    struct BTreeNode* n = &btree_nodes[id];
    ri_memset(n, 0, sizeof(*n));
    n->active = 1;
    n->is_leaf = (uint8_t)is_leaf;
    n->next_leaf = BTREE_INVALID_NODE;
    for (uint32_t i = 0; i < BTREE_ORDER + 1; i++) n->children[i] = BTREE_INVALID_NODE;
    return id;
}

// ─── Table / index lookup ────────────────────────────────────────────────────
static int find_table_catalog_index(const char* table_name) {
    for (uint32_t i = 0; i < object_catalog_count; i++) {
        if (!object_catalog[i].active) continue;
        if (!object_catalog[i].uses_rowstore) continue;
        if (ri_streq(object_catalog[i].name, table_name)) return (int)i;
    }
    return -1;
}

static int find_index_slot(const char* index_name) {
    for (uint32_t i = 0; i < ROW_INDEX_MAX; i++) {
        if (row_indexes[i].active && ri_streq(row_indexes[i].index_name, index_name))
            return (int)i;
    }
    return -1;
}

// ─── Lifecycle ────────────────────────────────────────────────────────────
void row_index_init(void) {
    ri_memset(row_indexes, 0, sizeof(row_indexes));
    ri_memset(btree_nodes, 0, sizeof(btree_nodes));
    btree_next_free_node = 0;
    kernel_serial_print("[ROW_INDEX] B-tree row index engine initialised.\n");
}

// ─── Core insert (used by both row_index_create's initial build and the
// notify_insert hook) — operates directly on a struct RowIndex*, given an
// already-encoded key and a row_id. Returns 0 on success, 1 if the
// duplicate cap for this exact key was hit (best-effort -- see header). ─────
static int btree_insert(struct RowIndex* idx, const uint8_t key[BTREE_MAX_KEY_BYTES], struct RowId id) {
    if (idx->root_node == BTREE_INVALID_NODE) {
        uint32_t leaf = alloc_node(1);
        if (leaf == BTREE_INVALID_NODE) return 1;
        struct BTreeNode* n = &btree_nodes[leaf];
        ri_memcpy(n->keys[0], key, BTREE_MAX_KEY_BYTES);
        n->ids[0][0] = id;
        n->id_active[0][0] = 1;
        n->id_count[0] = 1;
        n->key_count = 1;
        idx->root_node = leaf;
        idx->entry_count++;
        return 0;
    }

    // Descend, recording the path of internal node ids from root to (not
    // including) the leaf.
    uint32_t path[32];   // descent depth -- more than enough for BTREE_MAX_NODES at order 4
    uint32_t path_len = 0;
    uint32_t cur = idx->root_node;
    while (!btree_nodes[cur].is_leaf) {
        path[path_len++] = cur;
        struct BTreeNode* n = &btree_nodes[cur];
        uint32_t i = 0;
        while (i < n->key_count && key_cmp(key, n->keys[i]) >= 0) i++;
        cur = n->children[i];
    }
    uint32_t leaf = cur;
    struct BTreeNode* lf = &btree_nodes[leaf];

    // If this exact key already lives in the leaf, just append to its
    // duplicate list (no structural change, no split possible).
    for (uint32_t i = 0; i < lf->key_count; i++) {
        if (key_cmp(lf->keys[i], key) == 0) {
            if (lf->id_count[i] >= BTREE_MAX_DUPES_PER_KEY) {
                lf->key_capped[i] = 1;   // Phase 25: sticky -- a lookup on this key can no
                                          // longer be trusted as complete, ever, even after
                                          // later deletes bring the active count back down
                return 1;                 // duplicate cap hit
            }
            lf->ids[i][lf->id_count[i]] = id;
            lf->id_active[i][lf->id_count[i]] = 1;
            lf->id_count[i]++;
            idx->entry_count++;
            return 0;
        }
    }

    // New distinct key for this leaf — insert in sorted position.
    uint32_t i = 0;
    while (i < lf->key_count && key_cmp(lf->keys[i], key) < 0) i++;
    for (uint32_t j = lf->key_count; j > i; j--) {
        ri_memcpy(lf->keys[j], lf->keys[j - 1], BTREE_MAX_KEY_BYTES);
        ri_memcpy(lf->ids[j], lf->ids[j - 1], sizeof(lf->ids[j]));
        ri_memcpy(lf->id_active[j], lf->id_active[j - 1], sizeof(lf->id_active[j]));
        lf->id_count[j] = lf->id_count[j - 1];
        lf->key_capped[j] = lf->key_capped[j - 1];
    }
    ri_memcpy(lf->keys[i], key, BTREE_MAX_KEY_BYTES);
    ri_memset(lf->id_active[i], 0, sizeof(lf->id_active[i]));
    lf->ids[i][0] = id;
    lf->id_active[i][0] = 1;
    lf->id_count[i] = 1;
    lf->key_capped[i] = 0;   // brand-new distinct key: definitely not capped yet
    lf->key_count++;
    idx->entry_count++;

    if (lf->key_count <= BTREE_ORDER - 1) return 0;   // fits, no split needed

    // ── Leaf overflow: split. Left keeps [0, mid); right gets [mid, count).
    // The separator promoted to the parent is a COPY of the right leaf's
    // first key (classic B+-tree: leaves retain every key). ─────────────────
    uint32_t mid = BTREE_ORDER / 2;
    uint32_t new_right = alloc_node(1);
    if (new_right == BTREE_INVALID_NODE) return 0;   // pool exhausted: entry is in, tree just stays overfull
    struct BTreeNode* left = &btree_nodes[leaf];      // re-fetch: alloc_node may not move memory, but be explicit
    struct BTreeNode* right = &btree_nodes[new_right];
    uint32_t old_count = left->key_count;
    for (uint32_t k = mid; k < old_count; k++) {
        uint32_t dst = k - mid;
        ri_memcpy(right->keys[dst], left->keys[k], BTREE_MAX_KEY_BYTES);
        ri_memcpy(right->ids[dst], left->ids[k], sizeof(right->ids[dst]));
        ri_memcpy(right->id_active[dst], left->id_active[k], sizeof(right->id_active[dst]));
        right->id_count[dst] = left->id_count[k];
        right->key_capped[dst] = left->key_capped[k];
    }
    right->key_count = old_count - mid;
    left->key_count = mid;
    right->next_leaf = left->next_leaf;
    left->next_leaf = new_right;

    uint8_t sep[BTREE_MAX_KEY_BYTES];
    ri_memcpy(sep, right->keys[0], BTREE_MAX_KEY_BYTES);
    uint32_t child_to_attach = new_right;

    // ── Propagate the split up the recorded path ───────────────────────────
    for (int p = (int)path_len - 1; p >= 0; p--) {
        uint32_t parent_id = path[p];
        struct BTreeNode* parent = &btree_nodes[parent_id];

        // insert sep + child_to_attach into parent in sorted position
        uint32_t pi = 0;
        while (pi < parent->key_count && key_cmp(parent->keys[pi], sep) < 0) pi++;
        for (uint32_t j = parent->key_count; j > pi; j--)
            ri_memcpy(parent->keys[j], parent->keys[j - 1], BTREE_MAX_KEY_BYTES);
        for (uint32_t j = parent->key_count + 1; j > pi + 1; j--)
            parent->children[j] = parent->children[j - 1];
        ri_memcpy(parent->keys[pi], sep, BTREE_MAX_KEY_BYTES);
        parent->children[pi + 1] = child_to_attach;
        parent->key_count++;

        if (parent->key_count <= BTREE_ORDER - 1) return 0;   // absorbed, done

        // ── Internal-node overflow: split. The middle key is PROMOTED
        // (removed from both halves), not copied — differs from leaf split. ──
        uint32_t imid = BTREE_ORDER / 2;
        uint32_t new_parent_right = alloc_node(0);
        if (new_parent_right == BTREE_INVALID_NODE) return 0;   // pool exhausted: tree stays overfull here
        struct BTreeNode* pleft = &btree_nodes[parent_id];
        struct BTreeNode* pright = &btree_nodes[new_parent_right];
        uint32_t old_pcount = pleft->key_count;
        uint8_t promoted[BTREE_MAX_KEY_BYTES];
        ri_memcpy(promoted, pleft->keys[imid], BTREE_MAX_KEY_BYTES);

        for (uint32_t k = imid + 1; k < old_pcount; k++)
            ri_memcpy(pright->keys[k - (imid + 1)], pleft->keys[k], BTREE_MAX_KEY_BYTES);
        pright->key_count = old_pcount - (imid + 1);
        for (uint32_t k = imid + 1; k <= old_pcount; k++)
            pright->children[k - (imid + 1)] = pleft->children[k];
        pleft->key_count = imid;

        ri_memcpy(sep, promoted, BTREE_MAX_KEY_BYTES);
        child_to_attach = new_parent_right;
        // loop continues to path[p-1], or falls through below if p==0
    }

    // Path exhausted (the root itself split, or the root was a leaf that
    // just split) — create a new root above the old one. root_node's node
    // id still holds the left half in place (splits always keep the
    // original id as the left half), so it's still correct here regardless
    // of how many levels propagated.
    uint32_t new_root = alloc_node(0);
    if (new_root == BTREE_INVALID_NODE) return 0;   // pool exhausted: old root stays the de facto root, tree overfull
    struct BTreeNode* nr = &btree_nodes[new_root];
    ri_memcpy(nr->keys[0], sep, BTREE_MAX_KEY_BYTES);
    nr->children[0] = idx->root_node;
    nr->children[1] = child_to_attach;
    nr->key_count = 1;
    idx->root_node = new_root;
    return 0;
}

// ─── Core delete: tombstone one row_id from the given key's duplicate list.
// No rebalancing/merging in this first cut. ──────────────────────────────────
static void btree_delete(struct RowIndex* idx, const uint8_t key[BTREE_MAX_KEY_BYTES], struct RowId id) {
    if (idx->root_node == BTREE_INVALID_NODE) return;
    uint32_t cur = idx->root_node;
    while (!btree_nodes[cur].is_leaf) {
        struct BTreeNode* n = &btree_nodes[cur];
        uint32_t i = 0;
        while (i < n->key_count && key_cmp(key, n->keys[i]) >= 0) i++;
        cur = n->children[i];
    }
    struct BTreeNode* lf = &btree_nodes[cur];
    for (uint32_t i = 0; i < lf->key_count; i++) {
        if (key_cmp(lf->keys[i], key) != 0) continue;
        for (uint32_t d = 0; d < lf->id_count[i]; d++) {
            if (!lf->id_active[i][d]) continue;
            if (lf->ids[i][d].page_id == id.page_id && lf->ids[i][d].slot_index == id.slot_index) {
                lf->id_active[i][d] = 0;
                if (idx->entry_count) idx->entry_count--;
                return;
            }
        }
        return;   // key found, but this specific row_id wasn't in it (shouldn't happen)
    }
}

// ─── row_index_create ───────────────────────────────────────────────────────
struct build_ctx { struct RowIndex* idx; uint32_t column_index; int fail; };

static void build_cb(struct RowId id, const struct RowValues* values, void* ctxp) {
    struct build_ctx* ctx = (struct build_ctx*)ctxp;
    uint8_t key[BTREE_MAX_KEY_BYTES];
    if (encode_key(ctx->idx->column_type, values->values[ctx->column_index], key)) {
        ctx->fail = 1;
        return;
    }
    btree_insert(ctx->idx, key, id);
}

int row_index_create(uint32_t caller_uid, const char* index_name,
                     const char* table_name, const char* column_name) {
    if (!index_name || !table_name || !column_name) return 3;

    int tidx = find_table_catalog_index(table_name);
    if (tidx < 0) return 1;
    if (!catalog_check_access(caller_uid, table_name, PERM_WRITE)) return 2;

    if (find_index_slot(index_name) >= 0) return 4;
    int slot = -1;
    for (uint32_t i = 0; i < ROW_INDEX_MAX; i++) {
        if (!row_indexes[i].active) { slot = (int)i; break; }
    }
    if (slot < 0) return 4;

    struct RowTableLayout* layout = &table_headers[tidx].layout;
    uint32_t col = 0xFFFFFFFFu;
    for (uint32_t i = 0; i < layout->column_count; i++) {
        if (ri_streq(layout->column_names[i], column_name)) { col = i; break; }
    }
    if (col == 0xFFFFFFFFu) return 3;

    struct RowIndex* idx = &row_indexes[slot];
    ri_memset(idx, 0, sizeof(*idx));
    ri_strcpy(idx->index_name, index_name, OBJECT_NAME_LEN);
    idx->table_object_id = object_catalog[tidx].object_id;
    idx->column_index = col;
    idx->column_type = layout->column_types[col];
    idx->root_node = BTREE_INVALID_NODE;
    idx->entry_count = 0;
    idx->active = 1;

    struct build_ctx ctx = { idx, col, 0 };
    rowstore_table_scan(caller_uid, table_name, build_cb, &ctx);
    if (ctx.fail) {
        kernel_serial_printf("[ROW_INDEX] '%s': warning -- one or more rows had an unencodable value for column '%s'.\n",
                             index_name, column_name);
    }

    kernel_serial_printf("[ROW_INDEX] '%s' created on %s.%s: %u entrie(s) indexed.\n",
                         index_name, table_name, column_name, idx->entry_count);

    persist_row_index_defs();   // Gap Remediation Phase D
    return 0;
}

// ─── Auto-maintenance hooks ──────────────────────────────────────────────────
static void for_each_index_on_table(uint64_t table_object_id,
                                    void (*fn)(struct RowIndex*, uint32_t, const struct RowValues*, struct RowId),
                                    uint32_t column_hint_unused, const struct RowValues* values, struct RowId id) {
    (void)column_hint_unused;
    for (uint32_t i = 0; i < ROW_INDEX_MAX; i++) {
        if (!row_indexes[i].active) continue;
        if (row_indexes[i].table_object_id != table_object_id) continue;
        fn(&row_indexes[i], row_indexes[i].column_index, values, id);
    }
}

static void insert_apply(struct RowIndex* idx, uint32_t col, const struct RowValues* values, struct RowId id) {
    if (col >= values->count) return;
    uint8_t key[BTREE_MAX_KEY_BYTES];
    if (encode_key(idx->column_type, values->values[col], key)) return;
    btree_insert(idx, key, id);
}

static void delete_apply(struct RowIndex* idx, uint32_t col, const struct RowValues* values, struct RowId id) {
    if (col >= values->count) return;
    uint8_t key[BTREE_MAX_KEY_BYTES];
    if (encode_key(idx->column_type, values->values[col], key)) return;
    btree_delete(idx, key, id);
}

void row_index_notify_insert(uint64_t table_object_id, struct RowId id,
                             const struct RowValues* values, const struct RowTableLayout* layout) {
    (void)layout;
    if (!values) return;
    for_each_index_on_table(table_object_id, insert_apply, 0, values, id);
}

void row_index_notify_delete(uint64_t table_object_id, struct RowId id,
                             const struct RowValues* values, const struct RowTableLayout* layout) {
    (void)layout;
    if (!values) return;
    for_each_index_on_table(table_object_id, delete_apply, 0, values, id);
}

void row_index_notify_update(uint64_t table_object_id, struct RowId id,
                             const struct RowValues* old_values, const struct RowValues* new_values,
                             const struct RowTableLayout* layout) {
    (void)layout;
    if (!old_values || !new_values) return;
    for (uint32_t i = 0; i < ROW_INDEX_MAX; i++) {
        if (!row_indexes[i].active) continue;
        if (row_indexes[i].table_object_id != table_object_id) continue;
        struct RowIndex* idx = &row_indexes[i];
        uint32_t col = idx->column_index;
        if (col >= old_values->count || col >= new_values->count) continue;

        uint8_t old_key[BTREE_MAX_KEY_BYTES], new_key[BTREE_MAX_KEY_BYTES];
        int old_ok = !encode_key(idx->column_type, old_values->values[col], old_key);
        int new_ok = !encode_key(idx->column_type, new_values->values[col], new_key);
        if (old_ok && new_ok && ri_memcmp(old_key, new_key, BTREE_MAX_KEY_BYTES) == 0)
            continue;   // indexed value unchanged -- nothing to do

        if (old_ok) btree_delete(idx, old_key, id);
        if (new_ok) btree_insert(idx, new_key, id);
    }
}

// ─── Query ────────────────────────────────────────────────────────────────
// Shared traversal: descend to the leaf that would contain `lo` (or the
// leftmost leaf if lo is NULL/unbounded), then walk the leaf chain
// collecting active ids for keys in [lo, hi] until a key exceeds hi (or the
// chain ends). Exact-match lookup is range_scan(value, value).
static uint32_t range_scan_internal(struct RowIndex* idx, const uint8_t* lo, const uint8_t* hi,
                                    struct RowId* out_ids, uint32_t max_ids) {
    if (idx->root_node == BTREE_INVALID_NODE) return 0;

    uint32_t cur = idx->root_node;
    while (!btree_nodes[cur].is_leaf) {
        struct BTreeNode* n = &btree_nodes[cur];
        uint32_t i = 0;
        if (lo) {
            while (i < n->key_count && key_cmp(lo, n->keys[i]) >= 0) i++;
        } else {
            i = 0;   // unbounded lo: always take the leftmost child
        }
        cur = n->children[i];
    }

    uint32_t found = 0;
    while (cur != BTREE_INVALID_NODE) {
        struct BTreeNode* lf = &btree_nodes[cur];
        for (uint32_t i = 0; i < lf->key_count; i++) {
            if (lo && key_cmp(lf->keys[i], lo) < 0) continue;
            if (hi && key_cmp(lf->keys[i], hi) > 0) return found;   // sorted -- done entirely
            for (uint32_t d = 0; d < lf->id_count[i]; d++) {
                if (!lf->id_active[i][d]) continue;
                if (found < max_ids && out_ids) out_ids[found] = lf->ids[i][d];
                found++;
            }
        }
        cur = lf->next_leaf;
    }
    return found;
}

int row_index_find_for_column(uint64_t table_object_id, uint32_t column_index,
                              char* index_name_out) {
    for (uint32_t i = 0; i < ROW_INDEX_MAX; i++) {
        if (!row_indexes[i].active) continue;
        if (row_indexes[i].table_object_id != table_object_id) continue;
        if (row_indexes[i].column_index != column_index) continue;
        ri_strcpy(index_name_out, row_indexes[i].index_name, OBJECT_NAME_LEN);
        return 1;
    }
    return 0;
}

uint32_t row_index_lookup(uint32_t caller_uid, const char* index_name,
                          const char* value, struct RowId* out_ids, uint32_t max_ids) {
    int slot = find_index_slot(index_name);
    if (slot < 0) return 0;
    struct RowIndex* idx = &row_indexes[slot];

    int tidx = -1;
    for (uint32_t i = 0; i < object_catalog_count; i++)
        if (object_catalog[i].active && object_catalog[i].object_id == idx->table_object_id) { tidx = (int)i; break; }
    if (tidx < 0) return 0;
    if (!catalog_check_access(caller_uid, object_catalog[tidx].name, PERM_READ)) return 0;

    uint8_t key[BTREE_MAX_KEY_BYTES];
    if (encode_key(idx->column_type, value, key)) return 0;
    return range_scan_internal(idx, key, key, out_ids, max_ids);
}

// Descends to the one leaf that would hold `key` and reports whether that
// exact key's duplicate list was ever capped. Returns 0 (not capped) if the
// key isn't present at all -- an absent key has, by construction, never had
// an insert attempted against it that could have been dropped.
static int leaf_key_capped(struct RowIndex* idx, const uint8_t key[BTREE_MAX_KEY_BYTES]) {
    if (idx->root_node == BTREE_INVALID_NODE) return 0;
    uint32_t cur = idx->root_node;
    while (!btree_nodes[cur].is_leaf) {
        struct BTreeNode* n = &btree_nodes[cur];
        uint32_t i = 0;
        while (i < n->key_count && key_cmp(key, n->keys[i]) >= 0) i++;
        cur = n->children[i];
    }
    struct BTreeNode* lf = &btree_nodes[cur];
    for (uint32_t i = 0; i < lf->key_count; i++) {
        if (key_cmp(lf->keys[i], key) == 0) return lf->key_capped[i];
    }
    return 0;
}

uint32_t row_index_lookup_checked(uint32_t caller_uid, const char* index_name,
                                  const char* value, struct RowId* out_ids, uint32_t max_ids,
                                  uint8_t* out_complete) {
    if (out_complete) *out_complete = 1;
    int slot = find_index_slot(index_name);
    if (slot < 0) return 0;
    struct RowIndex* idx = &row_indexes[slot];

    int tidx = -1;
    for (uint32_t i = 0; i < object_catalog_count; i++)
        if (object_catalog[i].active && object_catalog[i].object_id == idx->table_object_id) { tidx = (int)i; break; }
    if (tidx < 0) return 0;
    if (!catalog_check_access(caller_uid, object_catalog[tidx].name, PERM_READ)) return 0;

    uint8_t key[BTREE_MAX_KEY_BYTES];
    if (encode_key(idx->column_type, value, key)) return 0;

    if (out_complete) *out_complete = (uint8_t)!leaf_key_capped(idx, key);
    return range_scan_internal(idx, key, key, out_ids, max_ids);
}

uint32_t row_index_range_scan(uint32_t caller_uid, const char* index_name,
                              const char* lo, const char* hi,
                              struct RowId* out_ids, uint32_t max_ids) {
    int slot = find_index_slot(index_name);
    if (slot < 0) return 0;
    struct RowIndex* idx = &row_indexes[slot];

    int tidx = -1;
    for (uint32_t i = 0; i < object_catalog_count; i++)
        if (object_catalog[i].active && object_catalog[i].object_id == idx->table_object_id) { tidx = (int)i; break; }
    if (tidx < 0) return 0;
    if (!catalog_check_access(caller_uid, object_catalog[tidx].name, PERM_READ)) return 0;

    uint8_t lo_key[BTREE_MAX_KEY_BYTES], hi_key[BTREE_MAX_KEY_BYTES];
    const uint8_t* lo_p = 0; const uint8_t* hi_p = 0;
    if (lo) { if (encode_key(idx->column_type, lo, lo_key)) return 0; lo_p = lo_key; }
    if (hi) { if (encode_key(idx->column_type, hi, hi_key)) return 0; hi_p = hi_key; }

    return range_scan_internal(idx, lo_p, hi_p, out_ids, max_ids);
}
