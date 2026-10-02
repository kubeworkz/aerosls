#ifndef ENV_CKPT_H
#define ENV_CKPT_H

#include <stdint.h>

/*
 * env_ckpt.h — POSIX-Environments Roadmap v0.2, Phase P1a:
 * the environment checkpoint record.
 *
 * ─── What this is ─────────────────────────────────────────────────────────
 * One record per POSIX environment, describing everything a restore needs
 * that a create does not: the environment's identity (partition, index,
 * env_id), the binding it must re-register under (drv.ramdisk.<index>,
 * aerosls.posix.<index>), its console, its three private regions, its
 * messenger endpoints, and — a field list, not a payload — the extent of its
 * durable storage. The records live in a fixed NVMe region alongside the
 * kernel's other persisted arrays, so they survive a reboot the same way
 * object_catalog[] and partition_table[] do.
 *
 * ─── What this is NOT, and why saying so is load-bearing ──────────────────
 * This is the record layer, not the payload layer. The two sidecars' page
 * tables and mapped frames, their register save areas, and the console's
 * buffered bytes are NOT here: that is 1344 frames (5.25 MiB) of environment
 * plus its page tables, and v0.2 §4 records the decision explicitly — "two
 * mechanisms, one record, is the honest shape; a single 5 MiB record in a
 * 512-sector band is not." The record carries the *extent*
 * (state_lba/state_sectors/state_bytes) so the payload mechanism has a place
 * to be named from.
 *
 * ─── The discipline this file exists to enforce ───────────────────────────
 * "Refusal over partial application" (v0.2 §4): a snapshot whose version,
 * record size, magic or internal consistency does not match is refused with a
 * named reason and leaves NO environment behind — not a fresh one, not a
 * half-restored one. env_ckpt_adopt() is all-or-nothing by construction, and
 * the refusal is a queryable fact (env_ckpt_last_refusal()) rather than a
 * log line, so a guard can assert it instead of grepping.
 *
 * ─── Why the version is checked and not migrated ──────────────────────────
 * One-way format changes are this codebase's established, documented
 * trade-off (see PERSIST_STORAGE_ISOLATION_PHASE3_MARK, persist.h). A record
 * written by a build with a different layout is refused and the environment
 * cold-starts, which is the same "refuse rather than guess" answer
 * failover_recover_from() gives when no checkpoint is held.
 */

/* ─── On-disk record format ──────────────────────────────────────────────────
 * magic identifies the record inside the array; version is bumped by the
 * WRITER whenever the layout changes; size is sizeof(struct EnvCkptRecord)
 * as the writing build saw it, which is the field that catches a same-version
 * rebuild with a different compiler/packing. All three are checked on adopt. */
#define ENV_CKPT_REC_MAGIC    0x534C53454E564B31ULL   /* "SLSENVK1" */
#define ENV_CKPT_REC_VERSION  1u

/* Environments per node in the first cut. v0.2 §8 sizes the node at four
 * environments; eight is the headroom, and the whole array must stay inside
 * one NVMe frame (the _Static_assert in env_ckpt.c is what holds that). */
#define ENV_CKPT_MAX          8u

#define ENV_CKPT_NAME_LEN     24u
#define ENV_CKPT_MAX_REGIONS  3u    /* POSIX heap + ramdisk heap + ramdisk storage */
#define ENV_CKPT_MAX_TASKS    4u    /* the two sidecars, plus the hook E7 fills */
#define ENV_CKPT_MAX_CHANS    4u    /* init's messenger endpoints */

/* Region kinds — the three cap regions E4 charges to the target partition.
 * Named rather than positional, because a restore that reattaches the POSIX
 * heap where the ramdisk storage belongs is exactly the silent-corruption
 * class the persist LBA guard exists for. */
#define ENV_CKPT_REGION_POSIX_HEAP 1u
#define ENV_CKPT_REGION_RD_HEAP    2u
#define ENV_CKPT_REGION_RD_STORAGE 3u

/* Task kinds. 2 is reserved and deliberately unused today: the Linux task is
 * E7's, and v0.2 §4 leaves the hook (the list exists, with the two sidecars
 * in it). Storing a kind the restore does not know is a refusal, not a skip. */
#define ENV_CKPT_TASK_POSIX_SIDECAR   0u
#define ENV_CKPT_TASK_RAMDISK_SIDECAR 1u
#define ENV_CKPT_TASK_LINUX           2u

#define ENV_CKPT_FLAG_QUIESCED         0x0001u  /* this capture held its partition
                                                 * paused while it was written */
