// kernel/env_ckpt.c — POSIX-Environments Roadmap v0.2, Phase P1a:
// the environment checkpoint record (see kernel/env_ckpt.h for what this is
// and, more importantly, what it deliberately is not).
//
// Freestanding: local helpers, no libc, per this codebase's convention
// (p_* in persist.c, tn_* in tenant.c, cm_* in checkpoint_mgr.c).

#include "env_ckpt.h"
#include "checkpoint_delta.h"
#include "partition.h"   /* P1a quiesce: partition_pause/_resume/_is_paused/_exists */

// The whole array is written into ONE NVMe frame by persist_environments()
// (below, in persist.c). If it ever outgrows a frame the writer would need
// batching, so growth is made a compile error instead of a silent truncation.
_Static_assert(sizeof(struct EnvCkptRecord) * ENV_CKPT_MAX <= 4096,
               "env_ckpt_table no longer fits one 4 KiB NVMe frame -- "
               "persist_environments() must batch, or ENV_CKPT_MAX must fall");

struct EnvCkptRecord env_ckpt_table[ENV_CKPT_MAX];
uint32_t             env_ckpt_count = 0;

static struct EnvCkptRefusal ec_refusal = { ENV_CKPT_REFUSE_NONE, 0, 0 };

// ─── Restore-side runtime state ───────────────────────────────────────────────
// Parallel to env_ckpt_table[] and deliberately NOT persisted: which records
// THIS boot still has to replay is a property of this boot, exactly as
// partition_paused[] is a property of the running kernel rather than of the
// snapshot. Kept in step with the table by reset()/drop()/adopt(), so a pending
// bit can never outlive the record it points at.
static uint8_t  ec_restore_pending[ENV_CKPT_MAX];
static uint32_t ec_restore_pending_count = 0;
static uint32_t ec_restore_refused_count = 0;
// The pause ledger for the replay: the partitions THIS pass resumed and
// therefore owes a repause. Same shape as EnvCkptQuiesce.paused_by_capture[],
// and for the same reason -- "what I did" is the only safe thing to undo.
static uint32_t ec_restore_resumed[ENV_CKPT_MAX];
static uint32_t ec_restore_resumed_count = 0;

// ─── Local helpers ────────────────────────────────────────────────────────────
static void ec_memcpy(void* d, const void* s, uint32_t n) {
    uint8_t* dd = (uint8_t*)d;
    const uint8_t* ss = (const uint8_t*)s;
    while (n--) *dd++ = *ss++;
}
static void ec_memset(void* d, uint8_t v, uint32_t n) {
    uint8_t* p = (uint8_t*)d;
    while (n--) *p++ = v;
}
// A task's name must be a real, NUL-terminated non-empty name inside its own
// field. A restore re-registers under this name, so an unterminated one would
// read past the record -- checked, not assumed.
static int ec_name_ok(const char* name) {
    for (uint32_t i = 0; i < ENV_CKPT_NAME_LEN; i++) {
        if (name[i] == '\0') return i > 0;
    }
    return 0;
}

static int ec_task_kind_ok(uint8_t k) {
    return k == ENV_CKPT_TASK_POSIX_SIDECAR ||
           k == ENV_CKPT_TASK_RAMDISK_SIDECAR ||
           k == ENV_CKPT_TASK_LINUX;
}
static int ec_region_kind_ok(uint32_t k) {
    return k == ENV_CKPT_REGION_POSIX_HEAP ||
           k == ENV_CKPT_REGION_RD_HEAP ||
           k == ENV_CKPT_REGION_RD_STORAGE;
}

static void ec_refuse(uint32_t code, uint32_t a, uint32_t b) {
    ec_refusal.code = code;
    ec_refusal.a    = a;
    ec_refusal.b    = b;
}
static void ec_clear_refusal(void) { ec_refuse(ENV_CKPT_REFUSE_NONE, 0, 0); }

