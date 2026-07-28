# AeroSLS Persistence Write-Amplification Scoping v0.1 — why one `insert` costs 323 NVMe commands, and the cheapest ways to fix it

## 0. Where this came from

This was found while scoping async/multi-page NVMe I/O (`docs/AeroSLS-LLM-Inference-Feasibility-v0.1.md` §3 item A). The question asked was whether that item benefits the codebase generally. Answering it honestly required auditing what the existing I/O call sites actually do — and that audit surfaced a larger, cheaper win sitting beside the driver work: **every persisted region is rewritten in full on every mutation.**

This is a scoping document. Nothing here is built. Every figure is either read directly from source or arithmetic on `sizeof()` values that were independently cross-checked against `kernel/persist.h`'s own layout table.

## 1. Measured cost

`kernel/persist.c` exposes 14 snapshot functions. Each writes a magic header frame, then one or more whole arrays via `persist_write_array()`, which walks forward one 4 KiB frame at a time:

```c
static void persist_write_array(const void* src, uint32_t total_bytes, uint64_t lba) {
    if (!persist_nvme_available()) return;
    const uint8_t* p = (const uint8_t*)src;
    uint32_t rem = total_bytes;
    while (rem > 0) {
        uint32_t chunk = rem < 4096u ? rem : 4096u;
        p_memset(p_buf, 0, 4096);
        p_memcpy(p_buf, p, chunk);
        nvme_write_sync(lba, p_buf);      /* one synchronous command per frame */
        p += chunk; rem -= chunk; lba += 8;
    }
}
```

Note that each iteration also does a full 4 KiB `memset` plus a `memcpy` through the single shared `p_buf` staging buffer, so the CPU cost scales with the same factor as the I/O cost.

Cost per call, computed from each function's regions against `persist.h`'s frame counts:

| Function | Frames (= 4 KiB commands) per call | Bytes written |
| --- | --- | --- |
| `persist_records()` | **323** | 1.26 MiB |
| `persist_databases()` | 89 | 356 KiB |
| `persist_schemas()` | 74 | 296 KiB |
| `persist_programs()` | 66 | 264 KiB |
| `persist_rowstore_headers()` | 40 | 160 KiB |
| `persist_row_journal()` | 35 | 140 KiB |
| `persist_partitions()` | 17 | 68 KiB |
| `persist_row_constraints()` | 12 | 48 KiB |
| `persist_catalog()` | 6 | 24 KiB |
| `persist_tenants()` | 5 | 20 KiB |
| `persist_views()` / `persist_vecstore_headers()` | 4 | 16 KiB |
| `persist_vec_index_defs()` / `persist_row_index_defs()` | 2 | 8 KiB |

A full `persist_restore_all()`-equivalent write of every region is **679 frames**.

### 1.1 The hot paths

Three of these are called per-mutation, not per-administrative-action:

**Legacy key-value `insert`/`update`/`delete`** → `persist_records()`, unconditionally, at `kernel/object_catalog.c:783` (insert), `:700` (update), `:845` (delete). Each is the last statement before `return 0`.

**SQL row insert/delete into a row-set table** → `persist_rowstore_headers()` at `kernel/rowstore.c:664` (`rowstore_row_insert()`) and `:747` (`rowstore_row_delete()`) = 40 frames each.

**Journaled row mutations** → `persist_row_journal()` from the shared entry point all three of `row_journal_notify_insert/update/delete()` funnel through (`kernel/row_journal.c:119`), plus again on `row_journal_commit_tx()` (`:149`, `:161`) = 35 frames each.

So a single SQL row insert into a journaled table costs **75 synchronous NVMe commands** (40 + 35), and a transaction of *N* such inserts costs roughly `N × 75 + 35` — all serialized, since every command is submit-then-spin at queue depth 1.

### 1.2 Amplification

`sizeof(struct SLSRecordField)` is 321 B (`key[64]` + `value[256]` + `active`). `sizeof(struct SLSObjectRecord)` is 10,284 B, and `object_records[128]` is 1,316,352 B = 322 frames — which matches `persist.h`'s stated 322 exactly, confirming the arithmetic.

Changing **one field value** — 321 bytes of logical state — therefore writes **1.26 MiB across 323 commands**. That is a write amplification of roughly **4,100×** by bytes, and 323× by operations against the theoretical minimum of one data frame plus one header.

### 1.3 A stale comment that hid this

`persist_records()` carries this note (`persist.c:147`):

```c
// Note: writes ~232 KiB (57 NVMe frames) — acceptable for a research kernel.
```

The real figure is 1.26 MiB / 322 frames — **5.6× larger than the comment claims.** The comment describes an earlier, smaller `SLSRecordField` layout and was never updated when the field widened. The "acceptable for a research kernel" judgement was therefore made against a number that no longer holds, and `persist.h`'s own layout table (which *is* accurate, and was verified during the capacity-sizing pass) contradicts it. Worth fixing regardless of whether anything else here is done, since it is actively misleading anyone who goes looking for this problem.

## 2. Options

### Option A — Defer and coalesce (cheapest, smallest change, recommended first)

> **Correction, and Option A as built — see §6.** The rationale below contained a factual error about transaction semantics, and the mechanism actually implemented differs from what this section proposed. §6 is authoritative for both; this section is retained unedited for provenance.

Do not persist on every mutation. Set a per-region "needs flush" flag and let an existing periodic hook do the write.

Two things make this unusually cheap here. First, `flush_daemon_tick()` already runs on core 1 via `microkernel_service_poll()` and already exists to do exactly this class of work — deferred dirty-region flushing:

```c
void flush_daemon_tick(void) {
    for (size_t i = 0; i < total_active_sls_objects; i++) {
        struct SLSObject obj = global_sls_object_table[i];
        flush_dirty_sls_region(obj.start_virtual_address, obj.size_in_bytes);
    }
}
```

