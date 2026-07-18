#include "constraint.h"
#include "kernel_io.h"
#include "index_mgr.h"   // key_matches_field logic reused via object_catalog

// ─── Globals ──────────────────────────────────────────────────────────────────
struct SLSConstraint constraint_table[CONSTRAINT_MAX];
uint32_t             constraint_count = 0;

// ─── Internal helpers ─────────────────────────────────────────────────────────
static void cn_cpy(char* dst, const char* src, int n) {
    int i = 0;
    for (; i < n - 1 && src[i]; i++) dst[i] = src[i];
    dst[i] = '\0';
}

static int cn_eq(const char* a, const char* b) {
    while (*a && *b) { if (*a != *b) return 0; a++; b++; }
    return *a == *b;
}

// Suffix match: "alice_dept" matches field "dept"
static int cn_field_match(const char* key, const char* field) {
    if (cn_eq(key, field)) return 1;
    int klen = 0; while (key[klen])   klen++;
    int flen = 0; while (field[flen]) flen++;
    if (klen > flen + 1 && key[klen - flen - 1] == '_')
        return cn_eq(key + klen - flen, field);
    return 0;
}

// Parse a decimal string into int64 (handles negative)
static int64_t cn_atoi(const char* s) {
    int64_t v = 0; int neg = 0;
    if (*s == '-') { neg = 1; s++; }
    while (*s >= '0' && *s <= '9') { v = v * 10 + (*s - '0'); s++; }
    return neg ? -v : v;
}

// ─── constraint_init ─────────────────────────────────────────────────────────
void constraint_init(void) {
    for (int i = 0; i < CONSTRAINT_MAX; i++) constraint_table[i].active = 0;
    constraint_count = 0;
    kernel_serial_print("[CONSTRAINT] Engine initialised.\n");
}

// ─── Internal: allocate and fill a slot ──────────────────────────────────────
static struct SLSConstraint* alloc_constraint(const char* table,
                                               const char* field,
                                               ConstraintType type)
{
    if (constraint_count >= CONSTRAINT_MAX) {
        kernel_serial_print("[CONSTRAINT] table full\n");
        return 0;
    }
    for (int i = 0; i < CONSTRAINT_MAX; i++) {
        if (constraint_table[i].active) continue;
        struct SLSConstraint* c = &constraint_table[i];
        cn_cpy(c->table_name, table, (int)sizeof(c->table_name));
        cn_cpy(c->field_name, field, (int)sizeof(c->field_name));
        c->type   = (uint8_t)type;
        c->active = 1;
        constraint_count++;
        return c;
    }
    return 0;
}

// ─── Public add helpers ───────────────────────────────────────────────────────
int constraint_add_unique(const char* table, const char* field) {
    struct SLSConstraint* c = alloc_constraint(table, field, CTYPE_UNIQUE);
    if (!c) return 1;
    kernel_serial_print("[CONSTRAINT] UNIQUE on ");
    kernel_serial_print(table); kernel_serial_print("."); kernel_serial_print(field);
    kernel_serial_print("\n");
    return 0;
}

int constraint_add_not_null(const char* table, const char* field) {
    struct SLSConstraint* c = alloc_constraint(table, field, CTYPE_NOT_NULL);
    if (!c) return 1;
    kernel_serial_print("[CONSTRAINT] NOT_NULL on ");
    kernel_serial_print(table); kernel_serial_print("."); kernel_serial_print(field);
    kernel_serial_print("\n");
    return 0;
}

int constraint_add_range(const char* table, const char* field,
                         int64_t min, int64_t max)
{
    struct SLSConstraint* c = alloc_constraint(table, field, CTYPE_RANGE);
    if (!c) return 1;
    c->range_min = min;
    c->range_max = max;
    kernel_serial_print("[CONSTRAINT] RANGE on ");
    kernel_serial_print(table); kernel_serial_print("."); kernel_serial_print(field);
    kernel_serial_print("\n");
    return 0;
}

int constraint_add_reference(const char* table, const char* field,
                              const char* ref_table)
{
    struct SLSConstraint* c = alloc_constraint(table, field, CTYPE_REFERENCE);
    if (!c) return 1;
    cn_cpy(c->ref_table, ref_table, (int)sizeof(c->ref_table));
    kernel_serial_print("[CONSTRAINT] REFERENCE on ");
    kernel_serial_print(table); kernel_serial_print("."); kernel_serial_print(field);
    kernel_serial_print(" -> "); kernel_serial_print(ref_table);
    kernel_serial_print("\n");
    return 0;
}

