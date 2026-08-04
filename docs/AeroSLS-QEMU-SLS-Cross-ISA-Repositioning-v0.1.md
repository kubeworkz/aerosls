# AeroSLS QEMU-SLS — Cross-ISA Repositioning Plan v0.1

*Supersedes the performance premise of `AeroSLS-QEMU-SLS-Viability-Analysis.md`.
Does not supersede its engineering content, which remains accurate.*

---

## 0. Why this document exists

`AeroSLS-QEMU-SLS-Viability-Analysis.md` Phase 1 states:

> **Expected impact:** 3–5x speedup on memory-intensive workloads (validated by
> KQEMU's historical results and FEX-Emu's MMU-bypass measurements).

The engineering behind that estimate is sound. The **market premise underneath
it was not examined**, and it does not survive examination:

**QEMU only uses TCG when hardware virtualization cannot be used at all.**
Where guest and host share an instruction set, QEMU uses KVM (Linux), Hypervisor
Framework (macOS), or WHPX (Windows), and runs at roughly native speed. TCG is
not on that path. Making TCG 3–5× faster in that configuration competes against
an accelerator that is already 10–50× faster, and loses.

### The precedent is exact, and it is cited in our own document

**KQEMU** was a kernel module that accelerated QEMU by giving guest code direct,
host-MMU-based access to memory instead of software address translation. That is
the same idea as QEMU-SLS Phase 1. KVM arrived, hardware virtualization made
KQEMU redundant, and it was removed from QEMU.

Our Viability Analysis cites KQEMU as *validation* for the 3–5× figure. KQEMU is
better read as the **tombstone for the same-ISA version of this strategy**. The
number was probably real. The product still died, because something structurally
faster appeared underneath it.

The other citation — **FEX-Emu** — points the opposite way, and that difference
is the whole content of this document. FEX is alive and used precisely because it
runs **x86-64 guests on ARM hosts**, where no hardware acceleration exists or can
exist.

---

## 1. What the finding does and does not invalidate

Hardware virtualization requires the guest and host instruction sets to **match**.
VT-x and AMD-V run x86 on x86. ARM EL2 runs ARM on ARM. There is no hardware path
for x86-on-ARM, ARM-on-x86, or RISC-V on anything.

| Configuration | Hardware accel available? | Is TCG relevant? | Effect on QEMU-SLS |
|---|---|---|---|
| x86 guest on x86 host | **Yes** — KVM / HVF / WHPX | No | **Dead.** This was the target. |
| ARM guest on ARM host | **Yes** — KVM / HVF | No | Dead. |
| x86 guest on ARM host | **No** | **Yes, only option** | **Untouched.** |
| x86 guest on RISC-V host | **No** | **Yes, only option** | **Untouched.** |
| ARM / RISC-V guest on x86 host | **No** | **Yes, only option** | **Untouched.** |
| Any guest, host without `/dev/kvm` | No | Yes | Untouched, but a weaker story. |

So the loss is real but bounded: **one row of a six-row table**. It happens to be
the row we were aiming at.

---

## 2. Evidence — what is verified, what is assumed

This project's standing rule is that a claim without a measurement behind it is a
hypothesis. Applying it to our own strategy:

| Claim | Status | Evidence |
|---|---|---|
| TCG is bypassed when hardware virt is available | **Verified** | Architectural; definitional for KVM/HVF/WHPX. |
| Hardware virt requires matching guest/host ISA | **Verified** | Definitional. No cross-ISA hardware path exists. |
| qemu-user already runs with softmmu disabled | **Verified in tree** | `tcg/tcg-internal.h:37` — `#ifdef CONFIG_USER_ONLY / #define tcg_use_softmmu false`. |
| QEMU has TCG backends for ARM64 and RISC-V hosts | **Verified in tree** | `tcg/aarch64/`, `tcg/riscv64/`. |
| AeroSLS has a working RISC-V port | **Verified in tree** | `arch/riscv/` — boot, traps, PLIC, SBI, `user_paging_riscv.c`, `walk_page_tables_riscv.c`. Makefile target `riscv-elf`. |
| The cluster nodes run without hardware acceleration | **Verified by observation** | No `-enable-kvm` in any node's `/proc/<pid>/cmdline`. |
| Softmmu elimination yields 3–5× | **UNVERIFIED** | Never measured on AeroSLS. Inherited from KQEMU and FEX. **Must not be asserted as ours.** |
| QEMU-SLS full-system shadow paging works | **UNVERIFIED** | `map_guest_ram` currently halts a node silently. See §6. |
| qemu-user is measurably faster than qemu-system on identical code | **UNVERIFIED** | Widely believed, never measured here. This is what `tests/softmmu_ceiling_measurement.sh` exists to establish. |
| `TCG_MAX_INSNS` is 512 against a fixed array | **Verified in tree** | `include/tcg/tcg.h:122`, and `TCGContext.gen_insn_end_off[TCG_MAX_INSNS]` at `tcg.h:436`. |

The last two lines are the honest state of the project. Everything in §4 is
conditional on closing them.

---

## 3. What survives, what is retired

### Survives — unaffected by the finding

| Asset | Why it survives |
|---|---|
| `kernel/qemu_sls_mmu.c` — shadow page tables | Required for cross-ISA full-system emulation, and again later for a real hypervisor. The single most reusable piece. |
| `kernel/frame_pool.c` hardening | Core AeroSLS. Watermarks, live-stack guard, reservation ranges. |
| `kernel/fault_report.h` | Core AeroSLS diagnostics. Paid for itself twice already. |
| The linker-script fix (`.bootstrap_stack`) | Core AeroSLS. Was a live memory-corruption bug. |
| `arch/x86/user_paging.c` — `arch_read_cr3()` factoring | Makes privileged paths host-testable. |
| Host test suite | `qemu_sls_mmu_host_test.c`, `fault_report_host_test.c`, and the rest. |
| The `sls/` osdep + build integration | Real, hard-won, and reusable for any TCG-in-kernel target. |
| `sls-x86-frontend.c` | **Gains value.** An x86 *guest* frontend is only interesting on a non-x86 host. |

### Retired

| Item | Action |
|---|---|
| "3–5× faster QEMU" as an AeroSLS claim | Remove from all material. Reattribute to KQEMU/FEX as prior art. |
| x86-on-x86 acceleration as a goal | Retired. KVM owns it. |
| Viability Analysis Phase 1 impact line | Annotate in place — do not delete. The reasoning trail is worth more than a clean document. |
| `qemu bench` as a *marketing* instrument | Keep as an engineering instrument; it is no longer producing a headline number. |

---

## 4. The repositioned goal

> **AeroSLS runs x86-64 workloads on non-x86 hardware, at full-system fidelity,
> using host page tables instead of software address translation.**

### Why AeroSLS is unusually well placed

Existing cross-ISA emulators are **user-mode**: FEX-Emu, Box64, and qemu-user
translate a process, not a machine. They can use the host MMU trivially, because
a user-mode guest has no page tables of its own — `guest_base` plus a host
mapping is sufficient. That is exactly what `tcg_use_softmmu false` gives them.

Whether that makes qemu-user *measurably* faster than qemu-system on identical
code is the question §5 exists to answer. It is widely believed and it is what
this whole strategy rests on, which is precisely why it must not be assumed here.

**Full-system** cross-ISA emulation is the gap. A full-system guest has its own
page tables, its own privileged state, and MMIO. Mainline QEMU handles that with
softmmu — a software TLB consulted on every guest memory access. Shadowing guest
page tables into host page tables to eliminate that requires control of the
host's page tables, which a userspace process does not have.

