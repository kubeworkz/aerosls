#ifndef ENV_PAYLOAD_H
#define ENV_PAYLOAD_H

#include <stdint.h>

/* The record layer this layer rides on (env_ckpt.h). Forward-declared so
 * this header stays include-able on its own; the .c includes env_ckpt.h. */
struct EnvCkptRecord;

/*
 * env_payload.h — POSIX-Environments Roadmap v0.2, Phase P1a:
 * the PAYLOAD half of the environment checkpoint.
 *
 * ─── What this is ─────────────────────────────────────────────────────────
 * env_ckpt.h is the record layer: a descriptor naming the environment so a
 * replay can re-create it. This file is the bytes: the two sidecars' mapped
 * user frames (image, stack, and the three private regions), each sidecar's
 * register save area at the instant the capture quiesced it, and the
 * console's buffered output. v0.2 §4 itemises exactly this list; without it
 * the replayed environment is EMPTY — "the environment came back" is true
 * and "the environment came back with its contents" is not.
 *
 * The two CONTENTS properties the roadmap's verification plan names are
 * what this layer exists for:
 *   - a file written before the checkpoint is byte-identical after the
 *     reboot (the bytes themselves ride P1b's durable extent; the POSIX
 *     sidecar's VFS state and the shell that reads them ride THIS layer),
 *   - a shell variable set before the checkpoint is visible after it (the
 *     variable table lives in the POSIX sidecar's heap, which lives in its
 *     frames, which live here).
 *
 * ─── Where the bytes go ───────────────────────────────────────────────────
 * A directory of 8 x 32-byte entries keyed (partition_id, index), written
 * through persist.h's LBA discipline (PERSIST_ENV_PAYLOAD_HDR/ENT_LBA), and
 * a per-environment BAND SLOT for the blob itself: ENV_PAYLOAD_SLOT_LBA()
 * sits immediately after P1b's extent band (1 130 496), in the stretch
 * env_storage.h records as unmapped until ROWSTORE_LBA_BASE 2 000 000.
 * A directory is a small record and a payload is 5 MiB — the same split
 * P1b made between its directory and its extents.
 *
 * ─── The placement rule, which is the load-bearing decision ───────────────
 * The sidecars' MEM-cap regions are IDENTITY-mapped: vaddr == phys, and the
 * sidecar's heap holds RAW POINTERS into that region (the global bump
 * allocator is initialised over the budget cap's physical base). Captured
 * bytes therefore only mean anything at the captured ADDRESSES — a pour into
 * a relocated region would leave every interior pointer dangling, silently.
 * So:
 *
 *   1. Region pages are poured at their captured vaddr, and ONLY if the
 *      replayed record places the region at the same base. A replay that
 *      landed the environment anywhere else is refused BEFORE a byte is
 *      written (EP_REFUSE_PLACEMENT) — the honest "this environment's
 *      contents did not come back" instead of a corrupt one.
 *   2. Image and stack pages are at USER_PROC_CODE_BASE + offset, fixed
 *      across boots by cap_create_sidecar, so they pour into the live
 *      process's own page-table frames.
 *   3. Only image/stack-window and private-region pages are captured at
 *      all. Shared-arena and lazily-granted frames belong to their owners
 *      and are restored fresh by them; capturing them here would both miss
 *      their restore path and risk writing another tenant's memory.
 *
 * ─── The register rule ────────────────────────────────────────────────────
 * A captured sidecar is poured memory AND register state only when it was
 * quiesced PARKED on a channel recv (the idle posture: blocked reading its
 * console or its device channel). The live process must be parked in the
 * SAME syscall; the kernel-side park bookkeeping (which channel, which
 * request frame, which deadline) stays THIS boot's — those name boot-local
 * objects the capture cannot carry. A capture taken while a sidecar was
 * mid-compute is replayed as an empty environment with a NAMED refusal
 * (EP_REFUSE_RUNNING): refusal over partial application, v0.2 §4's rule,
 * applied to memory. The boot arm quiesces at the shell's idle point, so
 * its captures are always the parked form.
 *
 * ─── Why the pour has no yield inside ─────────────────────────────────────
 * Ring-3 runs only while the control plane yields (unified boot). The pour
 * runs straight-line inside the restore pass with NO kernel_yield_to_ring3
 * between its validation and its last memcpy, so no sidecar can execute
 * over the memory being poured. tests/env_checkpoint_restore_check.sh pins
 * the absence of a yield in the pour body as a source clause.
 *
 * ─── What this layer does NOT do ──────────────────────────────────────────
 * It does not carry pids, channels or env_ids (per-boot facts, already
 * refused as identity in env_ckpt.h); it does not snapshot anything
 * outside the two sidecars (no kernel state — that is checkpoint_mgr's
 * sixteen regions); and it does not claim cross-node restore (P3).
 */

