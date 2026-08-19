# AeroSLS Kernel Capability Layer — Phase 1 Extension Spec v0.1

**Status:** Draft for review.
**Scope:** The kernel-side capability operations that the POSIX sidecar design
(`docs/AeroSLS-POSIX-Sidecar-Phase2-Design-v0.1.md`, §1, §4, §5, §7) depends on:
mint/derive/revoke, capability arguments on channel messages with transient-grant
auto-revoke, map-into-target-space with derived rights, and channel close-reason
codes. This spec is written against the Phase 1 contract (capability tables,
MEM caps, channel endpoints, `create_sidecar()`); it extends that contract, it
does not re-specify what Phase 1 already delivers (channel *creation* and
transport framing are assumed to exist).
**Relationship to Phase 2:** every message trace and security claim in the Phase 2
doc bottoms out in an operation specified here. Where this spec diverges from the
Phase 2 doc's wording, this spec wins; the divergence is called out inline.

---

## 0. Design summary

Four kernel operations carry the whole design:

1. **Mint / derive / revoke** — the kernel is the sole authority that creates
   capability table entries; a sidecar can carve sub-regions from its budget
   (`cap_alloc`), derive sub-regions with subset rights (`cap_derive`), alias
   entries (`cap_dup`), and revoke (`cap_revoke`, deep, lineage-tracked).
2. **Capability arguments on channel messages** — a message may carry MEM-cap
   descriptors; the kernel validates each against the *sender's* table and mints
   a derived entry in the *receiver's* table. Grants are **transient by
   default**: bound to the request tag, auto-revoked when the reply is sent or
   either side closes.
3. **Map-into-target-space** — implemented as a *transfer mode*, not a separate
   syscall: a cap argument flagged `MAP` is minted in the receiver's table *and*
   mapped into the receiver's address space with derived rights. In the Phase 2
   shared-address-space MVP the mapping is identity and the flag is a no-op on
   page tables; in isolation mode (Phase 4) it is the real per-principal
   mapping. Revocation of the cap tears down the mapping — no separate unmap op.
4. **Channel close-reason codes** — every teardown path (normal close, peer
   death, protocol violation, revocation, admin) delivers a coded close event to
   the peer, so a client can distinguish "driver died" from "I broke the
   protocol" and react accordingly.

Invariants that must hold unconditionally:

- **No amplification.** Every minted/derived/mapped rights set is
  `min(held, requested)`. A sidecar cannot pass, map, or derive stronger than it
  holds.
- **Kernel is the sole mint authority.** Table entries exist only because the
  kernel created them (directly, or via a validated operation). There is no
  ambient lookup, no global handle namespace, no way to name another sidecar's
  table.
- **Transient by default.** Cap arguments die with their request unless
  explicitly marked `PERSIST`. A receiver never accumulates memory rights by
  accident.
- **No orphaned authority.** Every teardown path (reply, error, close, crash,
  revocation) reaps the grants and mappings it owns. When a sidecar dies, the
  kernel revokes its table wholesale and delivers close events to every peer.
- **W^X preserved across transfers and mappings.** Executable rights are never
  passed over channels; the only X sources are kernel-minted image caps.

---

## 1. Model, terminology, and handle encoding

### 1.1 Tables and entries

- Every sidecar has exactly one kernel capability table. The kernel owns the
  memory and the mutation rights; sidecars reference entries by **handle**.
- An entry has a **type** (`CAP_MEM=1`, `CAP_CHAN=2`), a **rights** bitmask, a
  **flags** bitmask, a **generation**, and a payload (region for MEM, endpoint
  reference for CHAN). Types are a registry — new types add new tags, never
  overload old ones (same rule as the manifest, §2.3 of the Phase 2 doc).
- Rights: `MEM`: `R=0x1 W=0x2 X=0x4`. `CHAN`: `SEND=0x1 RECV=0x2 CLOSE=0x4`.
- Entry flags: `NOTRANSFER=0x1` (entry may never be passed as a message
  argument; kernel sets this on budget and other sensitive roots),
  `NOREVOKE=0x2` (kernel-reserved; the sidecar may not revoke it — used for the
  budget root so a bug can't revoke the sidecar's own foundation).

### 1.2 Handle encoding

A handle is an opaque `u64`, meaningful only within the calling sidecar's
table:

```
bits 63..32   generation (u32)
bits 31..0    table index (u32)
```

- The kernel validates `(index, generation)` against the calling sidecar's
  table on every operation. A freed slot is reused only with an incremented
  generation, so a stale handle fails with `CAP_ERR_REVOKED` instead of
  silently aliasing a new cap (use-after-free protection).
- There is **no cross-sidecar handle namespace**: sidecar A cannot guess or
  name sidecar B's handles. The only way to name another sidecar at all is to
  hold a channel cap to it (see §5.2). This is the "no ambient namespace"
  property made concrete.
- Boot Info Block slots (Phase 2 §6.1) are plain table indices (`u16`), not
  handles — the kernel fills the BIB from the table it just built, so the
  generation is trivially valid there.

### 1.3 Lineage: derivation tree and aliases

- `cap_derive` creates a **child**: the new entry records `parent = source`.
  Children are reaped when the parent is revoked (deep revocation, §3.4).
- `cap_dup` creates an **alias**: same object, same rights, `parent = NULL`,
  refcount on the entry. Revoking the source does *not* kill the alias — an
  alias is not a derivation. (This is the seL4 `copy` vs `derive` distinction;
  the Phase 2 design only needs derive and dup, but both are specified because
  the mechanism is identical.)
- The kernel maintains, per parent, a doubly-linked sibling list of children so
  revocation walks exactly the subtree.

### 1.4 Atomicity and ordering

- Capability operations are atomic with respect to the sidecar's execution. The
  kernel is single-threaded and sidecars execute cooperatively; no sidecar
  observes a half-applied operation.
- Channel messages on one endpoint are FIFO (Phase 1 contract). A message's cap
  arguments are validated **all-or-nothing**: if any descriptor is invalid the
  whole message is rejected (`CAP_ERR_PROTO`) and *no* grant is minted — a
  malformed argument cannot smuggle a valid one through.

---

## 2. Capability entry format (kernel data structure)

Landing: new `kernel/cap.c` / `include/cap.h` (or extension of the Phase 1
table implementation). Entry, 40 bytes:

```c
struct cap_entry {
    u16 type;                    // CAP_MEM, CAP_CHAN
    u16 rights;                  // MEM: R/W/X  CHAN: SEND/RECV/CLOSE
    u16 flags;                   // NOTRANSFER, NOREVOKE
    u16 gen;                     // generation for handle validation
    u32 refs;                    // alias count (cap_dup)
    struct cap_entry *parent;    // derivation lineage (NULL: root/alias)
    struct cap_entry *sib_next;  // sibling list under parent
    struct cap_entry *sib_prev;
    union {
        struct { u64 base; u64 len; } mem;      // CAP_MEM
        struct { struct chan *ep; } ch;         // CAP_CHAN
    } payload;
};
```

The table is a fixed-size array per sidecar (`limits.max_channels + headroom`
is *not* the bound — cap entries and channels are different objects; the table
size comes from the manifest's cap count and grows only via kernel-mediated
extension, capped by the budget).

---

## 3. Operations: mint, derive, alias, revoke, inspect

### 3.1 `k_cap_alloc` — mint from budget

```
k_cap_alloc(type, params, rights) -> handle | err
  type = CAP_MEM: params = { size u64, align u32 }
  type = CAP_CHAN: not a sidecar operation (see §0 scope note)
```

- For `CAP_MEM`, the kernel carves a region of `size` (aligned to `align`) from
  the calling sidecar's **budget root** and mints a child entry. The carve
  fails with `CAP_ERR_BUDGET` if the sidecar's outstanding allocations would
  exceed `budget.mem_bytes` — the budget is the sidecar-wide hard ceiling, and
  *every* allocation, derivation, and transfer of physical memory is accounted
  against it.
- The kernel itself mints the *budget root* at `create_sidecar()` time from the
  manifest (root, `NOREVOKE`, `NOTRANSFER`). The kernel is also the only entity
  that can mint from physical memory directly (image caps, kernel buffers) —
  sidecars mint only from their budget root.
- Rights requested are granted as-is (they cannot exceed what the operation
  implies: a budget carve grants exactly the requested R/W bits).

### 3.2 `k_cap_derive` — subset region, subset rights

```
k_cap_derive(src, offset, len, rights) -> handle | err
  rights != 0; (rights & ~src.rights) == 0      else CAP_ERR_RIGHTS
  offset + len <= src.payload.mem.len            else CAP_ERR_RANGE
```