**KQEMU did exactly this** — and it is worth being precise about why that is not
a refutation. KQEMU was full-system and used the host MMU, but it was *same-ISA*
(x86 on x86), which is the one configuration hardware virtualization later took
over completely. The technique was never disproved; its market was removed. No
equivalent exists for the cross-ISA case, where no hardware alternative can
appear, because ISA translation is not something a CPU can do for you.

**AeroSLS is an operating system. It owns the page tables.** That is the
structural advantage, and it is the same advantage the Viability Analysis
identified — it was simply pointed at a market that KVM already owned.

### The pieces, and where they stand

| Piece | Status |
|---|---|
| x86-64 guest frontend | **Exists, minimal.** `sls-x86-frontend.c` — ModRM, `0x8B`/`0x89`, control flow exits to C. Needs substantial extension. |
| Shadow page tables (x86 host) | **Exists, unproven.** `kernel/qemu_sls_mmu.c`. Currently halts. |
| Shadow page tables (RISC-V host, Sv39/Sv48) | **Does not exist.** `arch/riscv/walk_page_tables_riscv.c` is the starting point. |
| TCG backend for RISC-V host | **Exists in QEMU** — `tcg/riscv64/`. Not yet built for `SLS_IN_KERNEL`. |
| TCG backend for ARM64 host | **Exists in QEMU** — `tcg/aarch64/`. |
| AeroSLS on RISC-V | **Exists.** `arch/riscv/`, target `riscv-elf`. Not integrated with `sls/`. |
| AeroSLS on ARM64 | **Does not exist.** |

Two of the three hard pieces are already in the tree. That is the argument for
repositioning rather than abandoning.

### Honest scope note

Development would run AeroSLS-on-RISC-V under emulation, hosting an emulated
x86 guest — emulation inside emulation. Workable for correctness, useless for
performance measurement. **Any performance claim for this configuration requires
real non-x86 hardware.** A RISC-V or ARM64 single-board computer is the cheapest
way to make the numbers mean anything, and should be treated as a prerequisite
for publishing any figure, not an optimization.

---

## 5. Gate 0 — the bounding measurement (do this first)

> **Runnable:** `tests/softmmu_ceiling_measurement.sh --kernel <aarch64-Image>`
> The acceptance gate below is encoded in the script's exit status, and the
> same-ISA refusal is enforced rather than documented.

Before any further AeroSLS work, establish what softmmu elimination is worth.
**QEMU already ships both halves**, so this needs no AeroSLS code at all:

- `qemu-<arch>` (linux-user) → `tcg_use_softmmu` is **false**
- `qemu-system-<arch>` (full system, TCG) → `tcg_use_softmmu` is **true**

Same host, same guest binary, same TCG core. The difference is the memory path.

```bash
# On an x86-64 host, with a cross-ISA guest so TCG is genuinely in play.
# aarch64 chosen because both a user-mode and a system-mode QEMU exist for it.

qemu-aarch64 ./bench-static                  # softmmu OFF  (host MMU)
qemu-system-aarch64 ... ./bench-static       # softmmu ON   (software TLB)
```

**What this measures:** the upper bound on what QEMU-SLS Phase 1 can deliver.
Full-system emulation must also shadow guest page tables, handle MMIO, and take
faults, so real-world gain will be **at or below** this ratio.

**What it does not measure:** anything about AeroSLS. It is a ceiling, not a
forecast.

**Acceptance gate.** The ratio decides the plan:

| Ratio | Reading | Action |
|---|---|---|
| **≥ 3×** | The premise holds where TCG is the only option. | Proceed to §8. Publish the method alongside any number. |
| **1.5×–3×** | Real but not headline. | Proceed, but position as a capability, not a speed claim. |
| **< 1.5×** | The technique does not pay for its complexity. | Stop. Keep shadow paging for a future hypervisor; retire the TCG integration. |

The comparison is imperfect — user-mode avoids more than just the TLB lookup, so
it likely **overstates** the achievable gain. That bias is in the safe direction
for a ceiling, and it must be stated wherever the number is used.

---

## 5b. Measured: the softmmu=ON baseline