/* Directory + blob format. Bumped by the WRITER whenever the layout
 * changes; a reader refuses anything else by name (EP_REFUSE_FORMAT). */
#define ENV_PAYLOAD_REC_VERSION 1u
#define ENV_PAYLOAD_DIR_MAGIC   0x534C53504C4F4131ULL  /* "SLSPLOA1" */
#define ENV_PAYLOAD_BLOB_MAGIC  0x534C5350424C4F31ULL  /* "SLSPBLO1" */

/* 8 environments, one slot each — the slot number is the directory entry
 * index, exactly like P1b's extent slot == its directory slot. */
#define ENV_PAYLOAD_MAX         8u

/* The band: 16 384 sectors (8 MiB) per slot, starting where P1b's extent
 * band ends (1 114 112 + 16 384 = 1 130 496), ending at 1 261 568 — well
 * under ROWSTORE_LBA_BASE 2 000 000. 8 MiB is the itemised payload (three
 * 1 MiB regions + two images + two 64 KiB stacks) with headroom; a blob
 * that does not fit its slot is refused by name (EP_REFUSE_TOO_BIG), never
 * truncated. */
#define ENV_PAYLOAD_SLOT_SECTORS 16384ULL
#define ENV_PAYLOAD_DATA_LBA_BASE 1130496ULL
#define ENV_PAYLOAD_SLOT_LBA(slot) \
    (ENV_PAYLOAD_DATA_LBA_BASE + ((uint64_t)(slot) * ENV_PAYLOAD_SLOT_SECTORS))

/* Directory entry flags. */
#define ENV_PAYLOAD_F_VALID     0x0001u

/* Register-state form of one captured task. */
#define ENV_PAYLOAD_FORM_RUNNING 0u   /* mid-compute when quiesced — named refusal on restore */
#define ENV_PAYLOAD_FORM_PARKED  1u   /* blocked on a channel recv — the restorable posture */

/* Refusal reasons. A code plus operands, the same contract env_ckpt.h
 * gives its refusals: the code is what a guard asserts on, the text is
 * what an operator reads in the serial transcript. */
#define EP_REFUSE_NONE       0
#define EP_REFUSE_ABSENT     1   /* no directory entry for (partition, index) */
#define EP_REFUSE_IO         2   /* a=blob phase, b=device status */
#define EP_REFUSE_FORMAT     3   /* a=magic/version half that failed */
#define EP_REFUSE_SEQ        4   /* blob was captured by a different checkpoint sequence */
#define EP_REFUSE_PLACEMENT  5   /* a=region kind, b=0/1 base-vs-frames mismatch */
#define EP_REFUSE_TOO_BIG    6   /* a=blob sectors, b=slot sectors */
#define EP_REFUSE_NO_PROCESS 7   /* a=task index; a captured sidecar has no live process */
#define EP_REFUSE_RUNNING    8   /* a=task index; captured mid-compute, not parked */
#define EP_REFUSE_NOT_PARKED 9   /* a=task index, b=park_syscall; live process is not parked in the same recv */
#define EP_REFUSE_NO_MAPPING 10  /* a=task index, b=page index; a captured vaddr has no live frame */
#define EP_REFUSE_CONSOLE    11  /* the replayed environment's console binding is gone */
#define EP_REFUSE_SKIPPED    12  /* the caller asked for a metadata-only restore (the no-restore tooth) */

struct EnvPayloadRefusal {
    uint32_t code;
    uint32_t a;
    uint32_t b;
};

/* ─── On-disk directory ─────────────────────────────────────────────────────
 * One header frame + one entry frame, written as a pair every capture and
 * read on demand at restore (nothing needs it at boot: the payload is only
 * ever read by the restore pass). */
struct EnvPayloadDirHead {
    uint64_t magic;
    uint32_t version;    /* ENV_PAYLOAD_REC_VERSION */
    uint32_t count;      /* live entries written */
    uint32_t rec_size;   /* sizeof(struct EnvPayloadDirEntry) */
    uint32_t _pad;
};

