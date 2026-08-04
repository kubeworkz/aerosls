# AeroSLS QEMU-SLS — Guest Address Space Design

**Decision record v0.1 · 2026-08-04**

*Written to answer "GVA-direct or the guest_base window?" The answer turned out
to be different from what `qemu_sls_mmu.h` has said since Phase 1 was designed,
and the reason is worth recording.*

---

## 0. The question as it was posed

Step 5 shipped with `guest_base = QEMU_GPA_HOST_BASE`: a guest access at
address *A* becomes a host access at *A* + 32 TiB, using TCG_REG_R12 as an
index register. Measured result: **86 bytes of host code per guest load → 16**.

`kernel/qemu_sls_mmu.h` describes a different endpoint:

> the shadow PT maps guest virtual addresses directly (see `shadow_install()`
> in the .c), so emitted loads and stores use a bare GVA with no base register
> at all.

The question was which to build next, and whether the benchmark guest needs
real page tables to exercise it.

---

## 1. A blocking defect in the GVA-direct design

`qemu_sls_mmu_init()` (`qemu_sls_mmu.c:46`):

```c
for (int i = 0; i < 512; i++) shadow_pml4[i] = kernel_pml4[i];
```

All 512 entries, **copied by value**. A PML4 entry is a pointer to a PDPT, so
the shadow and the kernel now *share every lower-level table*.

`user_map_page()` (`arch/x86/user_paging.c:109`):

```c
uint64_t* pdpt = get_or_alloc(pml4, PML4_IDX(vaddr));
```

`get_or_alloc` returns the **existing** table when the entry is present. So
`user_map_page(shadow_pml4, 0x1000, guest_frame, ...)` walks into the kernel's
own PDPT → PD → PT and writes `pt[PT_IDX(0x1000)] = guest_frame`.

**That is not shadowing. That is overwriting the kernel's live page tables.**
Not a divergent view under a different CR3 — the same tables the kernel is
running on, permanently, whether or not the shadow root is loaded.

With `guest_base = 0`, a paging-off guest occupies linear 0–256 MiB. The kernel
image occupies 1–221 MiB. Installing the guest's mappings would remap the
kernel's own text and data out from under itself, one page at a time, while
executing.

This has never fired because the only range ever mapped this way is the GPA
window at 32 TiB — a PML4 slot nothing else uses, where sharing is harmless.
The same sharing is what makes §5's fix (publishing the window entry into the
kernel root) correct. It is exactly wrong for low addresses.

---

## 2. What fixing it would cost

Three ways out, in increasing order of work:

**(a) Copy-on-write page tables.** Before installing a guest mapping, clone the
PML4 entry's subtree so the shadow stops sharing it. Standard shadow-paging
technique. Needs a table allocator, a cloning walk, invalidation discipline,
and careful accounting so cloned tables are freed. Meaningful new machinery in
the most safety-critical code in the kernel.

**(b) Reserve a PML4 slot for the guest.** Guest addresses live in a range the
kernel never uses. But "guest address + fixed offset" **is** `guest_base` —
this is the model already shipped, described differently.

**(c) A separate address space with a world switch.** The shadow root maps only
guest memory plus a minimal trampoline (IDT, handler, stack), and entry/exit
switches CR3. What a real hypervisor does. The largest option by a wide margin.

---

## 3. What GVA-direct would actually buy

This is the question that settles it, and it should have been asked first.

With `guest_base` in a register, a guest load compiles to:

```
mov eax, [rbx + r12 + disp32]        ; base + index + displacement, one insn
```

With GVA-direct it would compile to:

```
mov eax, [rbx + disp32]              ; base + displacement, one insn
```

**Same instruction count. Same encoding class. A SIB byte, and one register.**
That is the entire difference. The measured 16 bytes/load already includes the
index-register form.

So GVA-direct buys: TCG_REG_R12 back in the allocatable pool, and a byte or two
of encoding per access. It costs: copy-on-write page tables in the kernel's
paging layer, or a world switch.

**That is not a trade worth making.**

---

## 4. Why the header comment describes the wrong target

`qemu_sls_mmu.h`'s design is right for a case this project has abandoned.

GVA-direct matters when the guest's addresses must be *the same* as the host's
— that is, when guest code runs **natively** on the host CPU and the hardware
walks the guest's own page tables. That is same-ISA virtualization, and it is
the case KVM owns and that
`AeroSLS-QEMU-SLS-Cross-ISA-Repositioning-v0.1.md` retired.

In cross-ISA emulation, guest code never executes natively. TCG-generated
**host** code executes, and it can address guest memory however we like. It has
a spare register and it is already emitting a displacement. An offset costs
nothing.

This is also why the entire ecosystem does it this way: qemu-user has used
`guest_base` for twenty years, and FEX-Emu and Box64 both map the guest at an
offset. Not one of them attempts address-space coincidence, and they are the
projects whose approach the repositioning plan identified as the right one.

**Decision: the guest_base window model is the design. GVA-direct is retired.**

---

## 5. Consequences

### Retired

- The GVA-direct language in `qemu_sls_mmu.h` — annotate, do not delete
- Task "Move to GVA-direct shadow paging" — closed as won't-do, with this
  document as the reason