/* The partition was ALREADY administratively paused when the capture ran — an
 * operator's intent, not something the capture did. A capture resumes only the
 * partitions IT paused, so this bit is what tells a restore which partitions to
 * put back into the paused state rather than leaving them running (v0.2 §4's
 * "restores that prior state"). */
#define ENV_CKPT_FLAG_PARTITION_PAUSED 0x0002u

struct EnvCkptRegion {
    uint64_t base;     /* frame base as the environment was placed */
    uint32_t frames;
    uint32_t kind;     /* ENV_CKPT_REGION_* */
};

struct EnvCkptTask {
    char     name[ENV_CKPT_NAME_LEN];  /* drv.ramdisk.<index> / aerosls.posix.<index> */
    uint8_t  kind;                     /* ENV_CKPT_TASK_* */
    uint8_t  _pad[3];
    uint32_t cap_count;                /* capabilities this task was granted */
    uint64_t entry;                    /* where the restore re-enters it */
    uint64_t stack_bottom;
    uint64_t stack_top;
};

struct EnvCkptRecord {
    uint64_t magic;
    uint32_t version;
    uint32_t size;         /* sizeof(struct EnvCkptRecord) at the writer */
    uint64_t sequence;     /* the checkpoint sequence this record belongs to */
    uint32_t partition_id; /* the partition the environment is charged to */
    uint32_t index;        /* the environment's index within its partition —
                              this is the name that matters for the registry */
    uint32_t env_id;       /* the environment manager's own handle */
    uint32_t console_id;   /* its E6 console binding */
    uint32_t flags;        /* ENV_CKPT_FLAG_* */
    uint32_t n_tasks;
    uint32_t n_regions;
    uint32_t n_chans;
    uint32_t chans[ENV_CKPT_MAX_CHANS];  /* init's messenger endpoints */
    uint64_t state_lba;     /* durable storage extent (P1b); 0 until then */
    uint64_t state_sectors;
    uint32_t state_bytes;
    uint32_t _pad;
    struct EnvCkptRegion regions[ENV_CKPT_MAX_REGIONS];
    struct EnvCkptTask   tasks[ENV_CKPT_MAX_TASKS];
    /* Deliberately absent: pids. They are not stable across a boot and are
     * not identity — the E5 teardown already resolves pids by name for
     * exactly this reason, and v0.2 §4 makes it an assertable property that a
     * restored environment equals the checkpointed one except for them. */
};

/* ─── The environment manager's registration ────────────────────────────────
 * A created environment lives in TWO places at once: init's `struct
 * Environment` (the three private regions it allocated from the frame pool, the
 * four messenger endpoints init holds to the environment's sidecars, and the
 * registry names those sidecars were created under) and the kernel's own
 * records (partition, index, the console binding, where the sidecars were
 * loaded). Neither side alone can describe the environment, so the record is
 * assembled from both:
 *
 *   init  →  ENV_REGISTER            (this struct: what only init knows)
 *   kernel→  env_ckpt_register_from() (stamps its own half and stores the record)
 *
 * This struct IS the wire body of ENV_REGISTER's reply (kernel/env_proto.h
 * encodes and parses exactly these bytes; its Rust twin is
 * user/proto/src/env_proto.rs). That is the point of it being a plain struct of
 * plain fields with no padding holes: `_Static_assert`s in both files pin the
 * size, and tests/env_register_pin_check.sh reads BOTH files and refuses to let
 * the two sides' field order, offsets or counts drift apart. A record written
 * from a body the other side built differently is the silent corruption this
 * whole file is arranged to prevent.
 *
 * What is deliberately NOT here: magic/version/size/sequence (this build's
 * format, not init's), console_id (the kernel binds the console), and each
 * task's `entry`/`stack_*` (kernel-assigned: `image_vbase + image_entry` and
 * the user stack cap.c mapped). A restore re-enters a sidecar where the KERNEL
 * put it, so those cannot come from init even in principle. */