**First reproducible measurement in this project. 2026-08-04, node 2.**

```
qemu bench 500   ->  502 insn(s) compiled, 0 interpreted
                     CODE  43,293 bytes emitted  ->  86 bytes/load
                     softmmu = ON
```

**86 bytes of host code per guest load** is the A/B baseline. Step 5 re-runs the
identical command with `tcg_use_softmmu` false; the two byte counts are the
comparison. Report both framings:

- **total bytes/load, ON vs OFF** — the honest end-to-end cost of a guest load
- **ON − OFF** — the bytes attributable to the inlined TLB lookup specifically

### Why bytes and not cycles

The cycle columns from the same run are **not usable**, and the reason is worth
recording because it applies to every future measurement taken on this host:

```
EXEC       26,350,118 cycles  ->  52,700 cycles/load
TRANSLATE 144,597,112 cycles in 8 blocks
```

52,700 cycles for one `MOV EAX,[EBX+disp32]` is roughly fifty times too high. A
softmmu load is 10–20 host instructions; even at 50× outer-emulation overhead
that is about a thousand cycles. The node runs under host `qemu-system-x86_64`
with **no KVM** (Hetzner Cloud does not expose nested virtualisation on this
instance type — `/dev/kvm` is absent). Our JIT emits fresh host code at fresh
addresses, so the outer emulator must translate that code before running it,
every time, because it is always cold. EXEC is timing **the outer emulator
compiling our JIT's output**, not our output executing.

**That confound is not neutral.** With softmmu off, each load emits a bare MOV
instead of an inlined TLB lookup — less host code, so less outer-translation
work. A cycle-based A/B on this box would report a speedup partly composed of
"fewer instructions for the outer emulator to compile", biasing the result **in
our favour**. For a number whose purpose is to justify a strategy, that is the
worst possible direction to be wrong in.

Generated-code size has none of those problems: it is what TCG emitted, counted
before anything runs. Deterministic, unaffected by the outer emulator, the TSC,
or host load.

### What this number is not

It is **not a speedup figure** and must not be quoted as one. Fewer bytes is
strong evidence of less work per access; converting that to time depends on
cache behaviour and host pipeline effects this hardware cannot observe. §9's
requirement stands unchanged: **no timing claim is publishable without KVM or
real non-x86 hardware.**

### Determinism — VERIFIED (2026-08-04)

Two runs of `qemu bench 500`, **in different boots of different builds**:

| | run A | run B | delta |
|---|---|---|---|
| **CODE bytes** | **43,293** | **43,293** | **0** |
| bytes/load | 86 | 86 | 0 |
| total cycles | 172,043,042 | 181,143,837 | **+5.3%** |
| EXEC cycles | 26,350,118 | 29,680,280 | +12.6% |

Byte counts bit-identical across a reboot; cycle counts moved 5–13% on the same
workload. That is the whole argument for using code size, demonstrated rather
than asserted, and it is a stronger check than the same-boot repeat originally
asked for.

**86 bytes/load is the confirmed softmmu=ON baseline.** Step 5 compares against
it directly.

### Five consecutive runs, one boot (2026-08-04)

| run | CODE bytes | EXEC cycles/load | arena after |
|---|---|---|---|
| 1 | **43,293** | 50,622 | 15% |
| 2 | **43,293** | 49,795 | 30% |
| 3 | **43,293** | 51,136 | 46% |
| 4 | **43,293** | 52,723 | 61% |
| 5 | **43,293** | 51,633 | 77% |

**CODE: identical five times out of five**, on top of four earlier boots across
three different builds. Nine samples, zero variation.

**EXEC: mean 51,182 cycles/load, σ ≈ 982, CV ≈ 1.9%**, range 49,795–52,723.
Within a single boot the cycle counter is far steadier than the 5–13% seen
across boots — so cross-boot comparison is what introduces most of the noise,
not the counter itself.

### Using CODE as a control for EXEC in Step 5