// ─── Validation ───────────────────────────────────────────────────────────────
// The single gate every path goes through: the live handler, the host-side
// record() call, and the adopt() replay of an on-disk snapshot. It checks the
// record's self-description (magic/version/size) and its internal consistency
// (counts in range, kinds known, names terminated, identity sane). It does NOT
// check the partition exists or that the environment is live -- a snapshot is
// restored onto a freshly-booted kernel, where "exists" is the restore's job,
// not the record's.
int env_ckpt_valid(const struct EnvCkptRecord* rec) {
    if (rec->magic != ENV_CKPT_REC_MAGIC)              return ENV_CKPT_REFUSE_BAD_MAGIC;
    if (rec->version != ENV_CKPT_REC_VERSION)          return ENV_CKPT_REFUSE_VERSION;
    if (rec->size != (uint32_t)sizeof(struct EnvCkptRecord))
                                                       return ENV_CKPT_REFUSE_REC_SIZE;
    if (rec->n_tasks   > ENV_CKPT_MAX_TASKS)           return ENV_CKPT_REFUSE_BAD_TASKS;
    if (rec->n_regions > ENV_CKPT_MAX_REGIONS)         return ENV_CKPT_REFUSE_BAD_REGIONS;
    if (rec->n_chans   > ENV_CKPT_MAX_CHANS)           return ENV_CKPT_REFUSE_BAD_TASKS;
    // 0xFFFFFFFF is the "no partition" sentinel the partition gate itself uses
    // (E4's CAP_EINVAL arm); a record claiming it cannot be restored anywhere.
    if (rec->partition_id == 0xFFFFFFFFu)              return ENV_CKPT_REFUSE_BAD_IDENTITY;
    // A flag this build does not know is not skipped: a record whose meaning is
    // partly unknown is one the restore cannot vouch for, and the copy that
    // carries it is a foreign build's (or corrupt). Refused, exactly as an
    // unknown task kind is.
    if (rec->flags & ~(ENV_CKPT_FLAG_QUIESCED | ENV_CKPT_FLAG_PARTITION_PAUSED))
                                                       return ENV_CKPT_REFUSE_BAD_FLAGS;

    for (uint32_t i = 0; i < rec->n_tasks; i++) {
        if (!ec_task_kind_ok(rec->tasks[i].kind))      return ENV_CKPT_REFUSE_BAD_TASK;
        if (!ec_name_ok(rec->tasks[i].name))           return ENV_CKPT_REFUSE_BAD_TASK;
        if (rec->tasks[i].entry == 0)                  return ENV_CKPT_REFUSE_BAD_TASK;
    }
    // The three regions are required, not optional: an environment without its
    // ramdisk storage is a different object, and silently restoring a partial
    // one is the failure this gate exists to prevent.
    if (rec->n_regions != ENV_CKPT_MAX_REGIONS)        return ENV_CKPT_REFUSE_BAD_REGIONS;
    for (uint32_t i = 0; i < rec->n_regions; i++) {
        if (!ec_region_kind_ok(rec->regions[i].kind))  return ENV_CKPT_REFUSE_BAD_REGIONS;
        if (rec->regions[i].frames == 0)               return ENV_CKPT_REFUSE_BAD_REGIONS;
        // ...and each of the three kinds exactly once. Three KNOWN kinds with
        // one repeated is a record whose heap and storage have been swapped or
        // doubled -- every field self-consistent, and a restore would reattach
        // the POSIX heap where the ramdisk storage belongs. The kinds are named
        // (not positional) precisely so this is checkable; not checking them
        // would make the names decoration.
        for (uint32_t j = 0; j < i; j++)
            if (rec->regions[j].kind == rec->regions[i].kind)
                return ENV_CKPT_REFUSE_BAD_REGIONS;
    }
    return ENV_CKPT_REFUSE_NONE;
}

// ─── Table management ─────────────────────────────────────────────────────────
void env_ckpt_reset(void) {
    ec_memset(env_ckpt_table, 0, (uint32_t)sizeof(env_ckpt_table));
    env_ckpt_count = 0;
    ec_memset(ec_restore_pending, 0, (uint32_t)sizeof(ec_restore_pending));
    ec_restore_pending_count  = 0;
    ec_restore_refused_count  = 0;
    ec_clear_refusal();
}

static int ec_find_slot(uint32_t partition_id, uint32_t index) {
    for (uint32_t i = 0; i < env_ckpt_count; i++)
        if (env_ckpt_table[i].partition_id == partition_id &&
            env_ckpt_table[i].index        == index) return (int)i;
    return -1;
}

int env_ckpt_record(const struct EnvCkptRecord* rec) {
    int rc = env_ckpt_valid(rec);
    if (rc != ENV_CKPT_REFUSE_NONE) { ec_refuse((uint32_t)rc, 0, 0); return rc; }

    int slot = ec_find_slot(rec->partition_id, rec->index);
    if (slot < 0) {
        if (env_ckpt_count >= ENV_CKPT_MAX) {
            ec_refuse(ENV_CKPT_REFUSE_FULL, env_ckpt_count, ENV_CKPT_MAX);
            return ENV_CKPT_REFUSE_FULL;
        }
        slot = (int)env_ckpt_count;
        env_ckpt_count++;
    }
    ec_memcpy(&env_ckpt_table[slot], rec, (uint32_t)sizeof(struct EnvCkptRecord));
    // Dirty marking, not a write: this is the persistence layer's incremental
    // contract (ckpt_mark_dirty) -- the next checkpoint writes the region, and a
    // missed mark costs a wasted write rather than a lost record.
    ckpt_mark_dirty(CKPT_REGION_ENV);
    ec_clear_refusal();
    return 0;
}