struct EnvPayloadDirEntry {
    uint32_t partition_id;
    uint32_t index;
    uint32_t slot;       /* 0..7; the LBA is derived, never stored twice */
    uint32_t flags;      /* ENV_PAYLOAD_F_VALID */
    uint64_t seq;        /* checkpoint sequence that captured it */
    uint64_t sectors;    /* blob length in 512 B sectors */
};

/* ─── The blob ───────────────────────────────────────────────────────────
 * [head][regions][tasks][page vaddrs][console bytes][padding to 4 KiB]
 * [page data, 4 KiB per page]. The vaddr index precedes the data so the
 * restore can validate EVERY destination before writing ANY byte —
 * refusal over partial application, made checkable. */
struct EnvPayloadHead {
    uint64_t magic;
    uint32_t version;
    uint32_t bytes;         /* total blob bytes including the head */
    uint32_t partition_id;
    uint32_t index;
    uint32_t seq_low;       /* checkpoint sequence (low 32; sequences are small) */
    uint32_t seq_high;
    uint32_t n_tasks;
    uint32_t n_pages;
    uint32_t console_len;
    uint32_t crc32;         /* over every byte after the head, in write order */
    uint32_t _pad;
};

struct EnvPayloadRegion {   /* the captured placement, for the placement rule */
    uint64_t base;
    uint32_t frames;
    uint32_t kind;          /* ENV_CKPT_REGION_* */
};

/* Register save area as captured: process.h's CapParkCtx, carried as nine
 * plain qwords so this header need not include scheduler.h. */
struct EnvPayloadTask {
    char     name[24];      /* drv.ramdisk.<i> / aerosls.posix.<i> */
    uint8_t  kind;          /* ENV_CKPT_TASK_* */
    uint8_t  form;          /* ENV_PAYLOAD_FORM_* */
    uint16_t park_syscall;  /* SYS_SLS_CAP_RECV / SYS_SLS_CHAN_WAIT when parked */
    uint64_t park[9];       /* r11, rcx, r15..rbp + user_rsp — CapParkCtx order */
    uint64_t user_rip;
    uint64_t user_rsp;
    uint32_t waiting_nchans; /* captured; 0 = single-channel recv park */
    uint32_t _pad;
};

/* ─── Public API ────────────────────────────────────────────────────────────*/

/* Capture every live record's payload into its band slot and write the
 * directory. CALLED INSIDE persist_environments()'s quiesce interval —
 * every process it reads is frozen by that quiesce, which is the only
 * reason a page-walk of a running sidecar means anything.
 * Returns the number of payloads written; a failure on one environment is
 * logged and leaves that entry invalid (the record still lands — a replay
 * without a payload is an empty environment, not a failed checkpoint). */
uint32_t env_payload_capture_all(void);

/* Best-effort: wait (bounded, yielding) until every named sidecar of `rec`
 * is parked on a channel recv, so the pour that follows has a restorable
 * process on the other side. Called by the restore pass BETWEEN the create
 * and the pass's repause, where a yield is still legal. Returns 1 when all
 * parked, 0 on timeout. */
int env_payload_wait_parked(const struct EnvCkptRecord* rec);

/* Pour the payload for (partition, index) into the environment the replay
 * created (`new_env_id` is where the console binding lives now), checking
 * the whole blob — format, sequence, placement, processes, park form,
 * every destination mapping — before writing a byte.
 * Returns 0 when the contents were restored, or an EP_REFUSE_* code, in
 * which case the environment stands as the empty replay it already is.
 * MUST NOT yield: see the header's no-yield rule. */
int env_payload_restore(uint32_t partition, uint32_t index,
                        uint32_t new_env_id, uint64_t want_seq,
                        uint32_t* out_pages, uint32_t* out_console);

/* 1 when a valid directory entry exists for this identity. */
int env_payload_present(uint32_t partition, uint32_t index);

/* The last refusal and its rendering (the code is the contract; the text
 * exists so the serial transcript says why the contents did not come
 * back). */
struct EnvPayloadRefusal env_payload_last_refusal(void);
void env_payload_refusal_text(const struct EnvPayloadRefusal* r,
                              char* out, uint32_t cap);

/* Reset the refusal latch (host tests, and the restore pass per attempt). */
void env_payload_clear_refusal(void);

#endif /* ENV_PAYLOAD_H */