int constraint_remove(const char* table, const char* field, int type) {
    int removed = 0;
    for (int i = 0; i < CONSTRAINT_MAX; i++) {
        struct SLSConstraint* c = &constraint_table[i];
        if (!c->active) continue;
        if (!cn_eq(c->table_name, table)) continue;
        if (!cn_eq(c->field_name, field)) continue;
        if (type >= 0 && c->type != (uint8_t)type) continue;
        c->active = 0;
        if (constraint_count) constraint_count--;
        removed++;
    }
    return removed > 0 ? 0 : 1;
}

// ─── UNIQUE helper — scan the table for an existing matching value ────────────
static int unique_violated(const char* table_name, const char* field,
                            const char* value, const char* skip_key)
{
    for (uint32_t oi = 0; oi < object_catalog_count; oi++) {
        if (!object_catalog[oi].active) continue;
        if (!cn_eq(object_catalog[oi].name, table_name)) continue;
        struct SLSObjectRecord* rec = &object_records[oi];
        for (uint32_t fi = 0; fi < RECORD_MAX_FIELDS; fi++) {
            if (!rec->fields[fi].active) continue;
            if (!cn_field_match(rec->fields[fi].key, field)) continue;
            if (skip_key && cn_eq(rec->fields[fi].key, skip_key)) continue;
            if (cn_eq(rec->fields[fi].value, value)) return 1;
        }
        break;
    }
    return 0;
}

// ─── REFERENCE helper — check value exists as a key in ref_table ─────────────
static int reference_violated(const char* ref_table, const char* value) {
    for (uint32_t oi = 0; oi < object_catalog_count; oi++) {
        if (!object_catalog[oi].active) continue;
        if (!cn_eq(object_catalog[oi].name, ref_table)) continue;
        struct SLSObjectRecord* rec = &object_records[oi];
        for (uint32_t fi = 0; fi < RECORD_MAX_FIELDS; fi++) {
            if (!rec->fields[fi].active) continue;
            if (cn_eq(rec->fields[fi].key, value)) return 0; // found
        }
        break;
    }
    return 1; // not found
}

// ─── constraint_check_insert ─────────────────────────────────────────────────
int constraint_check_insert(const char* table_name,
                             const char* key,
                             const char* value)
{
    for (int i = 0; i < CONSTRAINT_MAX; i++) {
        struct SLSConstraint* c = &constraint_table[i];
        if (!c->active) continue;
        if (!cn_eq(c->table_name, table_name)) continue;
        if (!cn_field_match(key, c->field_name)) continue;

        switch ((ConstraintType)c->type) {
        case CTYPE_NOT_NULL:
            if (!value || value[0] == '\0') {
                kernel_serial_print("[CONSTRAINT] VIOLATION: NOT_NULL on key ");
                kernel_serial_print(key); kernel_serial_print("\n");
                return 2;
            }
            break;
        case CTYPE_UNIQUE:
            if (unique_violated(table_name, c->field_name, value, 0)) {
                kernel_serial_print("[CONSTRAINT] VIOLATION: UNIQUE on key ");
                kernel_serial_print(key); kernel_serial_print(" value=");
                kernel_serial_print(value); kernel_serial_print("\n");
                return 1;
            }
            break;
        case CTYPE_RANGE: {
            int64_t v = cn_atoi(value);
            if (v < c->range_min || v > c->range_max) {
                kernel_serial_print("[CONSTRAINT] VIOLATION: RANGE on key ");
                kernel_serial_print(key); kernel_serial_print("\n");
                return 3;
            }
            break;
        }
        case CTYPE_REFERENCE:
            if (reference_violated(c->ref_table, value)) {
                kernel_serial_print("[CONSTRAINT] VIOLATION: REFERENCE on key ");
                kernel_serial_print(key); kernel_serial_print(" -> ");
                kernel_serial_print(c->ref_table); kernel_serial_print("\n");
                return 4;
            }
            break;
        }
    }
    return 0;
}

