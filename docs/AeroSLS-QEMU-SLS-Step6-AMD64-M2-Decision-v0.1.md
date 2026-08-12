# AeroSLS QEMU-SLS — M2 Decision Record: the paging-on address-space model, v0.1

*Decision record for milestone M2 of
`AeroSLS-QEMU-SLS-Step6-AMD64-Frontend-Plan-v0.1.md` (Gap B). Starting point, as
mandated: the shared-table defect documented in
`AeroSLS-QEMU-SLS-Guest-Address-Space-Design-v0.1.md` §1. Question: how does a
paging-on AMD64 guest's address space map onto the host, for both instruction fetch
and data access?*

**Status: DECIDED — the two-window `guest_base` model, which is already the shipped
design and already validated on hardware for the data path. This record consolidates
that decision against the three options, states what M2 still must build (the fetch
path and the invalidation policy), and marks two stale "not yet built" guards in the
code as history.** Verified against the tree 2026-08-11.

**Update 2026-08-11: the §3.1 fetch path is LANDED** — `sls/sls-i386-codefetch.c`,
the launcher's C dispatcher, and the hand-rolled frontend now resolve paging-on guest
fetches through the guest window via the single `sls_guest_addr_to_host()` rule
(sls-osdep.h); the paging-on refusal is retired; the `qemu paging` fixture's guest now
identity-maps its own code page as a real boot loader would (fetch honoring the
guest's tables made the old omission a fault). The §5 gates have **not** been run:
no toolchain or hardware in the working environment. Build + `qemu paging` from the
main tree remain the verification.

**Gate results 2026-08-11 (WSL x86_64 toolchain, QEMU boot): PASS.**
`make x86-iso SLS_X86_FRONTEND=on` (QEMU's decoder) and the default 18-opcode
frontend build both; the `qemu paging` fixture passes on **both** builds
(`pass:true`, EAX=0x5a5ac0de, 7/7 insns, paging_on=1, CR3=0x1000) and the
paging-off bench runs `ok:true` (256 loads, softmmu off). One real defect was
found and fixed during verification: the fixture's identity mapping wrote
`PT[0]=PTE(0)` into the SAME PT page the data mapping uses (GVA 0 and
TEST_GVA 0x400000 share PT index 0), so the load resolved to GPA 0 instead of
GPA 0x5000 — `EAX=0x001000bb`, the program's own first bytes. Fixed by giving
the identity mapping its own PT page (`PT2_GPA`, `sls-launcher.c`), the same
choice a real boot loader makes.

**Update 2026-08-11: the §3.2 invalidation policy is LANDED too** — CR3 reload
and `INVLPG` drop the populated shadow subtree / single PTE (kernel API +
C-dispatcher wiring), and guest stores to the guest's own page-table pages
fault and drop the subtree (table-page marks + install-time read-only mapping).
One prerequisite defect was found and fixed during verification: CR0.WP was
clear, so the write-protect traps never fired on real hardware — the launcher
now sets it around guest execution. Verified on both builds by the new `qemu
invl` fixture (res = A B C D), with `qemu paging` and the bench still passing;
see §3.2 for the details.

**Update 2026-08-11: the M3 wiring point is LANDED** — the decoder build's
`flush_page` helper (`helper_flush_page`, the call site
`gen_helper_flush_page()` emits for INVLPG/INVLPGA) no longer halts; it calls
`qemu_sls_mmu_shadow_invlpg()`, so a TCG-translated INVLPG invalidates the
guest-window shadow PTE. The C dispatcher's own INVLPG decode is routed
through the same helper under `SLS_X86_FRONTEND=on`, making the hook live and
tested on hardware before the decoder is wired into execution (one chokepoint
for INVLPG in the decoder build). Also fixed: the Makefile's `QEMU_INC` was
missing `-I ../qemu/target/i386`, so the `=on` build only ever compiled with a
manual include override — it now builds from the committed tree. Verified on
both builds: `qemu invl`/`paging`/`selfmod` all `pass:true`; the decoder
build's serial log shows `helper_flush_page(0x400000): decoder INVLPG hook ->
shadow invalidation` followed by the PTE drop. What remains before the decoder
executes guest code is the Step 6.3 translator-loop wiring itself — `MOV CR3`
under the real decoder (`helper_write_crN`) is the next stub that must become
real.

**Update 2026-08-12: the Step 6.3 translator-loop wiring is LANDED** — the
decoder executes guest code end to end, replacing the 18-opcode frontend. The
launcher's exec loop calls `x86_translate_code` through `translator_loop()`
(one file from `accel/tcg`, as the plan's 6.3 decision requires) under
`SLS_X86_FRONTEND=on`; `SLSCPUState` becomes the real `CPUX86State`, with boot
state (hflags, CRs, segment bases, CPUID feature bits incl. long mode) set so
the decoder sees a 64-bit long-mode-capable machine. The first helpers the
fixtures demand are real: `helper_write_crN` (CR0/CR3/CR4 with the same
shadow-MMU semantics the C dispatcher owned) and `helper_hlt` (halt + return
to the launcher loop). Build side: the `=on` build defines `TARGET_X86_64` +
`CONFIG_SYSTEM_ONLY` (the decoder was previously compiled 32-bit-only), a
`cpu-mmu-index.h` shim lets `translator_loop` resolve `mmu_index`, and the
`translator.x86.o` rule gained the `$(SLS_STAMP)` dependency it was missing.

One real defect was found and fixed during verification — and it is worth
recording precisely because every surface around it was correct. The launcher
stubbed glib's `g_once_init_enter` as a **global one-shot flag**: TCG lazily
computes each `TCGHelperInfo`'s call layout on first use, keyed off that
info's own `init` field, so exactly ONE helper (the first call ever emitted)
was initialized and every later helper kept `nr_in == 0`. The generated code
for the decoder's `INVLPG` therefore called `helper_flush_page` with **zero
argument setup** — the helper received the prologue's leftover registers: `env`
in rdi and the TB pointer in rsi (`0x7abb070`, page-masked to `0x7abb000`),
not the guest's `EBX=0x400000`. The fix restores per-address once semantics
(the guest is single-threaded, so a plain check-and-set suffices).

**Gate results 2026-08-12 (WSL x86_64 toolchain, QEMU boot): PASS.** On the
`SLS_X86_FRONTEND=on` build: `qemu invl` (the §3.2 gate), `qemu paging`, and
`qemu selfmod` all `pass:true` — the serial log shows
`helper_flush_page(0x400000): decoder INVLPG hook -> shadow invalidation`
followed by the PTE drop, and the invl fixture's four stages read the values
its tables currently say. The paging-off bench runs (`ok:true`, 64 loads, 2
blocks, 907 code bytes). The default 18-opcode frontend still builds clean.

---

## 0. The three options, as posed

| Option | Mechanism | The defect's framing |
|---|---|---|
| **(a) COW-cloned shadow tables** | Before installing a guest mapping, clone the PML4 entry's subtree so the shadow stops sharing with the kernel (`user_map_page`/`get_or_alloc` follows present entries instead of cloning — `arch/x86/user_paging.c:110`) | The defect doc's §2(a) — "standard shadow-paging technique" |
| **(b) `guest_base` window** | Guest accesses are `guest_va + base`, resolved by the host MMU through a window PML4 slot nothing else uses | §2(b) — "this is the model already shipped, described differently" |
| **(c) World-switch address space** | Shadow root maps only guest memory + a minimal trampoline (IDT, handler, stack); entry/exit switches CR3 | §2(c) — "what a real hypervisor does. The largest option by a wide margin" |

---

## 1. The defect, restated precisely (the starting point)

`qemu_sls_mmu_init()` copies all 512 kernel PML4 entries **by value**
(`kernel/qemu_sls_mmu.c:90-94`), so the shadow shares every lower-level table with the
kernel. `user_map_page()` follows a present entry rather than cloning it. Therefore
installing a shadow PTE at an address the kernel also maps does not *shadow* the
kernel's translation — it **overwrites the kernel's live page tables**, permanently,
whether or not the shadow root is loaded.

The defect's §1 proves the danger is real, not hypothetical: with a bare-GVA mapping,
a paging-off guest occupies linear 0–256 MiB and the kernel image occupies 1–221 MiB —
overlapping regions. The exact target of the AMD64 plan (a statically-linked non-PIE
Linux binary) loads at **0x400000**, squarely inside the kernel's own image range.

**The one constraint the defect makes load-bearing:** shadow PTEs may only ever be
installed where table sharing is harmless — a PML4 slot nothing else uses. Everything
below is judged against that constraint first.

---

## 2. Analysis

### Why (a) COW alone does not answer the question

The defect doc's §2(a) describes COW as making bare-GVA *safe*: clone the subtree,
then install guest mappings into the private copy without touching the kernel's
tables. That fixes the **corruption** — the kernel's tables survive. It does not fix
the **visibility** problem, and the visibility problem is the one that matters:

While the guest runs, the shadow root is the live CR3 (`sls-launcher.c` switches to
`qemu_sls_shadow_cr3`). If the shadow maps GVA 0x400000 → the guest's frame, then
*the kernel's own code and data at 1–221 MiB are not visible through the shadow at any
GVA the guest has claimed*. The kernel image occupies the same linear range the guest
ELF needs. A COW-cloned shadow does not change that — it just makes the overwrite
safe. The kernel still cannot execute out of its own image while guest mappings occupy
it, which is exactly the state the launcher is in on every guest exit (helper call,
syscall, HLT).

COW is therefore a *component*, not an answer. To make bare-GVA coherent you must
additionally either (i) world-switch to a kernel root on every exit — option (c) — or
(ii) rebase the kernel to the high-half canonical range, a kernel-wide change with no
other motivation in this project. The defect doc's own §3 measured what bare-GVA would
buy: TCG_REG_R12 back in the allocatable pool and **a byte or two of encoding per
access** — the window form already compiles a guest load to one instruction (86 → 16
bytes/load, measured 2026-08-04).

**Finding: (a) exists to serve bare-GVA, and bare-GVA is not worth its price. (a) is
rejected as a primary design; it is not even a prerequisite for the chosen option (b),
because the window slot's subtree is never shared.**

### Why (c) is rejected for M2

The world switch is the correct tool for the case this project explicitly retired:
guest code running *natively* on the host with the hardware walking the guest's own
tables — same-ISA virtualization, which KVM owns and the Repositioning plan removed
from the goal. In cross-ISA emulation, TCG-generated **host** code executes, and it
may address guest memory however the host chooses. There is no architectural need for
address-space coincidence; there is a spare register and a displacement, and an offset
costs nothing. This is also why qemu-user, FEX-Emu and Box64 all map the guest at an
offset — the ecosystem's answer, not an accident.

(c) also carries real cost beyond the trampoline: every guest exit (and this design
exits constantly — the hybrid C dispatcher, helper calls, faults) becomes a CR3
switch, a TLB flush, and a hand-written assembly entry/exit path with its own
correctness surface, in the most safety-critical code in the kernel. For M2 — *first
correct 64-bit guest, not first fast one* — that is the wrong currency.

**Finding: (c) is the largest option, serves a retired goal, and is rejected for M2.
It stays on record as the upgrade path if a future phase ever wants bare-GVA (or if
the R12 index register becomes the measured bottleneck — it is not today).**

### Why (b) is the decision — and the one refinement the defect forced

The window model is already the shipped design (Step 5) and its paging-on extension is
already implemented. The defect's analysis produced one refinement that is the actual
content of the decision: **the paging-on guest needs TWO windows, not one.**

With a single shared window, enabling guest paging is unimplementable: guest code at
GVA *V* needs `V + base` to mean frame(P) (the guest's own translation V → P), while
the emulator needs `P + base` to mean frame(P) (to walk those very page tables). When
V ≠ P, one of them silently reads the wrong frame — no fault, no log line, exactly the
failure shape the project has paid for repeatedly. The fix is two PML4 slots:

| Window | Address | Present in | Purpose |
|---|---|---|---|
| **Emulator window** | `QEMU_GPA_HOST_BASE` (32 TiB, slot 64) | Kernel root **and** shadow root | Identity `GPA + base → frame(GPA)`, never changes. How the emulator reads guest page tables (`gpa_to_hva`), loads images, does DMA — **including from inside `shadow_fault()`, which runs with the shadow root loaded, so this window has to be in both roots.** |
| **Guest window** | `QEMU_GUEST_WINDOW_BASE` (64 TiB, slot 128) | Shadow root **only** | `guest_base` for emitted code: guest access at V compiles to a host access at `V + guest_base` (`sls-launcher.c:271` sets `guest_base = QEMU_GUEST_WINDOW_BASE`). Identity while paging is off; populated from the guest's tables by `shadow_fault()` once it is on. |

The two windows are the reason the shared-table defect does not block guest paging:
every shadow PTE lives in slot 128's subtree, which the kernel never touches, so the
by-value copy's sharing is *load-bearing and safe* exactly where it is relied upon. The
`shadow_install()` window guard (`kernel/qemu_sls_mmu.c:429-446`) enforces this as an
assertion: any install outside the guest-window slot is refused with a message naming
the defect doc — the defect doc's §6 Step B, already landed.

**Decision: option (b), the `guest_base` window, in its two-window form. Bare-GVA
(which is what (a) and (c) exist to serve) is retired, as already recorded in the
Address-Space-Design doc §4. No COW cloning, no world switch.**

---

## 3. What this decision means for M2 — the remaining work, sharply bounded

The data path through the guest window is built and validated: the `qemu paging` run
of 2026-08-04 exercised the whole chain — guest loads its own CR3, enables paging,
reads through **GVA 0x400000 whose translation is GPA 0x5000**, the magic value
arrives — which is proof of the walk (`guest_walk` on `qemu_sls_guest_cr3`), the
fault-then-install path (`shadow_fault` → `shadow_install`, W-bit propagated, `invlpg`),
the identity drop, and the TLB flush. Two M2 pieces remain, and both are now small:

### 3.1 The fetch path — the one place still refusing paging-on guests

`sls/sls-i386-codefetch.c` reads `SLS_GPA_BASE + addr` and **refuses loudly when
`qemu_sls_guest_paging_on` is set**. Under the window model the fix is not a shadow
walk "somewhere else" — it is the same mechanism the data path already uses: fetch at
`QEMU_GUEST_WINDOW_BASE + gva`, and let the existing fault path populate the PTE
(`qemu_sls_guest_active` is already true during guest execution, so a host #PF on the
fetch dereference routes to `shadow_fault()` exactly like a TCG load's would). The
refusal's own comment names this as the design: "needs the shadow walk in
kernel/qemu_sls_mmu.c, which is not wired into instruction fetch yet."

Two call sites, not one:

- `sls-i386-codefetch.c`'s `cpu_ld*_code_mmu` / `get_page_addr_code_hostp` — switch to
  the guest window when paging is on, keep the emulator window (identity) as the
  paging-off fast path.
- The C dispatcher in `sls-launcher.c` fetches opcodes at `SLS_GPA_BASE + eip` —
  with paging on, `eip` is a GVA and the dereference must go through the guest window
  too. One rule, stated once: **fetch and data both resolve through the guest window;
  the emulator window is for the emulator, never for guest-code execution.**

**LANDED 2026-08-11.** Both call sites, plus the hand-rolled frontend's fetch macros
(`sls-x86-frontend.c`'s `GUEST_RB`/`GUEST_RD`), now resolve through the shared
`sls_guest_addr_to_host()` inline in `sls-osdep.h` — the one rule in one place, with
paging-off as the never-faulting fast path and paging-on faulting into `shadow_fault`
for population. The C dispatcher's guest *data* accesses (CALL's stack push, RET's
stack pop, LODSB) use the same rule, making the stated one-rule true rather than
half-true. One discovered consequence, fixed in the same change: the `qemu paging`
fixture's guest enabled paging without mapping its own code, which real hardware (and
now this kernel) faults on — the fixture identity-maps GVA 0 → GPA 0, as a real boot
loader would. The post-commit write-protect loop is skipped for paged guests (its
GPA keying needs the §3.2 walk), a named gap, not a surprise.

### 3.2 The invalidation policy for guest page-table changes

The kernel already has the right machinery, built for *code* pages:
`qemu_sls_mmu_write_protect_gpa()` installs PRESENT-without-WRITE, and the
permission-fault path in `shadow_fault()` (error_code bits 1+2) bumps the physical
page's tcache generation, flushes the page's TBs, and reinstalls writable. The M2
policy for guest-owned page tables, in three tiers:

1. **CR3 write and `invlpg`** — **LANDED 2026-08-11.** The C dispatcher's MOV CR3
   handler calls `qemu_sls_mmu_shadow_cr3_reload()` when paging is already on, which
   zeroes the guest window's populated subtree and flushes the TLB so the next
   accesses repopulate from the new root. `INVLPG` (0F 01 /7) is now decoded in the
   dispatcher and calls `qemu_sls_mmu_shadow_invlpg()`, which walks the shadow's own
   host tables and drops the one page's PTE (no-op if the page has none). Both are
   no-ops while paging is off, where hardware ignores CR3. **The M3 wiring point is
   LANDED 2026-08-11:** the decoder's `flush_page` helper is real, not a stub —
   `helper_flush_page()` (what `gen_helper_flush_page` calls for INVLPG 0F 01 /7
   and INVLPGA 0F DF) calls `qemu_sls_mmu_shadow_invlpg()`, and the C dispatcher's
   INVLPG decode routes through that same helper under `SLS_X86_FRONTEND=on` so the
   hook is exercised on hardware today. When the decoder is wired into execution,
   a TCG-translated INVLPG invalidates the guest-window shadow PTE with no further
   change. The remaining decoder-path gap is `MOV CR3` (`helper_write_crN`, still a
   halting stub), which is the Step 6.3 wiring milestone's problem, not M2's.
2. **Guest writes to its own page-table pages** — **LANDED 2026-08-11.**
   `guest_walk()` marks every page the guest's CURRENT tables use as a table (one
   byte per guest page, cleared at paging enable/reset); `shadow_install()` maps any
   marked frame PRESENT-without-WRITE, so a store to it faults; the permission path
   resolves the write's GPA, sees a marked table page, and drops the whole populated
   subtree (every leaf was derived from the pre-store tables) before reinstalling the
   page writable so the store lands. A second store to the same page is SILENT from
   then on — exactly hardware semantics, where a guest that edits a table it has
   already translated must flush the affected translations itself (INVLPG / CR3
   reload). The first store after every drop always traps, which is the guarantee:
   no stale leaf can be served past an edit nobody noticed.

   **One prerequisite defect found and fixed while implementing this:** CR0.WP was
   clear, so CPL-0 stores to read-only supervisor pages succeeded and the
   write-protect trap — this one AND the pre-existing code-page self-modifying trap —
   never fired on real hardware. `sls-launcher.c` now sets CR0.WP (bit 16) around
   guest execution and restores it after. This is what made tier 2 observable, and it
   means the code-page trap (tier 3) now works on real hardware for the first time —
   verified by the new `qemu selfmod` fixture (POST `/api/qemu/selfmod`): the guest
   patches its own code page mid-run (a MOV immediate and a JMP displacement), the
   store faults, the page generation bumps (3 TBs invalidated), and re-execution
   comes from a fresh translation of the patched bytes (EAX = 0x22222222, 12 insns,
   halted). PASS on both the default and `SLS_X86_FRONTEND=on` builds.

   Verified on hardware, both builds: the new `qemu invl` fixture (POST
   `/api/qemu/invl`) has the guest edit its PT page in place, execute `INVLPG`, and
   reload CR3, storing a distinct magic after each phase; all four must read back
   correct, and each stage can only pass if the invalidation before it actually
   dropped the stale translation. res = A B C D on both the default and
   `SLS_X86_FRONTEND=on` builds; the paging fixture still passes on both. The 107
   host-test checks (including the reworked `test_shadow_install` signature) pass.
3. **Self-modifying guest code** — already handled (generation bump + tcache flush),
   independent of paging.

### 3.3 Stale guards to mark as history (two, both found while writing this record)

The code has moved past its own documentation, and this project's rule is that
"history, not instructions" annotations are written down, not left to confuse:

- `kernel/qemu_sls_mmu.h:178` — `#define QEMU_GUEST_WINDOW_UNIMPLEMENTED 1`, and the
  "NOT YET BUILT" header block (lines 142-179) claiming the two-window design does not
  exist and that `qemu_sls_guest_paging_on` "must never be set to 1 in production".
  Both predate the implementation; `QEMU_GUEST_WINDOW_UNIMPLEMENTED` is referenced
  nowhere in code, and the paging path has run on hardware. The launcher's refusal
  that the block refers to was already removed by the guest-paging-reset fix
  (2026-08-06). Annotate in place, do not delete — same treatment the paging-reset
  guards received on 2026-08-10.
- `tcg/tcg.c:207-216` (qemu tree) and `kernel/qemu_sls_mmu.c:577` — both claim
  `guest_base`/the shadow window is `QEMU_GPA_HOST_BASE`. The launcher sets it to
  `QEMU_GUEST_WINDOW_BASE` (`sls-launcher.c:271`), and `shadow_fault()` recovers the
  GVA by subtracting the guest window — the comments describe the pre-two-window
  design. Annotate both.

---

## 4. Consequences

### Chosen

- `guest_base` window model, two-window form (emulator + guest windows, slots 64 and
  128). Guest accesses = `guest_va + QEMU_GUEST_WINDOW_BASE`, host-MMU-resolved.
- Fetch and data both resolve through the guest window for paging-on guests.
- `shadow_install()`'s window guard stays — it is the enforcement of the defect's
  constraint, not a style choice.
- The shadow root keeps inheriting the kernel's 512 PML4 entries by value — that
  sharing is the design, safe because all shadow writes stay inside slot 128.

### Rejected

- COW-cloned shadow tables — a component of bare-GVA, not an answer to the
  visibility problem; not needed by the window model.
- World-switch address space — serves the retired same-ISA goal; the largest option;
  kept on record as the future upgrade path only.
- Bare-GVA in any form — retired in the Address-Space-Design doc §4; re-confirmed.

### Deferred (named gaps, not surprises)

- Page-table reclaim walk at guest reset (pre-existing; recorded in the reset-defect
  doc — the paged subtree is stashed, not freed).
- The decoder build is wired into execution as of 2026-08-12 (Step 6.3
  translator-loop wiring; see the update block above) — `helper_write_crN` and
  `helper_hlt` are real, and the invl/paging/selfmod fixtures pass under the
  decoder. Still deferred: the 765-helper long tail of Step 6.4 (SSE/AVX/x87),
  MMIO regions (SCOPE.h CATEGORY 9: no devices in scope).

---

## 5. Gates — from the AMD64 plan M2, unchanged

1. **Checksum fixture:** a 64-bit guest loads its own CR3 (not identity), maps a data
   page at a high non-identity GVA, writes a known value, reads it back, returns the
   checksum. This exercises fetch-through-guest-tables, the data path, and the walk
   together — the first time fetch has gone through guest tables at all.
2. **Clean fault:** the identical fixture with one bogus translation (a leaf pointing
   at an unmapped GPA) faults cleanly to the launcher — an exit and a [FAULT] line,
   not a hang, not a wrong-value pass. The "stop at the fault, not after it" rule.
3. **No kernel corruption:** the host-test suite (`qemu_sls_mmu_host_test.c`, 107
   checks) stays green, and the byte-identical-restore assertion from the
   guest-paging-reset fix still holds after the fetch-path change — the fetch path is
   the first place the shadow is load-bearing for *instruction* fetch, which is the
   highest-stakes path in the kernel.

A word on §5's gate 3, in the project's own spirit: the shadow walk is real and
tested, but it has never been load-bearing for instruction fetch, and the shared-table
defect means the failure mode for getting it wrong is the kernel remapping its own
image while executing — the most expensive shape this project knows. The decision
record's job is to make the model boring. It is: two slots, one rule, no new
mechanism. The remaining work in §3 is wiring, and the gates in §5 exist to prove
that claim rather than assert it.
