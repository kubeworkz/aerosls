#include "aggregate.h"
#include "index_mgr.h"
#include "kernel_io.h"

// ─── Internal helpers ─────────────────────────────────────────────────────────
static int ag_eq(const char* a, const char* b) {
    while (*a && *b) { if (*a != *b) return 0; a++; b++; }
    return *a == *b;
}

// Suffix-match: "alice_score" matches field "score"
static int ag_fmatch(const char* key, const char* field) {
    if (!field || !field[0]) return 1;
    if (ag_eq(key, field)) return 1;
    int kl = 0; while (key[kl])   kl++;
    int fl = 0; while (field[fl]) fl++;
    if (kl > fl + 1 && key[kl - fl - 1] == '_')
        return ag_eq(key + kl - fl, field);
    return 0;
}

// Parse decimal string to int64 (handles negatives)
static int64_t ag_atoi(const char* s) {
    int64_t v = 0; int neg = (*s == '-' ? s++, 1 : 0);
    while (*s >= '0' && *s <= '9') { v = v * 10 + (*s - '0'); s++; }
    return neg ? -v : v;
}

// Copy string
static void ag_cpy(char* dst, const char* src, int n) {
    int i = 0; for (; i < n-1 && src[i]; i++) dst[i] = src[i]; dst[i] = '\0';
}

// ─── JSON emitter helpers ─────────────────────────────────────────────────────
static int ag_jstr(char* buf, int max, int n, const char* s) {
    if (n < max-1) buf[n++] = '"';
    while (*s && n < max-3) {
        if (*s == '"' || *s == '\\') buf[n++] = '\\';
        buf[n++] = *s++;
    }
    if (n < max-1) buf[n++] = '"';
    return n;
}

static int ag_ji64(char* buf, int max, int n, int64_t v) {
    char tmp[24]; int l = 0;
    if (v < 0) { if (n < max-1) buf[n++] = '-'; v = -v; }
    if (!v) { tmp[l++] = '0'; }
    else { while (v) { tmp[l++] = (char)('0' + v % 10); v /= 10; } }
    for (int i = l-1; i >= 0 && n < max-1; i--) buf[n++] = tmp[i];
    return n;
}

