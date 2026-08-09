#include "journal.h"
#include "kernel_io.h"
#include "transaction.h"
#include "object_catalog.h"

// ─── Globals ──────────────────────────────────────────────────────────────────
struct JournalEntry      journal_buffer[JOURNAL_MAX_ENTRIES];
uint32_t                 journal_entry_count      = 0;
struct JournalAttachment journal_attachments[JOURNAL_MAX_ATTACHMENTS];
uint32_t                 journal_attachment_count = 0;

static uint64_t journal_seq = 0;

// ─── Internal helpers ─────────────────────────────────────────────────────────
extern volatile uint64_t kernel_tick_counter;

static void jstrncpy(char* dst, const char* src, int n) {
    int i = 0;
    for (; i < n - 1 && src[i]; i++) dst[i] = src[i];
    dst[i] = '\0';
}

static int jstreq(const char* a, const char* b) {
    while (*a && *b) { if (*a != *b) return 0; a++; b++; }
    return *a == *b;
}

// ─── journal_init ────────────────────────────────────────────────────────────
void journal_init(void) {
    journal_entry_count      = 0;
    journal_attachment_count = 0;
    journal_seq              = 0;
    kernel_serial_print("[JOURNAL] Subsystem initialised.\n");
}

// ─── journal_for_object ──────────────────────────────────────────────────────
const char* journal_for_object(const char* object_name) {
    for (uint32_t i = 0; i < journal_attachment_count; i++) {
        if (journal_attachments[i].active &&
            jstreq(journal_attachments[i].object_name, object_name))
            return journal_attachments[i].journal_name;
    }
    return (const char*)0;
}

// ─── journal_write ────────────────────────────────────────────────────────────
void journal_write(const char* object_name,
                   const char* key,
                   const char* before_image,
                   const char* after_image,
                   JournalEntryType type,
                   uint64_t tx_id)
{
    // Only write if this object is attached to a journal
    const char* jname = journal_for_object(object_name);
    if (!jname) return;

    // Find target object_id from catalog
    uint64_t oid = 0;
    for (uint32_t i = 0; i < object_catalog_count; i++) {
        if (object_catalog[i].active &&
            jstreq(object_catalog[i].name, object_name)) {
            oid = object_catalog[i].object_id;
            break;
        }
    }

    // Ring-buffer slot (wrap on overflow)
    uint32_t slot = (journal_entry_count) % JOURNAL_MAX_ENTRIES;
    struct JournalEntry* e = &journal_buffer[slot];

    e->seq       = ++journal_seq;
    e->timestamp = kernel_tick_counter;
    e->tx_id     = tx_id;
    e->object_id = oid;
    e->type      = (uint8_t)type;
    // Auto-commit entries (no transaction) are immediately marked committed
    e->committed = (tx_id == 0) ? 1 : 0;

    jstrncpy(e->journal_name,  jname,        (int)sizeof(e->journal_name));
    jstrncpy(e->object_name,   object_name,  (int)sizeof(e->object_name));
    jstrncpy(e->key,           key,          (int)sizeof(e->key));
    jstrncpy(e->before_image,  before_image ? before_image : "",
             (int)sizeof(e->before_image));
    jstrncpy(e->after_image,   after_image  ? after_image  : "",
             (int)sizeof(e->after_image));

    journal_entry_count++;
}

// ─── journal_commit_tx ────────────────────────────────────────────────────────
void journal_commit_tx(uint64_t tx_id) {
    if (!tx_id) return;
    uint32_t total = journal_entry_count < JOURNAL_MAX_ENTRIES
                   ? journal_entry_count : JOURNAL_MAX_ENTRIES;
    for (uint32_t i = 0; i < total; i++) {
        if (journal_buffer[i].tx_id == tx_id && !journal_buffer[i].committed)
            journal_buffer[i].committed = 1;
    }
    // Write a CM (commit marker) entry to each journal that has pending entries
    // for this tx (look for the journal name from any matching entry)
    char seen_journal[32] = "";
    uint32_t n = journal_entry_count < JOURNAL_MAX_ENTRIES
               ? journal_entry_count : JOURNAL_MAX_ENTRIES;
    for (uint32_t i = 0; i < n; i++) {
        if (journal_buffer[i].tx_id == tx_id &&
            journal_buffer[i].type != (uint8_t)JENT_CM &&
            journal_buffer[i].type != (uint8_t)JENT_RB) {
            if (!jstreq(journal_buffer[i].journal_name, seen_journal)) {
                jstrncpy(seen_journal, journal_buffer[i].journal_name,
                         (int)sizeof(seen_journal));
                // Write commit marker
                uint32_t slot = journal_entry_count % JOURNAL_MAX_ENTRIES;
                struct JournalEntry* cm = &journal_buffer[slot];
                cm->seq       = ++journal_seq;
                cm->timestamp = kernel_tick_counter;
                cm->tx_id     = tx_id;
                cm->object_id = 0;
                cm->type      = (uint8_t)JENT_CM;
                cm->committed = 1;
                jstrncpy(cm->journal_name, seen_journal,
                         (int)sizeof(cm->journal_name));
                cm->object_name[0] = '\0';
                cm->key[0]         = '\0';
                cm->before_image[0]= '\0';
                cm->after_image[0] = '\0';
                journal_entry_count++;
            }
        }
    }
}

