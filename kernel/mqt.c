#include "mqt.h"
#include "aggregate.h"
#include "kernel_io.h"
#include "transaction.h"

// ─── Globals ──────────────────────────────────────────────────────────────────
struct SLSMQT mqt_table[MQT_MAX];
uint32_t      mqt_count = 0;

// ─── Internal helpers ─────────────────────────────────────────────────────────
static void mq_cpy(char* dst, const char* src, int n) {
    int i = 0; for (; i < n-1 && src[i]; i++) dst[i] = src[i]; dst[i] = '\0';
}

static int mq_eq(const char* a, const char* b) {
    while (*a && *b) { if (*a != *b) return 0; a++; b++; }
    return *a == *b;
}

// Convert int64 to decimal string into buf, returns pointer past end
static char* mq_i64str(char* buf, int64_t v) {
    char tmp[24]; int l = 0;
    if (v < 0) { *buf++ = '-'; v = -v; }
    if (!v) { tmp[l++] = '0'; }
    else { while (v) { tmp[l++] = (char)('0' + v % 10); v /= 10; } }
    for (int i = l-1; i >= 0; i--) *buf++ = tmp[i];
    *buf = '\0';
    return buf;
}

// Write a record into the MQT result table (direct, no WAL)
static void mqt_write_record(const char* mqt_name,
                              const char* key, const char* value)
{
    for (uint32_t oi = 0; oi < object_catalog_count; oi++) {
        if (!object_catalog[oi].active) continue;
        if (!mq_eq(object_catalog[oi].name, mqt_name)) continue;

        struct SLSObjectRecord* rec = &object_records[oi];
        // Check if key already exists → update
        for (uint32_t fi = 0; fi < RECORD_MAX_FIELDS; fi++) {
            if (!rec->fields[fi].active) continue;
            if (!mq_eq(rec->fields[fi].key, key)) continue;
            // Update existing
            for (int i = 0; value[i] && i < RECORD_VAL_LEN-1; i++)
                rec->fields[fi].value[i] = value[i];
            rec->fields[fi].value[RECORD_VAL_LEN-1] = '\0';
            return;
        }
        // Insert new
        for (uint32_t fi = 0; fi < RECORD_MAX_FIELDS; fi++) {
            if (rec->fields[fi].active) continue;
            for (int i = 0; key[i]   && i < RECORD_KEY_LEN-1; i++) rec->fields[fi].key[i]   = key[i];
            for (int i = 0; value[i] && i < RECORD_VAL_LEN-1; i++) rec->fields[fi].value[i] = value[i];
            rec->fields[fi].key[RECORD_KEY_LEN-1]   = '\0';
            rec->fields[fi].value[RECORD_VAL_LEN-1] = '\0';
            rec->fields[fi].active = 1;
            rec->field_count++;
            return;
        }
        return;
    }
}

// Clear all records in the MQT result table
static void mqt_clear(const char* mqt_name) {
    for (uint32_t oi = 0; oi < object_catalog_count; oi++) {
        if (!object_catalog[oi].active) continue;
        if (!mq_eq(object_catalog[oi].name, mqt_name)) continue;
        struct SLSObjectRecord* rec = &object_records[oi];
        for (uint32_t fi = 0; fi < RECORD_MAX_FIELDS; fi++) {
            rec->fields[fi].active = 0;
        }
        rec->field_count = 0;
        return;
    }
}

// ─── mqt_init ────────────────────────────────────────────────────────────────
void mqt_init(void) {
    for (int i = 0; i < MQT_MAX; i++) mqt_table[i].active = 0;
    mqt_count = 0;
    kernel_serial_print("[MQT] Materialized query table engine initialised.\n");
}

