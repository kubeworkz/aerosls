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