// ─── journal_rollback_tx ─────────────────────────────────────────────────────
void journal_rollback_tx(uint64_t tx_id) {
    if (!tx_id) return;
    uint32_t total = journal_entry_count < JOURNAL_MAX_ENTRIES
                   ? journal_entry_count : JOURNAL_MAX_ENTRIES;
    // Mark all pending entries for this tx as rolled back by converting to RB
    char seen_journal[32] = "";
    for (uint32_t i = 0; i < total; i++) {
        if (journal_buffer[i].tx_id == tx_id && !journal_buffer[i].committed) {
            journal_buffer[i].type      = (uint8_t)JENT_RB;
            journal_buffer[i].committed = 0; // stays uncommitted (rolled back)
            if (!jstreq(journal_buffer[i].journal_name, seen_journal))
                jstrncpy(seen_journal, journal_buffer[i].journal_name,
                         (int)sizeof(seen_journal));
        }
    }
    // Write RB marker
    if (seen_journal[0]) {
        uint32_t slot = journal_entry_count % JOURNAL_MAX_ENTRIES;
        struct JournalEntry* rb = &journal_buffer[slot];
        rb->seq       = ++journal_seq;
        rb->timestamp = kernel_tick_counter;
        rb->tx_id     = tx_id;
        rb->object_id = 0;
        rb->type      = (uint8_t)JENT_RB;
        rb->committed = 0;
        jstrncpy(rb->journal_name, seen_journal, (int)sizeof(rb->journal_name));
        rb->object_name[0] = '\0';
        rb->key[0]         = '\0';
        rb->before_image[0]= '\0';
        rb->after_image[0] = '\0';
        journal_entry_count++;
    }
}

// ─── journal_attach ──────────────────────────────────────────────────────────
int journal_attach(const char* journal_name, const char* object_name) {
    // Check not already attached
    for (uint32_t i = 0; i < journal_attachment_count; i++) {
        if (journal_attachments[i].active &&
            jstreq(journal_attachments[i].journal_name, journal_name) &&
            jstreq(journal_attachments[i].object_name,  object_name)) {
            kernel_serial_print("[JOURNAL] already attached.\n");
            return 1;
        }
    }
    if (journal_attachment_count >= JOURNAL_MAX_ATTACHMENTS) {
        kernel_serial_print("[JOURNAL] Attachment table full.\n");
        return 1;
    }
    struct JournalAttachment* a = &journal_attachments[journal_attachment_count++];
    jstrncpy(a->journal_name, journal_name, (int)sizeof(a->journal_name));
    jstrncpy(a->object_name,  object_name,  (int)sizeof(a->object_name));
    a->active = 1;
    kernel_serial_print("[JOURNAL] started: ");
    kernel_serial_print(object_name);
    kernel_serial_print(" -> ");
    kernel_serial_print(journal_name);
    kernel_serial_print("\n");
    return 0;
}

// ─── journal_detach ──────────────────────────────────────────────────────────
int journal_detach(const char* journal_name, const char* object_name) {
    for (uint32_t i = 0; i < journal_attachment_count; i++) {
        if (journal_attachments[i].active &&
            jstreq(journal_attachments[i].journal_name, journal_name) &&
            jstreq(journal_attachments[i].object_name,  object_name)) {
            journal_attachments[i].active = 0;
            kernel_serial_print("[JOURNAL] stopped: "); kernel_serial_print(object_name); kernel_serial_print("\n");
            return 0;
        }
    }
    kernel_serial_print("[JOURNAL] not attached.\n");
    return 1;
}