EXEC is contaminated because the outer emulator must translate our JIT's output
(§ above). But that contamination is *proportional to how much code we emit* —
which is exactly what CODE measures. That makes the confound estimable rather
than merely acknowledged:

1. Record CODE and EXEC with softmmu **ON** (done: 43,293 bytes, 51,182 cyc/load)
2. Record both with softmmu **OFF**
3. `code_ratio = CODE_on / CODE_off` predicts the EXEC improvement attributable
   purely to there being less code for the outer emulator to compile
4. `exec_ratio = EXEC_on / EXEC_off`

If **`exec_ratio ≈ code_ratio`**, the apparent speedup is entirely the artifact
and this hardware has measured nothing about execution. If **`exec_ratio >
code_ratio`**, the excess is real work eliminated, and its size is the first
honest estimate of what shadow paging buys.

At CV ≈ 1.9%, five runs per configuration resolve a difference of roughly 3–4%
between those two ratios. Both are obtainable in one boot per configuration
with the 64 MiB arena.

This does not lift §9's requirement — a *timing* claim still needs KVM or real
non-x86 hardware. It does mean the A/B can distinguish "less code to compile"
from "less work to do", which was the objection that made the cycle figures
unusable.

---

## 6. Node 2 — the undiagnosed silent halt

**Status: open. Blocking, and would have been blocking under any direction.**

### What is established