struct EnvCkptRegister {
    uint32_t partition_id;
    uint32_t index;
    uint32_t env_id;
    uint32_t n_regions;
    /* One entry per private region, kind-labelled rather than positional: the
     * wire order is the record's order (POSIX heap, ramdisk heap, ramdisk
     * storage) and the kind is carried so a swap between the two sides is a
     * refusal (ENV_CKPT_REFUSE_BAD_REGIONS) rather than a silently mis-labelled
     * heap. */
    struct EnvCkptRegion regions[ENV_CKPT_MAX_REGIONS];
    uint32_t n_chans;
    uint32_t chans[ENV_CKPT_MAX_CHANS];   /* init's messenger endpoints */
    uint32_t n_tasks;
    char     task_name[ENV_CKPT_MAX_TASKS][ENV_CKPT_NAME_LEN];
    uint32_t task_kind[ENV_CKPT_MAX_TASKS];
    /* No padding hole anywhere, so sizeof() is field-sum and both sides can
     * assert it: 16 + 3*16 + 4 + 4*4 + 4 + 4*24 + 4*4 = 200 bytes. */
};

/* ─── Refusal reasons ────────────────────────────────────────────────────────
 * A code plus two numeric operands, rather than a formatted string: the
 * kernel side is freestanding (no libc), and a guard asserting a *code* is
 * not fooled by wording drift the way a grep over a log line is. */
#define ENV_CKPT_REFUSE_NONE         0
#define ENV_CKPT_REFUSE_VERSION      1   /* a=found b=expected */
#define ENV_CKPT_REFUSE_REC_SIZE     2   /* a=found b=expected */
#define ENV_CKPT_REFUSE_COUNT        3   /* a=found b=max */
#define ENV_CKPT_REFUSE_EMPTY        4   /* a=count */
#define ENV_CKPT_REFUSE_BAD_MAGIC    5   /* a=record index */
#define ENV_CKPT_REFUSE_BAD_TASKS    6   /* a=index b=n_tasks */
#define ENV_CKPT_REFUSE_BAD_REGIONS  7   /* a=index b=n_regions */
#define ENV_CKPT_REFUSE_BAD_TASK     8   /* a=index b=n_tasks */
#define ENV_CKPT_REFUSE_BAD_IDENTITY 9   /* a=record index */
#define ENV_CKPT_REFUSE_DUPLICATE   10   /* a=index b=earlier index */
#define ENV_CKPT_REFUSE_FULL        11   /* a=count b=max */
#define ENV_CKPT_REFUSE_BAD_FLAGS   12   /* a=record index; a flag bit this build does not know */
/* Restore-side reasons (P1a, the replay). A record that adopted cleanly can
 * still be one this boot must NOT replay, and each of these says why: */
#define ENV_CKPT_REFUSE_UNQUIESCED   13  /* a=record index; captured while its partition ran */
#define ENV_CKPT_REFUSE_NO_PARTITION 14  /* a=record index; the partition it names is gone */
#define ENV_CKPT_REFUSE_IDENTITY     15  /* a=record index; the create produced a different environment */
#define ENV_CKPT_REFUSE_CREATE       16  /* a=record index; b=the ENV_* status or rc the create answered */

struct EnvCkptRefusal {
    uint32_t code;
    uint32_t a;
    uint32_t b;
};

/* ─── The table (the persisted array; persist.c owns the region) ─────────────
 * A real, fixed array with a real sizeof() — that is what makes it
 * expressible in persist.c's region table (p_region_specs) and therefore
 * covered by the torn-write checksum scan, exactly like tenants[]/views[]. */
extern struct EnvCkptRecord env_ckpt_table[ENV_CKPT_MAX];
extern uint32_t             env_ckpt_count;

/* ─── Public API ──────────────────────────────────────────────────────────── */

/* Empty the table (boot, and host tests between phases). */
void env_ckpt_reset(void);

/* Validate ONE record against this build's format. 0 = acceptable.
 * This is the gate env_ckpt_record() uses; it is exposed because the
 * refusal reasons are the feature, and a guard asserts on them directly. */
int env_ckpt_valid(const struct EnvCkptRecord* rec);

/* Store a record, upserting on (partition_id, index). Marks
 * CKPT_REGION_ENV dirty so a later checkpoint writes it. Returns 0, or an
 * ENV_CKPT_REFUSE_* code (and stores nothing) if the record is not valid. */
int env_ckpt_record(const struct EnvCkptRecord* rec);

/* Register an environment from the environment manager's half of it: build the
 * record out of `reg` (init's facts), the kernel's own stamps (magic, version,
 * size, `sequence`, `console_id`), and the per-task entries the kernel resolved
 * from its own process table (`task_entry[i]` is task i's `user_rip`, i.e.
 * exactly where cap_create_sidecar released the sidecar; 0 when the name did
 * not resolve). Then store it via env_ckpt_record().
 *
 * Returns 0, or an ENV_CKPT_REFUSE_* code — including a refusal when a task
 * names a sidecar the kernel cannot place (`entry` 0), because a record whose
 * restore target is unknown is not a record an operator should be able to
 * restore. Refusal-over-partial-application applies here too: nothing is
 * stored. */