// ─── mqt_create ───────────────────────────────────────────────────────────────
int mqt_create(const char* mqt_name,
               const char* base_table,
               uint8_t     fn,
               const char* agg_field,
               const char* where_field,
               const char* where_eq,
               const char* group_field)
{
    if (mqt_count >= MQT_MAX) {
        kernel_serial_print("[MQT] max MQTs reached\n");
        return 1;
    }
    // Reject duplicate MQT names
    for (int i = 0; i < MQT_MAX; i++) {
        if (mqt_table[i].active && mq_eq(mqt_table[i].mqt_name, mqt_name)) {
            kernel_serial_print("[MQT] already exists\n");
            return 1;
        }
    }

    // Allocate a result table object in the catalog (type=DB_TABLE)
    struct SLSVallocRequest vreq;
    for (int i = 0; i < OBJECT_NAME_LEN; i++) vreq.name[i] = 0;
    mq_cpy(vreq.name, mqt_name, OBJECT_NAME_LEN);
    vreq.type       = OBJ_TYPE_DB_TABLE;
    vreq.size_pages = 2;
    vreq.owner_uid  = 0;
    vreq.perm_mask  = 0;
    if (!sys_sls_valloc(&vreq)) {
        kernel_serial_print("[MQT] failed to allocate result table\n");
        return 1;
    }

    // Register the MQT definition
    struct SLSMQT* m = 0;
    for (int i = 0; i < MQT_MAX; i++) {
        if (!mqt_table[i].active) { m = &mqt_table[i]; break; }
    }
    mq_cpy(m->mqt_name,    mqt_name,    OBJECT_NAME_LEN);
    mq_cpy(m->base_table,  base_table,  OBJECT_NAME_LEN);
    mq_cpy(m->agg_field,   agg_field   ? agg_field   : "", RECORD_KEY_LEN);
    mq_cpy(m->where_field, where_field ? where_field : "", RECORD_KEY_LEN);
    mq_cpy(m->where_eq,    where_eq    ? where_eq    : "", RECORD_VAL_LEN);
    mq_cpy(m->group_field, group_field ? group_field : "", RECORD_KEY_LEN);
    m->fn     = fn;
    m->active = 1;
    mqt_count++;

    kernel_serial_print("[MQT] created: ");
    kernel_serial_print(mqt_name);
    kernel_serial_print(" <- ");
    kernel_serial_print(base_table);
    kernel_serial_print("\n");

    // Initial population
    mqt_refresh(mqt_name);
    return 0;
}

// ─── mqt_drop ────────────────────────────────────────────────────────────────
int mqt_drop(const char* mqt_name) {
    for (int i = 0; i < MQT_MAX; i++) {
        if (!mqt_table[i].active) continue;
        if (!mq_eq(mqt_table[i].mqt_name, mqt_name)) continue;
        mqt_table[i].active = 0;
        if (mqt_count) mqt_count--;
        // Free the result table from the catalog
        sys_sls_vfree(mqt_name);
        kernel_serial_print("[MQT] dropped: ");
        kernel_serial_print(mqt_name);
        kernel_serial_print("\n");
        return 0;
    }
    return 1;
}

