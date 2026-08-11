# AeroSLS QEMU-SLS — the guest-paging reset defect, v0.1

**Status: FIXED 2026-08-06 — correctness half. Leak half reduced, not closed.**
Found 2026-08-05. Pre-existing; the Guest Runtime UI exposed it, did not cause
it.

---

## FIX SUMMARY

`qemu_sls_mmu_guest_paging_reset()` restores the guest window's identity
mappings, clears `qemu_sls_guest_paging_on` and the guest CR3.
`sls_launch_guest()` calls it unconditionally — launching a guest is a machine
reset, and on real hardware a reset clears CR0.PG.

**The insight was that nothing was ever destroyed.** `guest_paging_enable()`
zeroes ONE PML4 entry; the PDPT/PD/PT subtree beneath it is orphaned, not
freed, and still describes exactly the identity window a later guest needs. So
the entry is stashed instead of discarded and put back on reset. Eight bytes
turn "the mappings are gone forever" into "the mappings are set aside".

**The launcher no longer refuses.** That refusal was a guard while this was
being written; with a real reset it became an unnecessary restriction. A node
that has run the paging test can now run guests again with no restart.

**Coverage:** 10 new checks in `qemu_sls_mmu_host_test.c` (107 total), asserting
the restored PML4 entry is byte-identical to the stashed one — not that the
flag is clear. *"paging_on is 0"* is precisely the assertion that would have
passed for the entire life of the defect, because the flag was never the broken
thing; the mappings were. Includes ten enable/reset cycles, idempotency, and
reset-without-prior-enable. Mutation tested: removing the stash, removing the
restore, and leaving the flag set each fail 3, 3 and 2 checks.

### What is NOT fixed

**The leak is reduced, not eliminated.** Reset restores the identity subtree,
so the ~130 frames it occupies are now *reused* across every cycle rather than
abandoned — that half is genuinely closed. But whatever `shadow_fault()` built
under the zeroed slot while the guest ran paged is still orphaned at reset,
because this kernel still has no page-table reclaim walk.

Size, stated rather than waved at: `shadow_fault` maps only pages the guest
actually touches, so `qemu paging` orphans a handful of frames per run, not the
65,536-page window. Strictly less than before, and now the smaller of the two
halves instead of the larger.

**So §4's gate is half-met.** The first half — paging then two benches giving
cold-then-warm — should now pass. The second — an unchanged frame-pool count
across ten paging runs — will not, and saying it passes would be false. Closing
it needs the reclaim walk, which is a separate change with its own risk and
deserves its own gate.

---

## 0. Symptom

Successive `qemu bench` runs behave correctly — the first compiles, the second
is served from the translation cache.

**After one `qemu paging`, the next benchmark hangs the node.** The launch does
not return, the kernel's HTTP server stops answering, and every endpoint returns
502 through the proxy.

### Severity was revised upward, and how

First reported as "benchmarks come back cold after the paging test", and that
was written up here as the symptom. It was the *mild presentation*: a node that
had already been restarted underneath by pm2, so each bench was a genuine first
run after boot.

Watching it again with the console open showed the real behaviour — `Failed to
fetch` on the benchmark, 45 console errors, and 502 Bad Gateway on every polled
endpoint (`/api/tiers`, `/api/objects`, `/api/wal`, `/api/metrics`,
`/api/services`, `/api/health`, `/api/security/audit`). A 502 means the proxy
could not reach the kernel at all.

This document previously said the benchmark was "running in the wrong address
space and getting away with it because its access pattern is simple". **That was
wrong.** It does not get away with it. It takes the node down.

---

## 1. Mechanism

Three separate problems, each verified in the tree.

### 1.1 `qemu_sls_guest_paging_on` is a one-way latch

| | |
|---|---|
| Initialised to 0 | `kernel/qemu_sls_mmu.c:24` |
| Set to 1 | `kernel/qemu_sls_mmu.c:277` |
| Cleared back to 0 | **nowhere** |

```bash
grep -n 'qemu_sls_guest_paging_on = 0' kernel/*.c ../qemu/sls/*.c
#   only the initialiser at :24
```

Once a guest enables CR0.PG, this kernel believes a guest has paging on for the
remainder of the boot — including for every *later, unrelated* guest.

### 1.2 The identity mappings are dropped and never restored