// ─── Registration: init's half + the kernel's half → one record ───────────────
int env_ckpt_register_from(const struct EnvCkptRegister* reg,
                           const uint64_t task_entry[ENV_CKPT_MAX_TASKS],
                           uint32_t console_id, uint64_t sequence) {
    if (reg == (const struct EnvCkptRegister*)0) {
        ec_refuse(ENV_CKPT_REFUSE_BAD_IDENTITY, 0, 0);
        return ENV_CKPT_REFUSE_BAD_IDENTITY;
    }
    // A count past the array a record can hold cannot be copied, so it is a
    // refusal with the reason a restore would have given, not a truncation.
    if (reg->n_regions > ENV_CKPT_MAX_REGIONS) {
        ec_refuse(ENV_CKPT_REFUSE_BAD_REGIONS, reg->n_regions, ENV_CKPT_MAX_REGIONS);
        return ENV_CKPT_REFUSE_BAD_REGIONS;
    }
    if (reg->n_tasks > ENV_CKPT_MAX_TASKS) {
        ec_refuse(ENV_CKPT_REFUSE_BAD_TASKS, reg->n_tasks, ENV_CKPT_MAX_TASKS);
        return ENV_CKPT_REFUSE_BAD_TASKS;
    }
    if (reg->n_chans > ENV_CKPT_MAX_CHANS) {
        ec_refuse(ENV_CKPT_REFUSE_BAD_TASKS, reg->n_chans, ENV_CKPT_MAX_CHANS);
        return ENV_CKPT_REFUSE_BAD_TASKS;
    }

    struct EnvCkptRecord rec;
    ec_memset(&rec, 0, (uint32_t)sizeof rec);

    // The kernel's own half: this build's format, and the sequence this
    // registration belongs to. init cannot state either, and if it could, it
    // would be the drift this file is arranged against.
    rec.magic        = ENV_CKPT_REC_MAGIC;
    rec.version      = ENV_CKPT_REC_VERSION;
    rec.size         = (uint32_t)sizeof(struct EnvCkptRecord);
    rec.sequence     = sequence;
    rec.console_id   = console_id;

    // init's half, copied field for field -- the layout agreement the pin
    // guard keeps honest.
    rec.partition_id = reg->partition_id;
    rec.index        = reg->index;
    rec.env_id       = reg->env_id;
    rec.n_regions    = reg->n_regions;
    for (uint32_t i = 0; i < reg->n_regions; i++) rec.regions[i] = reg->regions[i];
    rec.n_chans      = reg->n_chans;
    for (uint32_t i = 0; i < reg->n_chans; i++) rec.chans[i] = reg->chans[i];

    rec.n_tasks      = reg->n_tasks;
    for (uint32_t i = 0; i < reg->n_tasks; i++) {
        for (uint32_t c = 0; c < ENV_CKPT_NAME_LEN; c++)
            rec.tasks[i].name[c] = reg->task_name[i][c];
        // The name field is copied verbatim: a body whose name has no
        // terminator inside the field is REFUSED by the gate below rather than
        // truncated here, because truncation would silently re-register the
        // sidecar under a different name on a restore.
        rec.tasks[i].kind  = (uint8_t)(reg->task_kind[i] & 0xFFu);
        rec.tasks[i].entry = task_entry ? task_entry[i] : 0;
        // stack_bottom/stack_top stay 0: the sidecar's user stack is mapped by
        // cap_create_sidecar, which (unlike process_create) does not record its
        // bounds on the descriptor, so there is no honest value to copy today.
        // The restore layer owns re-creating the sidecar -- and therefore knows
        // the stack it just mapped. Inventing a number here would put a wrong
        // address in a record whose whole purpose is to be trusted.
    }

    return env_ckpt_record(&rec);
}

int env_ckpt_drop(uint32_t partition_id, uint32_t index) {
    int slot = ec_find_slot(partition_id, index);
    if (slot < 0) return 1;

    // Keep the table dense: slots [0, count) are live, the tail is zeroed. The
    // pending set shifts with it -- a pending bit that stayed behind would
    // point at a different record (or past the end) after the drop.
    if (ec_restore_pending[slot]) ec_restore_pending_count--;
    for (uint32_t i = (uint32_t)slot; i + 1 < env_ckpt_count; i++) {
        env_ckpt_table[i]    = env_ckpt_table[i + 1];
        ec_restore_pending[i] = ec_restore_pending[i + 1];
    }
    env_ckpt_count--;
    ec_memset(&env_ckpt_table[env_ckpt_count], 0, (uint32_t)sizeof(struct EnvCkptRecord));
    ec_restore_pending[env_ckpt_count] = 0;
    ckpt_mark_dirty(CKPT_REGION_ENV);
    return 0;
}

