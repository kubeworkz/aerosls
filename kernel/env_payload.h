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
 *      all — plus, for a Linux task, the pages inside its OWN image span
 *      (`linux_image_lo..hi`, a linked ET_EXEC's segments) and its budget
 *      span (`linux_heap_base..limit`, where brk and anonymous maps land).
 *      Shared-arena and lazily-granted frames belong to their owners and
 *      are restored fresh by them; capturing them here would both miss
 *      their restore path and risk writing another tenant's memory.
 *      Every captured page carries its PTE-decoded permissions
 *      (EP_PAGE_PERM_*), so a restore can prove the destination map
 *      grants exactly what the capture saw — a vaddr+CRC list cannot tell
 *      RX text from RW data, and a map that got that wrong would run
 *      bytes the workload never intended to execute.
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
 * refused as identity in env_ckpt.h); it does not snapshot anything outside
 * the environment's own tasks (the two sidecars and, E7 increment 20, their
 * Linux tenant — no kernel state, that is checkpoint_mgr's sixteen regions);
 * it does not carry a parked Linux task's queued request (B9: the gate's
 * restorable form is USER); and it does not claim cross-node restore (P3).
 */

/* Directory + blob format. Bumped by the WRITER whenever the layout
 * changes; a reader refuses anything else by name (EP_REFUSE_FORMAT).
 *
 * v2 (E7 increment 20): the page index carries each page's PERMISSIONS
 * (12 -> 16 bytes per entry, EP_PAGE_PERM_* below) and EnvPayloadTask
 * carries the Linux task's full context, fs_base, cursors and page range.
 * A v1 blob is refused, not misread: the reader's layout walk would place
 * every page entry 4 bytes early and pour the wrong frame's bytes. */
#define ENV_PAYLOAD_REC_VERSION 2u
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
#define ENV_PAYLOAD_FORM_USER    2u   /* E7 increment 20: a Linux task stopped in
                                       * USER space (the full ring3_ctx is the
                                       * state, not a park frame). The gate's
                                       * restorable Linux form; a parked Linux
                                       * task is queued state this blob does not
                                       * carry (B9) and is refused on restore. */
#define ENV_PAYLOAD_FORM_ENTRY   3u   /* E7 increment 21: the gate workload
                                       * stopped AT a `syscall` instruction it
                                       * has not executed — no request in flight,
                                       * no reply queued, ring3_ctx is the frame
                                       * the sysret will install, and the
                                       * restored task re-runs the trap against
                                       * the sidecar's restored fd table. The
                                       * deterministic form: proc_gate_stop_at_entry
                                       * (kernel/process.c) names the instant by
                                       * the workload's own fourth 64 KiB read
                                       * (src_off=196608), instead of hoping a
                                       * pause lands between a reply and a trap. */

/* Per-page permission bits (the page index's fourth word). Decoded from the
 * LIVE PTE at capture (pt_perm_from_pte in env_payload.c), so the blob says
 * what the map actually granted — RX for a linked image's text, RW for stack
 * and heap, never a default. Guard pages have no frame and therefore no
 * entry at all: the range is the pages that exist, not the range that was
 * reserved. */
#define EP_PAGE_PERM_WRITE 0x1u   /* PTE.W — writable */
#define EP_PAGE_PERM_EXEC  0x2u   /* !PTE.NX — executable */
#define EP_PAGE_PERM_USER  0x4u   /* PTE.U — user-accessible (always set here) */
#define EP_PAGE_PERM_MASK  0x7u
#define EP_PAGE_ENTRY_BYTES 16u   /* { vaddr u64, crc u32, perm u32 } */

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
#define EP_REFUSE_PERM       13  /* a=task index, b=page index; the live PTE's permissions are not the captured ones */
#define EP_REFUSE_FORM       14  /* a=task index, b=form; a Linux task whose captured form this blob cannot resume (parked/queued) */
#define EP_REFUSE_SHAPE      15  /* a=task index; a captured task's kind is not what its slot claims (Linux vs sidecar) */

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
 * plain qwords so this header need not include scheduler.h.
 *
 * E7 increment 20 adds the Linux half at the END, so the sidecar fields keep
 * their offsets and a sidecar's payload reads exactly as it did (all the new
 * words are zero for one). A Linux task is not sidecar-shaped: its state is
 * the FULL ring3_ctx (20 qwords — a user-space preemption, not the nine-word
 * park frame), the six syscall argument registers the entry stub captured,
 * its TLS base and its two memory cursors. `n_pages` is the same count the
 * per-task block carries; the restore cross-checks the two rather than
 * trusting either half alone. */
