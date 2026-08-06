# AeroSLS QEMU-SLS — the guest-paging reset defect, v0.1

**Status: OPEN. Found 2026-08-05. Pre-existing; the Guest Runtime UI exposed it,
did not cause it.**

---

## 0. Symptom, as reported

Successive `qemu bench` runs behave correctly — the first compiles, the second
is served from the translation cache. **After one `qemu paging`, every
subsequent benchmark comes back cold**, indefinitely, for the rest of the boot.

That is the visible part. It is not the important part.

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

## 3. Mitigation until it is fixed

**After running guest paging, restart the node before trusting any subsequent
guest run.** Nothing enforces this today and nothing says it on the console.

The Guest Runtime screen now disables its paging button after one run per page
load and says why, so the UI cannot walk a user into the broken state
repeatedly. That is a guard, not a fix: the shell command and the HTTP route
are both still reachable and still leave the node in this state.

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