const struct EnvCkptRecord* env_ckpt_find(uint32_t partition_id, uint32_t index) {
    int slot = ec_find_slot(partition_id, index);
    return slot < 0 ? (const struct EnvCkptRecord*)0 : &env_ckpt_table[slot];
}

uint32_t env_ckpt_count_live(void) { return env_ckpt_count; }

// ─── Adopt: the all-or-nothing replay of an on-disk snapshot ──────────────────
// "Refusal over partial application" (v0.2 §4). Two properties are deliberate:
//
//   1. The snapshot is validated IN FULL, off to the side, before a single
//      byte of the live table is touched. A snapshot that is good for records
//      0..k-1 and bad at k leaves nothing behind -- which is the difference
//      between a refused restore and a half-restored environment nobody can
//      account for.
//   2. The header's own fields (record size, version) are checked before the
//      records are, because a foreign build's records would each fail their
//      own checks too and the header is the honest reason to report.
int env_ckpt_adopt(const struct EnvCkptRecord* staging, uint32_t count,
                   uint32_t rec_size, uint32_t version) {
    // A refusal leaves NO environment behind, so the table is emptied first,
    // not conditionally: after this call the live set is either the adopted
    // snapshot or nothing at all.
    ec_memset(env_ckpt_table, 0, (uint32_t)sizeof(env_ckpt_table));
    env_ckpt_count = 0;
    // The pending set is armed by after_restore(), not by adopt(): a snapshot
    // that is still being validated is not yet a set of things to replay, and
    // the refusal paths above must leave nothing pending behind them.
    ec_memset(ec_restore_pending, 0, (uint32_t)sizeof(ec_restore_pending));
    ec_restore_pending_count = 0;
    ec_restore_refused_count = 0;

    if (version != ENV_CKPT_REC_VERSION) {
        ec_refuse(ENV_CKPT_REFUSE_VERSION, version, ENV_CKPT_REC_VERSION);
        return ENV_CKPT_REFUSE_VERSION;
    }
    if (rec_size != (uint32_t)sizeof(struct EnvCkptRecord)) {
        ec_refuse(ENV_CKPT_REFUSE_REC_SIZE, rec_size, (uint32_t)sizeof(struct EnvCkptRecord));
        return ENV_CKPT_REFUSE_REC_SIZE;
    }
    if (count == 0) {
        // A committed-but-empty snapshot is legitimate: it is what the writer
        // records once the last environment is destroyed, and it is the state
        // that must NOT resurrect a stale environment on the next boot.
        ec_clear_refusal();
        return 0;
    }
    if (count > ENV_CKPT_MAX) {
        ec_refuse(ENV_CKPT_REFUSE_COUNT, count, ENV_CKPT_MAX);
        return ENV_CKPT_REFUSE_COUNT;
    }

    for (uint32_t i = 0; i < count; i++) {
        int rc = env_ckpt_valid(&staging[i]);
        if (rc != ENV_CKPT_REFUSE_NONE) {
            ec_refuse((uint32_t)rc, i, 0);
            ec_memset(env_ckpt_table, 0, (uint32_t)sizeof(env_ckpt_table));
            return rc;
        }
        // Two records claiming one (partition, index) is a corrupt snapshot:
        // the second would silently win on a naive replay, and which one wins
        // would depend on write order rather than on anything meaningful.
        for (uint32_t j = 0; j < i; j++) {
            if (staging[j].partition_id == staging[i].partition_id &&
                staging[j].index        == staging[i].index) {
                ec_refuse(ENV_CKPT_REFUSE_DUPLICATE, i, j);
                ec_memset(env_ckpt_table, 0, (uint32_t)sizeof(env_ckpt_table));
                return ENV_CKPT_REFUSE_DUPLICATE;
            }
        }
    }

    ec_memcpy(env_ckpt_table, staging, count * (uint32_t)sizeof(struct EnvCkptRecord));
    env_ckpt_count = count;
    ec_clear_refusal();
    return 0;
}