int env_ckpt_register_from(const struct EnvCkptRegister* reg,
                           const uint64_t task_entry[ENV_CKPT_MAX_TASKS],
                           uint32_t console_id, uint64_t sequence);

/* Drop the record for (partition_id, index), if any. */
int env_ckpt_drop(uint32_t partition_id, uint32_t index);

/* Look one up. NULL when absent. */
const struct EnvCkptRecord* env_ckpt_find(uint32_t partition_id, uint32_t index);

/* Number of live records. */
uint32_t env_ckpt_count_live(void);

/* Adopt a snapshot read off NVMe, ALL OR NOTHING (v0.2 §4's "refusal over
 * partial application"). count/rec_size/version are the values the region
 * header carried; staging holds `count` records. On ANY fault the live table
 * is left empty, nothing is half-applied, and the reason is recorded.
 * Returns 0 on success, or an ENV_CKPT_REFUSE_* code. */
int env_ckpt_adopt(const struct EnvCkptRecord* staging, uint32_t count,
                   uint32_t rec_size, uint32_t version);

/* Recompute the live count from the table (post-restore bookkeeping, the
 * same shape catalog_after_restore() has for object_catalog[]). */
void env_ckpt_after_restore(void);

/* ─── Quiescing: freezing an environment before it is written down ───────────
 * A checkpoint taken while an environment is RUNNING describes a moving target.
 * The capture therefore pauses each captured environment's partition first, so
 * what is written down is a consistent instant, and resumes it afterwards.
 * Three rules make that safe, and each is a clause in
 * tests/env_ckpt_host_test.c:
 *
 *   1. A partition an operator had ALREADY paused is left paused. The capture
 *      resumes exactly the partitions it paused -- the discipline
 *      partition_migrate() already uses -- because a capture that un-paused
 *      somebody's deliberately frozen partition would be a silent state change
 *      wearing a checkpoint's name.
 *   2. The control plane's OWN partition (PARTITION_SYSTEM) is never frozen:
 *      pausing it would freeze the very machinery performing the capture, and
 *      no tenant environment belongs there (`cap_create_sidecar_in` places
 *      init's own children there, which is why E3's boot-spawned environments
 *      live there and why a manager-created one COULD). Such a record is
 *      captured with ENV_CKPT_FLAG_QUIESCED *clear* and counted separately, so
 *      the honest "this one was not frozen" is visible per record instead of
 *      being assumed.
 *   3. A record naming a partition that no longer exists is DROPPED, not
 *      captured. The environment it names cannot be restored -- there is no
 *      partition to put it back into -- so persisting it would be exactly the
 *      stale-versus-absent sin this phase exists to police. It is counted, so
 *      the event is assertable rather than only greppable. This is the same
 *      call init's `forget_dead_environments` makes on its side of the same
 *      problem: an environment whose partition is gone is over, and is never
 *      restarted.
 *
 * `out` may be NULL only if the caller will not call release (a caller that
 * quiesces must release). */

/* How one capture's quiesce went. Fixed-size and plain, so a caller can hold
 * one on its stack and hand it straight back to env_ckpt_release_capture(). */
struct EnvCkptQuiesce {
    uint32_t n_quiesced;        /* records this capture froze */
    uint32_t n_already_paused;  /* partitions that were paused before it */
    uint32_t n_unfrozen;        /* records captured without freezing: rule 2
                                 * (PARTITION_SYSTEM), or a pause that did not
                                 * take -- the honest "not frozen" count */
    uint32_t n_dropped;         /* records dropped: their partition is gone */
    uint32_t n_paused_by_capture;
    uint32_t paused_by_capture[ENV_CKPT_MAX];  /* the ids this capture must resume */
};

/* Freeze every captured environment: pause each partition holding a record
 * (except PARTITION_SYSTEM), set ENV_CKPT_FLAG_QUIESCED on the records that
 * were frozen and ENV_CKPT_FLAG_PARTITION_PAUSED on those whose partitions were
 * already paused, and drop the records of dead partitions. Fills `out`. */
void env_ckpt_quiesce_for_capture(struct EnvCkptQuiesce* out);