Second, **persisting inside an open transaction is arguably already wrong.** The point of a transaction is that its effects become durable atomically at commit; writing 1.26 MiB per statement mid-transaction contradicts that, and a `rollback` currently leaves those writes on disk with no undo (they are corrected only because the in-memory array is restored and the *next* persist rewrites everything). Moving the records/journal flush to commit-time is a correctness improvement as much as a performance one.

Win: a transaction of *N* statements goes from *N* whole-array writes to one. A burst of unrelated single inserts coalesces to one write per tick. No change to the on-disk format, no new failure mode in the restore path.

Cost: widens the window between "operation acknowledged" and "operation durable." That is a real semantic change and must be stated explicitly, not slipped in — an ack no longer implies durability until the next flush. Given that this kernel's HTTP layer already acknowledges before any `fsync`-equivalent barrier exists at all, and that NVMe write completion here is not a durability guarantee either (no flush command is ever issued — grep confirms no `NVME_NVM_FLUSH` anywhere), the practical honesty change is smaller than it first sounds, but the doc-level promise must be updated.

Risk: **low.** No format change, no new bookkeeping to get wrong, and it is straightforwardly testable (mutate, assert nothing written yet, tick, assert written).

### Option B — Dirty-frame tracking (biggest win, real risk, do second)

> **Option B as built differs from this proposal in its central mechanism — see §8.** The risk this section identifies is real and was the deciding factor; the implementation avoids it by construction rather than mitigating it. Retained unedited for provenance.

Track which 4 KiB frames of each region actually changed, and write only those.

Mechanism: a per-region dirty bitmap (322 bits = 41 bytes for records; negligible for every region). Mutation sites mark the byte range they touched via a helper that converts offset/length to frame indices — the call sites already have exactly the information needed, e.g. at `object_catalog.c:783` both `rec` and the field index `i` are in scope, so the range is `(uint8_t*)&rec->fields[i] - (uint8_t*)object_records`, length `sizeof(rec->fields[i])`. `persist_*()` then writes header + dirty frames only, and clears.

Win: one field update touches 1–2 frames instead of 322. Roughly **160× fewer bytes and commands** than today, and it composes with Option A (defer *and* write only what changed).

Risk: **this is the one to be careful about.** A *missed* dirty mark means a change that lives in RAM, is never written, and silently vanishes on reboot — a data-loss class that is invisible until someone notices absent data, and easy to reintroduce later when a new mutation site is added and the mark is forgotten. Today's brute-force whole-array rewrite is slow precisely because it is unconditionally correct; giving that up buys speed with a new obligation.

Required mitigations, not optional:
- A verify mode (compile-time or runtime flag) that on each persist also reads the full region back and compares it against memory, failing loudly on divergence. This turns a silent-loss bug into a test failure.
- Host tests that drive *every* public mutation API and then assert the on-disk image byte-matches memory. The existing `tests/persist_*_host_test.c` files already establish the fake-NVMe pattern needed for this.
- A "mark everything dirty" escape hatch used on first boot and after any format-version mismatch.

### Option C — Multi-page transfers (the original Item A)

Extend the driver to transfer more than one page per command (PRP lists). 323 commands becomes ~41 at 32 KiB per command.

Win: ~8× fewer commands, and it helps every sequential path in the codebase, not just persistence — the stream flush loop (`stream.c:322`, up to 16,384 frames), `stream_relocate_partition()`'s three-commands-per-page read/write/verify (`stream.c:448`), and the cross-node `stream_migrate_send_partition()` path all benefit identically.

Limitation: it does **not** reduce bytes written. A single field update still writes 1.26 MiB, so SSD wear and memory-bandwidth cost are unchanged. It makes the wrong amount of work faster rather than doing less work.

Risk: **low-moderate**, and confined to the driver. No format or consistency implications.

### Option D — Append-only log instead of snapshots

Replace whole-array snapshots with a redo log, replayed at boot.

Best asymptotics (write only the delta, always), and it would subsume both A and B. But it replaces the crash-consistency contract wholesale, needs log compaction, and interacts with the existing magic-header format-version guard in ways that need real design. **Not recommended now** — it is a storage-architecture project, and Options A+B capture most of the available win at a small fraction of the risk.

## 3. A pre-existing hazard worth naming separately

The current write order is header first, then data:

```c
write_hdr(PERSIST_REC_HDR_LBA, PERSIST_MAGIC_REC, rec_bytes, 0, 0);
persist_write_array(object_records, rec_bytes, PERSIST_REC_ENT_LBA);
```

And the restore side (`persist.c:395–402`) checks only the magic value and the recorded size before reading the array back. There is no checksum, no generation counter, and no torn-write detection anywhere in the persistence layer. A crash partway through the 322-frame data write therefore leaves a *valid* header pointing at a half-old, half-new array, and the next boot accepts it silently.

This matters for this document in two ways. First, it is a **pre-existing** exposure, not something any option above introduces — Option B in particular does not make consistency worse, and by writing fewer frames it narrows the window. Second, it means the persistence layer's real durability guarantee today is weaker than the code's confident tone implies, which is worth knowing before treating "we rewrite everything every time" as the safe baseline. Adding a trailing checksum or a header-after-data write order would be a small, independent improvement, and Option B's verify mode would partly cover it in testing.

Related: no NVMe flush/FUA command is issued anywhere in the codebase, so even a completed `nvme_write_sync()` is not a durability guarantee against power loss — only against process/kernel restart.

## 4. Recommended sequencing

