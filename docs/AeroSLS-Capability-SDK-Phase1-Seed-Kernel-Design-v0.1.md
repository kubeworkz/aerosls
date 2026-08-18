# AeroSLS Capability SDK — Phase 1 "Seed Kernel" Design v0.1

**Status:** Design only — no code in this phase. Deliverables 1–5 from the
architecture brief, plus a concrete integration map for the existing tree.

---

## 0. Scope and ground rules

Phase 1 proves the *core capability lifecycle*: **create → transfer → map →
revoke → destroy**, with no leaks and no aliasing, on the smallest honest
kernel surface. Everything is a capability; nothing in user space can forge
one; revocation is total once it returns.

Target architecture: **x86-64, 4-level paging, 4 KiB pages** — the main build
(`make x86-iso`), with the existing ring-3 pipeline (`process.h`,
`arch/x86/user_paging.c`, the SYSCALL gate in `syscall.asm`). The capability
layer is written arch-neutral; only the map/unmap hooks are per-arch, exactly
the split `arch/{x86,riscv,arm64}` already enforces. RISC-V (SV39) and AArch64
(4K granule, 4-level) ports need only new hooks.

Three capability types for Phase 1:

| Type | Meaning |
| --- | --- |
| `CAP_MEM` | a window onto a kernel-managed physical memory region |
| `CAP_CHAN_R` | read end of a channel (drain messages + incoming caps) |
| `CAP_CHAN_W` | write end of a channel (send messages + caps) |

Deliberate Phase-1 cuts, named honestly up front: no derived/weakened caps, no
badges, no untyped memory, no COW, no MMIO caps, no DMA (arena pages are never
handed to devices), no capability persistence. Each of those is a Phase-2
item; see §12.

---

## 1. Capability word and kernel objects

A capability is a single **64-bit word** — small enough to move atomically,
opaque to user space, validated by the kernel on every use.

```
 63    61 60    57 56          40 39       32 31          16 15          4  3  0
+--------+--------+--------------+----------+--------------+-------------+-------+
| type   | state  | object id    | perms    | offset (pg)  | len (pg)    | rsrvd |
| 3 bits | 4 bits | 17 bits      | 8 bits   | 16 bits      | 12 bits     | 0     |
+--------+--------+--------------+----------+--------------+-------------+-------+
```

- **type** — `0 NONE (free slot)`, `1 MEM`, `2 CHAN_R`, `3 CHAN_W`.
- **state** — `0 VALID`, `1 IN_TRANSIT` (reserved for Phase-2 blocked send),
  `2 REVOKED` (stamped into queued words by revocation). A slot word with
  type `NONE` is a free slot.
- **object id** — index into the kernel's object table. 17 bits → 131072
  objects; Phase 1 caps at `CAP_OBJECT_MAX 1024`.
- **perms** — `MEM: R=1<<0 W=1<<1 X=1<<2 MAP=1<<3`; `CHAN: SEND=1<<0
  RECV=1<<1 CLOSE=1<<2`.
- **offset / len** — the cap is a *view* into the object: `[offset,
  offset+len)` in 4 KiB pages. Always validated against the object's bounds.
- **reserved** — must be zero; the kernel checks it. This is the software
  "tag" (the same idea as `tools/simi/*`'s capability-tag regions, enforced
  here by kernel validation rather than interpreter tagging).

The word references an object, never the address of one:

```c
struct CapHolder {          /* one per place that can touch the object */
    uint32_t kind;          /* HOLDER_SLOT (pid, slot) or HOLDER_QUEUE (chan, qidx) */
    uint32_t pid;           /* for SLOT holders */
    uint16_t slot;          /* for SLOT holders */
    uint16_t chan_id;       /* for QUEUE holders */
    struct CapHolder *next, *prev;   /* doubly-linked — MDB-style */
};

struct CapObject {
    uint32_t id;
    uint8_t  kind;          /* CAP_MEM or CAP_CHAN */
    uint8_t  revoking;      /* set at the revoke linearization point */
    uint32_t refcount;      /* == number of VALID holders (slots + queued words) */
    uint64_t max_perms;     /* object's maximum permission mask */
    union {
        struct {            /* CAP_MEM */
            uint64_t phys_base;      /* physical address of page 0 */
            uint32_t npages;         /* object length in pages */
        } mem;
        struct {            /* CAP_CHAN */
            uint16_t end0_pid, end1_pid;   /* owning processes */
            uint16_t qdepth[2];            /* current queue depths */
            struct ChanMsg q[2][CHAN_QUEUE_DEPTH];  /* two directional queues */
            uint32_t qhead[2], qtail[2];
            uint32_t waiter_pid;         /* 0 = nobody blocked */
            uint8_t  wake;               /* set by send; consumed by scheduler */
        } chan;
    } u;
    struct CapHolder holders;   /* list head; invariant: refcount == walk length */
};

struct CapObject cap_objects[CAP_OBJECT_MAX];   /* id-indexed; free list chained in id */
```

The **holder list is the single source of truth** for "who can reach this
object". `refcount == number of VALID holders`, mutated only under the object
lock. Nothing is freed until the list is empty.

---

## 2. Capability table (per process)

```c
#define CAP_TABLE_ENTRIES 512     /* fd-like small-integer index space */

struct CapSlot  { uint64_t word; };
struct CapMap   {                /* one record per mapping derived from a cap */
    uint32_t obj_id;             /* keyed by object, NOT by slot: slots get reused */
    uint16_t cap_slot;
    uint64_t vaddr;
    uint16_t npages;
    uint16_t active;
};

struct CapTable {
    struct spinlock lock;                    /* IRQ-save spinlock */
    uint32_t pid;
    uint16_t freelist_head;                  /* free slots chained via word[56:40] */
    struct CapSlot slots[CAP_TABLE_ENTRIES]; /* 512 * 8 B = 4 KiB per process */
    struct CapMap  maps[CAP_MAP_MAX];        /* 64 records, ~1.5 KiB */
};

struct CapTable cap_tables[PROC_MAX];        /* indexed like proc_table[] */
```

- **Index mapping is a direct array index** — `slot i` in `cap_tables[pid]`.
  No hashing, no radix tree: for 512 slots the flat array is the fastest and
  simplest thing, and it matches this codebase's fixed-size-table posture
  (`ipc_queues[]`, `mq_table[]`, `proc_table[]`).
- **Allocation**: freelist of free slots; `cap_alloc_slot()` pops the head.
  Free slots chain their next index in the word's `[56:40]` field (type
  `NONE` marks free, so a slot can never be mistaken for a live cap). The
  freelist exists because Phase 1 has **revocation and reuse** — unlike the
  bump-allocated, never-reclaimed tables this repo uses elsewhere, capability
  slots must recycle or an fd-like index space exhausts.
- **Every slot mutation happens under the owning table's spinlock.** The
  kernel is SMP (`kernel/smp.c`; QEMU boots `-smp 2`), so plain C stores are
  not atomic against another core walking the same table.
- Per-process map registry lives *in* the table so revocation can find and
  tear down mappings under the same lock that guards the slots.

### 2.1 Locking strategy

Three lock domains, one global order. Deadlock-freedom is by construction:

```
table lock  <  channel lock  <  object lock
```

| Operation | Locks acquired (in order) |
| --- | --- |
| `cap_send` | sender table → channel → receiver table → object |
| `cap_recv` | channel → receiver table → object |
| `cap_map` / `cap_unmap` | own table (page tables + map registry) |
| `cap_revoke` | object (snapshot + set `revoking`) → **release** → per-holder table locks, one at a time → object again |
| `cap_create_mem` / `cap_arena_alloc` | object (new) |

Rules that make this safe:

1. **Never hold two table locks at once.** Revoke walks holders one table at
   a time; send holds sender table then receiver table (both below channel
   and object, so no cycle is possible with revoke, which holds object
   *outside* its table acquisitions).
2. **No blocking while holding a lock.** Locked sections contain only
   validation and word/queue writes. Sender-side blocking (queue full) and
   receiver-side blocking (queue empty) both happen *after* releasing locks.
3. **Refcount mutations only under the object lock.** The one exception —
   the *final* refcount subtraction in revoke — happens under a re-acquired
   object lock, so destruction still serializes with every holder move.

Compare: **seL4** uses a single kernel-wide lock (MCS) with preemption
points, and serializes capability operations against the capability
derivation tree (MDB). A per-table lock with a fixed order is strictly
weaker than seL4's global lock only in that two processes' independent
tables never contend — which is the point. **EROS** (and KeyKOS) were
single-CPU systems; EROS2's SMP plan was per-object locking with a
two-phase rendezvous protocol, which is exactly the shape this design
takes for the object domain while keeping Phase 1's simple spinlocks.

### 2.2 Revocation: immediate, with a deferred-cleanup protocol

Phase 1 chooses **immediate revocation with total post-return guarantee**:
when `cap_revoke` returns, no VALID capability to the object exists
anywhere, all holders' PTEs derived from it are torn down, and the object
(and its arena frames) are freed.

Why immediate, given the trade-offs:

- **seL4** (`seL4_CNode_Revoke`) walks the MDB subtree and deletes every
  derived cap; the guarantee is total and the cost is O(holders) — worst case
  O(entire capability tree). This is the correctness bar Phase 1 copies.
- **EROS** uses deferred revocation: a checkpoint marks the object and
  stale copies are caught at map time. Its own documentation is honest
  that *redemption* (a capability whose object was re-created after GC) can
  resurrect a supposedly-revoked capability — a subtlety a "prove the
  lifecycle" phase should not inherit.
- **KeyKOS** revokes promptly within a single kernel via space-bank
  accounting and capability-list cleanup, but that assumes single-CPU
  kernel context and per-object caches the Phase-1 kernel does not have.

The cost of immediate revocation is O(holders) table walks and O(holders)
PTE teardown — acceptable at 512-slot tables; revisit *delayed* revocation
(generation-stamped slots, epoch-based reclamation queue) when tables grow
in Phase 2. The generation counter is **not** needed in Phase 1 because the
object lock serializes every holder move (see §3.2); a `revoking` flag
suffices.

---

## 3. The transfer protocol (atomicity, double-free, use-after-free)

### 3.1 Invariants

1. **A slot is in exactly one state at any instant**: `FREE` (type `NONE`),
   `VALID`, or `IN_TRANSIT`. Only the owning table's lock can change it.
