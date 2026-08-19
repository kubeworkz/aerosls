# AeroSLS Sidecar Channels — Creation & Transport Framing Spec v0.1

**Status:** Draft for review.
**Scope:** How two sidecars get a wired endpoint (channel *creation*) and what
the kernel guarantees about message delivery once they have one (transport
*framing*). Closes open question #2 of
`docs/AeroSLS-Kernel-Capability-Layer-Spec-v0.1.md` (sidecar-to-sidecar channel
creation) and pins down the send/recv envelope that the capability-layer spec
(`§4`) and the POSIX sidecar design
(`docs/AeroSLS-POSIX-Sidecar-Phase2-Design-v0.1.md`, `§3.1`, `§5`) reference.
Transport (queueing, wakeups, ordering) and the kernel envelope are specified
here; cap arguments, transient grants, tag pairing, and close reasons are
specified in the capability-layer spec and are *referenced*, not re-specified.
**Deltas from the prior docs, called out:** `k_chan_send` gains a `timeout`
argument; `k_chan_recv` gains an undersize no-loss rule; `create_sidecar`
returns a CHAN cap instead of an opaque handle; `NEW_CHANNEL` is a new control
event kind alongside `MSG` and `CLOSE`.

---

## 0. Design summary

- **A channel is a pair of endpoints; each endpoint is a CHAN cap in one
  sidecar's table.** Rights: `SEND | RECV | CLOSE`. The kernel owns the
  transport; sidecars own their endpoint caps.
- **No ambient naming, ever.** A sidecar cannot ask "give me a channel to X".
  Every channel comes into existence through exactly one of four paths, each of
  which is capability- or manifest-mediated:
  1. **Kernel-bootstrap** — the kernel itself holds the peer endpoint
     (`kernel.*` peers: console, debug/tracer). One side of the channel is
     kernel code; the sidecar's end is in its initial table from the manifest.
  2. **Manifest wiring at `create_sidecar`** — the *created* sidecar's manifest
     names an existing sidecar (`peer: "drv.ramdisk.0"`); the kernel mints one
     endpoint into the created sidecar's table and **injects the counterpart
     into the existing peer's table** (bounded by the peer's limits; the peer
     may close it). Declaration is unilateral precisely because mutual
     declaration breaks boot order (the driver sidecar is created *before* the
     POSIX sidecar exists to declare it). A peer manifest may optionally *pin*
     the channel (declare it too); the kernel then verifies rights agreement
     and fails closed on mismatch.
  3. **Parent–child at `create_sidecar`** — the child's manifest may name
     `peer: "parent"`; the kernel wires the child's endpoint and **returns a
     CHAN cap to the child as the `create_sidecar` return value**. This is how
     Tier-2 fork (process = sidecar) gets its state-handoff channel without a
     circular dependency: the parent holds a channel to the child from the
     moment the child exists.
  4. **Dynamic: `k_chan_create(peer_chan)`** — a sidecar holding a CHAN cap to
     a peer asks the kernel for a *fresh* channel to that same peer. The kernel
     delivers the peer's end as a **`NEW_CHANNEL` control event** on the
     existing channel; the peer accepts by using it or closes it. First
     channels come from paths 1–3; everything after is a chain from one of
     them — connectivity is transitive from boot wiring, which is the whole
     "no ambient namespace" argument.