- The child inherits `src.flags` (NOTRANSFER propagates) and records
  `parent = src`. Rights are `min(requested, held)` — **no amplification**, and
  the check is structural (the kernel computes it; a sidecar cannot express a
  stronger grant than it holds).
- Deriving does not change budget accounting: the region is already charged to
  the sidecar (views are free, §5.4).
- Only valid for `CAP_MEM` in v1; channel-cap derivation (sub-rights like
  RECV-only) is a trivial extension, deferred.

### 3.3 `k_cap_dup` — alias

```
k_cap_dup(src) -> handle | err
```

Creates a new entry referencing the same object with the same rights,
`parent = NULL`, `refs++` on the shared object. Used by the POSIX sidecar core
when two internal components must hold the same cap without derivation
coupling. Revoking `src` leaves the alias alive; revoking *both* (and the
kernel's region teardown) is what frees the underlying object.

### 3.4 `k_cap_revoke` — deep, deferred-in-flight

```
k_cap_revoke(handle) -> status | err
  NOREVOKE entries: CAP_ERR_RIGHTS
```

- Revocation walks the entry's subtree (sibling list, §1.3) and marks every
  descendant **dead**: generation bump, payload cleared, refs dropped.
  Subsequent uses of any dead handle fail with `CAP_ERR_REVOKED`.
- **In-flight grants are deferred, not killed.** If an entry has outstanding
  transient grants (it was passed as a message argument and the request has not
  completed — §4.3), the revoke marks the entry dead *now* (no new use) but the
  outstanding grant survives until its request completes or either side closes,
  then is reaped. Rationale: the receiver may be mid-write into a granted
  buffer; the kernel cannot allow the sender's revoke to pull memory out from
  under an in-flight copy. This is seL4's "revoke does not kill in-flight
  invocations" rule.
- Revoking the budget root is impossible by construction (`NOREVOKE`), so the
  whole sidecar cannot accidentally revoke its own foundation. (Sidecar
  teardown is the kernel's job, §6.3.)

### 3.5 `k_cap_info` — introspection

```
k_cap_info(handle) -> { type u16, rights u16, flags u16,
                        base u64, len u64 } | err
```

Read-only. Used by the sidecar core at boot (cross-checking the BootInfo block
against the live table) and by the VFS when deciding what it may do (e.g.,
whether the ramdisk channel permits SEND). Never leaks another sidecar's
entries — a handle is always relative to the caller's own table.

---

## 4. Capability arguments on channel messages

### 4.1 Wire format (identical to Phase 2 §5.2)

```
k_chan_send(chan, tag u32, flags u16, payload_ptr, payload_len,
            caps[] , n_caps u16) -> status | err
  caps[i]: { slot u32, offset u32, len u32,
             rights u8, flags u8, pad u16 }      // 16 bytes
  arg flags: bit0 PERSIST | bit1 MOVE | bit2 MAP
```

`tag` is the out-of-band request id (the Phase 2 protocol's `req_id` — the
kernel must know it for reply pairing, so it travels in the kernel envelope,
never embedded in payload). `flags`: `bit0 REPLY` marks a reply send (see §4.3).

Validation, in order, all-or-nothing (§1.4):

1. `chan` is a valid CHAN cap with `SEND` right, endpoint open — else
   `CAP_ERR_STATE`.
2. `payload_ptr/len` lie inside the sender's address space — else
   `CAP_ERR_RANGE`.
3. Each descriptor: `slot` is a live entry of type MEM (`CAP_ERR_NOTFOUND` /
   `CAP_ERR_REVOKED` / `CAP_ERR_TYPE`), `offset+len` inside the region
   (`CAP_ERR_RANGE`), `rights ⊆ held` (`CAP_ERR_RIGHTS`), entry not
   `NOTRANSFER` (`CAP_ERR_RIGHTS`).
4. `n_caps` ≤ 8 (v1 cap per message — small enough to make table bookkeeping
   trivial, large enough for every Phase 2 trace).
5. If `flags & REPLY`, the endpoint must have exactly one outstanding request
   whose tag matches — else `CAP_ERR_STATE`.

On success the kernel mints, for each descriptor, a derived entry in the
**receiver's** table with `rights = min(held, requested)` and a `parent` link
back to the sender's entry (so sender-side revocation can reach it, §3.4).

### 4.2 Grant modes

| Mode | Flag | Lifetime | Phase 2 use |
|---|---|---|---|
| **Transient** (default) | — | Bound to `(endpoint, tag)`; revoked at reply, either-side close, or explicit receiver revoke | `RD_READ`/`RD_WRITE` buffers (§5.2 of Phase 2 doc) |
| **Persist** | `bit0` | Detached from the pairing; lives until revoked/closed | `RD_MAP` image grant |
| **Move** | `bit1` | Persist-like, plus the **sender's** entry is revoked atomically with the mint | Driver surrendering its backing-store view |
| **Map** | `bit2` | Like persist, plus the region is mapped into the receiver's space (§5) | Isolation mode; no-op in the shared-space MVP |

- `PERSIST` and `MOVE` are mutually exclusive (`CAP_ERR_PROTO` otherwise).
- `MAP` implies `PERSIST` (a mapped view must outlive the request to be useful).
- `MOVE` is the only case where the sender loses rights; everything else is a
  copy (the sender keeps its cap and may revoke later — which, for a persist
  grant, kills the receiver's entry via the lineage link).

### 4.3 Transient-grant lifecycle — the reply-scoped rule

This is the operation the whole ramdisk protocol leans on, so it gets exact
rules. State per endpoint: the channel has a **request window** (v1: exactly
one outstanding request) and the kernel records, per outstanding tag, the list
of transient grants minted for it.

1. **Send (request):** sender includes transient descriptors. Kernel mints
   grants in the receiver's table, binds them to `(endpoint, tag)`, records the
   tag as the endpoint's outstanding request. A second non-REPLY send while one
   is outstanding fails `CAP_ERR_STATE` (this is the Phase 2 protocol's
   `window = 1`, §5.4, now enforced by the kernel, not just by convention).
2. **Reply:** receiver sends with the same `tag` and `REPLY`. Kernel mints any
   reply grants, then **revokes every transient grant bound to that tag**
   (the receiver's buffer grant dies here — the Phase 2 driver's W-only cache
   slot grant is reaped the moment its `RD_OK` goes out). The window reopens.
3. **Receiver-side early revoke:** the receiver may revoke a transient grant
   explicitly at any time (e.g., it detected a bad descriptor and bails before
   replying); the kernel then treats the reply as carrying zero usable grants.
   The sender's own cap is untouched — it only loses the *view* it gave away.
4. **Sender-side revoke while in flight:** deferred (§3.4) — the grant stays
   alive until the reply or close, then reaps.
5. **Close or peer death:** all grants bound to the endpoint, transient and
   persist alike, are revoked in the teardown sweep (§6.3).
6. **Malformed/aborted message:** if the kernel rejects the message at step 4/5
   of §4.1 validation, no grants were minted and nothing needs reaping — the
   all-or-nothing rule makes failure paths trivially leak-free.

Failure-path guarantee, stated as an invariant: **at every moment, the set of
live grants is exactly the set the kernel minted minus the set it has already
reaped; a grant cannot outlive (a) its request's reply, (b) the endpoint, or
(c) an explicit revoke.**

### 4.4 Message completion events for the receiver

`k_chan_recv(chan, buf, cap_slots[], n) -> { kind, len, n_caps, tag }` where
`kind = MSG | CLOSE`. A received message's granted caps appear in `cap_slots`
as handles in the *receiver's* table (transient or persist per their flags).
The receiver learns the tag so it can reply with matching `tag`. A `CLOSE`
event carries the reason (see §6) and zero caps.

---

## 5. Map-into-target-space with rights

### 5.1 Design decision: mapping is a transfer mode, not a syscall

The Phase 2 doc asked for "mapping a MEM cap into a target sidecar's address
space" (§1). This spec delivers it as **cap-argument flag `MAP`** plus a
kernel-internal mapping path — there is no separate `mem_map` syscall. Reasons:

- The only legitimate user-space reason to map into another sidecar is a
  deliberate, request-reply-shaped exchange (share this buffer, here is your
  image). That is precisely a channel message carrying a cap argument.
- It keeps "no ambient naming" airtight: the target is named by the channel cap
  the sender already holds, never by a sidecar id.
- Teardown collapses into the existing machinery: **revocation of the received
  cap unmaps the region**. No `mem_unmap` op, no orphaned mappings, one less
  failure mode.

### 5.2 Semantics

```
k_chan_send(chan_to_target, tag, flags, ..., caps[ { slot, ..., flags: PERSIST|MAP } ])
  -> base u64 (the mapped address, in the reply or in the sender's completion status)
```

- The kernel derives the target sidecar from `chan_to_target`'s peer — the
  sender cannot name a sidecar it has no channel to.
- The kernel mints a MEM entry in the target's table (rights `⊆ held`, per
  §4.1) **and** establishes an address-space mapping of the region in the
  target's space with exactly those rights. `base` is kernel-chosen from the
  target's free regions and returned to both sides (the sender via its
  completion status, the target because it can `cap_info` the received handle).
- **`X` never travels over channels.** A `MAP` descriptor requesting `X` is
  rejected (`CAP_ERR_RIGHTS`) unless the source cap was kernel-minted as an
  image cap AND the target's manifest permits the image to be executed there
  (image caps are `rx` from `create_sidecar`; executable mappings of a *data*
  cap are refused outright). W^X survives every transfer.
- The target's subsequent revocation of the mapped handle unmaps the region
  (mapping lifetime == cap lifetime). If the kernel tears the *sender* down,
  the lineage revoke (§3.4) also unmaps in the target.

### 5.3 Two implementation modes

The semantic contract is identical; only the page-table work differs:

- **Shared-space mode (Phase 2 MVP):** all sidecars share one address space;
  the "mapping" is identity. The kernel still mints the target's table entry
  with derived rights, so *syscall-mediated* enforcement (can this sidecar pass
  this cap on? can it derive W from an R grant?) works exactly as specified.
  What is *not* enforced in this mode: a malicious receiver can read a W-only
  grant as data (it shares the space) — the documented trust-domain limitation
  of Phase 2 §7.4.
- **Isolated mode (Phase 4):** per-sidecar page tables. `MAP` creates a real
  per-principal mapping with the granted rights; W-vs-R between sidecars becomes
  hardware-enforced, and a compromised receiver can no longer read its W-only
  grant. **Nothing else in this spec changes** — this is the design goal: the
  Phase 4 switch is a mapping change, not an interface change.

### 5.4 Budget and accounting rules

- **Budget is accounted per physical region, not per view.** Allocating
  charges the sidecar; deriving, dup'ing, transferring, and mapping do not.
  When sidecar A maps its region into sidecar B, the region stays charged to A.
  A malicious A cannot inflate B's accounting, and B cannot exhaust A by
  refusing maps — each sidecar's ceiling is its own.
- The kernel *does* account page-table memory for mappings in isolated mode
  against the **kernel's** own reserve, never against either sidecar's budget.
- Shared-space mode: no mapping memory, nothing to account.

### 5.5 W^X in the shared space

In shared-space mode the region's page tables are global, so the kernel must
prevent a region from being simultaneously mapped `W` and `X` anywhere: the
kernel tracks each region's aggregate permissions (union over all mappings and
the original) and refuses any `MAP` that would produce `W+X` in the shared
space. In isolated mode the check is per-space (a region may be `W` in A and
`X` in B, which is not a violation). The MVP's regions are disjoint by
construction (image `rx`, heap `rw`), so the rule is a backstop, not a burden.