// ─── aggregate_exec ───────────────────────────────────────────────────────────
int aggregate_exec(const struct AggQuery* q, char* buf, int max) {
    int n = 0;
    #define JW(s)   do { const char* _s=(s); while(*_s&&n<max-2) buf[n++]=*_s++; } while(0)
    #define JWC(c)  do { if(n<max-2) buf[n++]=(c); } while(0)
    #define JWSTR(s) do { n=ag_jstr(buf,max,n,(s)); } while(0)
    #define JWI(v)  do { n=ag_ji64(buf,max,n,(v)); } while(0)

    // ── Find the table in the object catalog ──────────────────────────────────
    struct SLSObjectRecord* rec = 0;
    for (uint32_t oi = 0; oi < object_catalog_count; oi++) {
        if (!object_catalog[oi].active) continue;
        if (!ag_eq(object_catalog[oi].name, q->table)) continue;
        rec = &object_records[oi];
        break;
    }
    if (!rec) {
        JW("{\"error\":\"table not found\"}");
        if (n < max) buf[n] = '\0';
        return n;
    }

    // ── AGG_NONE: collect rows, sort by order_field, return JSON array ─────────
    if (q->fn == (uint8_t)AGG_NONE) {
        struct AggRow rows[AGG_MAX_ROWS];
        int row_count = 0;

        for (uint32_t fi = 0; fi < RECORD_MAX_FIELDS && row_count < AGG_MAX_ROWS; fi++) {
            if (!rec->fields[fi].active) continue;
            // WHERE filter
            if (q->where_field[0] && !ag_fmatch(rec->fields[fi].key, q->where_field)) continue;
            if (q->where_eq[0]    && !ag_eq(rec->fields[fi].value, q->where_eq))       continue;

            ag_cpy(rows[row_count].key,   rec->fields[fi].key,   RECORD_KEY_LEN);
            ag_cpy(rows[row_count].value, rec->fields[fi].value, RECORD_VAL_LEN);
            // Determine sort key: value of the order_field row for the same "row entity"
            // For simplicity use the row's own value as sort key if it matches order_field,
            // else use the record key for sort stability.
            if (q->order_field[0] && ag_fmatch(rec->fields[fi].key, q->order_field))
                ag_cpy(rows[row_count].sort_key, rec->fields[fi].value, RECORD_VAL_LEN);
            else
                ag_cpy(rows[row_count].sort_key, rec->fields[fi].key,   RECORD_VAL_LEN);
            row_count++;
        }

        // Insertion sort by sort_key
        if (q->order_field[0]) {
            for (int i = 1; i < row_count; i++) {
                struct AggRow tmp = rows[i];
                int j = i - 1;
                while (j >= 0) {
                    int cmp;
                    // Numeric sort if sort_key looks like a number
                    char* sk = rows[j].sort_key;
                    char* tk = tmp.sort_key;
                    int is_num = (*sk=='-'||(*sk>='0'&&*sk<='9')) &&
                                 (*tk=='-'||(*tk>='0'&&*tk<='9'));
                    if (is_num) {
                        int64_t a = ag_atoi(sk), b = ag_atoi(tk);
                        cmp = (a > b) ? 1 : (a < b) ? -1 : 0;
                    } else {
                        // Lexicographic
                        while (*sk && *tk && *sk == *tk) { sk++; tk++; }
                        cmp = (unsigned char)*sk - (unsigned char)*tk;
                    }
                    if ((q->order_desc && cmp < 0) || (!q->order_desc && cmp > 0)) {
                        rows[j + 1] = rows[j]; j--;
                    } else break;
                }
                rows[j + 1] = tmp;
            }
        }

        // Emit JSON array
        JW("{\"table\":"); JWSTR(q->table);
        if (q->order_field[0]) { JW(",\"order_by\":"); JWSTR(q->order_field); }
        JW(",\"rows\":[");
        for (int i = 0; i < row_count; i++) {
            if (i) JWC(',');
            JW("{\"key\":"); JWSTR(rows[i].key);
            JW(",\"value\":"); JWSTR(rows[i].value);
            JWC('}');
        }
        JW("],\"count\":");  JWI((int64_t)row_count);
        JWC('}');
        if (n < max) buf[n] = '\0';
        return n;
    }

    // ── Aggregate pass ────────────────────────────────────────────────────────
    // Accumulate into AggGroup buckets (one per distinct group_field value,
    // or a single global bucket when GROUP BY is not requested).
    struct AggGroup groups[AGG_MAX_GROUPS];
    int             group_count = 0;

    for (int i = 0; i < AGG_MAX_GROUPS; i++) groups[i].active = 0;

    for (uint32_t fi = 0; fi < RECORD_MAX_FIELDS; fi++) {
        if (!rec->fields[fi].active) continue;

        // WHERE filter
        if (q->where_field[0] && !ag_fmatch(rec->fields[fi].key, q->where_field)) continue;
        if (q->where_eq[0]    && !ag_eq(rec->fields[fi].value, q->where_eq))       continue;

        // Must match the aggregate field (for SUM/MIN/MAX/AVG)
        // COUNT(*) counts every matching row, so we don't need agg_field for COUNT
        if (q->fn != (uint8_t)AGG_COUNT && q->agg_field[0] &&
            !ag_fmatch(rec->fields[fi].key, q->agg_field))
            continue;

        // Determine group key
        const char* gk = "";  // default: no grouping (single global bucket)
        if (q->group_field[0]) {
            // find the group field value for this key's "row" — use the same
            // suffix approach: look for a field in the same table whose key
            // suffix matches group_field
            // Simple heuristic: if this field's key suffix matches group_field,
            // use its value as the group key
            if (ag_fmatch(rec->fields[fi].key, q->group_field))
                gk = rec->fields[fi].value;
            else
                continue;  // only group by the group_field itself
        }

        // Find or create a group bucket for gk
        struct AggGroup* g = 0;
        for (int i = 0; i < group_count; i++) {
            if (groups[i].active && ag_eq(groups[i].group_key, gk)) {
                g = &groups[i]; break;
            }
        }
        if (!g) {
            if (group_count >= AGG_MAX_GROUPS) continue;
            g = &groups[group_count++];
            ag_cpy(g->group_key, gk, (int)sizeof(g->group_key));
            g->count    = 0;
            g->sum      = 0;
            g->min_val  = 0x7FFFFFFFFFFFFFFFLL;
            g->max_val  = -0x7FFFFFFFFFFFFFFFLL - 1;
            g->has_data = 0;
            g->active   = 1;
        }

        // Accumulate
        g->count++;
        if (q->fn != (uint8_t)AGG_COUNT) {
            int64_t v = ag_atoi(rec->fields[fi].value);
            g->sum += v;
            if (!g->has_data || v < g->min_val) g->min_val = v;
            if (!g->has_data || v > g->max_val) g->max_val = v;
            g->has_data = 1;
        }
    }

    // No matches → single zero-count bucket for non-GROUP queries
    if (group_count == 0 && !q->group_field[0]) {
        groups[0].count    = 0;
        groups[0].sum      = 0;
        groups[0].min_val  = 0;
        groups[0].max_val  = 0;
        groups[0].has_data = 0;
        groups[0].active   = 1;
        group_count        = 1;
    }

    // Sort groups by key (ORDER BY on group key if requested)
    if (q->order_field[0] && group_count > 1) {
        for (int i = 1; i < group_count; i++) {
            struct AggGroup tmp = groups[i];
            int j = i - 1;
            while (j >= 0) {
                int64_t a = ag_atoi(groups[j].group_key);
                int64_t b = ag_atoi(tmp.group_key);
                int cmp = (a > b) ? 1 : (a < b) ? -1 : 0;
                if (!cmp) {
                    const char *sa = groups[j].group_key, *sb = tmp.group_key;
                    while (*sa && *sb && *sa == *sb) { sa++; sb++; }
                    cmp = (unsigned char)*sa - (unsigned char)*sb;
                }
                if ((q->order_desc && cmp < 0) || (!q->order_desc && cmp > 0)) {
                    groups[j+1] = groups[j]; j--;
                } else break;
            }
            groups[j+1] = tmp;
        }
    }

    // ── Emit JSON ─────────────────────────────────────────────────────────────
    static const char* fn_names[] = {"COUNT","SUM","AVG","MIN","MAX"};
    const char* fn_name = q->fn <= (uint8_t)AGG_MAX ? fn_names[q->fn] : "?";

    JW("{\"table\":"); JWSTR(q->table);
    JW(",\"fn\":"); JWSTR(fn_name);
    if (q->agg_field[0]) { JW(",\"field\":"); JWSTR(q->agg_field); }
    if (q->group_field[0]) { JW(",\"group_by\":"); JWSTR(q->group_field); }
    JW(",\"results\":[");

    int first = 1;
    for (int i = 0; i < group_count; i++) {
        struct AggGroup* g = &groups[i];
        if (!g->active) continue;
        // HAVING filter
        if (q->having_min_count > 0 && g->count < q->having_min_count) continue;
        if (!first) JWC(',');
        first = 0;

        JWC('{');
        if (q->group_field[0]) { JW("\"group\":"); JWSTR(g->group_key); JWC(','); }

        switch ((AggFunc)q->fn) {
        case AGG_COUNT: JW("\"count\":"); JWI(g->count); break;
        case AGG_SUM:   JW("\"sum\":");   JWI(g->sum);   break;
        case AGG_MIN:   JW("\"min\":");   JWI(g->has_data ? g->min_val : 0); break;
        case AGG_MAX:   JW("\"max\":");   JWI(g->has_data ? g->max_val : 0); break;
        case AGG_AVG:
            JW("\"avg\":");
            if (g->count > 0) {
                // Emit as integer (truncated)  — extend to fixed-point later
                JWI(g->sum / g->count);
                JW(",\"avg_exact\":"); JWI(g->sum);
                JW(",\"avg_count\":"); JWI(g->count);
            } else { JW("null"); }
            break;
        default: break;
        }
        JWC('}');
    }

    JW("]}");
    if (n < max) buf[n] = '\0';

    #undef JW
    #undef JWC
    #undef JWSTR
    #undef JWI
    return n;
}