// ─── constraint_check_update ─────────────────────────────────────────────────
int constraint_check_update(const char* table_name,
                             const char* key,
                             const char* old_value,
                             const char* new_value)
{
    for (int i = 0; i < CONSTRAINT_MAX; i++) {
        struct SLSConstraint* c = &constraint_table[i];
        if (!c->active) continue;
        if (!cn_eq(c->table_name, table_name)) continue;
        if (!cn_field_match(key, c->field_name)) continue;

        switch ((ConstraintType)c->type) {
        case CTYPE_NOT_NULL:
            if (!new_value || new_value[0] == '\0') {
                kernel_serial_print("[CONSTRAINT] VIOLATION: NOT_NULL update key ");
                kernel_serial_print(key); kernel_serial_print("\n");
                return 2;
            }
            break;
        case CTYPE_UNIQUE:
            // Skip the current row (skip_key = key) to allow update to same value
            if (unique_violated(table_name, c->field_name, new_value, key)) {
                kernel_serial_print("[CONSTRAINT] VIOLATION: UNIQUE update key ");
                kernel_serial_print(key); kernel_serial_print("\n");
                return 1;
            }
            break;
        case CTYPE_RANGE: {
            int64_t v = cn_atoi(new_value);
            if (v < c->range_min || v > c->range_max) {
                kernel_serial_print("[CONSTRAINT] VIOLATION: RANGE update key ");
                kernel_serial_print(key); kernel_serial_print("\n");
                return 3;
            }
            break;
        }
        case CTYPE_REFERENCE:
            if (reference_violated(c->ref_table, new_value)) {
                kernel_serial_print("[CONSTRAINT] VIOLATION: REFERENCE update key ");
                kernel_serial_print(key); kernel_serial_print("\n");
                return 4;
            }
            break;
        }
        (void)old_value;
    }
    return 0;
}

// ─── constraints_to_json ─────────────────────────────────────────────────────
static const char* ctype_name(uint8_t t) {
    switch ((ConstraintType)t) {
        case CTYPE_UNIQUE:    return "UNIQUE";
        case CTYPE_NOT_NULL:  return "NOT_NULL";
        case CTYPE_RANGE:     return "RANGE";
        case CTYPE_REFERENCE: return "REFERENCE";
        default:              return "?";
    }
}

int constraints_to_json(const char* filter_table, char* buf, int max) {
    int n = 0;
    #define JW(s) do { const char* _s=(s); while(*_s && n<max-2) buf[n++]=*_s++; } while(0)
    #define JWC(c) do { if (n<max-2) buf[n++]=(c); } while(0)

    JWC('[');
    int first = 1;
    for (int i = 0; i < CONSTRAINT_MAX; i++) {
        struct SLSConstraint* c = &constraint_table[i];
        if (!c->active) continue;
        if (filter_table && filter_table[0] && !cn_eq(c->table_name, filter_table))
            continue;
        if (!first) JWC(',');
        first = 0;
        JW("{\"table\":\""); JW(c->table_name); JW("\",");
        JW("\"field\":\"");  JW(c->field_name); JW("\",");
        JW("\"type\":\"");   JW(ctype_name(c->type)); JWC('"');
        if (c->type == (uint8_t)CTYPE_RANGE) {
            // emit min/max as decimals
            JW(",\"min\":"); char tmp[20]; int l=0;
            int64_t v = c->range_min;
            if (v < 0) { JWC('-'); v = -v; }
            if (!v) { tmp[l++]='0'; } else { while(v){tmp[l++]=(char)('0'+v%10);v/=10;} }
            for(int j=l-1;j>=0&&n<max-2;j--) buf[n++]=tmp[j];
            JW(",\"max\":"); l=0; v=c->range_max;
            if (v < 0) { JWC('-'); v = -v; }
            if (!v) { tmp[l++]='0'; } else { while(v){tmp[l++]=(char)('0'+v%10);v/=10;} }
            for(int j=l-1;j>=0&&n<max-2;j--) buf[n++]=tmp[j];
        }
        if (c->type == (uint8_t)CTYPE_REFERENCE) {
            JW(",\"ref\":\""); JW(c->ref_table); JWC('"');
        }
        JWC('}');
    }
    JWC(']');
    if (n < max) buf[n] = '\0';

    #undef JW
    #undef JWC
    return n;
}