2. **`refcount == number of VALID holders`** — VALID slots in tables plus
   VALID cap words sitting in channel queues. It is mutated only under the
   object lock (or by revoke's final batch, also under the object lock).
3. **An object is freed only under the object lock, and only when
   `refcount == 0`.** Therefore no slot write can ever target a freed
   object: every slot install happens under the object lock (send, recv)
   or against a snapshot taken under it (revoke), and the object cannot
   reach refcount 0 while a write is in flight.
4. **A queued cap word is a holder.** Between send and recv the object's
   holder is the queue entry, not a process table — so revocation can find
   and kill in-flight caps.
5. **`revoking` is the linearization point.** Once set (under the object
   lock), no transfer may install a new holder. A transfer that already
   holds the object lock completes; one that arrives after fails. Both are
   consistent with "revoke took effect at that instant".

Double-free is impossible because the VALID→FREE transition is the *only*
path back to the freelist, and it is guarded by the table lock and paired
with exactly one refcount decrement (or zero decrement on a send, where the
holder moves to the queue). Use-after-free is impossible because object
destruction requires refcount 0, and every reference to the object (slot
install, holder walk, PTE install) is serialized with the refcount mutation
by the object lock.

### 3.2 `cap_send` kernel path

```
cap_send(ch_w, cap_index, cookie)                       // syscall 292
  pid = current_pid
  T_s  = &cap_tables[pid]
  T_r  = NULL, obj = NULL, q = NULL

  // ── Phase A: validate the write end, under the sender's table lock ──
  lock(T_s.lock, irqsave)
  w = T_s.slots[ch_w].word
  if type(w) != CAP_CHAN_W or state(w) != VALID or !(perms(w) & SEND):
      unlock(T_s.lock); return -EBADF

  chan = &cap_objects[objid(w)].u.chan
  // find the directional queue: my end's write side targets the far end's read side
  q = (pid == chan.end0_pid) ? &chan.q[1] : &chan.q[0]

  // cap payload present?
  has_cap = (cap_index != CAP_NONE)
  if has_cap:
      sw = T_s.slots[cap_index].word
      if type(sw) == CAP_NONE: unlock(T_s.lock); return -EINVAL   // free slot
      if state(sw) != VALID:    unlock(T_s.lock); return -EINVAL  // IN_TRANSIT/REVOKED
      obj = &cap_objects[objid(sw)]

  // ── Phase B: queue space check (fail before any mutation) ──
  lock(chan_lock)                          // channel lock: order says table < channel
  if qdepth(q) == CHAN_QUEUE_DEPTH:
      unlock(chan_lock); unlock(T_s.lock); return -EAGAIN

  if has_cap:
      // ── Phase C: the atomic holder move, under the object lock ──
      lock(T_r.lock)                        // receiver's table (below channel in order)
      rslot = cap_alloc_slot(T_r)           // freelist pop; may fail
      if rslot < 0:
          unlock(T_r.lock); unlock(chan_lock); unlock(T_s.lock); return -ETABLEFULL
      lock(obj.lock)                        // order: table < channel < object ✓
      if obj.revoking:
          cap_free_slot(T_r, rslot)         // undo the reservation
          unlock(obj.lock); unlock(T_r.lock); unlock(chan_lock)
          unlock(T_s.lock); return -ECAPREVOKED
      // swap holders: sender slot FREE (-0 refcount), receiver slot VALID (+0)
      T_s.slots[cap_index].word = 0         // VALID -> FREE
      holder_remove(&obj.holders, HOLDER_SLOT(pid, cap_index))
      holder_insert(&obj.holders, HOLDER_SLOT(T_r.pid, rslot))
      T_r.slots[rslot].word = sw            // install the SAME word (view travels)
      unlock(obj.lock)
      // note: sender's map registry is NOT touched — see §5.1; mappings
      // persist until unmap or revoke, matching seL4/EROS map-cap semantics
  else:
      rslot = CAP_NONE

  // ── Phase D: publish to the queue and wake the receiver ──
  msg = { .cookie = cookie, .cap_word = (has_cap ? sw : 0), .has_cap = has_cap }
  enqueue(q, msg)                           // qtail++, qdepth++  (under chan lock)
  if chan.waiter_pid != 0 and !chan.wake:
      chan.wake = 1                         // consumed at next tick by the scheduler
  unlock(chan_lock); unlock(T_r.lock or none); unlock(T_s.lock)

  // ── Phase E: optional blocking sender (Phase 1: fail instead) ──
  // if we held the queue-full case above, a Phase-2 variant parks the sender
  // here with cap_index marked IN_TRANSIT and retries on wake.
  return 0   // receiver will get cap_index rslot in ITS table when it recvs
```

Notes:

- The receiver's slot is allocated and the cap installed **before** the
  queue entry is published, all under the channel lock — so no receiver can
  observe a queue entry whose cap is not already safely installed. If the
  queue is full, nothing has changed anywhere (fail-before-any-mutation,
  this repo's house rule).
- The receiver is **not told** which slot it will get; `cap_recv` fills it
  (see below). The sender's `cap_index` is dead the moment Phase C commits.
- **Revoke during transfer**: if revoke's `revoking` flag is observed, the
  receiver slot reservation is rolled back and the sender keeps its cap —
  the message is simply not sent. If revoke runs after Phase C, it finds
  the *receiver's* slot on the holder list and kills it there. No window
  exists where the object is freed under an in-flight install, because the
  install holds the object lock and destruction requires it.

### 3.3 `cap_recv` kernel path

```
cap_recv(ch_r, *cookie_out, block)                  // syscall 293; returns cap index or -err
  pid = current_pid
  lock(T.lock, irqsave)
  r = T.slots[ch_r].word
  if type(r) != CAP_CHAN_R or state(r) != VALID or !(perms(r) & RECV):
      unlock(T.lock); return -EBADF
  chan = &cap_objects[objid(r)].u.chan
  q = (pid == chan.end0_pid) ? &chan.q[0] : &chan.q[1]

  lock(chan_lock)
  if qdepth(q) == 0:
      if !block:
          unlock(chan_lock); unlock(T.lock); return -EAGAIN
      // Phase 1 blocking: park on the channel, wake at next tick (see §3.4)
      chan.waiter_pid = pid
      unlock(chan_lock); unlock(T.lock)
      park_current_process()               // -> scheduler resumes us later
      retry from the top                    // wake flag was consumed; queue now non-empty

  msg = peek(q)                             // do NOT dequeue yet
  lock(obj_of(msg).lock or none)            // object lock if msg.has_cap
  if msg.has_cap:
      // re-read the queued word UNDER the object lock — this is what
      // serializes us against revoke's REVOKED store (revoke holds the
      // object lock when it stamps queue words)
      if state(msg.cap_word) == REVOKED:
          dequeue(q)                        // drain the dead entry (refcount already
          unlock(obj.lock); unlock(chan_lock); unlock(T.lock)
          return -ECAPREVOKED               //        accounted by revoke — no double-count)
      if obj.revoking:
          // raced revoke between peek and lock: treat as revoked, same drain
          dequeue(q); unlock(obj.lock); unlock(chan_lock); unlock(T.lock)
          return -ECAPREVOKED
      nslot = cap_alloc_slot(T)             // into the RECEIVER's own table
      if nslot < 0:
          unlock(obj.lock); unlock(chan_lock); unlock(T.lock)
          return -ETABLEFULL                // message stays queued; try again later
      holder_remove(&obj.holders, HOLDER_QUEUE(chan_id, qidx))
      holder_insert(&obj.holders, HOLDER_SLOT(pid, nslot))
      T.slots[nslot].word = msg.cap_word    // queue holder -> slot holder (net 0 refcount)
      unlock(obj.lock)
  else:
      nslot = CAP_NONE
  dequeue(q)                                // now safe: cap is installed
  *cookie_out = msg.cookie
  unlock(chan_lock); unlock(T.lock)
  return nslot                              // fd-like: the new cap index, or CAP_NONE
```

Notes:

- **The receiver's table lock is the same lock that guards `cap_alloc_slot`**
  — and it is held across the peek/dequeue, so a concurrent `cap_send` from
  another core enqueues a strictly later message (FIFO is preserved).
- If the receiver's table is full, the message stays queued — nothing is
  lost, no partial state, retry semantics.
- `-ECAPREVOKED` on a drained entry is the receiver seeing exactly what
  happened: the capability was revoked while in flight. Refcount is not
  double-counted because the REVOKED stamp and its refcount subtraction
  happen together under the object lock in revoke (§3.4), and recv only
  ever *moves* a VALID word.

### 3.4 Blocking, wake, and the one new scheduler primitive

This kernel's scheduler is tick-driven: `schedule_ring3()` scans
`PROC_SUSPENDED` processes on every timer interrupt (`process.c`). Phase 1
fits blocking recv into that model with **one new per-process field and no
scheduler rewrite**:

- The channel gets `waiter_pid` (single waiter — channels are 1:1
  endpoints, so this is not a limitation) and a `wake` flag.
- A blocking `cap_recv` on an empty queue parks the caller by returning
  through the syscall path in the suspended state and recording
  `waiter_pid`. `cap_send` sets `chan.wake = 1` under the channel lock.
- At the next tick, `schedule_ring3()`'s scan treats `wake` as making the
  waiter runnable; on resumption the syscall's retry loop re-checks the
  queue. **Wake latency is therefore bounded by one tick** (1–10 ms at
  100 Hz–1 kHz) — honest, and exactly the level of mechanism this kernel
  already has.
- Phase 2 replaces this with an immediate wake (self-IPI / `kick_cpu` in
  `kernel/smp.c`) when a real wait-queue lands.

Non-blocking recv (`block=0`, `-EAGAIN`) needs **zero** scheduler changes and
is the Phase-1 default; the test in §8 uses blocking and notes the poll
fallback.

### 3.5 `cap_revoke` kernel path (sketch)

```
cap_revoke(cap_index)                        // syscall 294
  pid = current_pid
  lock(T.lock); w = T.slots[cap_index].word
  if type(w) == CAP_NONE or state(w) != VALID: unlock(T.lock); return -EINVAL
  obj = &cap_objects[objid(w)]

  lock(obj.lock)
  if obj.revoking: unlock(obj.lock); unlock(T.lock); return -EALREADY
  obj.revoking = 1                            // ← linearization point
  snapshot = copy(obj.holders)                // cheap: Phase-1 holder counts are tiny
  n_holders = refcount(obj)
  unlock(obj.lock)                            // drop BEFORE taking any table lock

  for each holder h in snapshot:              // one table lock at a time — no nesting
      lock(cap_tables[h.pid].lock)
      if h.kind == SLOT:
          cap_tables[h.pid].slots[h.slot].word = 0       // VALID -> FREE
          cap_unmap_object(cap_tables[h.pid], obj.id)    // tear down that process's PTEs
      unlock(cap_tables[h.pid].lock)

  lock(obj.lock)
  // stamp REVOKED into queued words and subtract their refcount, all under
  // the object lock — serialized against every recv's re-read (§3.3)
  for each QUEUE holder h in snapshot:
      chan = &cap_objects[h.chan_id].u.chan
      atomic_store64(&chan.q[dir][h.qidx].cap_word, stamp_REVOKED(word))
  obj.refcount -= n_holders                    // includes queued words
  if obj.refcount == 0:
      free_object(obj)                         // arena frames returned (§5.2)
  unlock(obj.lock)
  return 0
```

The one subtlety to keep straight: revoke marks *queued* words REVOKED under
the object lock (so recv's re-read serializes), but zeroes *slot* words under
each table lock (so it never needs to nest). A transfer cannot be
half-observed because every holder move is itself inside the object lock.

---

## 4. Channels

A channel is a `CAP_CHAN` object: two symmetric ends, each end owned by one
process, each end holding exactly one `CAP_CHAN_R` (its incoming queue) and
one `CAP_CHAN_W` (its outgoing queue). Two directional FIFO queues of
fixed-size messages:

```c
struct ChanMsg {
    uint64_t cookie;      /* user-defined 64-bit tag */
    uint64_t cap_word;    /* the MOVED capability word, or 0 */
    uint8_t  has_cap;
    uint8_t  _pad[7];
};                        /* 24 B; CHAN_QUEUE_DEPTH 16 → 384 B/queue */

#define CHAN_QUEUE_DEPTH 16
```

- `chan_create()` (syscall 291) mints the object and returns **two** cap
  indices: the caller's `CHAN_R` and `CHAN_W`. The far end's two caps are
  handed to the other process via `cap_send` — Phase 1 has no endpoint-
  handoff syscall; sending `CHAN_R`/`CHAN_W` caps across an existing
  channel is how endpoints migrate (which is itself a nice demo of
  capability transfer).
- Both queues are preallocated in the object (no allocation in the hot
  path), matching `IPCQueue`'s fixed-array convention in `ipc.h`.
- A channel is bidirectional by construction: each direction is one queue.
  Capabilities can flow in both directions, atomically, with messages —
  that is the "atomic transfer" guarantee: the cap and its cookie arrive
  together or not at all.
- Endpoint death: when a process holding an endpoint exits, its channels
  are drained: queued caps are revoked (refcount decremented), the
  surviving endpoint's caps are marked invalid, and the object is freed at
  refcount 0. Process exit therefore walks the process's holder list
  (it is a holder for every object it references) and calls the same
  cleanup as revoke per object.

---

## 5. Memory model

### 5.1 Representation

A `CAP_MEM` capability grants a **physical** window: the object stores
`phys_base` (physical address) and `npages`; the cap word stores
`offset` and `len` in pages plus a permission mask. The holder's mapping
is created on demand by the kernel from the cap's view — the holder never
sees or supplies physical addresses. This is the KeyKOS/EROS "segment cap
+ map" shape, versus seL4's VSpace-as-capability-tree; Phase 2 can adopt
seL4-style mapping caps if fine-grained map revocation is needed.

**Mappings outlive the cap's residence.** A mapping is a side effect
recorded in the *mapping process's* table (`CapMap` records keyed by
object id), not a property of the cap slot. Sending the cap away does not
unmap the sender — matching seL4 (deleting a frame cap does not delete
mapping caps) and EROS (maps are separate derived capabilities). Access
rights therefore persist for a process that mapped before sending; the
backstop is **revocation**, which tears down every holder's mappings
unconditionally. This is the deliberate Phase-1 reading of the brief's
"map on demand": a MEM cap transferred to a new holder becomes mappable in
*that* holder's address space immediately; the old holder keeps what it
already mapped until unmap or revoke. Named as a scope decision, not an
accident.

### 5.2 The shared-memory arena

```c
#define CAP_ARENA_SIZE      (64 * 1024 * 1024)   /* 16384 pages */
#define CAP_ARENA_FRAMES    (CAP_ARENA_SIZE / 4096)
#define CAP_ARENA_BASE      0x100000000ULL        /* 4 GiB: above the identity map */

uint8_t  cap_arena_bitmap[CAP_ARENA_FRAMES / 8];  /* 1 = owned */
uint32_t cap_arena_owner[CAP_ARENA_FRAMES];       /* owning object id, 0 = free */
```

- Reserved once at boot, after `frame_pool_init()`: the arena's 16K frames
  are marked reserved in `physical_memory_bitmap` so the general allocator
  (`allocate_physical_ram_frame`) never hands them out, and the arena is
  carved as one physically contiguous 64 MiB region (aligned to 2 MiB so a
  later phase can use huge pages).
- **Allocator**: simple bitmap with first-fit run search —
  `cap_arena_alloc(npages)` returns a contiguous run whose frames are all
  owner-free, marks each frame owned by the new object id, and mints the
  MEM cap in the caller's table. O(arena) worst case, O(run) typical —
  fine at this size; a buddy allocator is Phase 2.
- **No aliasing**: `cap_create_mem(phys_base, len, perm)` validates the
  range against `cap_arena_owner` (and against the kernel image + general
  RAM boundaries) and rejects any overlap with an existing owner. One
  frame, one object, one refcount trail. This is what makes "the new
  holder can map it safely" a kernel-enforced fact rather than a
  convention: the kernel can never be asked to map two different objects
  onto the same physical frame.

### 5.3 map / unmap flow

```
cap_map(cap_index, vaddr, flags)              // syscall 295; flags ⊆ cap perms
  lock(T.lock)
  w = T.slots[cap_index].word
  validate: type(w) == CAP_MEM, state(w) == VALID,
            flags & CAP_PERM_MAP, (flags & ~perms(w)) == 0,      // no perm escalation
            vaddr aligned, vaddr + len*4096 within user half,
            map records not full
  obj = &cap_objects[objid(w)]
  lock(obj.lock)
  if obj.revoking: unlock both; return -ECAPREVOKED   // dead cap, refuse to map
  // install PTEs: pml4 = proc_table[pid].cr3 (identity-mapped in kernel)
  for i in 0..len-1:
      pte = PTE(phys_base + (offset+i)*4096,
                R|W|X from (flags & obj.max_perms), USER bit, NX as needed)
      user_map_page(pml4, vaddr + i*4096, pte)          // arch/x86/user_paging.c
  record CapMap { obj_id, cap_slot, vaddr, len }
  unlock(obj.lock); unlock(T.lock)
  flush_tlb()                                            // CR3 reload (qemu_sls_flush_tlb)
  return 0

cap_unmap(cap_index, vaddr)                   // syscall 296
  lock(T.lock)
  find CapMap record by (cap_slot or obj_id, vaddr); if none: -EINVAL
  for i in 0..npages-1: clear PTE (walk PML4 via arch hook)
  drop record; flush_tlb(); unlock(T.lock); return 0
```

Why the object lock on map: a map racing a revoke must not install PTEs
for a revoked object. `revoking` is checked under the object lock, so map
either completes before the revoke linearization point (and its PTEs are
then torn down by revoke's `cap_unmap_object`) or fails cleanly.

The PTE flags come from `flags ∩ obj.max_perms`: a cap minted with only
`R` can never produce a writable mapping, even if the holder asks. The
kernel, not the holder, picks the physical address. TLB correctness uses
the kernel's existing full-CR3 reload; per-page `invlpg` and PCID/ASID are
a Phase-2 optimization (see §10).

---

## 6. API surface (Phase 1)

Seven syscalls plus one introspection call. Numbers **289–297** are the next
free range (confirmed via grep across every `kernel/*.h` `SYS_SLS_*` define:
288 is the current maximum, `SYS_SLS_RECONCILE_ENABLE`; 275–288 are taken by
storage/conn quotas, cluster init, service registry, workloads). Follows the
repo's single-arg `do_syscall(num, void* arg)` ABI with struct requests.

| # | Syscall | Request | Returns |
| --- | --- | --- | --- |
| 289 | `SYS_SLS_CAP_CREATE_MEM` | `{phys_base, npages, perm}` | cap index or -err |
| 290 | `SYS_SLS_CAP_ARENA_ALLOC` | `{npages, perm}` | cap index (frames from arena) or -err |
| 291 | `SYS_SLS_CHAN_CREATE` | `{}` | `{rd_idx, wr_idx}` packed in 64-bit return |
| 292 | `SYS_SLS_CAP_SEND` | `{ch_w_idx, cap_idx_or_NONE, cookie}` | 0 or -err |
| 293 | `SYS_SLS_CAP_RECV` | `{ch_r_idx, block, *cookie_out}` | received cap index, `CAP_NONE`, or -err |
| 294 | `SYS_SLS_CAP_REVOKE` | `{cap_idx}` | 0 or -err |
| 295 | `SYS_SLS_CAP_MAP` | `{cap_idx, vaddr, flags}` | 0 or -err |
| 296 | `SYS_SLS_CAP_UNMAP` | `{cap_idx, vaddr}` | 0 or -err |
| 297 | `SYS_SLS_CAP_LIST` | `{}` | prints tables to serial (debug; mirrors `sys_sls_proc_list`) |

Error convention: negative errno-style codes (`-EBADF -EINVAL -EAGAIN
-ETABLEFULL -ECAPREVOKED -EALREADY -ENOMEM`), returned through the
`uint64_t` ABI widened per this repo's convention.

### 6.1 SDK shape (the "sidecar")

Phase 1's user-space SDK is a thin `libaerocap` (in `user/`): one C wrapper
per syscall plus a header defining `cap_t` as `int32_t` (fd-like), the
`CAP_PERM_*` mask, and the errno codes. The sidecar pattern itself — a
broker process holding capabilities on behalf of clients, proxying them
over channels — is a Phase-2 demonstration built *on* this syscall
surface; the Seed Kernel only guarantees the primitives.

---

## 7. Integration map for the existing tree

| Touch point | Change |
| --- | --- |
| `kernel/cap.h` / `kernel/cap.c` (new) | capability layer: table, objects, arena, channels, transfer, revoke, map |
| `kernel/process.h` / `process.c` | `cap_tables[PROC_MAX]` extern; block/wake plumbing for recv |
| `kernel/syscall_dispatch.c` | seven `case SYS_SLS_CAP_*` entries + `#include "cap.h"` |
| `kernel/frame_pool.c` | reserve the arena range at boot (after `frame_pool_init`), like `frame_pool_reserve_range` |
| `arch/x86/user_paging.c` | export `user_unmap_page` (clear one PTE); reuse `user_map_page`, `qemu_sls_flush_tlb` |
| `arch/{riscv,arm64}` | the same two hooks later |
| `kernel/ipc.c` / `ipc.h` | reuse the fixed-queue + atomic-count conventions; the cap channel supersedes `ipc_user_queues[]` for capability traffic (plain IPC stays untouched) |
| `docs/COMMANDS.md` | shell commands `cap list`, `chan create`, `cap send`/`recv`/`revoke`/`map`/`unmap` for the Ring-0 shell |

---

## 8. User-space test scenario (deliverable 4)

Thread A and Thread B in two processes, one channel. Send is a **move**, so
the test is honest about it: B gets the only cap, maps, writes, sends the
cap **back**, A maps and reads. This also exercises both channel
directions. (If mappings persisted across send were relied on instead, the
one-way variant works too — but the return trip is the stronger proof.)

```
// ---- Thread A ---------------------------------------------------------
cap_t ch_rd, ch_wr;
chan_create(&ch_rd, &ch_wr);              // A owns both ends initially
// hand B its read end: send the CHAN_R cap, plain message, no payload
cap_send(ch_wr, /*cap=*/ch_rd, /*cookie=*/0xC0FFEE);   // ch_rd now belongs to B
                                           // (A must keep its CHAN_W to talk to B;
                                           //  A's own read end comes from a second
                                           //  channel or the return trip below)

cap_t mem = cap_arena_alloc(/*npages=*/1, CAP_PERM_R | CAP_PERM_W | CAP_PERM_MAP);
cap_send(ch_wr, /*cap=*/mem, /*cookie=*/0xDEAD);       // move the MEM cap to B
// mem is now FREE in A's table; A holds no cap to the page

cap_index_t got = cap_recv(ch_rd, &cookie, /*block=*/1); // waits; B sends it back
assert(got != CAP_NONE && cookie == 0xBEEF);

uint8_t *va = (uint8_t*)0x70000000;                    // a free user VA (A's table)
cap_map(got, (uint64_t)va, CAP_PERM_R | CAP_PERM_W);   // map at A's chosen VA
assert(strcmp((char*)va, "Hello") == 0);               // A READS WHAT B WROTE
cap_unmap(got, (uint64_t)va);
cap_revoke(got);                                       // destroy; frames → arena

// ---- Thread B ---------------------------------------------------------
cap_t ch_rd;                                           // B's read end (moved to B)
// B's table already contains ch_rd (it was sent to B while B was blocked in recv)
cap_index_t m = cap_recv(ch_rd, &cookie, /*block=*/1); // gets the MEM cap, index m
assert(cookie == 0xDEAD);

uint8_t *va = (uint8_t*)0x70000000;                    // B's own address space
cap_map(m, (uint64_t)va, CAP_PERM_R | CAP_PERM_W);     // B maps the same physical page
memcpy(va, "Hello", 6);                                // B WRITES
cap_send(ch_rd /*B's read end is B's WRITE side? no —*/,
         ...);
```

Convention fix: with `CHAN_R`/`CHAN_W` split caps, each process holds one of
each. In the one-channel variant, A holds `CHAN_W` (outgoing), B holds
`CHAN_R` (incoming); B needs its own `CHAN_W` to send back — so the setup
creates **two channels** (A→B and B→A), or the test uses the second
direction of one channel with A owning both ends' caps and moving only the
payload cap. The cleanest Phase-1 shape: **one channel, A keeps both ends,
B receives only the MEM cap** — the return trip is then B sending the cap
back *through the same channel* using a `CHAN_W` cap that A transfers to B
in the first message (caps can transfer channel caps — this is exactly the
endpoint-handoff property from §4):

```
// ---- Thread A ---------------------------------------------------------
cap_t rd, wr;
chan_create(&rd, &wr);                        // A: both ends
cap_t mem = cap_arena_alloc(1, R|W|MAP);
// send B its write end AND the memory cap in two messages (or one? one cap
// per message in Phase 1):
cap_send(wr, /*cap=*/rd, 0x1);                // move CHAN_R end to B (B now reads)
cap_send(wr, /*cap=*/mem, 0x2);               // move MEM cap to B
cap_index_t got = cap_recv(rd, &cookie, 1);   // blocks until B sends back
uint8_t *va = ...; cap_map(got, va, R|W);
assert(memcmp(va, "Hello", 6) == 0);

// ---- Thread B ---------------------------------------------------------
cap_index_t c1 = cap_recv(rd, &cookie, 1);    // CHAN_R cap — B ignores or stores
cap_index_t c2 = cap_recv(rd, &cookie, 1);    // MEM cap
cap_map(c2, vaB, R|W); memcpy(vaB, "Hello", 6);
cap_send(wr /*B now holds A's old CHAN_W? no — B holds rd which is CHAN_R...*/);
```

Resolved cleanly: **A sends B the `CHAN_W` end instead** — B then owns the
write end and can send the cap back; A keeps `CHAN_R` and reads the reply.
The final test pseudocode:

```
// ---- Thread A ---------------------------------------------------------
cap_t rd, wr;
chan_create(&rd, &wr);                          // A: CHAN_R (read) + CHAN_W (write)
cap_t mem = cap_arena_alloc(1, R|W|MAP);        // 4 KiB from the arena
cap_send(wr, /*cap=*/wr, /*cookie=*/1);         // A's CHAN_W moves to B → B can reply
cap_send(wr, /*cap=*/mem, /*cookie=*/2);        // MEM cap moves to B
// A's table now: { rd }  — no CHAN_W, no MEM

cap_index_t got = cap_recv(rd, &cookie, /*block=*/1);   // blocks; B replies
assert(cookie == 0xBEEF && got != CAP_NONE);
uint8_t *va = (uint8_t*)0x40000000;                    // free user VA in A
cap_map(got, (uint64_t)va, CAP_PERM_R | CAP_PERM_W);
assert(memcmp(va, "Hello", 6) == 0);                   // ★ A reads B's write
cap_unmap(got, (uint64_t)va);
cap_revoke(got);                                         // refcount → 0; frames freed
assert(cap_list_self_count() == 1);                      // only `rd` remains: no leaks

// ---- Thread B ---------------------------------------------------------
cap_index_t w = cap_recv(rd, &cookie, 1);       // the CHAN_W cap (cookie 1)
cap_index_t m = cap_recv(rd, &cookie, 1);       // the MEM cap   (cookie 2)
uint8_t *va = (uint8_t*)0x40000000;             // B's own address space
cap_map(m, (uint64_t)va, CAP_PERM_R | CAP_PERM_W);
memcpy(va, "Hello", 6);                         // ★ B writes
cap_send(w, /*cap=*/m, /*cookie=*/0xBEEF);      // move the cap back to A
cap_revoke(w);                                   // drop B's channel write end
// B's table: empty. Kernel assert: cap_objects[mem].refcount == 0,
// arena frame owner == 0  →  the full lifecycle closed without a leak.
```

This is the Seed Kernel's acceptance test: **create → transfer → map →
write/read → unmap → revoke**, with `refcount` and arena ownership asserted
back to zero by the kernel's own introspection (mirrors how `frame_pool.c`
exposes counters for host tests).

---

## 9. Analysis (deliverable 5)

### 9.1 Minimum hardware requirements

| Requirement | Needed for | Notes |
| --- | --- | --- |
| **MMU, 4-level paging (x86-64)** | per-process address spaces, kernel/user split | SV39 (RISC-V) or 4K/4-level (AArch64) equivalent |
| **User/Supervisor page bit** | ring-3 isolation | x86 U/S, RISC-V U, AArch64 AP\[1\] |
| **Write-protect bit** | `W` permission enforcement | W on PTE is the hardware trust anchor for MEM perms |
| **No-Execute bit** | `X` permission enforcement | x86 NX + EFER.NXE (already enabled by `syscall_gate_init`); RISC-V N, AArch64 UXN |
| **Aligned 64-bit atomics** | cap word moves, queue counters | already used by `ipc.c`'s `__atomic_*` ops |
| **APIC + timer interrupt** | tick scheduling / wake | already present (`kernel/smp.c`, `lapic.h`) |
| **TLB shootdown (IPI)** | unmapping a page while another core may run the process | Phase 1: broadcast IPI flush or accept stale-TLB window — see below |

Explicitly **not** required in Phase 1:

- **ASID/PCID** — full CR3 reload on map/unmap is correct with a single
  address space per process; PCID is a Phase-2 latency optimization.
- **IOMMU** — *until the arena pages are given to a DMA device*. The
  moment a NIC/NVMe writes into a MEM-cap frame, you need the IOMMU or
  bounce buffers; Phase 1 keeps arena pages out of every DMA path and
  says so in the frame-reservation code.
- **Hardware capability tags (CHERI)** — capabilities are enforced
  entirely in kernel software (word validation + reserved-bit check).
  CHERI would harden the *user-side* of the SDK later; it is not needed to
  prove the lifecycle. The repo's `tools/simi` capability-tag work is the
  same philosophy one level down.
- **MPU-only systems (ARMv8-M, no MMU)** cannot run this design as
  written: per-process virtual address spaces and shared physical
  mappings require page tables. An MPU-only variant (seL4's M-profile
  port, CHERI-RISC-V) is a different memory model (physical addressing,
  memory-protection regions) and is out of Phase 1 scope.

**One honest SMP caveat**: unmapping PTEs requires a TLB flush on every
core that might run the target process. Phase 1's simplest correct choice
is a broadcast flush IPI on unmap/revoke (rare operations; `kernel/smp.c`
already has the machinery), accepting that a currently-running core may
hold the process's old TLB entries for the few instructions until the IPI
lands. On single-core (or QEMU `-smp 1`) this is a no-op.

### 9.2 Ballpark latency

Per-operation budget on ~3 GHz real x86-64, cache-hot:

| Step | Cost |
| --- | --- |
| SYSCALL/SYSRET entry+exit pair | ~60–100 ns |
| table lock + validation (two words, one object lookup) | ~20–30 ns |
| channel lock + queue slot write | ~20–30 ns |
| receiver table lock + slot install (send) / install (recv) | ~20–30 ns |
| object lock acquire/release | ~10–20 ns |
| **`cap_send` total (kernel side)** | **~150–250 ns** |
| **`cap_recv` total (kernel side)** | **~120–200 ns** |
| **send→recv round trip, both sides already running** | **~300–500 ns** (≈1–1.5 µs with syscall overhead both sides) |
| **blocked-receiver round trip (Phase-1 tick wake)** | **+0.5–10 ms** (one tick — the honest cost of the tick-based wake; Phase 2's immediate wake removes this) |
| QEMU TCG (development default) | 10–100× slower: expect 5–20 µs per round trip — fine for lifecycle testing, meaningless for numbers |

For reference: seL4's published IPC round-trip on x86-64 is roughly
0.5–1 µs (a few thousand cycles); a 1 µs Phase-1 round trip is comfortably
in the same class with no lock-free tricks. The per-operation costs above
are dominated by syscall entry and lock traffic, exactly where seL4 spends
its budget too (its differentiator is the *preemption* and *verification*
story, not raw syscall speed).

### 9.3 Benchmark methodology

1. **Baseline first**: measure an empty syscall (e.g., `SYS_SLS_SERIAL_WRITE`
   or a no-op) with `rdtsc` around it — subtract from everything else.
2. **Loopback microbenchmark** (the one that matters for the Seed Kernel):
   two processes, pre-created channel, N = 1,000,000 iterations of
   `cap_send` / `cap_recv`; timestamp per call with `read_tsc()`
   (`kernel/dashboard.h` already exposes it) and record min / median / p99
   (use a small histogram array — fixed-size, this repo's style).
3. **Kernel-side counters**: mirror `ipc_stats` (`total_posted`,
   `total_dispatched`, rolling `avg_latency_ns` via the same
   `(old*7+sample)/8` update) into `cap_stats`, exposed by
   `SYS_SLS_CAP_LIST`. This gives the shell a live latency view with zero
   new instrumentation.
4. **Throughput**: fill the channel with batched sends (queue depth 16),
   measure messages/sec and caps/sec delivered; the cap-transfer path must
   not slow down plain message traffic (assert `has_cap=0` fast path).
5. **Contention**: N threads hammering one channel's send end (per-table
   lock serialization) and N threads on *different* channels (must scale —
   that is the point of per-table locks over seL4's global lock).
6. **Lifecycle hygiene**: a soak loop that does arena-alloc → send → recv →
   map → write → unmap → revoke 10,000× and asserts
   `cap_objects[*].refcount == 0` and arena free-frame count returns to
   the starting value — a leak test, not a speed test.
7. **Where to run**: real hardware for numbers, QEMU for correctness; under
   QEMU use `-icount`/TCG and only compare *relative* numbers (empty syscall
   vs cap round trip), never absolute µs.

---

## 10. Phase-2 direction (named, not built)

- Delayed revocation with generation-stamped slots + epoch reclamation,
  when tables outgrow O(holders) teardown.
- Blocked send with `IN_TRANSIT` reservation and immediate wake (self-IPI).
- Mapping caps as first-class capabilities (seL4 VSpace-style) so map
  granularity is revocable; per-page `invlpg` + PCID.
- Badges on channel caps (seL4 `seL4_Badge`) for multiplexed endpoints.
- Untyped memory + a retype primitive (KeyKOS space-bank accounting for
  arena frames, replacing the flat bitmap).
- COW and copy-on-send for large MEM views; DMA/IOMMU support for arena
  frames.
- The sidecar broker: a Ring-3 service that holds capabilities for
  clients and proxies them over channels — the SDK's first real user.

---

## 10b. Implementation deltas (v0.1 design → the code in `kernel/cap.c`)

This design was written first and then implemented; the addendum records where
building it changed the design, so the doc and the code never silently disagree:

1. **`chan_create` mints FOUR caps, not two.** Each end needs both a read and a
   write cap, so the endpoint-handoff-by-transfer story in §4/§8 could not
   bootstrap: the far end's caps cannot travel over the very channel they
   belong to. The implemented API provisions both endpoints (`out_rd`,
   `out_wr`, `out_far_rd`, `out_far_wr`) and, with a `far_pid` argument, mints
   the far end's caps directly into the other process's table — the concrete
   Phase-1 bootstrap (kernel-mediated provisioning, socket-pair style).
   Endpoint handoff by *transferring channel caps as payloads* is still
demonstrated by the host test.
2. **The receiver's slot is installed at `cap_recv` time, not pre-installed by
   the sender.** The design's §3.2 Phase C allocated the receiver's slot during
   `cap_send`; the implementation drops that entirely. `cap_send` moves the
   holder from the sender's slot to the *queue entry* (refcount unchanged —
   the queued cap IS the holder), and `cap_recv` moves it from the queue to a
   fresh slot in the receiver's table. Single-install protocol: no
   double-install hazard, and `cap_send` never touches the receiver's table,
   which shrinks the send lock set to sender-table → channel → object.
3. **Lock order corrected.** The §2.1 table listed `cap_recv` as taking
   channel → receiver table → object, which violates the stated global order
   (table < channel < object) and deadlocks against a concurrent send. The
   implementation has `cap_recv` take the receiver's OWN table first:
   `table → channel → object` — consistent with the global order, and the
   receiver's table is the caller's own, so it is known before the channel
   lock is needed.
4. **Object ids and object structs are monotonic and never recycled.** Phase 1
   has no reclamation of object table slots: `cap_obj_next` only grows, a
   destroyed object is marked inactive but its struct is retained, and ids are
   never reused. This removes the entire id-reuse/ABA class of use-after-free
   (a stale cap word can never alias a recycled object) at the cost of a
   1024-object ceiling per boot — documented as Phase-2 reclamation work.
5. **Blocking `cap_recv` is a user-space SDK concern in Phase 1.** The kernel
   `cap_recv` is non-blocking (`CAP_EAGAIN` on empty; the `block` field is
   accepted and ignored, documented in `cap.h`). §3.4's tick-based parking is
   deferred to Phase 1.5 — the kernel's scheduler has no way to park a process
   mid-syscall without new machinery, and the design already blessed the
   poll-loop fallback for the test.
6. **Mappings outlive sends, and revocation tears down mappings in ALL
   tables, not just the current holders'.** The §5.1 decision (maps survive
   sends) means a process that mapped and then sent the cap away is a
   non-holder with a live PTE. The implementation therefore scans every
   table's map registry (16 × 64 records) during revoke and unmaps the
   object's PTEs wherever they are — the map registry is keyed by object id
   precisely so this works.
7. **`cap_create_mem` also rejects ranges below 1 MiB** (machine-reserved
   low memory, per `frame_pool.c`'s own reservation), in addition to the
   design's kernel-image / arena / existing-object checks.
8. **The arena carve folds into `reserved_below`.** `frame_pool_reserve_
   contiguous()` extends the machine-owned watermark, so the arena frames are
   protected from partition reclamation exactly like the kernel image — a
   guard the design mentioned but did not specify.

## 11. Summary of decisions

1. **x86-64 Phase 1**; arch-neutral capability layer, per-arch map/unmap hooks.
2. **Cap = 64-bit word** (type/state/object-id/perms/offset/len); object =
   kernel struct with MDB-style holder list; `refcount == #VALID holders`.
3. **Per-process flat table, 512 slots, fd-like indices, freelist, per-table
   spinlock.** Global lock order: table < channel < object.
4. **Immediate revocation** (seL4-style guarantee, EROS-redemption
   subtlety avoided), linearized by the object's `revoking` flag under the
   object lock.
5. **Send = move** (sender slot freed, receiver slot installed, queue
   published) — all under channel + object locks; queue-full and
   table-full fail before any mutation; `-ETABLEFULL` never loses a cap.
6. **Queued caps are holders**; revoke stamps queued words REVOKED under
   the object lock; double-free/UAF are excluded by the state machine +
   lock ordering + refcount-under-object-lock invariants (§3.1).
7. **MEM = physical window + offset view**, arena bitmap with per-frame
   owners (no aliasing), mappings outlive sends but die on revoke.
8. **Blocking recv** fits the tick scheduler with `waiter_pid` + `wake`
   flag; wake latency = one tick; non-blocking recv is the zero-change
   fallback.
9. **Syscalls 289–297**, struct requests through the existing
   `do_syscall` ABI; `libaerocap` in `user/` as the SDK skin.
10. **Target: 1 µs-class send→recv round trip**, benchmarked with TSC
    histograms + `cap_stats`, lifecycle leak-test as the acceptance gate.
11. **Shell hooks `cap list` / `cap demo`** (user/shell.c). `cap list`
    wires `SYS_SLS_CAP_LIST` to the serial console, mirroring `proc list`.
    `cap demo` runs the acceptance scenario live on a real boot: it uses
    two cap tables (A = pid 0, the Ring-0 shell itself; B = pid 1, a
    second table created lazily by `cap_table_index`) so the channel's
    pid-keyed direction logic routes A→B through q[1] and B→A through
    q[0] — the four caps are minted by `cap_chan_create`, then the demo
    arena-allocs 1 page, sends the MEM cap A→B, receives it into B's
    table, writes `Hello` to the arena page's physical address through
    the kernel identity map (the honest kernel-context memory model:
    `cap_map` returns `CAP_ENOSYS` there by design — ring-3 PTE install
    is host-tested), sends the cap back, reads `Hello`, and revokes both
    the MEM cap and all four channel caps, asserting `arena_free` returns
    to 16384/16384 and `objects` to 0. Boot verification
    (tests/cap_boot_check.sh) boots the ISO under QEMU, asserts the
    `[CAP] shared-memory arena: 64 MiB ...` carve line and the
    `[CAP] Seed kernel capability layer online` line in the serial log,
    then types `cap list` and `cap demo` at the `uid:> ` prompt and
    asserts the `[CAP] Capability tables:` and
    `[CAP-DEMO] A reads 'Hello' back: PASS` responses — so the arena
    carve, the syscall path, and the full create→transfer→map→write/read
    →revoke lifecycle are all proven on a real boot, not just by the
    host test. (`cap_debug_obj_phys()` was added to cap.{h,c} so the
    demo can resolve a MEM object's physical base.)

12. **The ring-3 SDK: `user/libaerocap/aerocap.h`** — one C wrapper per
    syscall (`cap_create_mem`, `cap_arena_alloc`, `cap_chan_create`,
    `cap_send`, `cap_recv`, `cap_revoke`, `cap_map`, `cap_unmap`,
    `cap_list`) with request structs byte-identical to the kernel's
    `SLSCap*Request`, hidden behind `_sls_syscall()` (no raw syscall
    numbers, no structs, in user code). `tests/aerocap_abi_host_test.c`
    pins the ABI identity with sizeof/offsetof checks against
    kernel/cap.h, so a drift on either side fails CI. `cap_map` resolves
    the calling process from the kernel's own scheduler state, so no pid
    argument appears in the SDK — the kernel side already does that for
    every cap syscall. The example `user/examples/cap_roundtrip.c`
    exercises the whole MEM lifecycle from a real ring-3 process — the
    one thing the kernel-shell `cap demo` cannot do (the shell has no
    user CR3, so `cap_map` returns `CAP_ENOSYS` there). Live
    verification (tests/aerocap_boot_check.sh) uploads the compiled
    example to the guest's HTTP API (`program_upload.py` →
    `/api/program/upload`), spawns it (`/api/program/spawn`), and
    asserts `write+read 'Hello' PASS (ring-3 user mapping)` — a real
    user PTE installed by `cap_map` — plus the send/revoke steps and a
    clean exit (code 0).

13. **Two real bugs found by booting the ring-3 example** (both fixed
    here, both invisible to the host tests):

    - **`DEMO_VA` must not sit in PML4 slot 0 (0–512 GiB).** The first
      example mapped at 64 GiB and the ring-3 write faulted with #PF
      error=0x7 (present + write + user → protection violation). The
      kernel identity-maps slot 0 with supervisor-only (U/S=0) entries,
      and `user_clone_page_table()` copies ALL 512 PML4 entries into
      every process, so a user leaf PTE installed there is still denied
      by the supervisor walk (the CPU checks U/S at every level). The
      program's own code works because it lives at 64 TiB (slot 128),
      where fresh USER-bit tables are created. `DEMO_VA` moved to
      1 TiB (slot 2), and the SDK example documents the constraint.

    - **`_sls_syscall()` omitted `rdi` from its clobber list.** The
      kernel's `syscall_entry_stub` clobbers RDI (the `.unknown_syscall`
      path does `mov rsi,rdi; mov rdi,rax`) and never restores it, but
      the inline asm listed only `rcx, r11, r8-r10, rsi, rdx` — so GCC
      was free to keep a live value in RDI across a syscall. The example
      tripped it: the compiler parked `&req` in RDI across the second
      `cap_arena_alloc`, then `cap_revoke` reused the stale (now
      kernel-clobbered) value, crashing the kernel with a #GP on a
      non-canonical request pointer. Fixed by loading RDI inside the
      asm (`mov %1,%%rdi`) and adding `rdi` to the clobber list.
      (user/libsls/sls.h)

14. **Two-party verification: `cap_peer` + `cap_two_party`** (user/examples/
    cap_peer.c, cap_two_party.c) and **`SYS_SLS_GETPPID` (298)** with a
    `parent_pid` field on the process descriptor (kernel/process.{h,c}).
    This kernel's spawn is synchronous (`program_spawn()` blocks in
    `kernel_enter_ring3` until the child exits), so the Phase-1 "two live
    processes" exchange is turn-based: the parent spawns the child from
    inside its own syscall, the child runs to completion, then the parent
    resumes. The capability handoff is still fully real — kernel-mediated,
    across two distinct tables and two distinct address spaces:

        [peer] getppid -> 101                          ; child resolves its parent
        [peer] chan_create ok (far_pid=parent): mine rd=0 wr=1 far frd=0 fwr=1
        [peer] arena_alloc ok, MEM cap=2               ; child allocs a 4 KiB MEM cap
        [peer] map ok (user PTE in child's page table) ; child maps + writes 'Hello'
        [peer] send ok — wrote 'Hello' and moved the MEM cap to the parent
        [PROC] PID 102 'cap_peer' exited (code=0).     ; child exits, parent resumes
        [roundtrip] child ran and exited; pid=102
        [roundtrip] recv ok — MEM cap arrived in our table, slot=2
        [roundtrip] mapped + read 'Hello' back from the child: PASS
        [roundtrip] revoke ok — channel end + MEM cap torn down

    The child resolves the parent with `sls_getppid()` (298) — the pid is
    recorded at spawn time via `process_find_current()`, so no pid-prediction
    hacks — then mints the channel with `far_pid = parent`, landing the far
    end (frd=0, fwr=1) directly in the PARENT's table. The parent recvs on
    slot 0, gets the transferred MEM cap at slot 2, maps it, and reads the
    child's 'Hello' back through its own user PTE. Verified live by
    tests/aerocap_boot_check.sh (boots the ISO, uploads both binaries,
    spawns cap_two_party via the guest HTTP API, asserts the transcript).

15. **Four coupled nested-spawn bugs fixed to make the child's run and the
    parent's resume correct** (kernel/process.c, arch/x86/process_enter.asm).
    A kernel-context spawn (HTTP/shell) never hit these; a process spawning
    a process from inside a syscall hit all of them at once:

    - **GS state at child entry.** The parent's syscall entry had already
      `swapgs`'d, so the child entered Ring-3 with GS_BASE = &per_cpu_data.
      The child's FIRST syscall then `swapgs`'d the WRONG direction and
      `mov rsp,[gs:8]` read physical address 0x8 — a garbage RSP and an
      immediate triple fault (verified live: PID 102 died before its first
      sls_puts). `nested_ring3_prep()` now `swapgs`es before sysret so the
      child always enters Ring-3 with GS_BASE = 0 (the user view).
    - **Per-process syscall stack.** `syscall_entry_stub` pushes onto
      [gs:8]; the boot value is a fixed 8 KiB stack in .bss that sits
      DIRECTLY ABOVE proc_table (0x386ea80 vs 0x386cda0 in the shipped
      link). A nested child's frames growing past its bottom smashed kernel
      state (proc_table[0].pid became 0x63000000). Each process now gets a
      DEDICATED syscall stack (an allocated, identity-mapped frame) and
      [gs:8] is pointed at it before entering Ring-3.
    - **Callee-saved registers across kernel_enter_ring3.** The asm cleared
      rbx/rbp/r12-r15 (a security measure) before sysret, but the C caller's
      continuation relies on them — a latent ABI bug the single-process path
      survived by luck (its `pd` read happened to come out right).
      kernel_enter_ring3 now pushes the six callee-saved registers into its
      frame and process_exit pops them before ret'ing to the continuation.
    - **The blocked parent must not look runnable.** While the child runs,
      the parent is parked inside its own syscall; a timer tick would have
      made schedule_ring3 switch to its EMPTY ring3_ctx (garbage iretq).
      The parent is marked PROC_BLOCKED (a new state, excluded by the same
      exact-value checks that exclude PROC_HELD) for the child's lifetime,
      and process_exit marks it RUNNING again on resume. process_exit also
      restores the parent's user RSP (captured at spawn; the child's
      syscall entries overwrite gs:0) and skips its own swapgs when the
      spawner is a process (the parent resumes inside its syscall with
      GS_BASE = &per_cpu_data, the state its .syscall_return expects).

16. **`user_clone_page_table()` deep-copies user slots — a fork-without-COW
    bug** (arch/x86/user_paging.c). The old code copied ALL 512 PML4
    entries, so a child's PML4 initially SHARED the parent's lower-level
    page-table pages. Loading the child's binary at the same vaddr
    (USER_PROC_CODE_BASE) then walked into those shared PT/PD/PDPT pages and
    rewrote them — corrupting the PARENT's address space. Verified live: the
    parent "ran" the child's code at its own RIP (its PML4 mapped
    cap_peer.bin where cap_two_party.bin should have been) and exited with
    the child's failure path. The fix keeps supervisor (kernel) slots shared
    by pointer — that identity map is what makes syscalls work after the CR3
    switch — and DEEP-COPYs every U/S=1 slot down to fresh PTEs, so loading
    the child's image rewrites only the child's tables. This is the deepest
    bug the two-party test flushed out: it is invisible to any single-process
    workload,    and it would break ANY fork/spawn-from-a-process pattern.