`qemu_sls_mmu_guest_paging_enable()` zeroes the PML4 entry covering the whole
512 GiB guest window. That is correct for the guest enabling paging: identity
mappings were only valid while guest virtual == guest physical, and leaving
them would let an access silently resolve through them instead of faulting.

But nothing puts them back. `qemu bench` is a **paging-off** guest that expects
GVA == GPA. After a paging test it executes in an address space built for a
different guest, with `shadow_fault()` walking that guest's stale page tables.

**So the cold cache is a symptom, not the defect.** The benchmark is not merely
recompiling — it is running in the wrong address space and getting away with it
because its access pattern is simple enough. A more demanding guest would fail
in less obvious ways.

### 1.3 The leak's "bounded" argument no longer holds

`qemu_sls_mmu.c` says of the dropped PDPT/PD/PT subtree:

> The PDPT/PD/PT frames below the entry are LEAKED, not freed. There is no
> page-table reclaim path in this kernel yet … **Bounded and one-off: it happens
> at most once per guest launch**, and costs the ~130 frames the identity window
> used. Recorded rather than hidden.

That reasoning was sound when `qemu paging` was a console command a developer
ran occasionally. It is not sound now that it is a button in the Guest Runtime
screen: each click leaks ~130 frames, with no reclaim path, for as long as
someone keeps clicking.

**The comment is not wrong — its premise changed underneath it.** Worth noting
because the premise was never written down as a dependency, so nothing flagged
it when the button was added. That is the same shape as every stale figure this
project found in August: a correct statement whose world moved.

---

## 2. Why the obvious fix does not work

Re-running `qemu_sls_mmu_map_guest_ram()` to reinstate identity mappings is
refused by design:

> `map_guest_ram REFUSED: GPA … is already backed by frame …. Re-mapping it
> would orphan that frame and lose whatever the guest had there.`

`guest_ram_frames[]` still records every frame, so the range is "already
backed". The refusal is correct — silently re-mapping would orphan frames and
lose guest contents. But it means **there is no path that re-installs identity
mappings for frames already held**, which is exactly what a reset needs.

---

## 3. Guards in place

**After running guest paging, the node must be restarted before any further
guest run.**

Three guards, none of them the fix:

**`sls_launch_guest()` refuses when `qemu_sls_guest_paging_on` is set.** This is
the important one — it converts an unrecoverable node hang into a returned error
and a message naming the cause. It costs one comparison and covers every caller:
the shell command, the HTTP route, and the bench. The refusal is placed *after*
the `sls_last_*` counter resets, so a refused launch reports zeros rather than
the previous successful run's figures beside `ok:false`.

**`sls_test_guest_paging()` prints what it did** on its PASS path, since the
console had no way to know the node was now unusable.

**The Guest Runtime screen disables both buttons** after a paging run and
explains the state, so it cannot walk someone into the refusal.

All three are guards. The shell command and HTTP route still enable paging and
still leave the node needing a restart; they just no longer take it down
silently.

---

## 4. Shape of the real fix

A guest-reset path, run when a new guest is launched — launching a guest is a
machine reset, and on real hardware a reset clears CR0.PG.

It needs three things, and the third is the one with teeth:

1. Clear `qemu_sls_guest_paging_on` and the guest CR3.
2. Re-install identity mappings for the frames `guest_ram_frames[]` already
   holds — a new path, since `map_guest_ram()` correctly refuses this.
3. Decide what happens to the leaked subtree. Either reclaim it, which means
   writing the page-table reclaim path this kernel has never had, or reuse the
   existing subtree instead of dropping and rebuilding it. The second is
   probably smaller and is worth costing before assuming the first.

**Suggested gate:** `qemu paging` followed by `qemu bench` twice, on one boot,
producing a cold run and then a warm one — plus a frame-pool count that is
unchanged across ten paging runs. The second half matters as much as the first;
without it a fix can look right and still leak.

---

## 5. Why this is filed rather than fixed

It is a genuine MMU change touching frame accounting, in a kernel where the
last address-space bug (`.bootstrap_stack` orphaned above `_kernel_image_end`)
surfaced as `rip=0xcdcdcdcdcdcdcdcd` several layers from its cause. It wants
its own session, its own gate, and a rested reader — not the tail end of a long
one.