/* Undo env_ckpt_quiesce_for_capture(): resume the partitions THIS capture
 * paused, and no others. Idempotent, and safe with a NULL/empty result. MUST be
 * called on every path out of a capture -- including the failure paths -- or a
 * failed checkpoint leaves a tenant frozen with nothing on disk to show for
 * it. That is the `leak-unpause` tooth in v0.2 §4. */
void env_ckpt_release_capture(const struct EnvCkptQuiesce* q);

/* Restore side: put back the pause state the snapshot recorded. For every
 * adopted record carrying ENV_CKPT_FLAG_PARTITION_PAUSED, pause its partition
 * again (idempotently); partitions the snapshot does not name are left exactly
 * as they are. Returns the number of partitions this call re-paused. A record
 * in PARTITION_SYSTEM is deliberately NOT re-paused (rule 2 above: a boot that
 * comes up with the control plane's own partition frozen is a worse failure
 * than losing the flag) -- the record still says what was true, and the
 * refusal to act on it is reported by the caller. */
uint32_t env_ckpt_apply_restored_pauses(void);

/* Drop the record for the environment `env_id` in `partition_id` -- how a
 * destroy tells the checkpoint the environment is gone. Without it a destroyed
 * environment's record outlives it and the next restore brings back an
 * environment an operator removed. Returns 0 if a record was dropped, 1 if
 * there was none. */
int env_ckpt_drop_env(uint32_t partition_id, uint32_t env_id);

/* ─── Restore-through-create: turning a record back into a live environment ──
 * A record is a DESCRIPTOR, not a payload (see the file header). So a restore
 * does what v0.2 §4 says it does: it replays the descriptor through the
 * environment manager's EXISTING create path -- the same ENV_CREATE round trip
 * an HTTP `POST /api/partition/{id}/env` makes -- and then checks that what
 * came back is the environment the record described. Nothing here has a second
 * create path: a restored environment is a created environment, or it is not
 * restored at all.
 *
 * Four properties are load-bearing, and each is a clause in
 * tests/env_checkpoint_restore_check.sh and a section of
 * tests/env_ckpt_host_test.c:
 *
 *   1. A record is replayed at most once per boot. env_ckpt_after_restore()
 *      arms every adopted record; env_ckpt_restore_next() hands one out and
 *      the caller settles it (replayed) or refuses it (with a reason). The
 *      pending set is runtime state and is deliberately NOT persisted -- which
 *      environments THIS boot still has to replay is a property of this boot,
 *      exactly as partition_paused[] is.
 *   2. A record captured WITHOUT a quiesce is refused, not replayed
 *      (ENV_CKPT_REFUSE_UNQUIESCED) -- unless it is PARTITION_SYSTEM's, the one
 *      partition a capture never freezes (rule 2). What such a record
 *      describes is a moving target, and a create that reproduced it would be
 *      the `dirty-capture` failure wearing a restore's name. The
 *      PARTITION_SYSTEM exemption is the same decision the capture made,
 *      carried to the other side.
 *   3. A record whose partition is gone is refused (ENV_CKPT_REFUSE_NO_PARTITION)
 *      rather than attempted: there is nowhere to put the environment, and the
 *      create would have to be refused anyway -- by the paused-partition gate,
 *      or by the allocator, with a reason that would not have said "the
 *      partition is gone".
 *   4. The environment that comes back must be the one the record described
 *      (env_ckpt_restore_same_identity()). Identity is the record's stable
 *      half -- partition, index, the task names and kinds, the region kinds --
 *      NOT its pids, its env_id, its per-boot frame bases, its freshly minted
 *      channels or the console key init binds at create (the region frame
 *      counts ARE compared: they are the environment's shape, constants in the
 *      manager, and a heap that came back a different size is a different
 *      environment). A mismatch is refused
 *      (ENV_CKPT_REFUSE_IDENTITY) and the environment the create made is
 *      destroyed: refusal over partial application, applied to the replay, so
 *      a refused restore leaves no environment behind. That is the
 *      `stale-descriptor` tooth.
 *
 * What this does NOT do yet, said plainly so it cannot be mistaken: the
 * payload (the sidecars' page tables, mapped frames, register save areas and
 * the console's buffered bytes) is not captured, so the environment that comes
 * back is EMPTY -- its files and its shell variables are gone. This layer
 * therefore counts a replay as "replayed", never as "restored": the extents
 * the record carries (state_lba/state_sectors/state_bytes, and each region's
 * base) are what the payload increment fills in, and its boot guard is what
 * will assert the bytes. */