17. **Blocking `cap_recv` is now REAL kernel-side parking** (Phase 1.5) —
    delta 5's "user-space SDK concern" deferral is superseded for the
    blocking case. An empty recv with block=1 hands off to process.c's
    `cap_wait_chan()` (a weak hook in cap.c, strong override in process.c —
    same pattern as cap_current_pid): the current process saves the user
    state its syscall entry frame holds (r11=user RFLAGS, rcx=user RIP, the
    six callee-saved regs, and [gs:0]'s user RSP — exactly the eight
    pushed values, read back off its own syscall stack), is marked
    PROC_BLOCKED on the channel, and the CPU switches to the next runnable
    process. A later send calls `cap_wake_chan()`: the parked process
    becomes runnable again flagged `resume_kernel`. Its next schedule — via
    timer tick OR the current process parking/exiting — iretq's it into
    `cap_recv_resume()` on a FRESH syscall stack with a synthetic kernel
    frame (CS=0x08/SS=0x10): it swapgs's, re-pushes the saved entry frame,
    re-runs `do_syscall(SYS_SLS_CAP_RECV)` against the SAME user request
    struct, and jumps into the stub's shared return path
    (`.syscall_return`, exported as `syscall_return_path`) so the process
    sysrets to ring-3 exactly as if the syscall had simply taken longer.
    The old mid-syscall call chain on the abandoned stack is never unwound
    (one dead frame per park; bounded, Phase-1 posture). The design's §3.4
    tick-based wake becomes "wake on send, next schedule" — the send is the
    wake, no tick latency in the common case.

    Two correctness details worth recording: `kernel_switch_next()` must set
    `next->state = PROC_RUNNING` (schedule_ring3 does; the park switch
    initially did not, so a switched-to child ran as SUSPENDED and its own
    syscalls could not resolve it as current — caught live as the child's
    second send returning EBADF and its exit finding "no active Ring-3
    process"); and `cap_wait_chan` refuses to park when NO other process is
    runnable (returns, and the syscall degrades to CAP_EAGAIN — the Phase-1
    fallback), which guarantees at least one runnable process exists
    whenever a process parks or an async child exits.

18. **Non-blocking spawn (`SYS_SLS_PROGRAM_SPAWN_NB`, 299) — two processes
    genuinely LIVE at once** (`program_spawn_nb` in kernel/process.c,
    `sls_program_spawn_nb` in libsls). The child is created exactly like a
    synchronous spawn but gets a SYNTHETIC ring3_ctx (15 zeroed GPRs + a
    iretq frame with rip=entry, cs=0x23, rflags=0x202, rsp=user_rsp,
    ss=0x1B) and is marked SUSPENDED with `has_ring3_ctx=1`; the spawn
    returns to the caller immediately. The scheduler iretq's into it on the
    next tick or when the caller parks. This required widening the
    scheduler's notion of schedulable from `kernel_rsp != 0` (only
    kernel_enter_ring3-entered processes) to a `proc_runnable()` helper
    (`kernel_rsp || has_ring3_ctx || resume_kernel`) in the pickers, the
    schedule_ring3 current-scan, and process_find_current — an async child
    never calls kernel_enter_ring3, so kernel_rsp stays 0 for its whole
    life. Every context switch now also repoints per_cpu_data[0].kernel_rsp
    at the next process's OWN syscall stack ([gs:8] is read on its next
    syscall entry), and ALL spawn paths (not just nested ones) wire it up
    before the first sysret. process_exit gains a third shape: an async
    child (parent_pid != 0 && kernel_rsp == 0) has NO kernel_enter_ring3
    continuation to return to, so it hands the CPU to the next runnable
    process (usually the parent, woken by its send) via the same kernel
    switch; if nobody is runnable it halts (a parked process was never
    woken — a liveness bug, not a crash).