// ─── mqt_refresh ─────────────────────────────────────────────────────────────
void mqt_refresh(const char* mqt_name) {
    struct SLSMQT* m = 0;
    for (int i = 0; i < MQT_MAX; i++) {
        if (mqt_table[i].active && mq_eq(mqt_table[i].mqt_name, mqt_name)) {
            m = &mqt_table[i]; break;
        }
    }
    if (!m) return;

    // Build the aggregate query
    struct AggQuery q;
    mq_cpy(q.table,       m->base_table,  OBJECT_NAME_LEN);
    q.fn = m->fn;
    mq_cpy(q.agg_field,   m->agg_field,   RECORD_KEY_LEN);
    mq_cpy(q.where_field, m->where_field, RECORD_KEY_LEN);
    mq_cpy(q.where_eq,    m->where_eq,    RECORD_VAL_LEN);
    mq_cpy(q.group_field, m->group_field, RECORD_KEY_LEN);
    q.having_min_count = 0;
    q.order_field[0]   = '\0';
    q.order_desc       = 0;

    // Clear existing results
    mqt_clear(mqt_name);

    // Execute query in-memory (max 4KB result buffer)
    static char result_buf[4096];
    aggregate_exec(&q, result_buf, (int)sizeof(result_buf));

    // Parse the JSON result and write individual records into the MQT table.
    // Format from aggregate_exec:
    //   {"table":"...","fn":"...","results":[{"count":N},...]
    //   or {"results":[{"group":"g","count":N},...]}
    //
    // Rather than parsing JSON, we call aggregate_exec's internals differently.
    // We re-run the aggregate directly and write results ourselves.

    // ── Re-run aggregate directly and write to MQT result table ───────────────
    struct SLSObjectRecord* base_rec = 0;
    for (uint32_t oi = 0; oi < object_catalog_count; oi++) {
        if (!object_catalog[oi].active) continue;
        if (!mq_eq(object_catalog[oi].name, m->base_table)) continue;
        base_rec = &object_records[oi]; break;
    }
    if (!base_rec) return;

    // Group accumulators (reuse AggGroup from aggregate.h)
    struct AggGroup { char gk[64]; int64_t cnt,sum,mn,mx; int has,act; };
    static struct AggGroup gs[32]; // max 32 groups
    int gc = 0;
    for (int i = 0; i < 32; i++) gs[i].act = 0;

    // Suffix match helper (inline)
    #define SFXMATCH(key, field) __extension__({ \
        int _r=0; \
        if(!(field)[0]){_r=1;} \
        else if(mq_eq((key),(field))){_r=1;} \
        else { int _kl=0; while((key)[_kl]) _kl++; \
               int _fl=0; while((field)[_fl]) _fl++; \
               if(_kl>_fl+1 && (key)[_kl-_fl-1]=='_') _r=mq_eq((key)+_kl-_fl,(field)); } \
        _r; })

    for (uint32_t fi = 0; fi < RECORD_MAX_FIELDS; fi++) {
        if (!base_rec->fields[fi].active) continue;
        const char* fkey = base_rec->fields[fi].key;
        const char* fval = base_rec->fields[fi].value;

        // WHERE filter
        if (m->where_field[0] && !SFXMATCH(fkey, m->where_field)) continue;
        if (m->where_eq[0]    && !mq_eq(fval, m->where_eq))        continue;

        // For aggregate (non-COUNT): must match agg_field
        if (m->fn != (uint8_t)AGG_COUNT && m->agg_field[0] &&
            !SFXMATCH(fkey, m->agg_field))
            continue;

        // Determine group key
        const char* gk = "";
        if (m->group_field[0]) {
            if (!SFXMATCH(fkey, m->group_field)) continue;
            gk = fval;
        }

        // Find or create group bucket
        struct AggGroup* g = 0;
        for (int i = 0; i < gc; i++) {
            if (gs[i].act && mq_eq(gs[i].gk, gk)) { g = &gs[i]; break; }
        }
        if (!g) {
            if (gc >= 32) continue;
            g = &gs[gc++];
            mq_cpy(g->gk, gk, 64);
            g->cnt = 0; g->sum = 0;
            g->mn = 0x7FFFFFFFFFFFFFFFLL;
            g->mx = (-0x7FFFFFFFFFFFFFFFLL - 1LL);
            g->has = 0; g->act = 1;
        }

        g->cnt++;
        if (m->fn != (uint8_t)AGG_COUNT) {
            int64_t v = 0; const char* s = fval;
            int neg = (*s == '-' ? s++, 1 : 0);
            while (*s >= '0' && *s <= '9') { v = v*10 + (*s - '0'); s++; }
            if (neg) v = -v;
            g->sum += v;
            if (!g->has || v < g->mn) g->mn = v;
            if (!g->has || v > g->mx) g->mx = v;
            g->has = 1;
        }
    }
    #undef SFXMATCH

    // Write results into the MQT result table
    char vbuf[32];
    extern volatile uint64_t kernel_tick_counter;
    mq_i64str(vbuf, (int64_t)kernel_tick_counter);
    mqt_write_record(mqt_name, "refreshed_tick", vbuf);

    if (gc == 0) {
        // No matches — write zero
        mqt_write_record(mqt_name, "result", "0");
        mqt_write_record(mqt_name, "count",  "0");
    } else {
        // Single bucket (no GROUP BY) or multiple (GROUP BY)
        for (int i = 0; i < gc; i++) {
            if (!gs[i].act) continue;
            // Determine the value to write
            int64_t result_val = 0;
            switch ((AggFunc)m->fn) {
            case AGG_COUNT: result_val = gs[i].cnt; break;
            case AGG_SUM:   result_val = gs[i].sum; break;
            case AGG_MIN:   result_val = gs[i].has ? gs[i].mn : 0; break;
            case AGG_MAX:   result_val = gs[i].has ? gs[i].mx : 0; break;
            case AGG_AVG:   result_val = gs[i].cnt > 0 ? gs[i].sum / gs[i].cnt : 0; break;
            default:        result_val = gs[i].cnt; break;
            }
            mq_i64str(vbuf, result_val);

            if (m->group_field[0] && gs[i].gk[0]) {
                // Grouped: key = group value, value = result
                mqt_write_record(mqt_name, gs[i].gk, vbuf);
                // Also write per-group count with suffix _count
                char ckey[72];
                mq_cpy(ckey, gs[i].gk, 64);
                int cl = 0; while (ckey[cl]) cl++;
                mq_cpy(ckey + cl, "_count", 8);
                char cbuf[24]; mq_i64str(cbuf, gs[i].cnt);
                mqt_write_record(mqt_name, ckey, cbuf);
            } else {
                // Non-grouped: key = "result"
                mqt_write_record(mqt_name, "result", vbuf);
                char cbuf[24]; mq_i64str(cbuf, gs[i].cnt);
                mqt_write_record(mqt_name, "count", cbuf);
            }
        }
    }

    kernel_serial_print("[MQT] refreshed: ");
    kernel_serial_print(mqt_name);
    kernel_serial_print("\n");
}