void env_ckpt_after_restore(void) {
    // Compact the live set defensively: the restore-side validation already
    // guaranteed slots [0, count) are valid, so this is a no-op on a good
    // snapshot and the thing that stops a bad one being described as loaded.
    uint32_t out = 0;
    for (uint32_t i = 0; i < env_ckpt_count; i++) {
        if (env_ckpt_valid(&env_ckpt_table[i]) != ENV_CKPT_REFUSE_NONE) continue;
        if (out != i) env_ckpt_table[out] = env_ckpt_table[i];
        out++;
    }
    for (uint32_t i = out; i < ENV_CKPT_MAX; i++)
        ec_memset(&env_ckpt_table[i], 0, (uint32_t)sizeof(struct EnvCkptRecord));
    env_ckpt_count = out;

    // The restore's other half: everything this boot found on disk is now a
    // record to replay. Armed here, not in adopt(), because the pause replay
    // (env_ckpt_apply_restored_pauses) runs between the two and acts on the
    // same settled set.
    env_ckpt_restore_begin();
}

// ─── Quiescing: freezing an environment before it is written down ────────────
// See env_ckpt.h for the three rules these implement and why each exists.
static int ec_in_list(const uint32_t* list, uint32_t n, uint32_t id) {
    for (uint32_t i = 0; i < n; i++) if (list[i] == id) return 1;
    return 0;
}

void env_ckpt_quiesce_for_capture(struct EnvCkptQuiesce* out) {
    if (out == (struct EnvCkptQuiesce*)0) return;
    ec_memset(out, 0, (uint32_t)sizeof *out);

    // The walk does NOT advance its index after a drop: env_ckpt_drop() keeps
    // the table dense by shifting the tail down, so index i is the next record
    // to examine once the drop has happened.
    for (uint32_t i = 0; i < env_ckpt_count; ) {
        struct EnvCkptRecord* rec = &env_ckpt_table[i];
        uint32_t p = rec->partition_id;

        // Rule 3: the partition is gone, so the environment it names cannot be
        // restored -- there is nowhere to put it back. Dropped, not persisted.
        if (!partition_exists(p)) {
            uint32_t idx = rec->index;
            env_ckpt_drop(p, idx);
            out->n_dropped++;
            continue;
        }

        // Rule 2: never freeze the control plane's own partition. The record
        // says so rather than pretending: QUIESCED clear.
        if (p == PARTITION_SYSTEM) {
            rec->flags &= ~ENV_CKPT_FLAG_QUIESCED;
            if (partition_is_paused(p)) rec->flags |= ENV_CKPT_FLAG_PARTITION_PAUSED;
            else                        rec->flags &= ~ENV_CKPT_FLAG_PARTITION_PAUSED;
            out->n_unfrozen++;
            i++;
            continue;
        }

        if (ec_in_list(out->paused_by_capture, out->n_paused_by_capture, p)) {
            // Already frozen by THIS capture -- another record in the same
            // partition. The partition is paused because WE paused it, so the
            // record is QUIESCED and explicitly NOT operator-paused. Checking
            // this first is what stops a second record in one partition from
            // being mistaken for rule 1's "an operator had already paused it"
            // -- which would mark it PARTITION_PAUSED and bring it back frozen.
            rec->flags |= ENV_CKPT_FLAG_QUIESCED;
            rec->flags &= ~ENV_CKPT_FLAG_PARTITION_PAUSED;
            out->n_quiesced++;
        } else if (partition_is_paused(p)) {
            // Rule 1: an already-paused partition stays paused across the
            // capture -- we do not resume what we did not pause.
            rec->flags |= ENV_CKPT_FLAG_QUIESCED | ENV_CKPT_FLAG_PARTITION_PAUSED;
            out->n_already_paused++;
        } else if (partition_pause(p) == 0) {
            rec->flags |= ENV_CKPT_FLAG_QUIESCED;
            rec->flags &= ~ENV_CKPT_FLAG_PARTITION_PAUSED;
            if (out->n_paused_by_capture < ENV_CKPT_MAX)
                out->paused_by_capture[out->n_paused_by_capture++] = p;
            out->n_quiesced++;
        } else {
            // Unreachable while partition_exists() is the same precondition
            // partition_pause() checks -- kept because "could not freeze it"
            // must never be recorded as "froze it", whatever makes it reachable
            // later.
            rec->flags &= ~(ENV_CKPT_FLAG_QUIESCED | ENV_CKPT_FLAG_PARTITION_PAUSED);
            out->n_unfrozen++;
        }
        i++;
    }
}

void env_ckpt_release_capture(const struct EnvCkptQuiesce* q) {
    if (q == (const struct EnvCkptQuiesce*)0) return;
    for (uint32_t i = 0; i < q->n_paused_by_capture && i < ENV_CKPT_MAX; i++) {
        uint32_t p = q->paused_by_capture[i];
        // Idempotent, and safe against the partition having been destroyed (or
        // resumed by somebody else) between the quiesce and the release: a
        // release is a cleanup path, and it must not be the thing that faults.
        if (!partition_exists(p))  continue;
        if (!partition_is_paused(p)) continue;
        partition_resume(p);
    }
}