19. **Phase-1.5 live verification: `cap_overlap` + `cap_peer_ovl`**
    (user/examples/cap_overlap.c, cap_peer_ovl.c; phase 3 of
    tests/aerocap_boot_check.sh). The parent spawns the child
    NON-blocking, provisions the channel itself (chan_create with
    far_pid = the child's pid — the child cannot, it has not run yet; the
    far end is minted into the child's table at frd=0/fwr=1), then blocks
    in cap_recv. The kernel parks it mid-syscall and iretq's into the
    child; the child sends the MEM cap (waking the parent), spins (proving
    it stays live while the parent is parked), sends a 'done' marker, and
    exits; the parent resumes INSIDE its recv, reads 'Hello' back through
    its own mapping, re-parks for the marker, and tears down. The live
    transcript shows the two park/wake cycles end to end:

        [ovl] spawn_nb returned immediately, child pid=104 (child has NOT run yet)
        [ovl] chan_create ok (far_pid=child): mine rd=0 wr=1  far frd=0 fwr=1
        [CAP] recv block: parked PID 103 on channel 2
        [CAP] recv block: switching to PID 104 'cap_peer_ovl'
        [CAP] woken PID 103 (channel 2)
        [peer2] sent MEM cap (parent woken)
        [ovl] recv#1 ok — woken by child; MEM cap slot=2
        [ovl] mapped + read 'Hello' back from the live child: PASS
        [CAP] recv block: parked PID 103 on channel 2        ; second park
        [peer2] spin i=0 / i=1 / i=2                          ; child stays live
        [CAP] woken PID 103 (channel 2)
        [ovl] recv#2 ok — 'done' marker ... PASS
        [PROC] PID 104 'cap_peer_ovl' exited (code=0).
        [PROC] async exit: handing CPU to PID 103 'cap_overlap'
        [ovl] done

    The design doc's §10 Phase-2 "immediate wake (self-IPI)" remains future
    work — Phase 1.5's wake is send-side and takes effect on the next
    schedule, which is immediate when the sender parks or exits (as here)
    and one tick away when it keeps running.

20. **The resume frame's push/pop mirror and the compiler-stack-clobber —
    two bugs only a resumed syscall can expose.** The first build of
    `cap_recv_resume` pushed the saved frame in POP order (r11 first),
    because `syscall_return_path` pops r11 first. That is wrong: the pops
    read `[top-64]` first, so a push-ordered-by-pop puts park_ctx.rbx in
    the rcx slot — sysret jumped to the user's stack (a #PF at the request
    struct's own address, caught live). The capture and the resume must
    both follow the ENTRY order (rbp [top-8], rbx, r12..r15, rcx, r11
    [top-64]); the capture was already correct after the mirror was
    proven, and the resume's push order was fixed to match. The second bug
    is subtler and survived the first fix: even with the push order
    right, GCC's own codegen destroyed the frame — the pushes happened in
    inline asm, whose rsp change the compiler does not track, so it
    emitted `add $0x40,%rsp` and `call do_syscall`, whose return address
    and frame landed exactly on the pushed values; the resumed process
    sysret'd to `0x3eb023`, a stale return-address value. The fix folds
    the `do_syscall` call AND the jump into `syscall_return_path` into the
    SAME asm block with rsp parked at `top-64`, so the C chain grows
    strictly below `top-72` and can never touch the frame. Both bugs are
    invisible to any test that does not actually resume a parked process
    into ring-3 — the overlap phase's live transcript is what caught them.

21. **Immediate wake is a send-side HANDOFF, not a self-IPI** (Phase 1.5,
    `SYS_SLS_YIELD` = 300). The design's §10 self-IPI idea is rejected for
    the Seed Kernel: a send that wakes a parked receiver hands the CPU to
    it RIGHT THEN — the receiver runs inside the sender's send syscall,
    and the sender's own entry frame is parked on its stack marked
    `resume_sysret`, so its next schedule sysrets it past the completed
    send as if the syscall had merely taken a while (`cap_sysret_resume`
    → `.syscall_return`). The explicit `sls_yield()` gives a process the
    same mid-syscall park so the two-party transcript stays deterministic.
    The wake's latency is zero when the sender parks/exits, one tick when
    it keeps running — matching the §10 target without IPI machinery.

22. **The sysret-resume's original entry frame is NOT survivable — resume
    must REBUILD from park_ctx (and every park site must capture the full
    frame).** The first handoff build made `cap_sysret_resume` pop the
    process's ORIGINAL entry frame at `[top-64..top-8]`, on the theory
    that nothing runs on that stack between the entry pushes and the
    switch-away. Live verification disproved it: intermittently (≈1 run in
    3) the resumed process sysret'd to 0 and took a Ring-3 #PF, with the
    fault-time dump showing the entry-frame region overwritten by
    `kernel_switch_next`'s `f[20]` iretq-frame array — GCC's -O2 inlining
    freely reuses the "dead" `[top-64..top-8]` region for the resumed
    chain's locals, exactly the delta-20 hazard class, one level up. Two
    compounding fixes: (a) `cap_sysret_resume` now REBUILDS the frame
    from `park_ctx` (push in entry order, `RAX=0`, jump to
    `.syscall_return`) exactly like `cap_recv_resume` — the rebuild
    overwrites whatever the C chain scribbled; and (b) every park site
    must capture the FULL eight-register frame into `park_ctx` BEFORE the
    chain runs — `cap_wait_chan` already did; `cap_maybe_handoff` and
    `sys_sls_yield` captured only `user_rsp` (the old pop-based resume
    didn't need the regs), and a rebuild without them sysret'd to the
    descriptor's stale zeros — caught as `park_ctx r11=0 rcx=0` at the
    fault. The second bug was my own regression from fix (a); the full
    capture at all three sites is what makes the rebuild sound.

23. **The overlap test's release→recv gap is closed by releasing INSIDE
    the recv syscall** (`SLSCapRecvRequest.release_pid`, reclaimed from
    padding). A separate `sls_proc_release()` followed by `cap_recv(block=1)`
    left a user-mode window in which a timer tick scheduled the HELD
    child first: its send#1 then found nobody parked, the handoff didn't
    happen, and the boot-check's "recv#1 printed before the sender's send
    returned" ordering proof failed (about 1 run in 3 — the same
    nondeterminism family as delta 22, different trigger). `cap_recv_release`
    does both in ONE syscall — the child is released and the parent parks
    atomically, so the child always runs while the parent is parked and
    the immediacy proof is deterministic. The recv's re-run on resume
    re-attempts the (already-done) release; `process_release`'s -2
    ("not held") is ignored. Verified: 7/7 consecutive clean boot-check
    runs, zero faults, ordering proof green every time.

24. **Phase 2 teardown: a process's exit reclaims everything it owns.**
    Three new primitives, wired into `process_exit()` (all three resume
    shapes) and `process_kill()`:

    - `cap_table_teardown(pid)` (kernel/cap.c) — under the table lock:
      PRE-DRAIN the read queue of every channel the process held a CHAN_R
      on (caps queued for a dead reader can never be recv'd, so each is
      revoked — its HOLDER_QUEUE node removed, its object freed at
      refcount 0, its arena frames returned); DROP every slot holder
      (objects destroyed at refcount 0); POST-DRAIN both queues of every
      channel whose last holder died in the drop (the far_pid=0
      self-channel shape otherwise pins in-flight objects forever);
      UNMAP the process's cap-derived PTEs; and UNBIND the pid→table
      slot (without this, repeated spawn/exit cycles exhaust the 15
      process tables and every new process's cap syscall returns
      CAP_ETABLEFULL). `cap_table_find()` is a pure lookup — teardown
      must never lazily bind a table for a pid that never used caps.
      `cap_object_free_resources()` (arena-frame return / channel
      deactivation) is factored out of cap_revoke's finalize so both
      paths share one definition of "free a dead object."

    - `user_destroy_page_table(cr3)` (arch/x86/user_paging.c) — walks
      the user half of the PML4, frees the process's own intermediate
      tables and leaf data frames (binary, user stack, SIMI scratch),
      then the PML4 itself. Kernel-half slots are shared by pointer and
      never touched. The REFUSAL set is precise: frames below
      `_kernel_image_end` (kernel image + bootstrap stack + low memory),
      the cap arena (`cap_frame_in_arena`), and the shared SIMI
      activation-cache code frames (`simi_frame_is_cached` — they are
      mapped into EVERY process that spawns that object and must outlive
      any one of them; the per-activation scratch page is NOT cached and
      IS freed). Leaves/intermediates are freed with
      `frame_pool_frame_owner()`'s recorded partition id so the right
      quota counter is decremented for both accounted (loader/stack) and
      unaccounted (page-table/SIMI-scratch) frames.

    - `proc_free_syscall_stack(pd)` (kernel/process.c) — the two
      contiguous syscall-stack frames, recovered from
      `syscall_stack_top` (top = lower + 8192 − 8).

    **Ordering is load-bearing and runs at the TOP of process_exit, while
    the exiting process's own CR3 is still loaded**: cap teardown first
    (its unmap walk needs `cap_proc_cr3(pid)`, which requires the
    descriptor still `active`), then the page-table walker (which sees no
    cap PTEs left), then the syscall stack, then `active = 0`. This is
    safe even though the exit path itself is running on the freed
    syscall stack and under the freed PML4: from that point on execution
    lives entirely in the kernel half (shared by pointer, never freed);
    the freed frames' contents stay physically intact until the CR3
    switch at the bottom of the exit path abandons the address space;
    and no allocation can re-hand them in between (single CPU, IF=0
    inside the syscall). The frame pool's free primitives reject
    double-frees / bogus addresses as a backstop.

    **Live-caught bug #1 — the coarse machine-owned watermark refuses
    every real process frame.** The arena carve folds into
    `frame_pool_reserve_contiguous`'s `reserved_below` watermark, which
    therefore covers not just the arena but every ALLOCATABLE frame below
    it (the pool hands those out to processes: this kernel's processes
    all live at 0xe1exxx, below the arena at 0xe200000). A first build
    used `frame_pool_frame_is_machine_owned()` as the walker's guard and
    freed ZERO frames for every process (`[TORE] ... 0 frame(s) freed`),
    leaking ~12 frames per spawn/exit cycle. Fixed with the precise
    refusal set above; the frame-count proof below is what caught it
    (73931 grows by ~240 without the fix). The stale "frames between the
    image and the arena were never allocatable anyway" comment in
    frame_pool.c is factually wrong — the pool does hand them out — and
    the watermark's conflation remains a latent quirk of
    `frame_pool_frame_is_machine_owned` (documented in
    `cap_frame_in_arena`'s comment), deliberately not widened in scope.

    **Live-caught bug #2 — the parent's `cap_list` raced the last
    child's exit.** The recycle parent dumps the cap tables after its
    20-cycle loop, but the last child is still SUSPENDED mid-send after
    the final handoff; without a yield the dump can run before the
    child's exit teardown, showing `objects active=1 arena free=16383`.
    `sls_yield()` before the dump deterministically lets the child run
    to its exit first.

    **The boot-check assertion that makes this milestone falsifiable**
    (tests/aerocap_boot_check.sh phase 4): `cap_recycle` spawns
    `cap_recycle_child` 20 times (GATED non-blocking spawn; each child
    allocates two MEM caps, sends one, exits WITHOUT revoking anything);
    20 > 15 proves cap-table unbinding, the final `cap list` proves the
    arena is fully returned (`objects active=0/1024 arena free=16384/
    16384`), ≥20 `[TORE] PID ... teardown:` lines prove every exit ran
    teardown, and — the strongest proof — `/api/metrics`
    `ram_allocated_frames` is byte-for-byte IDENTICAL before and after
    the loop (73931 → 73931, observed across repeated runs), so page
    tables, syscall stacks, binaries, arena frames, and cap tables all
    return exactly. The immediate-wake ordering assertion was also
    hardened from line numbers to byte offsets (a pre-existing flake: a
    timer can interleave the child's print onto the parent's log line
    under -smp 4; the parent always starts printing first, so offsets
    still prove the order). Verification: aerocap boot check 3/3 clean
    (plus the pre-fix runs), cap boot check PASS, all three host tests
    PASS (the scheduler test gained no-op stubs for the two new externs
    its `#include` of process.c now references).

    **Delta 25 — partition_destroy() reclaims EVERYTHING, and the
    per-process teardown it leans on now survives kill-vs-resume races.**
    The request was to wire Phase-2 teardown into `partition_destroy()`;
    inspection showed it already runs the per-process teardown via
    `process_kill_partition()` → `process_kill()` (gained last milestone),
    so the real work was making `partition_reclaim_all_frames()` complete
    and making the kill path safe. Four changes:

    1. **The frame-pool watermark fix (the "reclaim everything" core).**
       `frame_pool_reserve_contiguous()` had folded the arena carve into
       `reserved_below`, so the coarse machine-owned watermark covered
       EVERY allocatable frame below the arena — and every process frame
       in this kernel lives below the arena (0xe2xxxxx). A destroyed
       partition's remaining frames (page-table/binary/stack of a
       process whose kill raced ahead, or any non-process accounted
       allocation) were reported machine-owned and leaked forever. The
       arena is now tracked as its own precise range, so frames between
       the image and the arena are reclaimable; `frame_pool_reserve_host_test`
       gained a scenario pinning it (a tenant frame below the arena is
       reclaimable).

    2. **`pending_teardown` — deferring kills of RUNNING targets.** The
       milestone's own request made the self-kill race reachable: a
       destroy can now target a process that is executing (self-kill via
       SYS_SLS_PROC_KILL with one's own pid, or the destroy killing the
       caller). `process_kill()` defers a RUNNING target (marks
       `pending_teardown`, leaves it running); `schedule_ring3()` runs
       the full teardown at the next switch.

    3. **`spawn_owner_uid()` — ring-3 spawns used to hand children to
       uid 0.** The dispatch threaded `kernel_get_current_thread_id()`
       (the microkernel task id — 0 for ring-3 callers) as the child's
       owner_uid, silently parking every ring-3-spawned child in
       PARTITION_SYSTEM where no destroy could touch it. Now the real
       caller uid (via `process_find_current()`).

    4. **A ring-3 ABI fix the verification forced: `PROC_USER_STACK_PAGES`
       4 → 8.** `SLSUploadRequest` is a 16,457-byte struct — LARGER than
       the entire 16 KiB ring-3 stack, so `SYS_SLS_UPLOAD_BINARY` was
       unusable from ring-3 (caught live: the upload struct overflowed
       the stack and faulted below the stack base). Also discovered that
       flat binaries are mapped WITHOUT the WRITE bit, so ring-3 statics
       are read-only — the upload request must be a stack local, never a
       global (a global write faulted with #PF error=0x7).

    **Design constraint discovered (the reason the test embeds the
    child's binary).** HTTP-created program objects always get
    `owner_uid=0` → partition 0 (PARTITION_SYSTEM), and
    `catalog_check_access()`'s partition boundary requires the object's
    partition to equal the spawner's current partition. So a process
    inside a non-zero partition CANNOT spawn any HTTP-uploaded program —
    the object is in partition 0 and the check denies it. The only way to
    get a spawnable object in a fresh partition is to create it from
    ring-3 (SYS_SLS_VALLOC with owner_uid = the assigned uid, so
    `partition_id` defaults to the caller's current partition) and upload
    its binary from ring-3 (SYS_SLS_UPLOAD_BINARY). This is a real API
    surface finding: partitions as currently built isolate program
    objects too — a provisioning design decision, not just a test
    workaround (the boot check's `part_recycle` embeds the 400-byte
    child binary and uploads it per cycle).

    **Live-caught bug — stale `resume_sysret`/`park_ctx` across slot
    reuse.** A partition destroy killed a SUSPENDED child that was still
    marked `resume_sysret` after its send handoff (the handoff had
    handed the CPU to the woken parent; the child was awaiting its own
    send-resume). `process_kill()` ran the teardown but left the flag and
    the child's populated `park_ctx` in the reused descriptor slot. The
    next cycle's spawn — which reset `resume_kernel` but NOT
    `resume_sysret`/`park_ctx` — produced a "new" child that, on its
    first switch, iretq'd into `cap_sysret_resume()` and sysret'd to the
    DEAD child's post-send continuation: the fresh child exited code 0
    without ever running main, the parent stayed parked forever, and the
    kernel halted (the boot check's first symptom was a hang). Fixed with
    `proc_clear_resume_state()` (park_ctx, park_req, waiting_chan,
    has_ring3_ctx, resume_kernel, resume_sysret, handoff_target,
    pending_teardown) called at spawn init AND every teardown site
    (process_exit, process_kill, the deferred schedule_ring3 path) —
    defense in depth: a fresh process must never resume like its slot's
    previous occupant, and a dead process must never leave stale state
    behind.

    **Verification.** New `tests/partition_teardown_boot_check.sh`:
    `part_recycle` runs 4 create → assign(uid 1000) → ring-3 valloc +
    upload of the embedded child → gated spawn-into → recv → destroy
    cycles; asserts all 4 cycles, ≥4 `[TORE]` teardowns, the final
    `cap list` (objects active=0, arena free=16384/16384), and — the
    strongest proof — `/api/metrics` `ram_allocated_frames` byte-for-byte
    identical across the whole loop (73931 → 73931, 3/3 runs). The
    regression stack also passed: aerocap boot check (immediate-wake
    ordering proof + 26 teardowns + exact frame stability), cap boot
    check (kernel shell), and all four host tests (scheduler fairness,
    cap lifecycle, aerocap ABI, frame-pool reserve — the latter gained
    the watermark scenario).

    **Delta 26 — the ring-3 provisioning SDK (the wrapper layer delta 25
    named as the natural next step).** `user/libaerocap/aerocap_provision.h`
    is the skin over the object-catalog / program-upload syscalls a ring-3
    program needs to provision children INSIDE a fresh partition:
    `sls_obj_valloc()` (partition-aware creation — owner_uid, perm_mask,
    and partition_id are all expressible, unlike libsls/sls.h's pre-
    existing 3-arg `sls_valloc()`) and `sls_upload_binary()` (the 16,457-
    byte SLSUploadRequest built on the caller's stack, never a read-only
    global). The partition LIFECYCLE wrappers already existed in
    libsls/sls.h (sls_partition_create/assign/destroy — added in an
    earlier LPAR phase and previously unused by any ring-3 program), so
    the provisioning header deliberately does NOT redefine them — it
    documents and reuses them, and `part_recycle.c` now uses the SDK
    exclusively (zero raw syscall numbers or request structs remain).
    Two ABI findings surfaced while building it: (1) sls.h's
    `sls_valloc_req` was a 5-field struct missing `partition_id` and
    `database_id` — the kernel reads both, so any call would have consumed
    stack garbage; fixed to the 7-field kernel layout. (2) The new header
    is pinned by tests/aerocap_abi_host_test.c exactly like aerocap.h:
    sizeof/offsetof equality for every provisioning struct (sls.h's
    partition structs + the upload struct) and numeric equality for every
    shared constant, against kernel/partition.h, kernel/object_catalog.h,
    and kernel/loader.h. Verification: aerocap ABI host test ALL PASS
    with the new pins, partition teardown boot check 2/2 clean (frame
    count 73931 -> 73931 exact), aerocap boot check PASS, cap boot check
    PASS.