---

## 6. Channel close events and reason codes

### 6.1 Close is terminal and observable

- Either side may call `k_chan_close(chan, reason, detail)`; the kernel
  delivers a close event to the peer and the endpoint becomes unusable: sends
  fail `CAP_ERR_STATE`, outstanding transient grants are reaped (§4.3 rule 5),
  persist grants are revoked, `MAP` mappings are unmapped.
- Close is idempotent: a second close or a close after peer death is a
  no-op returning `CAP_OK`.
- Close events are delivered in-band on the next `k_chan_recv` (`kind = CLOSE`,
  §4.4) so the receiving sidecar processes them at its own safe points (its
  event loop), never mid-operation — consistent with the Phase 2 cooperative
  model.

### 6.2 Reason code registry

`reason` is `u16`; `detail` is `u32` carrying protocol-specific context (e.g.,
the ramdisk driver's `RD_ERR_*` code). Well-known codes:

| Code | Name | Meaning | Typical `detail` |
|---|---|---|---|
| 0x0001 | `CLOSE_PEER` | Peer called `k_chan_close` normally (clean teardown) | 0 |
| 0x0002 | `CLOSE_PEER_DEAD` | Peer sidecar terminated — exit, fault, or kernel kill | exit code, or fault kind (fault/page fault, OOM/budget, panic) |
| 0x0003 | `CLOSE_REVOKED` | Channel cap revoked by the kernel or by the peer holding it | 0 |
| 0x0004 | `CLOSE_PROTO` | Protocol violation — malformed frame, bad cap descriptor, window misuse | offending error code (e.g., `RD_ERR_PROTO`) |
| 0x0005 | `CLOSE_ADMIN` | Kernel administration: sidecar restart, system shutdown, resource reclamation | 0 |
| 0x8000+ | (protocol-defined) | Reserved for protocol registries; e.g., ramdisk driver defines 0x8001..0x80FF | protocol-specific |

Rules:

- `CLOSE_PEER_DEAD` is generated by the kernel, never by a sidecar — a sidecar
  cannot fake another sidecar's death.
- Protocol registries (like the ramdisk `RD_*`) must use the 0x8000+ range and
  document their codes; well-known codes below 0x8000 are reserved.
- A receiver that cannot interpret a reason code treats it as
  `CLOSE_PROTO`-class: tear down, log, don't retry blindly.

### 6.3 Teardown ordering on sidecar death (the "no orphaned authority" path)

When the kernel terminates a sidecar (exit, fault, budget exhaustion, admin
kill) it performs, in order, with no preemption between steps:

1. Mark the sidecar dead and record the reason (exit code or fault kind).
2. **Revoke every entry in its table**, including outstanding transient grants
   it sent and grants it received (deferred-revoke does not apply here — the
   sidecar is gone, nothing is in flight).
   - This unmaps every `MAP` view it held and every view mapped *into* it
     (lineage from other sidecars' caps).
3. For every channel endpoint it held, deliver `CLOSE_PEER_DEAD` (reason in
   `detail`) to the peer — this is the ramdisk-driver-death notification the
   Phase 2 VFS relies on to mark mounts stale (§5.4 of the Phase 2 doc).
4. Free the table, the space, and the endpoint bookkeeping.

A sidecar that *calls* `k_chan_close` goes through the same sweep minus step 1
(reason = `CLOSE_PEER`, and peers get `CLOSE_PEER` not `CLOSE_PEER_DEAD`).

---

## 7. Error codes

Common to all operations in §3–§6. `u16`, stable ABI.

| Code | Name | Meaning |
|---|---|---|
| 0 | `CAP_OK` | success |
| 1 | `CAP_ERR_NOTFOUND` | handle index does not exist in this table |
| 2 | `CAP_ERR_REVOKED` | handle generation mismatch — entry dead (revoked/freed) |
| 3 | `CAP_ERR_RIGHTS` | insufficient rights: derive/dup/map/transfer would amplify, or `NOREVOKE`/`NOTRANSFER` violated |
| 4 | `CAP_ERR_RANGE` | offset/len outside the cap's region, or payload pointer outside the caller's space |
| 5 | `CAP_ERR_BUDGET` | allocation would exceed the sidecar's budget ceiling |
| 6 | `CAP_ERR_SPACE` | no free mapping address / mapping conflict (isolated mode) |
| 7 | `CAP_ERR_TARGET` | channel peer is not mappable or is gone |
| 8 | `CAP_ERR_STATE` | wrong state: send on closed endpoint, reply without matching outstanding request, window busy |
| 9 | `CAP_ERR_TYPE` | cap type does not match the operation |
| 10 | `CAP_ERR_PROTO` | malformed descriptor/message — rejected atomically, nothing minted |
| 11 | `CAP_ERR_NOMEM` | kernel out of memory for table/mapping bookkeeping |

---

## 8. Security invariants (mapped to Phase 2 §7.3)

| Invariant | Enforced by | Phase 2 payoff |
|---|---|---|
| No amplification; every grant is `min(held, requested)` | Kernel computes rights on every mint/derive/map (§3.2, §4.1) | A buggy coreutils applet cannot escalate an fd or cap it holds |
| Kernel is sole mint authority; no ambient naming | Table mutation only via kernel ops; handles are per-sidecar; targets named via channel caps (§1.2, §5.2) | A user program cannot mint caps or address the ramdisk driver directly |
| Transient grants die with their request | Reply-scoped auto-revoke + close sweep (§4.3, §6) | The driver never accumulates buffer rights; leaks are impossible by construction |
| Revocation reaches descendants (lineage) | Deep revoke with deferred in-flight rule (§3.4) | Sender-side revoke kills persist grants without yanking in-flight buffers |
| Close events on every teardown path | Kernel-generated `CLOSE_PEER_DEAD` etc. (§6) | VFS distinguishes driver crash from protocol error; survives driver death |
| W^X across transfers and mappings | `X` never over channels; aggregate-permission check in shared space (§4.2, §5.5) | Buggy code can corrupt data but cannot execute injected code |
| Budget is per physical region | Allocation-only accounting (§5.4) | Sidecar ceilings hold regardless of sharing/mapping patterns |

The Phase 2 trust-domain statement is unchanged and is *reinforced* by this
spec: the kernel enforces **sidecar-level** authority (the sidecar's table is
the sidecar's table — any code in the sidecar can use any entry in it). The
POSIX core's job of sub-dividing that authority among tasks (fds, permission
checks) is software discipline on top, exactly as §7.4 of the Phase 2 doc says.

---

## 9. Implementation notes

- **Handle validation fast path:** `cap_lookup(sidecar, handle)` =
  `index < table_len && gen matches` — two loads, no hash. Revoked entries
  leave the slot in place with a bumped generation so lookups fail cheaply and
  predictably.
- **Revocation algorithm:** recursive walk of the sibling list (§1.3); the
  deferred-in-flight rule means the walk additionally checks a per-entry
  "outstanding grants" count and, if nonzero, marks the entry for reaping at
  request completion instead of freeing now. Grant bookkeeping lives on the
  endpoint (`struct chan_req { tag, grant_list }`).
- **All-or-nothing message validation** is a two-pass: pass 1 validates all
  descriptors against the sender's table (no writes), pass 2 mints the
  receiver's entries and records the endpoint's grant bindings. A pass-1
  failure leaves nothing to roll back.
- **Window enforcement** is a single field on the endpoint (`outstanding_tag`)
  — v1's window=1 is O(1).
- **Where it lands:** `kernel/cap.c` (table, lineage, ops), `kernel/chan.c`
  extension (cap args, grants, close events), arch hook for isolated-mode
  mapping (`arch/*/mmap.c` — a stub returning identity in shared mode).
- **Testing:** the host-side test harness (per repo convention) should get a
  `cap_ops` unit test: derive/amplify rejection, deep revoke, transient
  auto-revoke on reply/close, MOVE atomicity, all-or-nothing validation,
  generation-based stale-handle rejection, and the death-sweep ordering.

---

## 10. Open questions

1. **Window > 1:** the tag-based grant binding generalizes to a per-tag grant
   table, but v1 deliberately locks `window = 1` (one outstanding request per
   endpoint) to keep the kernel bookkeeping O(1). Confirm the POSIX sidecar has
   no workload needing pipelined block requests before lifting this.
2. **Channel creation** is now specified in
   `docs/AeroSLS-Sidecar-Channels-Transport-Spec-v0.1.md`: four creation paths
   (kernel-bootstrap, manifest wiring at `create_sidecar`, `peer:"parent"` with
   `create_sidecar` returning a CHAN cap, and dynamic `k_chan_create` over an
   existing channel), the kernel send/recv envelope with its byte layout, and
   the transport guarantees. It also extends this spec's `k_chan_send` with a
   `timeout_ns` argument and `k_chan_recv` with an undersize no-loss rule.
3. **`MAP` on a budget root:** should a sidecar be able to map its *whole*
   budget into another sidecar (e.g., a future "shared heap" abstraction)?
   v1 says no (`NOTRANSFER` on the root); revisit when a workload asks.
4. **Cap argument count:** v1 caps `n_caps ≤ 8` per message. The Phase 2
   traces use at most 1–2; 8 is headroom. Raise only with window > 1, so the
   per-request bookkeeping stays bounded.
5. **Deferred-revoke edge:** a sender that revokes an in-flight grant and then
   *dies* before the reply — the sweep (§6.3 step 2) reaps the grant at close
   anyway; confirm the ordering (peer close event before grant reap) is what
   the VFS wants to see (it is, for Phase 2: mounts go stale only after the
   close event is delivered).