- **Transport guarantees are simple because the transport is in-kernel:**
  FIFO per direction with total order including control events; no message
  loss (recv failures don't consume); backpressure via bounded per-receiver
  queues; reliable copies (no acks, no retries, no partial messages); payloads
  capped at 4 KiB with buffers carried as MEM caps (blocks, file data, image
  grants) — never inline.
- **The kernel envelope is a defined byte layout**, used verbatim by the POSIX
  sidecar's internal bus (the Phase 2 "internal bus mirrors the channel
  protocol" property) and usable unchanged by a future remote-channel transport
  (cluster), because it is fully marshalled: header + payload + cap descriptors.

---

## 1. Channel model

### 1.1 Endpoints and ownership

- A channel is a bidirectional message pipe between exactly two owners. Each
  owner holds a CHAN cap whose payload references the endpoint. The kernel
  records, per endpoint: `{ owner_sidecar, peer_sidecar, recv_queue, flags,
  outstanding_tag, grant_bookkeeping }`.
- CHAN cap rights (`SEND | RECV | CLOSE`, capability-layer spec §1.1) are
  checked on every operation; a future `cap_derive` on CHAN caps can split a
  channel into send-only/recv-only handles (deferred, capability-layer spec
  §3.2).
- The **kernel can be an owner** (console, tracer). Kernel-held endpoints never
  block on their own queue: the kernel drops-and-counts when the sidecar's
  receive queue is full (console input/logs are lossy by nature; the count is
  reported via `cap_info` for diagnostics). Sidecar-held endpoints always
  backpressure (§3.2).
- Channel state machine: `LIVE → CLOSED` (terminal). Close is idempotent and
  final (capability-layer spec §6.1). There is no half-open state in v1.

### 1.2 Endpoint identity and the kernel registry

- The kernel keeps an internal registry of sidecar names it itself minted from
  manifests (`drv.ramdisk.0`, `aerosls.posix.0`, …) → sidecar. This registry
  exists **only** for manifest resolution at `create_sidecar` time; it is not
  exposed as a syscall, so there is no ambient lookup a sidecar could use to
  enumerate or connect to peers it has no authority toward.
- Names beginning `kernel.` resolve to kernel-held endpoints and are only
  valid in manifests (a sidecar cannot create a channel to `kernel.*` at
  runtime).
- The reserved peer name `parent` resolves to the sidecar that invoked
  `create_sidecar` for this sidecar (path 3). A manifest that names `parent`
  for a kernel-created root sidecar fails closed.

---

## 2. Creation paths

### 2.1 Path 1 — kernel-bootstrap (`kernel.*`)

Wired at `create_sidecar` from the manifest (`console`, optional
`debug_channel`). The kernel mints one endpoint into the sidecar's table with
the manifest's rights (`rw` → `SEND|RECV|CLOSE`). The other end is a kernel
object. No new mechanism; specified here only for completeness of the model.

### 2.2 Path 2 — manifest wiring to an existing sidecar

At `create_sidecar(child_manifest)`:

1. Kernel parses the child's manifest; for each `CAP_CHAN` whose `peer` names
   an existing sidecar, it looks up the peer in its registry.
2. Kernel mints the child's endpoint into the child's table (rights per the
   child's manifest).
3. Kernel mints the counterpart endpoint into the **peer's** table with
   `SEND|RECV|CLOSE`, subject to the peer's `limits.max_channels` (if the peer
   is at its cap: `create_sidecar` fails, fail-closed — the child's manifest
   over-committed the peer's table).
4. The peer is **not asked**; it is informed implicitly because a new CHAN cap
   now exists in its table, and it may revoke it at will (capability-layer
   spec §3.4 — revoke is cheap and explicit). This is the incoming-connection
   model: consent is the peer's right to close, not a pre-handshake.
5. **Optional pinning:** if the peer's own manifest also declares this channel
   (same peer name, same rights), the kernel verifies the two declarations
   agree on rights and fails `create_sidecar` on any mismatch. Pinning is how a
   security-sensitive peer (e.g., the ramdisk driver) guarantees it will never
   be wired a channel with unexpected rights.

**Boot trace (Phase 2):** the kernel creates `drv.ramdisk.0` first (its
manifest: storage MEM cap, console, and optionally a *pinned* declaration of
the POSIX channel). It then creates `aerosls.posix.0`; that manifest's
`ramdisk` cap names `drv.ramdisk.0`; the kernel mints the POSIX sidecar's
endpoint from the manifest and injects the driver's counterpart endpoint into
the driver's table. The driver's server loop, which was blocked on recv of its
bootstrap channel, simply starts receiving from the new one when the POSIX
sidecar sends — no driver-side code change is needed for wiring.

### 2.3 Path 3 — parent–child (`peer: "parent"`)

At `create_sidecar(child_manifest)` where the child names `parent`:

1. Kernel mints the child's endpoint into the child's table (its manifest may
   pin rights, e.g. `rw`).
2. Kernel mints the parent's endpoint and **returns it as the syscall's result
   value** — `create_sidecar` returns a CHAN cap, not an opaque handle.

This is the Tier-2 fork primitive (Phase 2 §4.4): `fork` = `create_sidecar`
(copied manifest) + state handoff (`{regs, fd_table → caps, cwd, env}`) over
the returned channel. The parent holds a channel to the child before the child
runs its first instruction; the child holds one to its parent from its initial
table. No circular dependency, no ambient lookup — the capability relationship
is the parent–child relationship.

### 2.4 Path 4 — dynamic: `k_chan_create`

```
k_chan_create(peer_chan, rights u8, flags u16) -> handle | err
  peer_chan: CHAN cap with SEND right (validates the caller may talk to the peer)
  rights:    SEND|RECV|CLOSE subset of what the peer should get on its end
             (the caller's own end always gets SEND|RECV|CLOSE)
```

1. Kernel validates `peer_chan` (live CHAN cap, endpoint `LIVE`, peer alive).
2. Kernel mints a fresh endpoint pair: the caller's end (returned handle) and
   the peer's end with `rights`, subject to the peer's `limits.max_channels`
   (else `CAP_ERR_TARGET`).
3. Kernel appends a **`NEW_CHANNEL` control event** (see §5) to the peer's
   receive queue on the *existing* channel, carrying the new cap's handle.
   Ordering: the event is queued behind any in-flight messages on that
   endpoint, so the peer observes it in a consistent order with the rest of its
   traffic.
4. The peer accepts implicitly (the cap is in its table) or revokes it.
   Revocation delivers `CLOSE_REVOKED` to the caller with the peer's identity
   in `detail` if the peer was mid-handshake — the caller's `recv` on the new
   channel returns `CLOSE`, so a refused connection is indistinguishable from a
   closed one, which is the correct semantics.

The unilateral-push rule has one honest consequence: a peer that holds a
channel cap can be *given* new channels by the holder. The peer's defenses are
(a) its `limits.max_channels` bound — the kernel rejects the create when the
peer's table is full, so a malicious sender cannot exhaust unboundedly; and
(b) revoke-and-close. A peer that wants stricter policy can refuse by revoking
the connecting channel itself, or, in a future revision, by declaring an
explicit "accept-list" — see §10 open question 3.

### 2.5 Summary table

| Path | Who initiates | Peer consent | Use in Phase 2 |
|---|---|---|---|
| 1. `kernel.*` bootstrap | manifest | kernel | console, debug/tracer |
| 2. Manifest wiring | manifest of created sidecar | implicit (close-right) + optional pinning | POSIX ↔ ramdisk driver |
| 3. `peer:"parent"` | manifest of child | kernel returns child cap to parent | Tier-2 fork handoff |
| 4. `k_chan_create` | a sidecar holding a channel | implicit (close-right), bounds | runtime topology, respawned drivers |

---

## 3. Transport framing

### 3.1 The kernel envelope (canonical byte layout)

The kernel moves **opaque payloads + cap descriptors**; it never interprets
payload contents. The marshalled form below is the canonical envelope used by
the kernel's queue, by the POSIX sidecar's internal bus (Phase 2 §3.1), and —
unchanged — by a future remote-channel transport. Little-endian.

```
offset  size  field
0       8     magic        "AEROSCH\x01"
8       2     version      = 1
10      2     kind         MSG=0 | CLOSE=1 | NEW_CHANNEL=2
12      2     flags        bit0 REPLY | bit1 NO_REPLY
14      2     cap_count
16      4     tag          request id, chosen by the sender (0 = none)
20      2     payload_len  (≤ 4096 for MSG)
22      2     reserved (0)
24      4     reserved (0)
28      n     payload
28+n    16m   caps[]       m = cap_count × 16-byte descriptors
```

- MSG payload: opaque to the kernel (e.g., an `RD_READ{lba,count}` request —
  the protocol layer above).
- CLOSE payload (when `kind = CLOSE`): `{ reason u16, detail u32 }` — the
  close-reason registry of the capability-layer spec §6.2, delivered verbatim.
- NEW_CHANNEL payload (when `kind = NEW_CHANNEL`):
  `{ handle u32, rights u8, flags u8, tag u32 }` — the peer's new cap in its
  own table; no cap descriptors are ever attached to control events (the
  kernel mints directly and reports handles in the payload).
- Cap descriptors in MSG kind are exactly the 16-byte descriptors of the
  capability-layer spec §4.1 (`{slot, offset, len, rights, flags, pad}`);
  `n_caps ≤ 8`.

### 3.2 Queueing, flow control, backpressure

- Each endpoint has a kernel-owned **receive queue** for traffic arriving at
  that side. The queue depth is the *receiver's* manifest
  `limits.chan_queue_depth` — each side controls how much it is willing to
  buffer. Queue entries are whole messages (header + payload + minted-cap
  references).
- **Sender side:** `k_chan_send` enqueues into the peer's receive queue. If
  the queue is full, the sender **blocks** (cooperative: the kernel parks the
  sidecar and wakes it when queue space frees) unless a `timeout_ns` is given
  (§3.4), in which case it fails `CAP_ERR_TIMEOUT` with the message *not*
  enqueued and no caps minted (all-or-nothing, capability-layer spec §1.4).
- This is real flow control: a slow driver backpressures the POSIX sidecar's
  block cache; a wedged driver is handled by the deadline, not by unbounded
  buffering.
- **Cap-argument bookkeeping bound:** transient grants are minted into the
  receiver's table at *send* time (capability-layer spec §4.1), so a queued
  message's grants already occupy table slots. Bound: queued messages ×
  `n_caps ≤ chan_queue_depth × 8` entries — the kernel reserves this headroom
  against the receiver's table at `create_sidecar` and fails `create_sidecar`
  if the manifest's `chan_queue_depth × 8` would exceed the table budget.

### 3.3 Ordering guarantees

- FIFO per direction: messages from A to B arrive in exactly the order A sent
  them, and vice versa. No reordering, ever — the queue is append-only and
  drains in order.
- **Control events participate in the same total order.** A `CLOSE` or
  `NEW_CHANNEL` event is appended to the same queue as messages, so a receiver
  that has seen messages M1, M2 will never see a close event "before" them.
  This is what lets the Phase 2 VFS mark mounts stale only after the requests
  ahead of the close have been processed.
- Request/reply ordering is enforced by the **window rule** (one outstanding
  non-REPLY message per endpoint; capability-layer spec §4.3), so a reply can
  never overtake or interleave with another request on the same endpoint.

### 3.4 Send/recv signatures (extensions to the capability-layer spec)

```
k_chan_send(chan, tag u32, flags u16, payload_ptr, payload_len,
            caps[], n_caps, timeout_ns u64) -> status | err
  flags: bit0 REPLY | bit1 NO_REPLY
  timeout_ns = 0 → block until enqueued; nonzero → CAP_ERR_TIMEOUT on expiry

k_chan_recv(chan, buf, buf_len, cap_slots[], n_slots)
        -> { kind, flags, tag, len, n_caps, needed } | err
  kind = MSG | CLOSE | NEW_CHANNEL
  On undersize (buf_len < payload_len or n_slots < cap_count):
      CAP_ERR_BUFSZ, message stays queued, `needed` reports the requirement.
```

- **No-loss recv rule:** a recv that fails (`CAP_ERR_BUFSZ`, or any validation
  error) does **not** consume the message. The sidecar core always recvs with
  4 KiB buffers and 8 slots, so this path is for bug-detection, but the
  semantic must be: the kernel only consumes on success.
- `NO_REPLY` (fire-and-forget): occupies no window, carries no tag pairing,
  and — critically — **may not carry transient grants** (they are bound to a
  tag that will never receive a reply; capability-layer spec §4.2 would leak
  them). `NO_REPLY` + transient grant ⇒ `CAP_ERR_PROTO` at validation, nothing
  minted. `NO_REPLY` + `PERSIST`/`MOVE`/`MAP` caps is allowed (detached
  lifetime). Console logs and tracer output are `NO_REPLY`, cap-free.
- **Zero-copy fast path (semantics identical):** when the receiver is already
  blocked on `k_chan_recv` at send time, the kernel copies the payload
  directly sender-buffer → receiver-buffer (one copy instead of two) and mints
  caps as usual. This is an optimization only; the observable behavior is
  byte-identical to the queued path.

### 3.5 Notification and multi-endpoint wait

```
k_chan_wait(chans[], n, timeout_ns u64) -> { idx u32, kind u8 } | err
  chans: CHAN caps with RECV right, all in the caller's table
  Returns the index of an endpoint with a queued message or control event,
  or CAP_ERR_TIMEOUT after timeout_ns.
```

- The Phase 2 event loop (core's `chan_runtime` + scheduler) is a
  `k_chan_wait` over {console, ramdisk, …} plus a timer — the loop never
  busy-polls and can sleep between events.
- `timeout_ns` is how the sidecar implements the Phase 2 §5.4 client-side
  deadlines: the block-cache request's *reply* wait is a `k_chan_wait` with
  the remaining deadline; on `CAP_ERR_TIMEOUT` the core tears down the
  endpoint (per the ramdisk protocol's timeout rule) and re-opens.
- Blocked `recv` and `wait` are woken by the kernel when a message or control
  event arrives; a close event always wakes a blocked receiver (close is
  never lost — it is queued like a message).

---

## 4. What the kernel envelope guarantees (the contract)

1. **No loss.** A successfully enqueued message is delivered exactly once, or
   the endpoint closes — in which case the close event is delivered and the
   message's grants are reaped (capability-layer spec §4.3 rule 5). Recv
   failures never consume (§3.4).
2. **No reordering.** FIFO per direction, total order including control events
   (§3.3).
3. **No partial messages.** Messages are atomic: header + payload + caps are
   enqueued, delivered, and validated as one unit. A receiver never sees a
   half message, and a failed validation never leaves a partial mint.
4. **No fabrication.** A receiver can only see messages from the peer it is
   wired to, and caps it receives were validated against the sender's table by
   the kernel (capability-layer spec §4.1). There is no way to inject a
   message into a channel you do not hold.
5. **Bounded and backpressured.** Payload ≤ 4 KiB, caps ≤ 8 per message,
   queue depth per receiver manifest, send blocks or times out — never
   unbounded buffering, never silent drop (except kernel-peer console, which
   drops-and-counts by design, §1.1).
6. **Reliable by construction.** The transport is kernel memory copies; no
   acks, no retries, no CRC (the kernel path cannot corrupt or reorder). A
   protocol layer that wants end-to-end integrity over a *remote* transport
   later can add it above this envelope without changing the contract.
7. **Interruptible.** Every blocking entry point (`send`, `recv`, `wait`)
   honors either a timeout or a close event; a wedged or dead peer cannot
   wedge the caller forever.
8. **Teardown-complete.** Every terminal path (reply-reap, close, peer death,
   revocation) is specified in the capability-layer spec §4.3/§6 and holds
   over this transport unchanged.

---

## 5. Control events

Control events are kernel-generated messages in the same queue as data. They
carry no cap descriptors, occupy no request window, and are never sent by
sidecars (a sidecar cannot forge a close event or a `NEW_CHANNEL`).

| Event | Payload | Semantics |
|---|---|---|
| `CLOSE` | `{reason, detail}` | Terminal (§6 of the capability-layer spec). Reason registry: `CLOSE_PEER`, `CLOSE_PEER_DEAD`, `CLOSE_REVOKED`, `CLOSE_PROTO`, `CLOSE_ADMIN`, 0x8000+ protocol-defined. Never lost; always wakes a blocked receiver. |
| `NEW_CHANNEL` | `{handle, rights, flags, tag}` | A new endpoint was minted into *this* sidecar's table by a peer (`k_chan_create`) or by the kernel (path 2 injection at `create_sidecar` — the kernel may also emit this event for path-2 injections so the peer's runtime learns about the cap *without* having to notice an unexplained table entry). The receiver accepts by using it or revokes it. |

Path-2 injection emits `NEW_CHANNEL` too (step 4 of §2.2): the peer's event
loop sees the event and can `cap_info` the handle. This keeps the peer's
runtime fully informed of its own table, which matters for auditability and
for the driver's server loop (it can open the new endpoint and start serving
without polling for "unknown" caps).

---

## 6. Limits and DoS surface

| Limit | Enforced by | Purpose |
|---|---|---|
| `payload_len ≤ 4096` | kernel at send | Blocks and file data travel as MEM caps; inline payloads are control/small-data only |
| `n_caps ≤ 8` | kernel at send | Bounded per-message table churn |
| `chan_queue_depth` (per receiver) | kernel queue | Backpressure; bounds kernel memory per channel |
| `limits.max_channels` (per sidecar) | kernel at mint | Bounds the table; a malicious peer's `k_chan_create` storm fails `CAP_ERR_TARGET` when the peer's table is full |
| `window = 1` | kernel per endpoint | Request/reply ordering; bounds grant bookkeeping |
| `timeout_ns` | caller | A wedged peer cannot wedge the caller (Phase 2 §5.4) |
| `queue_depth × 8` table headroom | kernel at `create_sidecar` | Guarantees queued messages' grants always fit (§3.2) |

The two deliberately accepted asymmetries: (a) kernel-peer endpoints drop
(console) instead of backpressuring — the kernel must never block on a
sidecar; (b) unilateral channel push (paths 2/4) means a peer can be given
channels it did not request — bounded, closable, and revocable, per §2.4.

---

## 7. Error codes (extension of the capability-layer spec §7)

| Code | Name | Added by | Meaning |
|---|---|---|---|
| 12 | `CAP_ERR_BUFSZ` | this spec | recv buffer or cap slots too small; message NOT consumed; `needed` reported |
| 13 | `CAP_ERR_TIMEOUT` | this spec | send/wait deadline expired; nothing enqueued/minted on send |

All other codes (0–11) are unchanged and used verbatim (`CAP_ERR_STATE` for
send-on-closed and window violations, `CAP_ERR_PROTO` for `NO_REPLY`+transient
and malformed envelopes, `CAP_ERR_TARGET` for a dead or table-full peer).

---

## 8. Layering

```
Layer 3  protocol          RD_* (Phase 2 §5), POSIX internal bus, future
                           registry protocol — payload contents, opaque to kernel
Layer 2  envelope          magic/kind/flags/tag/caps — THIS SPEC (canonical bytes)
Layer 1  transport         kernel queue, FIFO, backpressure, wakeups — THIS SPEC
Layer 0  caps              mint/derive/revoke, grants, close reasons —
                           capability-layer spec
```

- The POSIX sidecar's **internal bus** (Phase 2 §3.1) is Layer 2 + Layer 3:
  components exchange the same envelope over in-core mailboxes, with cap
  arguments as `Arc`-backed handles instead of kernel mints. Splitting a
  component into its own sidecar later is a transport swap (mailbox → channel)
  with zero reformatting — the Phase 2 design goal, made concrete by the
  canonical byte layout.
- A **remote-channel transport** (cluster, future): the envelope is fully
  marshalled, so a channel backed by a network connection is a Layer-1
  replacement. The guarantees of §4 that the network cannot provide (no
  reordering, no loss) become the transport's responsibility (sequencing,
  retransmit, CRC) — which is why §4's guarantee 6 is stated as "reliable by
  construction *because* the transport is in-kernel".

---

## 9. Security notes

- **No ambient naming:** every creation path names the peer through either a
  validated manifest (paths 1–3, kernel-resolved) or a held CHAN cap (path 4).
  The kernel registry is never exposed; there is no "connect to sidecar by
  name" syscall. This is the property the Phase 2 security analysis (§7) leans
  on for "a user program cannot address the ramdisk driver directly" — it
  holds at the channel layer by construction.
- **Injection is bounded and revocable:** path-2/4 pushes respect the peer's
  `max_channels` and the peer's right to revoke; a refused or revoked channel
  surfaces as `CLOSE_REVOKED`/`CLOSE_PEER_DEAD` with the peer's identity in
  `detail`, so a connecting sidecar can distinguish "peer refused" from "peer
  died".
- **No forging of control events:** `CLOSE`/`NEW_CHANNEL` are kernel-generated
  (§5). A sidecar cannot fake a peer's death or inject a channel into a peer's
  table itself — only the kernel mints, per capability-layer spec §1.2.
- **Fire-and-forget cannot leak grants:** `NO_REPLY` + transient is rejected
  atomically (§3.4), so the only cap-carrying one-way traffic is explicitly
  `PERSIST`/`MOVE`/`MAP`, whose lifetimes are owned and revocable.

---

## 10. Open questions

1. **Accept-list for peers** (path-2/4 hardening): should a manifest be able
   to declare "only accept channels from `drv.ramdisk.0`"-style pins that make
   the kernel *refuse* unknown injectors instead of injecting? The Phase 2
   trust-domain model doesn't need it, but the ramdisk driver is the natural
   first candidate if it does. (Pinning today verifies rights; it does not
   restrict *who* may inject.)
2. **Channel re-creation after death:** **DECIDED** — see
   `docs/AeroSLS-Driver-Respawn-Spec-Decision-v0.1.md`. Fresh channel via
   `create_sidecar`'s control-channel return (this spec's §2.3, amended to be
   unconditional), never same-endpoint rewire; `k_chan_create` cannot do it
   (it needs a live peer). The decision also adds `CAP_SPAWN` authority and
   the VFS stale→backoff→spawn→attach→revalidate→live state machine.
3. **Console backpressure:** drop-and-count for kernel-peer endpoints is right
   for logs, but if `/dev/console` ever needs lossless input (interactive
   editing with echo), the console channel needs either a bigger queue or a
   flow-control rule. Defer; document the count in `cap_info` now.
4. **Window > 1:** still deferred (capability-layer spec §10.1). This spec's
   `tag` is the same field, so lifting the window is bookkeeping-only.
5. **Byte-order and `magic` policy:** v1 is fixed little-endian with no
   negotiation; the magic covers format, not endianness. If a remote transport
   ever crosses heterogeneous nodes, the envelope needs a BOM/endianness bit —
   cheap to add at Layer 1 without touching Layer 2's fields.