// ─── mqt_refresh_for_table ───────────────────────────────────────────────────
void mqt_refresh_for_table(const char* table_name) {
    for (int i = 0; i < MQT_MAX; i++) {
        if (mqt_table[i].active && mq_eq(mqt_table[i].base_table, table_name))
            mqt_refresh(mqt_table[i].mqt_name);
    }
}

// ─── mqts_to_json ─────────────────────────────────────────────────────────────
int mqts_to_json(char* buf, int max) {
    int n = 0;
    static const char* fn_names[] = {"COUNT","SUM","AVG","MIN","MAX","NONE"};
    #define JW(s) do { const char* _s=(s); while(*_s&&n<max-2) buf[n++]=*_s++; } while(0)
    #define JWC(c) do { if(n<max-2) buf[n++]=(c); } while(0)
    #define JWSTR(s) do { JWC('"'); JW(s); JWC('"'); } while(0)

    JWC('[');
    int first = 1;
    for (int i = 0; i < MQT_MAX; i++) {
        struct SLSMQT* m = &mqt_table[i];
        if (!m->active) continue;
        if (!first) JWC(',');
        first = 0;
        JW("{\"name\":"); JWSTR(m->mqt_name);
        JW(",\"base_table\":"); JWSTR(m->base_table);
        JW(",\"fn\":"); JWSTR(m->fn <= 5 ? fn_names[m->fn] : "?");
        if (m->agg_field[0]) { JW(",\"field\":"); JWSTR(m->agg_field); }
        if (m->group_field[0]) { JW(",\"group_by\":"); JWSTR(m->group_field); }
        if (m->where_field[0]) { JW(",\"where\":"); JWSTR(m->where_field); }
        JWC('}');
    }
    JWC(']');
    if (n < max) buf[n] = '\0';

    #undef JW
    #undef JWC
    #undef JWSTR
    return n;
}