struct EnvPayloadTask {
    char     name[24];      /* drv.ramdisk.<i> / aerosls.posix.<i> */
    uint8_t  kind;          /* ENV_CKPT_TASK_* */
    uint8_t  form;          /* ENV_PAYLOAD_FORM_* */
    uint16_t park_syscall;  /* SYS_SLS_CAP_RECV / SYS_SLS_CHAN_WAIT when parked */
    uint64_t park[9];       /* r11, rcx, r15..rbp + user_rsp — CapParkCtx order */
    uint64_t user_rip;
    uint64_t user_rsp;
    uint32_t waiting_nchans; /* captured; 0 = single-channel recv park */
    uint32_t n_pages;       /* captured pages for THIS task (the block's count) */
    /* ── E7 increment 20: the Linux task's own state (zero for a sidecar). ── */
    uint64_t linux_regs[20];/* struct TaskContext: r15..r8, rbp, rdi, rsi,
                             * rdx, rcx, rbx, rax, rip, cs, rflags, rsp, ss */
    uint64_t linux_args[6]; /* rdi, rsi, rdx, r10, r8, r9 at the capture */
    uint64_t fs_base;       /* §5.3 TLS base — live, not staged */
    uint64_t brk_cur;       /* §5.4 the break this task grew to */
    uint64_t mmap_top;      /* §5.4 the anonymous-map cursor */
    uint64_t heap_base;     /* the page RANGE: [heap_base, heap_limit) holds
                             * both brk growth and anonymous maps */
    uint64_t heap_limit;
    uint64_t stack_bottom;  /* the kernel-mapped argument stack */
    uint64_t stack_top;
    uint64_t image_lo;      /* the linked ET_EXEC image's page span */
    uint64_t image_hi;
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

/* The wait's two bounds, in timer ticks (timer.h: ~100 Hz). EP_WAIT_TICKS is
 * the STALL window — it restarts on every observed bit of progress, so it
 * measures quiet, not elapsed time — and EP_WAIT_CEIL_TICKS is the absolute
 * stop, sized under failover's ENV_MIGRATE_SLOT_GRACE_TICKS (600 s) because
 * this wait runs inside a restore the sweep is already covering. They are here
 * rather than in the .c because they are part of the wait's contract: the host
 * test asserts which of the two answered, not only that something did. */
#define EP_WAIT_TICKS      3000u                 /* ~30 s of quiet */
#define EP_WAIT_CEIL_TICKS (EP_WAIT_TICKS * 6u)  /* ~3 min absolute */

/* ─── E7 increment 22: the gate's TEETH (a build profile, never shipped) ──
 * A gate clause is worth what its failure side is worth: a harness that has
 * never watched a broken environment redden is a claim, not a gate. This
 * block is the fault injection tests/e7_gate_check.sh --teeth hangs on the
 * gate's own clauses.
 *
 * NOTHING HERE IS IN A SHIPPED IMAGE. The arms are compiled only with
 * -DE7G_TOOTH=1 (`make ... E7_TOOTH=1`), and even there they fire only when
 * the restore route's body names one (`{"tooth":"no-fd"}`). Every other
 * build refuses a named tooth by name before the pass runs
 * (env_payload_build_has_teeth()), so a request cannot talk a shipped kernel
 * into breaking an environment.
 *
 * The four teeth and the clause each one is aimed at:
 *
 *   descriptor  the payload's Linux task RECORD is dropped: no adoption, no
 *               pages for it, no context; the sidecars pour normally. Aimed
 *               at B2 ("the tenant was adopted"). The released child then
 *               restarts from _start, so B3 (the FIRST post-restore read is
 *               the captured continuation) and B4 (exactly one Copying)
 *               redden with it, by name.
 *   no-fd       the continuation's read is forwarded with a descriptor the
 *               sidecar's table does not carry, so the REAL fd lookup in the
 *               real table answers -EBADF. Aimed at B3/B4: the read never
 *               reaches the VFS, so no offset line and no Done appear.
 *   no-heap     the reply's destination page — the child's own copy buffer —
 *               is unmapped just before the read is forwarded, so the
 *               deliver's checked copy answers -EFAULT. Aimed at B4/B5 with
 *               B3 LEFT GREEN: the read WAS issued at the captured offset and
 *               only its bytes could not land. The tenant then unwinds, and
 *               its 64 KiB Vec's allocator header sits in that page's last
 *               bytes, so `free` takes the #PF AT THE WITHHELD PAGE — the
 *               address the harness matches against this tooth's own line.
 *               That (B3 green, a B5 fault at the named page) is the
 *               signature separating "the read never happened" from "the
 *               page was gone", which is what the tooth exists to prove the
 *               harness can tell apart.
 *   fs_base     the ADOPTED context's TLS base is re-stamped one page off the
 *               capture's — into the page below it, chosen from the record's
 *               own page list and only when that page's captured grant is
 *               writable, so the replay is not asked for an address no
 *               captured map backs. Aimed at B2, which now compares the
 *               adopted `fs_base=` with the capture's: adoption still
 *               happens (rip/rsp/brk/mtop and every page are untouched) and
 *               the continuation read is issued, so B3 stays green — then
 *               musl uses a pointer read from the wrong TCB and the tenant
 *               dies, which is why the tooth's declared reds are B2 and the
 *               B4/B5 consequences of that death, not B2 alone. It is the
 *               complement of `descriptor` — there the tenant never adopts
 *               and B3 restarts at zero; here it adopts, reads the captured
 *               continuation, and dies on a TLS base one page over.
 *
 * A tooth never crashes the kernel: each break lands on a path the shipped
 * code already has (a name that resolves to no process, a fd the table does
 * not carry, a user page that is not mapped), so the reds are the gate's own
 * refusals and lines, never a blanket fault. */
#ifndef E7G_TOOTH
#define E7G_TOOTH 0
#endif

#define ENV_PAYLOAD_TOOTH_NONE       0
#define ENV_PAYLOAD_TOOTH_DESCRIPTOR 1
#define ENV_PAYLOAD_TOOTH_NO_FD      2
#define ENV_PAYLOAD_TOOTH_NO_HEAP    3
#define ENV_PAYLOAD_TOOTH_FS_BASE    4

/* The fd the no-fd tooth substitutes: past every table this repo has, so the
 * sidecar's lookup answers EBADF instead of reaching an empty-but-in-range
 * slot (a different, weaker refusal). */
#define ENV_PAYLOAD_TOOTH_DEAD_FD    120u

/* 1 only in a teeth build; the route refuses a named tooth otherwise. */
int env_payload_build_has_teeth(void);

/* "descriptor" | "no-fd" | "no-heap" | "fs_base" -> code; -1 for anything
 * else (the route refuses a typo by name: a misspelt tooth must not silently
 * run the control and look green). */
int env_payload_tooth_from_name(const char* name);

/* The canonical name of a code ("none" for NONE), never NULL. */
const char* env_payload_tooth_name(int tooth);

/* Select the tooth the next pour installs on the Linux task. NONE for every
 * ordinary restore; effective only in a teeth build (a no-op elsewhere, so
 * the caller needs no #if). */
void env_payload_set_tooth(int tooth);

/* Best-effort: wait (yield-ing) until every named sidecar of `rec` is parked
 * on a channel recv AND has announced its [env-id] line, and both have held
 * for the settle window, so the pour that follows has a restorable process on
 * the other side. Called by the restore pass BETWEEN the create and the
 * pass's repause, where a yield is still legal. Returns 1 when the posture is
 * reached, 0 otherwise.
 *
 * The bound is a STALL window (EP_WAIT_TICKS), restarted by any observed
 * progress -- a switch-in on one of the record's sidecar tasks, or the
 * announcement landing -- with EP_WAIT_CEIL_TICKS as the absolute stop. That
 * shape is deliberate (live run 12, 2026-11-01): a fixed 30 s deadline expired
 * while the replayed `drv.ramdisk.1` was still serving the environment's own
 * boot I/O, so the wait answered "not parked", the pour refused by name, and a
 * healthy environment was reported as one whose contents could not come back.
 * A sidecar that is still being scheduled is still working; only an
 * environment whose tasks have stopped being scheduled is out of time, and
 * then the refusal is the truth. The pour's own check is unchanged and remains
 * the authority -- this wait only decides how long the pass is willing to look
 * for the posture it needs. */
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

/* ─── The image-window list, shared with the migrate path (P3) ─────────────
 * The reboot payload's page walk collects BOTH the sidecars' image-window
 * frames and their private-region pages; the migrate payload (P3 inc. 3/4)
 * used to carry only the region frames, and the destination poured them
 * under a freshly-booted sidecar whose boot stack, .data and .bss were its
 * OWN — a mixed state that #PF'd the restored environment on its first
 * console input (live find, 2026-10-23). The staged payload now appends the
 * image-window tail after the payload pages, in the canonical order this
 * pair defines: the record's sidecar tasks in record order, ascending
 * vaddr within each task — an order both nodes derive from their OWN live
 * processes, so no vaddrs cross the wire (the region pages are positional
 * the same way, and the placement gate validates the halves).
 *
 * `env_payload_image_window_list` fills vaddrs[] and the owning process per
 * page and returns the count. err: 0 = ok (count may be 0 — no live
 * sidecar resolves, e.g. a host test's world), 1 = SOME but not all named
 * sidecars resolve (a half-environment's bytes cannot be resumed), 2 = more
 * pages than cap. `env_payload_proc_parked` is the posture predicate the
 * sender gates on (the pour resumes the instant the sidecars were frozen
 * in; a mid-compute instant cannot be resumed), and
 * `env_payload_resolve_vaddr` is the page-table resolve the pour needs on
 * both ends. */
struct ProcessDescriptor;
uint32_t env_payload_image_window_list(const struct EnvCkptRecord* rec,
                                       uint64_t* vaddrs,
                                       struct ProcessDescriptor** pds,
                                       uint32_t cap, uint32_t* err);
int env_payload_proc_parked(const struct ProcessDescriptor* pd);
int env_payload_resolve_vaddr(uint64_t cr3, uint64_t vaddr,
                              uint64_t* out_phys);

#endif /* ENV_PAYLOAD_H */