uint32_t env_ckpt_apply_restored_pauses(void) {
    uint32_t applied = 0;
    for (uint32_t i = 0; i < env_ckpt_count; i++) {
        const struct EnvCkptRecord* rec = &env_ckpt_table[i];
        if (!(rec->flags & ENV_CKPT_FLAG_PARTITION_PAUSED)) continue;
        uint32_t p = rec->partition_id;
        // Rule 2 on the way back: a boot that comes up with the control
        // plane's own partition frozen is a worse failure than losing the
        // flag. The record still says what was true; this declines to act.
        if (p == PARTITION_SYSTEM) continue;
        if (!partition_exists(p))  continue;
        if (partition_is_paused(p)) continue;   // one partition, many records
        if (partition_pause(p) == 0) applied++;
    }
    return applied;
}

int env_ckpt_drop_env(uint32_t partition_id, uint32_t env_id) {
    for (uint32_t i = 0; i < env_ckpt_count; i++) {
        if (env_ckpt_table[i].partition_id == partition_id &&
            env_ckpt_table[i].env_id == env_id) {
            return env_ckpt_drop(partition_id, env_ckpt_table[i].index);
        }
    }
    return 1;
}

// ─── Restore-through-create ──────────────────────────────────────────────────
// See env_ckpt.h for the four properties and what this does not do yet (the
// payload). The one non-obvious ordering rule lives in the driver
// (env_service_restore_pending()): a partition the snapshot left PAUSED is
// resumed for the create and re-paused afterwards, because the create path
// refuses a paused partition (E4's placement gate) -- so the pause has to be
// stepped out of and then put back, exactly as the capture steps into it.
void env_ckpt_restore_begin(void) {
    ec_memset(ec_restore_pending, 0, (uint32_t)sizeof(ec_restore_pending));
    for (uint32_t i = 0; i < env_ckpt_count && i < ENV_CKPT_MAX; i++)
        ec_restore_pending[i] = 1u;
    ec_restore_pending_count = env_ckpt_count;
    ec_restore_refused_count = 0;
    ec_restore_resumed_count = 0;
    ec_clear_refusal();
}

uint32_t env_ckpt_restore_pending(void) { return ec_restore_pending_count; }
uint32_t env_ckpt_restore_refused(void) { return ec_restore_refused_count; }

// The dirty-capture gate. A record the capture never froze describes a moving
// target, so replaying it would be a create dressed as a restore -- refused
// with its own reason. PARTITION_SYSTEM is the documented exception (the
// capture never freezes the control plane's own partition, rule 2), and a
// record naming it is exactly as trustworthy as any other record captured
// under that rule.
int env_ckpt_restore_admissible(const struct EnvCkptRecord* rec) {
    if (rec == (const struct EnvCkptRecord*)0)   return ENV_CKPT_REFUSE_BAD_IDENTITY;
    if (!(rec->flags & ENV_CKPT_FLAG_QUIESCED) &&
        rec->partition_id != PARTITION_SYSTEM)
        return ENV_CKPT_REFUSE_UNQUIESCED;
    if (!partition_exists(rec->partition_id))    return ENV_CKPT_REFUSE_NO_PARTITION;
    return ENV_CKPT_REFUSE_NONE;
}

// Drop one slot from the pending set by (partition, index). Keeps the table's
// own identity as the key, so a caller that speaks the pair it was handed
// cannot settle the wrong record.
static void ec_restore_clear_pending(uint32_t partition_id, uint32_t index) {
    for (uint32_t i = 0; i < env_ckpt_count; i++) {
        if (env_ckpt_table[i].partition_id == partition_id &&
            env_ckpt_table[i].index == index) {
            if (ec_restore_pending[i] && ec_restore_pending_count > 0)
                ec_restore_pending_count--;
            ec_restore_pending[i] = 0;
            return;
        }
    }
}

int env_ckpt_restore_next(uint32_t* partition, uint32_t* index, uint32_t* env_id) {
    for (uint32_t i = 0; i < env_ckpt_count && i < ENV_CKPT_MAX; i++) {
        if (!ec_restore_pending[i]) continue;
        struct EnvCkptRecord* rec = &env_ckpt_table[i];

        // The gate: a record that must not be replayed is refused HERE, so no
        // caller can forget to ask. It leaves the pending set because there is
        // nothing left to try -- the reason stays queryable.
        int why = env_ckpt_restore_admissible(rec);
        if (why != ENV_CKPT_REFUSE_NONE) {
            uint32_t p = rec->partition_id, idx = rec->index;
            // a = the record's slot, b = the partition it names, so the NO_PARTITION
            // rendering can say WHICH partition is gone rather than only that one is.
            env_ckpt_restore_refuse(p, idx, (uint32_t)why, i, p);
            continue;
        }

        if (partition) *partition = rec->partition_id;
        if (index)     *index     = rec->index;
        if (env_id)    *env_id    = rec->env_id;
        return 1;
    }
    return 0;
}