- Any need for the benchmark guest to build page tables *for this purpose*

### Still required, and now the actual next work

**Guest paging support is still needed** — not to relocate addresses, but
because a real guest OS enables paging and expects its own page tables to be
honoured. Under the window model that means: a guest access at GVA *A* is
translated by the *guest's* tables to GPA *P*, and the emitted code must reach
host address *P* + `guest_base`.

That is what `qemu_sls_mmu_shadow_fault()` already walks `qemu_sls_guest_cr3`
to do. It is not obsolete — it changes from "install GVA → frame" to "install
GVA → frame **within the window's PML4 subtree**", which is precisely the range
where table sharing is already safe.

**So the defect in §1 does not block guest paging. It blocks only the
address-coincidence variant, which is now retired.**

### The paging-off case

A guest with paging disabled has GVA == GPA, and the window mapping already
covers it with no fault at all. That is why the benchmark works today. No
identity path in `shadow_fault` is needed — it is already handled by the
contiguous mapping.

---

## 5b. VALIDATED on hardware — 2026-08-04

`qemu paging`, node 2:

```
[SLS-LAUNCHER] guest CR3 = 0x0000000000001000
[QEMU-SLS MMU] guest paging ENABLED, guest CR3=0x1000.
               Guest window identity dropped (PML4 slot 128)
[SLS-LAUNCHER] guest halted after 7 instructions
[SLS-PAGING]   7 insn(s), paging_on=1, EAX=0x5a5ac0de
[SLS-PAGING]   PASS
```

A guest loaded its own CR3, enabled paging, and read through **GVA 0x400000**
whose physical target is **GPA 0x5000**. The magic value arrived. Identity
mapping alone returns 0, so the value is proof the shadow walker resolved
through the guest's own four-level page tables.

That single result exercises the whole design at once: the two-window split,
`guest_base` addressing, the identity drop, the TLB flush, the shadow walk, and
the MOV CRn dispatch path.

### Two bugs it found on the way

**The hybrid TCG/C design never worked.** `sls_x86_translate_block()` signalled
"unknown opcode" by writing `-2ULL` into `eip` -- discarding the address needed
to resume -- and `sls_launch_guest()` read that as *halted*. So the C dispatcher
could only handle instructions appearing at the **start** of a block; anything
it was meant to handle mid-block killed the guest.

Invisible for as long as it existed, because the benchmark guest is built
entirely from opcodes the frontend already knows. `MOV CR3` was the second
instruction of the first guest that needed the fallback. Fixed by setting `eip`
to the declined instruction's own address so the launcher loops round and the
dispatcher takes it, plus a guard: a block that translates nothing has not
advanced `eip` and would spin forever, so it halts and names the opcode.

There were **two** sites writing that sentinel -- the second in the ModRM
decoder for SIB and RIP-relative operands -- found by grepping for the sentinel
rather than assuming the first fix covered it.

**And the test's own diagnostic was wrong.** Its first hardware run reported
`EAX=0` and blamed the identity mapping, while the line above it read
*2 instructions, CR3=0*. The load had never executed. A failure message that
interprets a result without first checking the program ran is a confident wrong
answer, which is the thing this project keeps paying for. It now checks
execution before interpreting output.

---

## 6. Next work, in order

**Step A — annotate `qemu_sls_mmu.h`.** The header currently instructs a future
reader to build something this document retires. In place, with the reasoning,
in keeping with how the 3–5× claim was handled.

**Step B — make table sharing explicit and guarded.** The sharing in §1 is
load-bearing and undocumented; §5's window fix depends on it and GVA-direct is
destroyed by it. Add an assertion in `map_guest_ram` / `shadow_install` that
refuses any mapping outside the window's PML4 slot, with a message naming this
document. That turns an invisible constraint into an enforced one.

*Gate:* a host test that attempts a low-address `shadow_install` and asserts
refusal. Mutation-tested.

**Step C — guest paging, under the window model.** `shadow_fault` walks the
guest's tables and installs GVA → (frame + window). Needs a guest that actually
enables paging, which means frontend support for `MOV CRn`, and `WRMSR` for
EFER. This is the real remaining work for running an actual guest OS.

*Gate:* a guest that enables paging, takes a shadow fault, and completes.

**Step D — Phase 2 (persistent translation cache).** Unchanged and still the
higher-value phase; see
`AeroSLS-QEMU-SLS-Phase2-Persistent-Translation-Plan-v0.1.md`. Translation is
85% of cost; this is 15%.

Steps A and B are hours. C is the substantial one. D remains the phase with the
differentiated story.

---

## Appendix — how this was found

Not by design review. Step 5's A/B was measured, the result recorded, and the
next step scoped — at which point reading `qemu_sls_mmu_init()` to check what
"full design" required turned up the `for (i = 0; i < 512; i++)` copy.

The defect is invisible in every test that exists, because every mapping made
so far lives at 32 TiB where sharing is benign. It would have surfaced as the
kernel remapping its own text while executing, on the first guest mapped at a
low address — a fault whose cause would have been several layers from its
symptom.

The pattern is the same one this project keeps meeting: **a constraint that
holds for every case tried so far, relied upon by one piece of code and fatal
to another, written down nowhere.** Step B exists to convert it from a property
into an assertion.