// ─── journal_dump ────────────────────────────────────────────────────────────
void journal_dump(const char* journal_name, uint64_t since_seq) {
    static const char* const TYPES[] = {"PT","UP","UB","DL","CM","RB"};
    kernel_serial_print("[JOURNAL] === "); kernel_serial_print(journal_name);
    kernel_serial_print(" ===\n");
    uint32_t total = journal_entry_count < JOURNAL_MAX_ENTRIES
                   ? journal_entry_count : JOURNAL_MAX_ENTRIES;
    uint32_t shown = 0;
    for (uint32_t i = 0; i < total; i++) {
        struct JournalEntry* e = &journal_buffer[i];
        if (!jstreq(e->journal_name, journal_name)) continue;
        if (e->seq < since_seq) continue;
        const char* tname = (e->type <= (uint8_t)JENT_RB)
                          ? TYPES[e->type] : "??";
        // Safe serial print with no variadic calls
        kernel_serial_print("  ");
        kernel_serial_print(tname);
        kernel_serial_print("  obj=");
        kernel_serial_print(e->object_name[0] ? e->object_name : "-");
        kernel_serial_print("  key=");
        kernel_serial_print(e->key[0] ? e->key : "-");
        kernel_serial_print("  before=");
        kernel_serial_print(e->before_image[0] ? e->before_image : "-");
        kernel_serial_print("  after=");
        kernel_serial_print(e->after_image[0] ? e->after_image : "-");
        kernel_serial_print(e->committed ? "  COMMITTED" : "  PENDING");
        kernel_serial_print("\n");
        shown++;
    }
    if (!shown)
        kernel_serial_print("  (no entries)\n");
}

// ─── journal_purge ───────────────────────────────────────────────────────────
void journal_purge(const char* journal_name) {
    uint32_t purged = 0;
    uint32_t total = journal_entry_count < JOURNAL_MAX_ENTRIES
                   ? journal_entry_count : JOURNAL_MAX_ENTRIES;
    for (uint32_t i = 0; i < total; i++) {
        if (journal_buffer[i].journal_name[0] &&
            jstreq(journal_buffer[i].journal_name, journal_name) &&
            journal_buffer[i].type == (uint8_t)JENT_RB) {
            journal_buffer[i].journal_name[0] = '\0'; // mark slot free
            purged++;
        }
    }
    kernel_serial_print("[JOURNAL] Purged from ");
    kernel_serial_print(journal_name);
    kernel_serial_print("\n");
}

// ─── journal_to_json ─────────────────────────────────────────────────────────
// Serialise journal entries for a given journal into buf[max] as a JSON array.
int journal_to_json(const char* journal_name, uint64_t since_seq,
                    char* buf, int max)
{
    static const char* const TYPES[] = {"PT","UP","UB","DL","CM","RB"};
    int n = 0;

    // Helpers — write string / uint64 into buf at position n
    #define JW(s) do { const char* _s=(s); while(*_s && n<max-2) buf[n++]=*_s++; } while(0)
    #define JWC(c) do { if (n < max-2) buf[n++] = (c); } while(0)
    #define JWSTR(s) do { JWC('"'); JW(s); JWC('"'); } while(0)
    #define JWU64(v) do { \
        char _t[20]; int _l=0; uint64_t _v=(v); \
        if(!_v){_t[_l++]='0';} else{while(_v){_t[_l++]=(char)('0'+_v%10);_v/=10;}} \
        for(int _i=_l-1;_i>=0&&n<max-2;_i--) buf[n++]=_t[_i]; \
    } while(0)

    JWC('[');

    uint32_t total = journal_entry_count < JOURNAL_MAX_ENTRIES
                   ? journal_entry_count : JOURNAL_MAX_ENTRIES;
    int first = 1;
    for (uint32_t i = 0; i < total; i++) {
        struct JournalEntry* e = &journal_buffer[i];
        if (!e->journal_name[0]) continue;
        if (!jstreq(e->journal_name, journal_name)) continue;
        if (e->seq < since_seq) continue;

        if (!first) JWC(',');
        first = 0;

        const char* tname = (e->type <= (uint8_t)JENT_RB) ? TYPES[e->type] : "?";

        JWC('{');
        JW("\"seq\":"); JWU64(e->seq);
        JW(",\"type\":"); JWSTR(tname);
        JW(",\"object\":"); JWSTR(e->object_name);
        JW(",\"key\":"); JWSTR(e->key);
        JW(",\"before\":"); JWSTR(e->before_image);
        JW(",\"after\":"); JWSTR(e->after_image);
        JW(",\"tx\":"); JWU64(e->tx_id);
        JW(",\"committed\":"); JWC(e->committed ? '1' : '0');
        JWC('}');
    }

    JWC(']');
    if (n < max) buf[n] = '\0';

    #undef JW
    #undef JWC
    #undef JWSTR
    #undef JWU64

    return n;
}