void env_ckpt_restore_settle(uint32_t partition_id, uint32_t index) {
    ec_restore_clear_pending(partition_id, index);
    ec_clear_refusal();
}

void env_ckpt_restore_refuse(uint32_t partition_id, uint32_t index,
                             uint32_t code, uint32_t a, uint32_t b) {
    ec_restore_clear_pending(partition_id, index);
    ec_restore_refused_count++;
    ec_refuse(code, a, b);
}

// Identity, not placement. The two sides are the adopted record and the record
// the create's own ENV_REGISTER produced, so what is compared is what a create
// is obliged to reproduce: the environment's identity (partition, index), the
// tasks it is made of (names and kinds -- a restore that came back with a
// different sidecar is a different environment), and the region KINDS and their
// count (the three kind-labelled regions, no swap, no missing storage).
// ── The pause, stepped out of for one create and put straight back ───────────
int env_ckpt_restore_resume_for_create(uint32_t partition_id) {
    if (!partition_exists(partition_id)) return -1;
    if (!partition_is_paused(partition_id)) return 0;
    if (partition_resume(partition_id) != 0) return -1;
    // Ledger AFTER the resume succeeded, and only then: what is recorded is
    // what actually happened, so repause() cannot un-pause a partition this
    // pass never touched.
    for (uint32_t i = 0; i < ec_restore_resumed_count; i++)
        if (ec_restore_resumed[i] == partition_id) return 1;   // already owed
    if (ec_restore_resumed_count < ENV_CKPT_MAX)
        ec_restore_resumed[ec_restore_resumed_count++] = partition_id;
    return 1;
}

uint32_t env_ckpt_restore_repause(void) {
    uint32_t applied = 0;
    for (uint32_t i = 0; i < ec_restore_resumed_count && i < ENV_CKPT_MAX; i++) {
        uint32_t p = ec_restore_resumed[i];
        // Idempotent and fault-safe, exactly as the capture's release is: a
        // cleanup path must not be the thing that faults, and a partition
        // somebody resumed in the meantime is left running.
        if (!partition_exists(p))       continue;
        if (!partition_is_paused(p))    { if (partition_pause(p) == 0) applied++; }
    }
    ec_restore_resumed_count = 0;
    return applied;
}

uint32_t env_ckpt_restore_resumed(void) { return ec_restore_resumed_count; }

int env_ckpt_restore_same_identity(const struct EnvCkptRecord* want,
                                   const struct EnvCkptRecord* got) {
    if (want == (const struct EnvCkptRecord*)0 || got == (const struct EnvCkptRecord*)0)
        return 0;
    if (want->partition_id != got->partition_id) return 0;
    if (want->index        != got->index)        return 0;
    if (want->n_tasks      != got->n_tasks)      return 0;
    if (want->n_regions    != got->n_regions)    return 0;
    for (uint32_t i = 0; i < want->n_tasks && i < ENV_CKPT_MAX_TASKS; i++) {
        if (want->tasks[i].kind != got->tasks[i].kind) return 0;
        for (uint32_t c = 0; c < ENV_CKPT_NAME_LEN; c++)
            if (want->tasks[i].name[c] != got->tasks[i].name[c]) return 0;
    }
    for (uint32_t i = 0; i < want->n_regions && i < ENV_CKPT_MAX_REGIONS; i++) {
        if (want->regions[i].kind   != got->regions[i].kind)   return 0;
        if (want->regions[i].frames != got->regions[i].frames) return 0;
    }
    return 1;
}

// ─── Refusal reporting ────────────────────────────────────────────────────────
struct EnvCkptRefusal env_ckpt_last_refusal(void) { return ec_refusal; }

// Hand-rolled because the kernel is freestanding. Writes at most cap-1 bytes
// plus the terminator, and always terminates -- a truncating renderer is fine,
// an unterminated one would corrupt the serial transcript.
static void ec_put(char** p, char* end, const char* s) {
    while (*s && *p < end) *(*p)++ = *s++;
}
static void ec_put_u32(char** p, char* end, uint32_t v) {
    char tmp[10];
    int n = 0;
    if (v == 0) tmp[n++] = '0';
    while (v > 0 && n < 10) { tmp[n++] = (char)('0' + (v % 10u)); v /= 10u; }
    while (n > 0 && *p < end) *(*p)++ = tmp[--n];
}