| Observation | Evidence |
|---|---|
| `qemu_sls_mmu_init()` succeeded at boot | `node2.log:17` — `[QEMU-SLS MMU] shadow PML4=0x000000000ad59000 (kernel PT inherited)` |
| `map_guest_ram` never completed | No `guest RAM GPA ...` line, no OOM line, no overlap-refusal line. All three paths print. |
| The node did not reboot | `grep -c 'System ready'` = **1** |
| The guest CPU is **halted, not spinning** | `utime` flat at 743 across 30 s; `stime` +27 (QEMU's own idle polling) |
| The kernel was healthy before the command | `help` on node 1 returned in full |
| Reproducible on a clean node | Node 2 had never run a bench before |
| Independent of `n_loads` | 8 and 256 behave identically → fixed-cost step, not the load path |

### Why this is hard, and why gdb is the answer

**Every deliberate halt path in this codebase prints before halting** — the 130
helper stubs each announce their own name, `exit` halts loudly, `handle_page_fault`
emits `[FAULT]`. Something reached a `hlt` without passing through any of them.

Three hypotheses were advanced from reading the code during the session — wedged
at boot, triple fault, quadratic allocation — and **all three were wrong**. Each
was killed by a two-command measurement. The lesson is already written into this
project's rules and was violated anyway: *an absence is not a measurement*, and
inferring dynamics from static reasoning has now failed three times in a row on
this one bug.

So: read the program counter. Do not reason about it.

### Procedure

> **Runnable:** `tests/node_gdb_attach.sh 2` — relaunches node 2 with the
> gdbstub, taking its argv from `/proc` so the replacement cannot differ from the
> node that hung, and refusing to proceed if the kernel ELF is stripped or newer
> than the ISO the node is actually running.

`my_sls_kernel.bin` is a full ELF, not stripped, with `.symtab` — there is no
`objcopy -O binary` step; GRUB loads it as ELF via multiboot2.

```bash
tests/node_gdb_attach.sh 2                                    # relaunch with -gdb
tools/aeroslsctl --host localhost:3002 shell "qemu bench 8"   # will wedge
gdb my_sls_kernel.bin -ex 'target remote :1234'
```

```
(gdb) info registers rip rsp cr3
(gdb) x/12i $rip-24
```

**Do not trust `bt`.** `X86_CFLAGS` is `-O2` with no `-fno-omit-frame-pointer`
and no `-g`, so there are no frame pointers and no DWARF. Map the PC by hand:

```bash
nm -n ~/aerosls/my_sls_kernel.bin | awk '$1 <= "<rip>"' | tail -3
```

`nm -n` sorts by address; the last symbol at or below RIP is the containing
function.

**The disassembly matters as much as the symbol.** `cli; hlt` in a loop is a
deliberate panic path — something called it without printing. A bare `hlt` is an
idle loop that stopped receiving interrupts, which is a completely different bug.
No log evidence can distinguish those two; the instruction stream can.

### Two things the answer will settle

- If the PC is inside `user_map_page` or the shadow walk, the 65,536 mappings at
  the 32 TiB window touched something the boot-time PML4 inheritance did not
  cover, and the shadow tables are half-built when it dies.
- If the PC is in `sls_launcher_init` **before** `map_guest_ram` — the CPUID
  feature-detection block is the untested part — then the mapping never started
  and every memory-arithmetic hypothesis from this session was irrelevant.

### Regardless of the cause — a design defect worth fixing

`sls_launcher_init()` maps `QEMU_GUEST_RAM_PAGES` = 65,536 pages = **256 MiB**
eagerly, for a benchmark whose program is 10 instructions and whose buffer is 512
bytes. On a node with `-m 1G` and a kernel image that has grown to **173 MiB**
since TCG was linked in, that is a large, unnecessary, and unmeasured commitment
made before anything is measured. Map what the caller needs.

---

## 7. Immediate hygiene

| Item | Rationale |
|---|---|
| **Add `-enable-kvm` to the dev cluster** | The nodes currently emulate x86 on x86 through TCG — the exact configuration with no reason to exist. Check `ls /dev/kvm` and `grep -E 'vmx\|svm' /proc/cpuinfo` first; nested virt is not available on every cloud instance type. This is the single largest development-velocity win available today. |
| **Add `-fno-omit-frame-pointer -g` to `X86_CFLAGS`** | Costs one rebuild and some image size. Makes `bt` work. Today's debugging consumed hours inferring positions that a backtrace would have printed. |
| **Make guest RAM mapping proportional** | See §6. `sls_bench_load_path` already computes exactly what it needs. |
| **Tidy the duplicate refusal message** | `sls_bench_load_path()` prints a detailed refusal *and* returns −1; the `user/shell.c` handler then prints a second, less informative one. |
| **Rename the local `void *guest_base`** | `sls-launcher.c:346`, before a global of that name exists and the shadowing becomes a real bug. |
| **Annotate the Viability Analysis** | Phase 1's impact line. In place, not deleted. |

---

## 8. Sequenced plan

Each step has a gate. No step begins before its predecessor's gate is met — the
failure mode this document exists to correct was building on an ungated premise.

**Step 0 — Bounding measurement.** §5.
*Gate:* a ratio, with the method recorded. If < 1.5×, stop and re-scope.

**Step 1 — Diagnose the node 2 halt.** §6.
*Gate:* a named function and a root cause. Not a fix that makes the symptom go
away — three plausible fixes were already proposed for this bug and all three
were aimed at the wrong thing.

**Step 2 — Hygiene.** §7. KVM on the dev cluster first; it makes everything after
it faster.
*Gate:* four nodes healthy, `reap_slot_e2e.sh` passing.

**Step 3 — Prove full-system shadow paging on x86.** Get `qemu bench` producing a
softmmu=ON number, then complete the original Step 5 (`tcg_use_softmmu` false
under `SLS_IN_KERNEL`) and produce the A/B.
*Gate:* two numbers from one binary, one flag apart. This is still worth doing on
x86 — not as a product, but because it is the **only way to validate the shadow
paging design before porting it to a second architecture.**

**Step 4 — Decide the host architecture.** RISC-V (a port exists) or ARM64 (none
does, but hardware is far easier to obtain and the guest ecosystem is larger).
*Gate:* an explicit decision, recorded, with hardware on hand.

**Step 5 — Port shadow paging to the chosen host.** Sv39/Sv48 or ARM64 long-format
descriptors. `arch/riscv/walk_page_tables_riscv.c` is the starting point on that
path.
*Gate:* the host test suite passes on the new architecture. Write the tests
first — `qemu_sls_mmu_host_test.c` exists precisely because a bug in this layer
surfaces as a `#PF` inside translated code, several layers from its cause.

**Step 6 — Extend the x86 guest frontend.** Currently a handful of opcodes. This
is the largest remaining body of work and the least novel; QEMU's `target/i386/`
is the reference.
*Gate:* a real x86-64 guest binary runs to completion.

Steps 0–2 are days. Steps 3–6 are the actual project.

---

## 9. Risks and honest unknowns

**The bounding measurement may kill the plan.** That is its purpose. A ratio
below 1.5× means the complexity is not worth carrying, and the correct response
is to keep the shadow paging for a future hypervisor and retire the rest. Writing
that acceptance criterion down *before* seeing the number is the entire point of
§5.

**Cross-ISA full-system is a smaller market than "QEMU is slow."** Real, but
smaller. It should be sized honestly before Step 4, not assumed.

**FEX-Emu and Box64 are established and fast.** They are user-mode only, which is
the gap this targets — but a user-mode emulator plus a container often solves the
customer's actual problem without full-system emulation at all. That substitution
should be understood before committing to Steps 5–6.

**The x86 guest frontend is a large, unglamorous body of work.** x86-64 is a big
instruction set. This is the schedule risk.

**No performance claim is publishable without non-x86 hardware.** Emulated
measurement of an emulator measures the outer emulator.

**The kernel image is 173 MiB and growing.** Linking TCG did that. Every
allocation decision made before TCG was linked in was made under different
arithmetic, and `map_guest_ram`'s 256 MiB request is one such decision. There may
be others that have not surfaced yet.

---

## 10. Marketing position

**Do not claim:** "3–5× faster QEMU." Not measured here, and false in the
configuration most readers will assume (x86 on x86), where KVM is already ~native.

**Can claim, once Step 0 and Step 3 produce numbers:** that AeroSLS eliminates
QEMU's software MMU in full-system emulation by shadowing guest page tables into
host page tables — something a userspace emulator structurally cannot do — and
that the measured effect on the load path is *X*, by *this* method, on *this*
hardware.

**The honest frame:** the interesting claim was never the multiplier. It is that
**an operating system can do something to an emulator that a process cannot.**
That claim is still entirely true. It was aimed at a workload where nobody needs
it. Pointing it at cross-ISA — where binary translation is not a fallback but the
only mechanism that exists — costs no engineering and restores the premise.

---

## Appendix A — session record

This plan came out of a debugging session that produced findings worth keeping
independently of the strategy change.

**A live bug in shipped code.** `qemu run <hex>` passed `max_insns = 100000`
against `TCGContext.gen_insn_end_off[TCG_MAX_INSNS]`, a fixed 512-entry array.
Any guest with more than 512 translatable instructions would have written ~3,586
entries past the end of the translator's own state. It had never fired because
every guest tried so far exits to C within a few instructions. Found only because
a benchmark asked for 4,096 loads. Now clamped.

**A memory-corruption bug in the linker script.** `*(.bootstrap_stack)` nested
inside `.bss` did not match, so the section became an orphan placed above
`PROVIDE(_kernel_image_end)`, leaving 16 frames of live kernel stack allocatable.
Surfaced as `rip=0xcdcdcdcdcdcdcdcd`.

**Eleven checks that could not fail**, each found by mutation testing, each the
same shape: the assertion was true for a reason other than the one under test.

**Three wrong hypotheses about one bug**, all from reading code instead of
measuring, all corrected by two-command probes. Recorded in §6 because the
pattern matters more than the bug.

**A process failure worth keeping.** Fixes were pushed twice on hypotheses
without first requesting the node log, costing two restarts — in a session whose
entire subject was measuring rather than assuming. The standing rule that came
out of it: *when a node stops answering, the first request is the log, not a
patch.*