/* Arm the pending set: every live record becomes one to replay. Called by
 * env_ckpt_after_restore(), so a boot that adopted nothing (or refused the
 * snapshot) has nothing pending -- the vacuity control the restore check
 * reads. Also resets this boot's refusal counter. */
void env_ckpt_restore_begin(void);

/* 0 when `rec` may be replayed, or the ENV_CKPT_REFUSE_* code saying why not
 * (UNQUIESCED, NO_PARTITION). Exposed because the reasons are the feature: a
 * guard asserts on the code, and env_ckpt_restore_next() applies exactly this
 * gate so a caller cannot forget it. */
int env_ckpt_restore_admissible(const struct EnvCkptRecord* rec);

/* The next record to replay: 1 and (partition, index, env_id) filled, or 0 when
 * nothing is left to replay. Records that may not be replayed are refused HERE
 * -- counted, with the reason in env_ckpt_last_refusal() -- and that record
 * leaves the pending set, so one gate covers every caller. The record handed
 * out stays pending until the caller settles or refuses it, so a caller that
 * gives up leaves it honestly still-pending rather than silently done. */
int env_ckpt_restore_next(uint32_t* partition, uint32_t* index, uint32_t* env_id);

/* The replay happened and re-registered: this record is done. */
void env_ckpt_restore_settle(uint32_t partition, uint32_t index);

/* The replay did not happen, for `code`: this record leaves the pending set, the
 * reason is recorded (env_ckpt_last_refusal(), and the counter behind
 * env_ckpt_restore_refused()), and nothing is stored. */
void env_ckpt_restore_refuse(uint32_t partition, uint32_t index,
                             uint32_t code, uint32_t a, uint32_t b);

/* How many records this boot still has to replay, and how many replays it has
 * refused so far (both counters are per-boot; restore_begin() resets them). */
uint32_t env_ckpt_restore_pending(void);
uint32_t env_ckpt_restore_refused(void);

/* 1 when `got` -- the record the create's own ENV_REGISTER produced -- is the
 * environment `want` described. Compares identity only: (partition, index), the
 * task names and kinds, and the region kinds and their frame counts. Bases,
 * entries, channels, env_id, console_id and the sequence are per-boot facts a
 * create legitimately re-decides; the payload increment is what will make the
 * bases matter, and until then asserting them would be asserting a number
 * nothing yet reproduces. 0 when either side is NULL or disagrees. */
int env_ckpt_restore_same_identity(const struct EnvCkptRecord* want,
                                   const struct EnvCkptRecord* got);

/* ── Stepping out of a restored pause for the length of one create ──────────
 * A snapshot that recorded an administratively-paused partition re-pauses it at
 * boot (env_ckpt_apply_restored_pauses), and the create path then REFUSES to
 * place an environment into a paused partition (E4's placement gate: "target
 * partition N is paused -- cannot charge it"). So a replay into such a
 * partition would be refused for a reason that says nothing about the
 * environment. These two are the capture's pause, mirrored: resume the target
 * only for the create, then put the pause back.
 *
 * The discipline is the capture's, and so is the failure it prevents:
 * env_ckpt_restore_resume_for_create() records in a LEDGER what IT resumed, and
 * env_ckpt_restore_repause() puts back exactly the ledger -- never "every
 * partition with a record" -- so a partition an operator keeps paused around
 * the replay is not un-paused by it. A caller must call repause() on EVERY path
 * out of the pass, with no early return between the two (the same rule the
 * capture's quiesce interval carries), or a failed restore leaves a partition
 * an operator had frozen RUNNING: the `leak-unpause` tooth, on the restore side.
 * Both are idempotent and safe with an empty ledger. */

/* Resume `partition_id` because a replay is about to create into it. Returns 1
 * when it WAS paused and is now resumed (the caller owes a repause), 0 when it
 * was already running (nothing to put back), -1 when it could not be resumed
 * because it is gone -- refused, not attempted. */
int env_ckpt_restore_resume_for_create(uint32_t partition_id);

/* Put back every pause this pass stepped out of, clear the ledger, and return
 * how many partitions were re-paused. */
uint32_t env_ckpt_restore_repause(void);

/* How many partitions the current pass resumed and has not yet re-paused. */
uint32_t env_ckpt_restore_resumed(void);

/* The last refusal, and its rendering. The code is the contract; the text
 * exists so the kernel's serial transcript says which record and why. */
struct EnvCkptRefusal env_ckpt_last_refusal(void);
void env_ckpt_refusal_text(const struct EnvCkptRefusal* r, char* out, uint32_t cap);

#endif /* ENV_CKPT_H */