void env_ckpt_refusal_text(const struct EnvCkptRefusal* r, char* out, uint32_t cap) {
    if (cap == 0) return;
    char* p   = out;
    char* end = out + (cap - 1);
    if (r == (const struct EnvCkptRefusal*)0) { *p = '\0'; return; }
    switch (r->code) {
        case ENV_CKPT_REFUSE_NONE:  ec_put(&p, end, "no refusal"); break;
        case ENV_CKPT_REFUSE_VERSION:
            ec_put(&p, end, "record version ");       ec_put_u32(&p, end, r->a);
            ec_put(&p, end, " is not this build's ");  ec_put_u32(&p, end, r->b); break;
        case ENV_CKPT_REFUSE_REC_SIZE:
            ec_put(&p, end, "record size ");          ec_put_u32(&p, end, r->a);
            ec_put(&p, end, " is not this build's ");  ec_put_u32(&p, end, r->b);
            ec_put(&p, end, " (foreign build)"); break;
        case ENV_CKPT_REFUSE_COUNT:
            ec_put(&p, end, "snapshot holds ");       ec_put_u32(&p, end, r->a);
            ec_put(&p, end, " records, table holds "); ec_put_u32(&p, end, r->b); break;
        case ENV_CKPT_REFUSE_EMPTY:   ec_put(&p, end, "snapshot is empty"); break;
        case ENV_CKPT_REFUSE_BAD_MAGIC:
            ec_put(&p, end, "record ");               ec_put_u32(&p, end, r->a);
            ec_put(&p, end, " has no environment record magic"); break;
        case ENV_CKPT_REFUSE_BAD_TASKS:
            ec_put(&p, end, "record ");               ec_put_u32(&p, end, r->a);
            ec_put(&p, end, " claims ");              ec_put_u32(&p, end, r->b);
            ec_put(&p, end, " tasks, more than a record holds"); break;
        case ENV_CKPT_REFUSE_BAD_REGIONS:
            ec_put(&p, end, "record ");               ec_put_u32(&p, end, r->a);
            ec_put(&p, end, " has an inconsistent region list (");
            ec_put_u32(&p, end, r->b); ec_put(&p, end, ")"); break;
        case ENV_CKPT_REFUSE_BAD_TASK:
            ec_put(&p, end, "record ");               ec_put_u32(&p, end, r->a);
            ec_put(&p, end, " has an unusable task "); ec_put_u32(&p, end, r->b); break;
        case ENV_CKPT_REFUSE_BAD_IDENTITY:
            ec_put(&p, end, "record ");               ec_put_u32(&p, end, r->a);
            ec_put(&p, end, " names no partition"); break;
        case ENV_CKPT_REFUSE_DUPLICATE:
            ec_put(&p, end, "record ");               ec_put_u32(&p, end, r->a);
            ec_put(&p, end, " repeats record ");      ec_put_u32(&p, end, r->b);
            ec_put(&p, end, "'s (partition, index)"); break;
        case ENV_CKPT_REFUSE_BAD_FLAGS:
            ec_put(&p, end, "record ");               ec_put_u32(&p, end, r->a);
            ec_put(&p, end, " carries flags this build does not know"); break;
        case ENV_CKPT_REFUSE_FULL:
            ec_put(&p, end, "table is full (");       ec_put_u32(&p, end, r->a);
            ec_put(&p, end, " of ");                  ec_put_u32(&p, end, r->b); break;
        case ENV_CKPT_REFUSE_UNQUIESCED:
            ec_put(&p, end, "record ");               ec_put_u32(&p, end, r->a);
            ec_put(&p, end, " was captured without quiescing -- not replayed"); break;
        case ENV_CKPT_REFUSE_NO_PARTITION:
            ec_put(&p, end, "record ");               ec_put_u32(&p, end, r->a);
            ec_put(&p, end, " names partition ");     ec_put_u32(&p, end, r->b);
            ec_put(&p, end, ", which is gone -- nowhere to restore it"); break;
        case ENV_CKPT_REFUSE_IDENTITY:
            ec_put(&p, end, "the create for record "); ec_put_u32(&p, end, r->a);
            ec_put(&p, end, " produced a different environment -- refused"); break;
        case ENV_CKPT_REFUSE_CREATE:
            ec_put(&p, end, "the create for record "); ec_put_u32(&p, end, r->a);
            ec_put(&p, end, " failed (status ");      ec_put_u32(&p, end, r->b);
            ec_put(&p, end, ")"); break;
        default: ec_put(&p, end, "unclassified refusal"); break;
    }
    *p = '\0';
}