1. **Fix the stale comment** (§1.3). Minutes, zero risk, stops the problem hiding.
2. **Option A (defer/coalesce), starting with the transaction path.** Biggest win per unit of risk, no format change, and it corrects the questionable mid-transaction persist semantics at the same time. Flush at commit for the tx path; flush on `flush_daemon_tick()` for direct mutations.
3. **Option C (multi-page transfers).** Independent of A and B, benefits stream/relocate/migrate paths equally, confined to the driver. This is the item the feasibility doc already recommended; it keeps its value here.
4. **Option B (dirty-frame tracking), only with the verify mode and the API-coverage tests built alongside it.** Largest win, but it trades unconditional correctness for speed and should not land without the safety net.
5. **Option D:** not scheduled.

Steps 1–3 involve no new correctness obligations. Step 4 does, and should be treated accordingly.

## 5. Verification ceiling

Everything in §1 is read from source or is arithmetic on `sizeof()` values; the 322-frame figure for `object_records[128]` was derived independently and matches `persist.h`'s own table, which is the cross-check that validates the method. Call-site line numbers were verified individually.

**No measurements were taken.** No wall-clock timing, no tokens-per-second, no before/after benchmark — this environment cannot build or boot the kernel (no cross-compiler, `nasm`, or QEMU; the same ceiling disclosed in the Multi-Node Partition Scaling Roadmap's Phase 7 addendum). Command counts and byte counts are exact; **any claim about how much wall-clock time this actually costs would be a guess**, because per-command NVMe latency on the target hardware is unknown and the synchronous submit-and-spin path's real cost depends on it. The case for acting on this rests on the operation counts and the amplification ratio, which are hard numbers, not on an estimated speedup.

Effort estimates in §2 are informed judgement, not measurement.

---

## 6. Findings addendum: Option A as built — a batching bracket, and a correction

Option A is implemented. What landed differs from what §2 proposed, in one important way and for a reason that only surfaced during implementation.

### 6.1 A factual error in §2's rationale, corrected

§2's Option A argued that *"persisting inside an open transaction is arguably already wrong… a `rollback` currently leaves those writes on disk with no undo."* **That is wrong on both counts**, and reading `kernel/transaction.c` and `kernel/object_catalog.c` properly is what showed it.

This WAL is **stage-then-apply**, not apply-then-undo. `sys_sls_update()` checks `tx_get_active(tid)` first (`object_catalog.c:650`); if a transaction is open it calls `wal_stage()`, journals a before-image, and **returns without touching `object_records[]` and without calling `persist_records()`** (`:665–675`). So mutations inside a transaction never persist anything. `sys_sls_tx_rollback()` marks the staged entries `WAL_STATE_ABORTED` and returns — there is nothing on disk to undo, because nothing was written. The existing behaviour was correct; the criticism was unfounded.

The real finding is different, and better for the performance case. `sys_sls_tx_commit()` deliberately clears `ctx->active = 0` *before* its apply loop (`transaction.c:104`, with a comment explaining that it prevents re-staging). Each `sys_sls_update()` call in that loop therefore takes the **direct-write** path — and ends in `persist_records()`. So an N-operation commit issued **N × 323 commands (N × 1.26 MiB)** to reach a final array state that one write produces identically. The waste is real and larger than §2 estimated, but it lives in the commit path's write count, not in transaction correctness.

### 6.2 What was built

A depth-counted batching bracket in `kernel/persist.c`, declared in `persist.h`:

```c
void     persist_defer_begin(void);
void     persist_defer_end(void);
int      persist_defer_active(void);
uint32_t persist_defer_pending_mask(void);
```

Between `begin` and the matching `end`, each of the 14 `persist_*()` functions records a bit in a pending mask and returns; `persist_defer_end()` then issues exactly one real write per distinct region touched. Every `persist_*()` gained a single guard line (`if (persist_defer_note(PERSIST_PEND_X)) return;`) as its first statement — no other logic changed.

**Why this is safe, stated precisely:** every `persist_*()` writes the *current whole contents* of its array, never a delta. Writing after each of N steps and writing once after the last step therefore produce identical bytes on disk. This is not an argument in the doc — Scenario 3 of the new host test asserts it directly, replaying an identical mutation sequence batched and un-batched and requiring byte-for-byte equal disk contents.

`sys_sls_tx_commit()` wraps its apply loop in the bracket, closing it *after* `journal_commit_tx()` and the MQT-refresh loop so any region those touch is folded into the same flush rather than escaping it.

### 6.3 What was deliberately NOT built, and why

§2 proposed hooking deferred flushing into `flush_daemon_tick()`. **That would have been a bug.** `flush_daemon_tick()` runs on Core 1 (`kernel/smp.c`'s `ap_kernel_main()`), while every `persist_*()` caller today runs on the BSP — `kernel/kernel.c:344–349` runs *both* `http_server_run()` and `sls_shell_loop()` there, despite a stale comment in that block claiming the HTTP server is on Core 1. `persist.c`'s `p_buf` is a single shared static DMA staging buffer with no lock, so moving persist writes onto the AP would race the BSP's and tear the buffer mid-frame.

Cross-core deferred flushing needs locking this kernel does not have. The bracket captures the large win (N→1 per transaction) without touching the concurrency model at all. Direct, non-transactional mutations still persist immediately — their durability semantics are **unchanged**, which also means the §2 concern about widening the ack-to-durable window does not apply to what shipped.

### 6.4 Verification

New `tests/persist_defer_host_test.c` — 24 checks, 9 scenarios, all real execution against the unmodified `kernel/persist.c`:

- **The un-batched baseline is measured, not assumed:** one `persist_records()` issues exactly **323** writes, asserted both against `ceil(sizeof/4096) + 1` and against the literal 323 in `persist.h`'s layout table — so a future struct change that moves this cost fails the test instead of silently invalidating this document.
- N→1 collapse (5 calls → 1 region write; 1615 → 323 commands).
- **Byte-identical disk contents** between batched and un-batched runs of the same mutation sequence.
- Multiple distinct regions each flush exactly once.
- Nesting: an inner `end` does not flush early.
- An unbalanced `end` is ignored rather than underflowing the depth counter — which would have wrapped to ~4 billion and silently swallowed every subsequent write, so this is asserted explicitly.
- Pending-mask introspection, and an empty bracket as a true no-op.
- A 20-operation transaction-shaped loop: **323 writes instead of 6,460 — 24.0 MiB saved per commit.**

`kernel/persist.c` and `kernel/transaction.c` compile clean (`gcc -fsyntax-only`, zero errors) both with `-I` flags and with none, matching the real Makefile's `X86_CFLAGS` shape; `transaction.c`'s two `kernel_serial_print*` implicit-declaration warnings are pre-existing (lines 67/86, unrelated to the edits at 5/107/160) and unchanged. Full regression: **61/61 host tests passing** (60 pre-existing + this phase's new one), zero regressions.

**Not verified:** `sys_sls_tx_commit()`'s own use of the bracket is compile-checked and reviewed, not executed — no host test links `kernel/transaction.c` (its `object_catalog.c` + `journal.c` + `lock_mgr.c` + `mqt.c` graph is most of the DB engine), and none did before this change either. The bracket mechanism it depends on *is* fully tested. Named honestly rather than folded into the pass count. As always, no wall-clock measurement was taken; the claims here are command counts and byte counts, which are exact.

### 6.5 Status

Option A closed. Options B (dirty-frame tracking), C (multi-page transfers), and D (append-only log) remain open and unchanged in priority — §4's sequencing still holds, with step 2 now done and step 3 (multi-page transfers) next. §3's pre-existing torn-write and no-flush-command hazards are untouched by this work and remain open.

---

## 7. Findings addendum: Option C as built — multi-page transfers

Option C is implemented, following §4's sequencing directly after Option A.

### 7.1 What was built

**Driver (`drivers/nvme_io.c`/`.h`).** Every transfer in this driver was previously exactly one 4 KiB page: both `nvme_read_sync()` and `nvme_write_sync()` set `prp1` alone with `NLB = 7`, and a PRP1-only command structurally cannot span more than one page. Added:

- `nvme_build_prp(buf_phys, page_count, prp_list_page, out_prp1, out_prp2)` — the PRP address arithmetic, deliberately factored out as a **pure function** (see §7.3).
- `nvme_read_pages_sync()` / `nvme_write_pages_sync()` — one command for up to `NVME_MAX_PAGES_PER_XFER` (32 pages / 128 KiB).
- A shared PRP list page allocated once in `nvme_io_init()`, reused per transfer. Safe only because this driver is strictly synchronous with one command outstanding; the code says so, and says what must change if queue depth ever exceeds 1.

The 32-page cap is deliberately conservative: the NVMe Maximum Data Transfer Size is reported in Identify Controller, which this driver never issues (`nvme_admin.c` sends no Identify command). 128 KiB is below the smallest MDTS in practical use and far under a single PRP list page's 512-entry limit, so it is safe without that query. Raising it should be gated on actually reading MDTS rather than guessed. Allocation failure of the list page is non-fatal — the multi-page functions fail cleanly and callers keep working via the single-page path.

**Caller (`kernel/persist.c`).** `persist_write_array()`/`persist_read_array()` now move full pages in batches through a page-aligned 128 KiB staging buffer, leaving only a trailing partial frame on the single-page zero-padded path (the on-disk tail must be zero-filled, not carry whatever followed the array in memory).

Staging rather than DMA-ing straight from the caller's array is a deliberate choice: a PRP list requires the whole transfer to begin 4 KiB aligned, and none of the persisted arrays (`object_records[]`, `databases[]`, …) carry an alignment attribute. Copying into an aligned scratch buffer satisfies that without touching a single array declaration anywhere in the codebase. The copy is also strictly *less* CPU work than before — one large sequential `memcpy` per batch replaces the per-frame `memset` + `memcpy` pair the old loop did for every single frame.

### 7.2 Measured result

For `persist_records()` (1.26 MiB, 322 data frames + 1 header):

| | Before | After |
| --- | --- | --- |
| Bytes written | 1.26 MiB | 1.26 MiB (unchanged — Option C does not reduce bytes, §2 said so) |
| 4 KiB page-writes | 323 | 323 |
| **NVMe commands** | **323** | **13** (11 batched + 1 tail + 1 header) |

**24.8× fewer round trips for identical bytes.** Composed with Option A, a 20-operation transaction commit goes from 6,460 commands to 13.

### 7.3 Verification, and an honest note on what can't be tested

`drivers/nvme_io.c` cannot be host-tested as a whole — it writes MMIO doorbells and polls a hardware completion queue, and no test in this suite has ever linked it. That is unchanged. What *did* change is that the dangerous part is now isolated and covered.

PRP construction is where a mistake is qualitatively worse than a normal bug: the controller DMAs to whatever addresses the list names, so a wrong entry silently corrupts unrelated memory rather than failing loudly. Factoring it into pure arithmetic made it fully testable. New `tests/nvme_prp_host_test.c` — **31 checks** against the real function (the file `#include`s `nvme_io.c` directly with its few externs stubbed, the same technique `scheduler_fairness_host_test.c` already uses for `kernel/process.c`):

- 1-page (no `prp2`), 2-page (`prp2` is the second page *directly*, not a list — the case most easily got wrong), 3-page (smallest real list), and 32-page (the cap) layouts.
- Every list entry consecutive **and page-aligned** — NVMe requires zero offset on all but the first PRP.
- Exactly `page_count - 1` entries written, nothing past the end (off-by-one overrun).
- All six rejection paths: zero count, over-cap (rejected, not silently truncated), unaligned buffer, half-page offset in the no-list case, list needed but NULL, NULL out-params.
- The multi-page entry points fail cleanly when the I/O queue was never created — the real "NVMe MMIO above the 4 GiB identity map" boot this driver's own comments describe.

`tests/persist_defer_host_test.c` was extended to count **commands separately from pages**, so the batching is verified end-to-end through the real `persist.c`: 323 page-writes leaving as 13 commands, asserted as a >20× reduction. Its existing byte-identity proof still passes unchanged, which is what confirms multi-page batching did not alter the resulting disk image.

**Blast radius.** 27 host tests link `kernel/persist.c` while faking the NVMe primitives, so all 27 needed the two new symbols. Each received a **faithful looping stub** — implemented as a loop over that file's existing single-page fake, not a no-op — so the bytes still land exactly where the real driver would put them and every pre-existing assertion in those files keeps verifying real behaviour *through the new code path* rather than being silently bypassed. This follows the same stub convention documented for the DSPP phase (12 files) and Multi-Node Phase 6 (5 files); no new linkage mechanism was introduced.

Compile-check: `drivers/nvme_io.c` and `kernel/persist.c` both clean with `-I` flags and with none (matching the real Makefile's `X86_CFLAGS` shape), zero errors. `nvme_io.c`'s 5 warnings are all the pre-existing `-Waddress-of-packed-member` idiom this file already used at its `nvme_io_init`/`nvme_read_sync`/`nvme_write_sync` sites; the new function uses the identical pattern. Full regression: **62/62 host tests passing**, zero regressions.

**Still not measured:** no wall-clock timing. The 24.8× is a command-count ratio, which is exact; what that converts to in real time depends on per-command NVMe latency on the target hardware, which remains unknown here. Real-hardware behaviour of the PRP list itself — as opposed to its arithmetic — is unverified for the same reason everything else in this project is: this environment cannot build or boot the kernel.

### 7.4 Status

Options A and C closed. **Option B (dirty-frame tracking) is now the remaining large win** — and note that A and C reduced *commands*, not *bytes*: a single field update still writes the full 1.26 MiB. Only B addresses that, and it is the one carrying a real correctness obligation (§2). Option D remains unscheduled. §3's pre-existing torn-write and missing-flush-command hazards are untouched and still open.

The stream paths named in §2 (`stream_write_chunk()`'s up-to-16,384-frame flush, `stream_relocate_partition()`'s three-commands-per-page loop, `stream_migrate_send_partition()`) would benefit from the same driver capability but were **not** converted here — they pass scattered frame-pool frames rather than one contiguous buffer, so they need a gather path or per-batch staging of their own. Named as remaining work rather than quietly left out.

---

## 8. Findings addendum: Option B as built — shadow comparison, not dirty marks

Option B is implemented, completing §4's sequencing. It departs from §2's proposal in one central respect, and the departure is the point.

### 8.1 The mechanism changed, because §2's own risk analysis was decisive

§2 proposed that each mutation site mark the byte range it touched, and named the risk exactly: *"A **missed** dirty mark means a change that lives in RAM, is never written, and silently vanishes on reboot — a data-loss class that is invisible until someone notices absent data, and easy to reintroduce later when a new mutation site is added and the mark is forgotten."*

That risk was judged not worth accepting when an alternative removes it entirely. **The implementation compares against a shadow copy of the region instead.** Dirtiness is *derived from the bytes*, not asserted by a caller — so there is no mark to forget, and any mutation from any call site, present or future, is detected. It also required **zero call-site changes anywhere in the codebase**.

The trade is one shadow buffer per covered region (1.26 MiB for `object_records[]`) plus a sequential compare pass per write. That compare is memory-bandwidth work measured in microseconds, against NVMe round trips measured in tens of microseconds each — and Option A already made these calls infrequent. Strictly favourable, and far safer.

§2's three "required mitigations" were carried over rather than dropped: the verify mode and the force-full escape hatch are both built (§8.3), and the exhaustive API-coverage test is Scenario 6 (§8.4).

### 8.2 What it does

`persist_write_array_diffed()` walks the region a frame at a time, compares each against the shadow, and writes only differing frames — coalescing **consecutive** dirty frames into single multi-page commands, so it composes with Option C rather than undoing it. The shadow is then updated to mirror what is on disk.

Applied to `object_records[]` only: at 322 frames it is both the largest region and the only one on the per-mutation hot path. The other thirteen regions (1–89 frames) keep the unconditional whole-region write. The helper is region-agnostic, so opting another in is adding a buffer and a flag, not new logic.

### 8.3 The invariant, and what protects it

Correctness rests on one thing: **the shadow must equal what is actually on disk.** That holds today because each shadowed region has exactly one writer and one reader — verified by grep before building (`PERSIST_REC_ENT_LBA` is touched only at `persist.c:298` and `:560`, with no raw-LBA writer anywhere).

Three things defend it:

- **Cold start writes everything.** An invalid shadow means the on-disk image is absent, stale, or from another kernel build, so nothing may be skipped.
- **Post-restore seeding.** After `persist_restore_all()` reads the region back, disk and memory are identical by construction, so the shadow is seeded there — letting the first post-boot write diff properly instead of rewriting all 322 frames. Deliberately *not* done on the size-mismatch/cold-start branches, where a full first write is the correct conservative behaviour.
- **`persist_shadow_invalidate()`** — the escape hatch. `persist.h` states the obligation plainly: any *new* writer to a shadowed region must call it, or real changes will be silently skipped.
- **Verify mode** (`persist_verify_set()`) reads the whole region back after each write and compares against memory, logging loudly and self-repairing with a full rewrite on divergence. Off by default (it costs a full-region read per write); it converts "a needed write was skipped" from silent reboot-surviving data loss into an immediate visible failure.

### 8.4 Measured result

| | Before Option B | After |
| --- | --- | --- |
| One-field change, frames written | 322 | **1** |
| One-field change, bytes written | 1.26 MiB | **4 KiB** |
| Unchanged region re-persisted | 322 frames | **0 frames** (header only) |

**322× less written for a single-field update** — and this is the first of the three options to reduce *bytes* rather than just commands, which is what SSD wear and memory bandwidth actually cost.

### 8.5 Verification

New `tests/persist_shadow_host_test.c` — **25 checks**, all real execution against the unmodified `kernel/persist.c`. Because the risk here is data loss rather than slowness, nearly every scenario ends by asserting **disk == memory**, not merely that the frame count dropped; a version of this feature that is fast but occasionally skips a needed frame would sail through a count-only suite and be worse than not doing the work at all.

- Cold start writes the entire region; an unchanged region writes zero frames; a one-field change writes one.
- **Scenario 6 (exhaustive):** mutate each of the 128 records individually, re-persisting and verifying disk == memory after **every single one** — 128 write-and-verify cycles. This is what would catch a frame-index off-by-one that only bites at particular offsets (records straddling a 4 KiB boundary), which a handful of hand-picked indices would miss.
- **Granularity is per-frame, not per-record**, asserted tightly: 40 consecutive records *span* 100 frames but only dirty 40, because each mutation touches ~44 bytes of a 10,284-byte record. A record-granular implementation would write 100 here and still pass a loose bound.
- Verify mode catches an out-of-band disk corruption and self-repairs. Its counterpart is asserted too: **with verify off, that same corruption is *not* noticed** — the documented limitation, asserted rather than assumed — and `persist_shadow_invalidate()` then repairs it.
- Composes with Option A's bracket: ten mutations inside one bracket flush once, writing only dirty frames, disk matching memory.

`tests/persist_defer_host_test.c` needed updating: its harness wipes the fake disk between scenarios, which is precisely an out-of-band change to a shadowed region — persist.c correctly kept believing the region was clean and skipped writing it. Its `reset_disk()` now calls `persist_shadow_invalidate()` (modelling a fresh boot, and demonstrating the documented obligation), and its deferral-measurement loop invalidates per step so it keeps isolating batching rather than conflating the two features. **This was the feature working as designed catching a stale test assumption, not a bug** — but it is exactly the kind of interaction worth stating rather than quietly fixing.

Compile-check: `kernel/persist.c` clean with `-I` flags and with none, zero errors, zero warnings. Full regression: **63/63 host tests passing**, zero regressions.

**Not verified:** no wall-clock measurement, as throughout. The 322× is a byte/frame ratio, which is exact. Real-hardware behaviour is unverified for the standing reason that this environment cannot build or boot the kernel.

### 8.6 Status

**Options A, B, and C are closed.** §4's sequencing is complete except step 5 (Option D, append-only log), which remains unscheduled and is now clearly unnecessary for the foreseeable term — A+B+C together took a single-field update from 323 commands / 1.26 MiB to 2 commands / 8 KiB.

Still open, unchanged by any of this work:
- §3's pre-existing hazards: header-written-before-data with no checksum or torn-write detection, and no NVMe flush/FUA command anywhere (so a completed write is not a power-loss durability guarantee). **Closed in §9.**
- The stream paths (§7.4) — they pass scattered frame-pool frames and need a gather path of their own.
- The other thirteen persisted regions still write in full; extending shadow comparison to them is now mechanical, and worth doing only if any becomes hot.

---

## 9. Findings addendum: §3's hazards closed — and a live corruption bug they uncovered

§3's two pre-existing hazards are fixed. Doing so also surfaced something considerably more serious than either of them.

### 9.1 The bug this work found: 24 wrong LBA constants, 10 overlapping regions

Adding checksums immediately failed verification on two regions in an existing, previously-passing test. The cause was not the new code.

**`kernel/persist.h`'s LBA `#define`s did not match `kernel/persist.h`'s own documented layout table — 24 of 35 disagreed.** The capacity-sizing pass (Gap Analysis §18) rewrote the layout *comments* — carefully, with `sizeof()`-verified frame counts and a deliberate 1-frame safety gap after every region — but **never updated the actual constants**. The result was ten pairs of overlapping regions, silently corrupting each other on every write:

```
PERSIST_DATABASE_ENT_LBA   [6696,6720)  overlaps  PERSIST_DATABASE_GRANT_LBA [6704,7384)
PERSIST_DATABASE_GRANT_LBA [6704,7384)  overlaps  PERSIST_VIEW_ENT_LBA       [6760,6784)
PERSIST_DATABASE_GRANT_LBA [6704,7384)  overlaps  PERSIST_TENANT_ENT_LBA     [6792,6824)
PERSIST_PART_ASSIGN_LBA    [5808,5904)  overlaps  PERSIST_ROWSTORE_ENT_LBA   [5832,6136)
   ... 6 more
```

Concretely: writing `database_grants[]` overwrote two of the three frames holding `databases[]`. Writing the partition assignment table overwrote the row-store table headers. This is precisely the failure mode §18 was written to prevent, and its own prose describes as *"a silent on-disk data-corruption time bomb the moment any of those constants grew."* The constants grew; the bomb went off; nothing detected it.

**Why no test caught it for so long:** the surviving frame in each overlap happens to be the *first* one, and every existing test only asserts on low-indexed entries — `databases[0]`, `database_grants[2]` — which live in exactly the bytes that survive. The tests passed on luck, not correctness. Data above roughly index 93 in `databases[]` was being destroyed on every write, invisibly.

Fixed by setting all 24 constants to the documented values. Verified with an overlap checker built from the real `#define`s and real `sizeof()`s: **zero overlaps**, all regions inside the intended envelope, clear of `STREAM_DIR_LBA`. That checker is now a permanent host test — see §10.

This is the strongest argument available for having done the checksum work at all: it converted a silent, years-old corruption bug into an immediate, loud test failure the first time it ran.

### 9.2 Torn-write detection

Three changes that only work together:

1. **The header now carries an FNV-1a checksum of its region's data** (header offset 24). Hand-rolled and file-local, matching this codebase's per-file helper convention; non-cryptographic is the right tool for detecting an interrupted write.
2. **The header is written LAST.** A crash before it lands leaves the *previous* header, whose checksum will not match the partially-rewritten data — so the torn state is detected rather than trusted.
3. **Verification happens BEFORE loading.** `persist_read_array()` loads straight into live arrays, so verifying afterwards would already have polluted kernel state. Every region is now scanned up front, streamed off disk and checksummed *without being retained*, and only regions that verify are allowed to load.

This **fails closed**: a mismatch cold-starts that region rather than loading torn data. It is detection, not recovery — recovering a previous good copy would need A/B double-buffering of every region, which the LBA layout has nowhere near enough slack for (78 spare frames against 679 in use). Named as a limit rather than implied.

Backward compatibility: the checksum field is additive, so headers written by an older build have zero there and are accepted exactly as before. Rejecting them would discard every existing snapshot. The honest cost — such images get no torn-write protection until rewritten — is asserted in the test, not just documented.

### 9.3 Durability ordering

`nvme_flush_sync()` (NVM Flush, opcode 0x00) was added to the driver; no flush command existed anywhere in the codebase before, so every "persisted" write was only durable against a process restart, never against power loss.

Region commit now runs **data → flush → header → flush**. The first barrier is what makes the checksum meaningful: without it the controller could commit the header before the data it vouches for, reopening the exact window being closed. The second means the region is genuinely on media when the call returns.

### 9.4 Verification

New `tests/persist_crash_consistency_host_test.c` — **13 checks**, real execution against the unmodified `kernel/persist.c`, with a fake NVMe that records an **operation log** so ordering can be asserted rather than assumed:

- A torn data frame is detected, and — the load-bearing assertion — **the live array is left exactly as the caller had it**, proving verification precedes loading.
- A stale header over newer data is rejected too (the reverse ordering failure).
- Ordering asserted from the op log: data frames written before the header, a flush *between* them, and a second flush after.
- A pre-checksum image still loads; a torn pre-checksum image loads its corrupt bytes **undetected** — the documented limitation, asserted rather than assumed; and rewriting the region restores protection.

`tests/persist_rdbms_vecstore_host_test.c` began failing 4 checks when checksums landed. Investigation showed this was **the new code correctly reporting real corruption**, not a test problem — it passes again now that the LBA constants are fixed, and it is the test that led to §9.1.

One bug was introduced and caught during this work: the region scanner initially shared `p_buf` with the restore blocks, clobbering the header a block had just read and making every region look size-mismatched. It now has its own buffer. The crash-consistency test caught it on its first run.

Blast radius: `nvme_flush_sync()` needed a stub in the 28 host tests that fake the NVMe layer — same faithful-stub convention as §7.3.

Compile-check: `kernel/persist.c`, `drivers/nvme_io.c`, `kernel/transaction.c` all clean with `-I` flags and with none, zero errors. Full regression: **64/64 host tests passing**, zero regressions.

**Not verified:** as throughout, no real-hardware run. Torn writes are simulated by damaging frames underneath persist.c, which is what an interrupted write leaves behind, but an actual power-loss test on real hardware is out of reach here. `nvme_flush_sync()`'s real behaviour against a physical controller's volatile cache is compile-checked and reviewed only.

### 9.5 Status

§3 is closed. Remaining open items:

- The LBA overlap checker is now permanent — **see §10.**
- The stream paths (§7.4) — scattered frame-pool frames, need a gather path.
- Recovery (as opposed to detection) of a torn region would need A/B region double-buffering and a larger LBA envelope.
- The other thirteen regions still write in full (§8.6).

---

## 10. Findings addendum: the layout guard is permanent

`tests/persist_lba_layout_host_test.c` — 6 checks. §9.5 called this the highest-value follow-up in the document; it is done.

### 10.1 Why this shape

Everything is derived from the **real `#define` values and real `sizeof()`s**, never from a transcription. That is the whole point: §9.1 happened precisely because a hand-maintained description (the comment table) and the actual constants drifted apart. A test that re-encoded the expected LBAs by hand would be a third copy to drift. Deriving from the code means changing any governing constant — `PARTITION_MAX`, `DATABASE_GRANT_MAX`, `RECORD_VAL_LEN` — without moving the LBAs to match fails here rather than corrupting a disk.

It links no `.c` file at all: `sizeof()` on an extern array with a known bound is a compile-time constant, so the headers suffice. The test is therefore free of the persistence layer's dependency graph and runs instantly.

### 10.2 What it asserts

- **No two extents overlap**, compared pairwise across every region, **headers included** — the original corruption had a data array landing on both another array and a neighbouring region's header.
- **Envelope**: every extent ends below `STREAM_DIR_LBA` and starts at or above the first region. Overflow past the stream directory would corrupt an unrelated subsystem. (Current margin: 79 frames.)
- **The documented 1-frame safety gap** really exists between regions from `partitions` onward. persist.h *claims* this gap; zero-slack adjacency is what made the original bug possible, so the claim is now enforced rather than trusted.
- **Two named regression guards** for the specific historical failures — `database_grants[]` starting 16 sectors inside `databases[]`, and `partition_assign_table[]` running straight through `table_headers[]` — so a recurrence reads as *that bug* rather than an anonymous overlap.
- On failure it prints the full derived layout, so the output is diagnosable without re-deriving anything by hand.

### 10.3 The test was verified to actually fail

A guard that has never failed is not known to work. The original bug was reintroduced deliberately (`PERSIST_DATABASE_GRANT_LBA` 6760 → 6704) and the test caught it: **4 overlaps reported with exact LBA ranges**, both the general overlap check and the named `database_grants` regression guard failing, exit code non-zero. The constant was then restored and the test passes again.

Full regression: **65/65 host tests passing**, zero regressions.

### 10.4 Residual limitation, named

This guards the *static layout*. It cannot catch a caller passing a wrong LBA at a call site — e.g. `persist_write_array(x, n, PERSIST_VIEW_ENT_LBA)` inside `persist_tenants()`. The checksum work (§9) covers that case from the other direction, since such a write would land outside the region the checksum then verifies. Between the two, both the layout and its use are now watched; neither alone would be sufficient.

---

## 11. Findings addendum: remaining items closed out

The three items left open across §7.4, §8.6 and §9.5. Two were built; one is a deliberate decision not to build, recorded with its trigger.

### 11.1 Stream gather path — built

`stream_write_chunk()`'s `is_last` flush issued one 4 KiB command per frame: up to **STREAM_MAX_FRAMES (16,384)** synchronous round trips for one 64 MiB stream. §7.4 deferred this because a stream's pages are separately allocated frame-pool frames — scattered in memory, so the contiguous multi-page path cannot describe them.

That framing turned out to be too pessimistic. **A PRP list is natively a scatter list**: every entry is an independent page address with no requirement that they be consecutive. So no copying and no staging buffer is needed — `nvme_build_prp_gather()` / `nvme_write_pages_gather_sync()` (`drivers/nvme_io.c`) describe the scattered frames to the controller directly.

Batching walks runs of **consecutive populated frames**, because one NVMe command covers one contiguous *LBA* range. A NULL frame (a hole never written) must therefore end the current run rather than be skipped — writing across a hole would shift every later frame onto the wrong LBA.

Measured: **103 frames flush in 4 commands instead of 103** — a 25.8× reduction, and the ratio improves with stream size up to the 32-page cap.

The flush loop was extracted into `stream_flush_frames()` so the index arithmetic could be tested in isolation. That was not cosmetic: **the first draft had a real bug** — the frame that filled a batch was written with that batch *and* re-seeded into the next run, so it went to disk twice and every later frame landed one LBA early. Batching a loop that writes to computed LBAs is precisely where an off-by-one does not crash; it silently misplaces data that only surfaces on read-back. It was caught by writing the test.

`tests/stream_gather_flush_host_test.c` — 13 checks, all asserting **where bytes land**, not just command counts: contiguous runs, a run of exactly the batch limit, **one past the batch limit** (the named regression guard for that bug), a hole splitting a run with nothing written into the hole, four-batch runs, and the empty case.

### 11.2 Shadow-compare extended to the two hot regions — built

§8.6 left eleven regions writing in full, "worth doing only if any becomes hot." Two already were, per §1.1: `persist_rowstore_headers()` (40 frames, every row insert/delete) and `persist_row_journal()` (35 frames, every journaled mutation). A single SQL row insert into a journaled table paid 75 whole-region frames.

Both now shadow-compare their large array, on the same mechanism as `object_records[]`.

**The other eleven are deliberately left alone**, and that is the substantive half of this item. They are administrative — create-database, create-view, create-tenant, schema-set, program-upload — written a handful of times per boot, not per mutation. Shadowing all of them would cost roughly **1.4 MiB of additional BSS** to optimise operations nobody performs in a loop. The mechanism is region-agnostic, so opting one in later is adding a buffer and a flag; this is a reversible judgement about which regions are hot, not a structural limit.

Their shadows are left invalid after restore rather than seeded (unlike `object_records[]`), so the first write per boot is a full one. Conservative and correct; the cost is one extra full write per region per boot.

### 11.3 A/B torn-region recovery — deliberately not built

§9.2 delivered *detection*: a torn region is caught and cold-started rather than silently loaded. Recovery — actually restoring the previous good copy — needs A/B double-buffering, and this is a decision not to build it.

**The arithmetic.** Double-buffering means a second copy of every region: **~679 additional frames**. The layout currently has **79 frames** of margin before `STREAM_DIR_LBA`. It does not fit, and cannot be made to fit by rearrangement — closing the gap requires moving `STREAM_DIR_LBA` and everything above it, which is a disk-layout redesign with a migration path for existing images, not a tuning change.

**Why the residual risk is proportionate.** Before this work, a torn write produced *silent corruption loaded into live kernel state*. It now produces a *detected, reported, single-region cold start* — the failure mode moved from "wrong data trusted indefinitely" to "known data loss, announced." That is the larger share of the available benefit, and it came at a fraction of the cost. Additionally, `nvme_flush_sync()` narrows the window itself: the data is on media before the header that validates it, so the interval in which a crash can tear a region is now bounded by one flush rather than by an unbounded controller cache.

**The trigger for revisiting.** If a disk-layout redesign happens for any other reason — the 10 GiB image growing, the stream region moving, adding a region that no longer fits — A/B double-buffering should be designed in at that point, when the envelope is being reconsidered anyway. Doing it now would mean forcing that redesign for this feature alone, which is not warranted.

### 11.4 Verification

Blast radius: the three tests linking `kernel/stream.c` that fake the NVMe layer needed gather stubs, again as faithful loops over the single-page fake so their existing assertions keep exercising the new path.

Full regression: **66/66 host tests passing**, zero regressions. `kernel/persist.c`, `kernel/stream.c` and `drivers/nvme_io.c` compile clean with `-I` flags and with none, zero errors.

### 11.5 Status

Every item raised in this document is now closed or has a recorded decision. Cumulative effect on a single-field update: **323 commands / 1.26 MiB → 2 commands / 8 KiB**. A 20-operation transaction commit: **6,460 commands → 13**. A 64 MiB stream flush: **16,384 commands → 512**. Alongside that, one live data-corruption bug found and fixed (§9.1), and two guards added that would have caught it (§9, §10).

Unchanged and still true: none of this has been measured in wall-clock terms or run on real hardware, for the standing reason that this environment cannot build or boot the kernel. Every figure above is an exact operation or byte count.
