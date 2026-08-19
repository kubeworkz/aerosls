# AeroSLS Ramdisk Driver Sidecar — Implementation Plan v0.1

**Status:** Draft for review. Implementation plan.
**Scope:** The `aerosls.ramdisk.v1` sidecar end-to-end: its manifest, entry
point, endpoint-agnostic `RD_*` server loop (with transient buffer grants),
and how it adopts endpoints at boot and at respawn. This is the driver that
the POSIX sidecar design mounts its root filesystem on
(`docs/AeroSLS-POSIX-Sidecar-Phase2-Design-v0.1.md` §5), served over the
transport of `docs/AeroSLS-Sidecar-Channels-Transport-Spec-v0.1.md`, under the
grant rules of `docs/AeroSLS-Kernel-Capability-Layer-Spec-v0.1.md`, and
respawned per `docs/AeroSLS-Driver-Respawn-Spec-Decision-v0.1.md`. The plan is
written so the driver can be built from this document alone, with the four
prior docs as reference.
**Design goal, restated:** the driver is **dumb, passive, and
connection-agnostic**. It has no filesystem knowledge, no notion of names or
peers, no timers, and no initiator role. It holds a read-only view of the
ramdisk region, serves raw blocks to whoever holds a channel to it, and
otherwise does nothing until spoken to.

---

## 0. TL;DR

- A ~300-line Rust sidecar: manifest (storage cap + console, **no declared
  POSIX channel**), the standard crt0/BootInfo bootstrap, and one event loop
  that serves `RD_*` on every CHAN cap it holds with `RECV` right.
- Endpoints are adopted from two sources, idempotently: the **initial table
  scan** (covers respawn, where the POSIX control-channel end is in the
  initial table) and **`NEW_CHANNEL` events on the console channel** (covers
  boot, where the kernel injects the POSIX endpoint via manifest wiring).
- Request buffers arrive as **transient MEM grants** with least-privilege
  rights (read buffers are W-only, write buffers are R-only); the kernel
  mints, bounds, and auto-revokes them; the driver re-checks size and rights
  before a single byte moves, and its one `unsafe` function is a
  `memmove`-semantics copy with double-checked bounds.
- First message on a new endpoint **must be `RD_INFO`** — the implicit
  handshake that both boot (POSIX sidecar §5.2) and respawn re-attach (respawn
  decision §5 step 9a) already perform.
- Malformed frames close the endpoint (`CLOSE_PROTO`); everything else is an
  error reply with the client's `req_id` echoed. The driver holds **zero
  grant state between requests** — transient grants are auto-revoked at
  reply, and the one durable grant it ever issues (`RD_MAP`) dies with the
  driver through lineage revocation, which is exactly what the VFS's
  drop-on-stale policy expects (§8.1).

---

## 1. Manifest

Source form (JSON; `genmanifest` produces the packed TLV per Phase 2 §2.2):

```json
{
  "format": "aerosls/sidecar-manifest",
  "version": "1.0",
  "personality": "aerosls.ramdisk.v1",

  "image": { "kind": "elf", "offset": 0x8000, "size": 0x4000, "entry": "_start" },

  "budget": { "mem_bytes": 262144, "stack_bytes": 16384, "heap_initial_bytes": 65536 },

  "cpu": { "share": 50, "preemptible": true },

  "limits": {
    "max_tasks": 1,
    "max_fds": 0,
    "max_channels": 16,
    "max_open_files": 0,
    "chan_queue_depth": 32
  },

  "caps": [
    { "name": "budget",  "type": "mem",  "size": 262144,    "rights": "rw" },
    { "name": "storage", "type": "mem",  "size": 33554432,  "rights": "r" },
    { "name": "console", "type": "chan", "peer": "kernel.debug.console", "rights": "rw" },
    { "name": "img.ro",  "type": "mem",  "size": 16384,     "rights": "rx" }
  ],

  "bootstrap": { "console_channel": "console", "log_level": "info" },
  "flags": { "isolated": false, "wx_policy": "w^x" }
}
```

Rationale per field:

| Field | Value | Why |
|---|---|---|
| `personality` | `aerosls.ramdisk.v1` | Registry name; kernel treats it as opaque (Phase 2 §2.1) |
| `budget` | 256 KiB | The driver's own memory is tiny: code, stack, per-endpoint state (≤16 × 48 B), heap for nothing else — **buffers are client grants, never driver allocations** |
| `cpu.share` | 50 | Half the baseline share; it is a passive server, most of its time is blocked in `wait` |
| `max_tasks` | 1 | Single-threaded server loop; no internal process model |
| `max_fds` / `max_open_files` | 0 | The driver has no POSIX layer at all — this manifest is the smallest personality in the system |
| `max_channels` | 16 | Headroom for: console (1), the POSIX-facing endpoint (1), respawn control-channel end (1), plus slack for future clients (a second sidecar wanting the ramdisk) and injected endpoints — the bound the kernel checks on injection (transport spec §2.2, respawn decision §2.2) |
| `chan_queue_depth` | 32 | Clients are window=1 and quiet; 32 messages of ≤4 KiB is generous and bounds kernel memory |
| `storage` MEM cap, **`r`** | read-only | The boot ramdisk is kernel-owned image memory. Critically, **the driver's write capability derives from its storage cap rights**: an `r` cap makes it structurally incapable of `RD_WRITE`, and it answers `RD_ERR_RO` — no policy code needed. A future writable driver changes one manifest letter |
| `console` **`rw`** | send + recv | Send for logging; **recv is required because `NEW_CHANNEL` events arrive on the console channel** (transport spec §5 — the kernel delivers path-2 injections there). The driver never expects MSG data on console, only control events |
| **no POSIX channel declared** | — | Per respawn decision §2.3: boot wiring and respawn wiring both come from the *creator's* side; the driver is endpoint-agnostic and never names the POSIX sidecar |

The manifest is identical for boot (kernel-created, path 2) and respawn
(POSIX-core-created, path 3): the only difference is *where the POSIX-facing
endpoint comes from* (injected + `NEW_CHANNEL` at boot; initial-table entry at
respawn), and the driver's adoption logic (§4) handles both with one code
path.

---

## 2. Code layout and language

Rust, `#![no_std]` with a minimal panic handler, built for the sidecar ABI.
The kernel is C; the driver and the POSIX core share one protocol crate so the
framing can never drift between the two implementations.

```
user/
  proto/                   // shared crate: RD_* framing, both sides import it
    src/lib.rs             //   header struct, cap descriptor, RD_* types,
                           //   status codes, parse/serialize (no alloc)
  ramdisk/
    Cargo.toml
    src/lib.rs             //   rust_entry: bootstrap (§3)
    src/endpoints.rs       //   EndpointSet: adoption + per-endpoint state (§4)
    src/server.rs          //   RD_* dispatch + handlers (§5, §6)
    src/copy.rs            //   the one unsafe module: block copy (§7)
    src/manifest.toml      //   manifest source, compiled by genmanifest
    src/crt0.S             //   the shared _start stub (RISC-V + x86-64)
```

`user/proto` also holds the shared channel-envelope types (magic, kind,
flags, tag) so the driver and the POSIX core literally speak the same bytes —
this is the transport spec's "canonical envelope" (§3.1) made code.

Unsafe surface (audit checklist in §7): one module, one function, two call
sites (RD_READ, RD_WRITE). Everything else in the driver is safe Rust.

---

## 3. Entry point and bootstrap

### 3.1 crt0

Identical pattern to the POSIX sidecar (Phase 2 §6.2), shared `crt0.S`:

```asm
# RISC-V
_start:
    mv   s0, a0          # 1. save BootInfo pointer
    la   sp, boot_stack_top   # 2. own stack
    mv   a0, s0
    call rust_entry      # 3. never returns
```

### 3.2 `rust_entry`

```rust
#[no_mangle]
pub extern "C" fn rust_entry(bib: *const BootInfo) -> ! {
    let bib = unsafe { &*bib };
    assert_eq!(bib.magic, BOOT_INFO_MAGIC);

    // 1. Find initial caps by name (kernel built them in manifest order).
    let budget  = bib.find_cap(CAP_MEM,  "budget").unwrap();
    let storage = bib.find_cap(CAP_MEM,  "storage").unwrap();
    let console = bib.find_cap(CAP_CHAN, "console").unwrap();

    // 2. Heap (tiny: per-endpoint state only).
    unsafe { HEAP.init(budget.base, budget.len) };

    // 3. Channel runtime + logging.
    chan_runtime::init();
    let console = chan_runtime::open(console, "console");
    log::init(console.clone());
    log::info!("ramdisk driver booting");

    // 4. Validate storage geometry (raw — no filesystem knowledge).
    let blocks = storage.len / BLOCK_SIZE;
    assert!(storage.len % BLOCK_SIZE == 0, "storage not block-aligned");
    assert!(blocks >= 1, "empty storage");
    log::info!("storage: {} blocks of {} B (read-only)", blocks, BLOCK_SIZE);

    // 5. Adopt initial endpoints: every CHAN cap except console.
    let mut eps = EndpointSet::new(console);
    for h in bib.caps.iter().filter(is_chan).filter(is_not(console)) {
        eps.adopt(h);
    }

    // 6. Serve forever.
    server::run(&mut eps, console)
}
```

Step 5 is the respawn path: when the POSIX core created this instance, its end
of the control channel is in the initial table (respawn decision §3.1), and
the scan adopts it before the client's first request can arrive.

### 3.3 What the driver does *not* do at boot

- Does not parse the filesystem superblock (that is the POSIX sidecar's VFS —
  the driver is raw-block dumb by design, Phase 2 §5.1).
- Does not contact or wait for the POSIX sidecar (it may not exist yet; the
  driver is passive — the client drives everything).
- Does not check "is my peer the POSIX sidecar" anywhere, ever (§4).

---

## 4. Endpoint adoption

### 4.1 The two sources

| Source | When | Mechanism |
|---|---|---|
| Initial table scan | always (boot **and** respawn) | `cap_info` over the BIB's CHAN caps, excluding console |
| `NEW_CHANNEL` on console | boot (path-2 injection) and any runtime injection | kernel control event (transport spec §5), payload `{handle, rights, flags, tag}` |

Adoption is **idempotent**: `adopt(handle)` adds the endpoint to the served
set if absent, ignores it if already present. The boot `NEW_CHANNEL` for the
injected POSIX endpoint is therefore belt-and-suspenders on top of the scan —
the scan cannot see the injected cap (it is not in the initial table at
`rust_entry` time — injection happens at the *POSIX* sidecar's
`create_sidecar`, after the driver started), so the `NEW_CHANNEL` event is the
actual boot notification; the idempotence covers races and the respawn case.

### 4.2 The console channel is control-only

- `console` is **not** an `RD_*` endpoint. The driver waits on it solely for
  `NEW_CHANNEL` (and would log a `CLOSE` if the kernel ever closed it — which
  should not happen; treat it as a kernel bug).
- Any MSG arriving on console is dropped and logged (the kernel should never
  send one; if it does, that is a kernel bug, not a driver decision).

### 4.3 Per-endpoint state — smaller than expected

```rust
struct Endpoint {
    handle: u32,
    state: EndpointState,          // AwaitingHandshake | Active
}
enum EndpointState { AwaitingHandshake, Active }
```

That is the *entire* persistent per-endpoint state. No outstanding-request
bookkeeping: window=1 (kernel-enforced) means the client cannot send a second
request until the driver replies, and the driver processes each request to
completion synchronously within one event-loop iteration. The transient grant
handle exists only for the duration of the handler call and is gone after the
reply (kernel auto-revoke) — **the driver holds zero grant state between
iterations** (see §8.2).

An `EndpointSet` is a fixed array of `Endpoint` (size = `max_channels`),
indexed by handle, with a `wait()` that calls `k_chan_wait` over all members.

---

## 5. The `RD_*` server loop

### 5.1 Loop (pseudocode)

```rust
pub fn run(eps: &mut EndpointSet, console: Handle) -> ! {
    loop {
        // Block until a message or control event arrives on any endpoint
        // (console included). The kernel wakes us; no timers exist here —
        // backoff/respawn is the POSIX core's job (respawn decision §5).
        let (idx, kind) = chan_runtime::wait(&eps.wait_list());

        match kind {
            Kind::MSG => {
                let h = eps.wait_list()[idx];
                if h == console {
                    log::warn!("unexpected MSG on console");     // kernel bug
                    continue;
                }
                if let Err(close) = server::dispatch(eps.by_handle(h)) {
                    // Malformed frame: close the endpoint, never interpret
                    // garbage (Phase 2 §5.4, transport spec §6.2).
                    k_chan_close(h, CLOSE_PROTO, close.detail);
                    eps.drop(h);
                    log::warn!("endpoint {h}: closed ({close})");
                }
            }
            Kind::NEW_CHANNEL { handle, .. } => {
                eps.adopt(handle);                               // §4.1
                log::info!("adopted endpoint {handle}");
            }
            Kind::CLOSE { reason, detail } => {
                if h == console {
                    log::error!("kernel closed console: {reason:#x}");  // kernel bug
                } else {
                    eps.drop(h);
                    log::info!("endpoint {h} closed: {reason:#x} detail={detail}");
                    // Client died or timed out mid-request: the kernel reaped
                    // any transient grants (§8.2); nothing to clean here.
                }
            }
        }
    }
}
```

### 5.2 Dispatch (handshake first)

```rust
fn dispatch(ep: &mut Endpoint) -> Result<(), Close> {
    let msg = recv(ep);          // header + payload + minted grants

    // --- frame-level validation (proto.rs shared with the client) ---
    check!(msg.magic == AEROSRD_MAGIC,   Close::proto(RD_ERR_PROTO));
    check!(msg.version == 1,             Close::proto(RD_ERR_PROTO));
    check!(msg.payload_len <= 4096,      Close::proto(RD_ERR_PROTO));
    check!(msg.cap_count <= 1,           Close::proto(RD_ERR_PROTO));

    // --- handshake: first message on any endpoint must be RD_INFO ---
    if ep.state == EndpointState::AwaitingHandshake {
        check!(msg.ty == RD_INFO && msg.cap_count == 0,
               Close::proto(RD_ERR_PROTO));       // reply nothing; close
        ep.state = EndpointState::Active;
        log::info!("endpoint {}: handshake complete", ep.handle);
    }

    match msg.ty {
        RD_INFO  => reply_info(ep, &msg),
        RD_READ  => read_blocks(ep, &msg),
        RD_WRITE => write_blocks(ep, &msg),
        RD_FLUSH => reply_ok(ep, &msg, 0),
        RD_MAP   => reply_map(ep, &msg),
        _        => Err(Close::proto(RD_ERR_INVAL)),   // unknown type
    }
}
```

Why `RD_INFO` first is free: the POSIX sidecar's boot handshake (Phase 2 §5.2)
and its respawn re-attach (respawn decision §5 step 9a) both send `RD_INFO`
as the first message on the endpoint. The handshake rule just makes that
order *mandatory* and gives the driver a cheap way to refuse a channel that
is not speaking the protocol.

---

## 6. Request handling

All replies echo the client's `req_id` (the tag) with `REPLY` set. Every
error is a normal reply with a status code — except frame-level violations,
which close the endpoint (§5.2). The client's `req_id` is the transport tag,
so the kernel's reply pairing (§4.3 of the capability spec) and the protocol's
correlation are the same number.

### 6.1 `RD_INFO` (no caps)

- Reply payload: `{ block_size u32 = 512, blocks u64, flags u32 }`.
- `blocks` comes from the storage cap (`storage.len / 512`, computed once at
  boot). `flags` bit0 (read-only) = `(storage.rights & W) == 0`. With the §1
  manifest that bit is always 1 — but compute it from the cap, not from a
  constant, so a future writable manifest works unchanged.

### 6.2 `RD_READ{lba u64, count u32}` — one W-only grant

```rust
fn read_blocks(ep, msg) -> Result<(), Close> {
    // payload must be exactly 12 bytes
    let (lba, count) = parse_rw(msg)?;                    // else reply RD_ERR_INVAL
    check!(count > 0 && count <= MAX_IO, reply(ep, msg, RD_ERR_INVAL));

    let grant = msg.caps.exactly_one()?;                  // else RD_ERR_CAP
    check!(grant.rights & W != 0, reply(ep, msg, RD_ERR_CAP));  // kernel min'd;
                                                            // re-check received rights
    let bytes = count as usize * BLOCK_SIZE;
    check!(grant.len >= bytes,      reply(ep, msg, RD_ERR_CAP));  // size re-check
    check!(lba + count <= blocks,   reply(ep, msg, RD_ERR_RANGE));

    // grant.ptr and storage.ptr are both validated: kernel minted the grant
    // (region + rights), we re-checked len; storage is our own cap region.
    unsafe { copy_blocks(storage.ptr.add(lba*BLOCK_SIZE), grant.ptr, bytes) };
    reply(ep, msg, RD_OK, bytes)
}
```

The grant was requested **W-only** by the client (Phase 2 §5.2) — the driver
receives a cap it cannot even *read* as data through kernel ops. In the
shared-space MVP the memory is physically readable anyway (the documented
trust-domain limitation, capability spec §5.3); the W-only mint is defense in
depth and the isolation-mode enforcement point.

### 6.3 `RD_WRITE{lba u64, count u32}` — one R-only grant

Same shape, mirrored:

```rust
check!(storage.rights & W != 0, reply(ep, msg, RD_ERR_RO));   // structural, §1
check!(grant.rights & R != 0,   reply(ep, msg, RD_ERR_CAP));
check!(grant.len >= bytes,      reply(ep, msg, RD_ERR_CAP));
check!(lba + count <= blocks,   reply(ep, msg, RD_ERR_RANGE));
unsafe { copy_blocks(grant.ptr, storage.ptr.add(lba*BLOCK_SIZE), bytes) };
reply(ep, msg, RD_OK, bytes)
```

With the read-only manifest, `RD_WRITE` always answers `RD_ERR_RO` — the
driver is structurally incapable of modifying the boot image, and a client
that asks learns it in one round trip.

### 6.4 `RD_FLUSH` (no caps)

Ramdisk in RAM: no-op, reply `RD_OK`. The handler exists so the protocol
surface is stable for a future cached/writable backend; the driver's contract
is "after a successful `RD_FLUSH` reply, all prior writes are durable" — for a
RAM region that is trivially true.

### 6.5 `RD_MAP` (no caps on request; persist grant on reply)

```rust
fn reply_map(ep, msg) -> Result<(), Close> {
    // Derive the client's durable view from OUR storage cap. The kernel
    // validates slot/rights; we request R (or RW if storage is writable).
    reply(ep, msg, RD_OK, 0,
          caps: [ CapArg {
              slot: storage_slot, offset: 0, len: storage.len,
              rights: if storage.rights & W != 0 { R | W } else { R },
              flags: PERSIST | MAP,        // durable; isolated-mode mapping
          } ])
}
```

- `PERSIST | MAP`: the grant outlives the request and, in isolation mode,
  becomes a real per-client mapping (capability spec §5). In the shared-space
  MVP the mapping is identity.
- **Lineage:** the client's grant derives from the driver's storage cap. When
  the driver dies, the kernel's deep revocation (capability spec §3.4) kills
  that descendant — the POSIX sidecar's mapped image view dies *with the
  driver*, which is exactly the VFS's drop-on-stale policy (respawn decision
  §6, §8.1). The replacement driver mints a fresh storage cap, so the
  replacement's grant is a fresh lineage — no stale authority survives.

### 6.6 `MAX_IO` and request sizing

`MAX_IO = 64` blocks (32 KiB) per request, v1. Bounds the driver's per-request
copy work and the grant size the client must allocate; matches a block-cache
readahead. Larger transfers compose: the client issues multiple requests
(its block cache does this already).

---

## 7. The block copy — the entire unsafe surface

```rust
// copy.rs — THE audited module. memmove semantics on purpose.
pub unsafe fn copy_blocks(src: *const u8, dst: *mut u8, bytes: usize) {
    // `ptr::copy` = memmove: safe even if src and dst alias, which is
    // possible in shared-space mode (a client's grant could in principle
    // overlap the storage region, whose address layout is not secret).
    core::ptr::copy(src, dst, bytes);
}
```

Preconditions, established at both call sites before this runs:

1. `src` is inside the driver's storage region: `storage.base + lba*512`,
   with `lba + count <= blocks` checked against `storage.len`.
2. `dst` is inside the grant region: the kernel minted the grant against the
   *client's* table with validated `offset/len`, and the driver re-checked
   `grant.len >= bytes` and the rights bit.
3. `bytes <= MAX_IO * BLOCK_SIZE` (32 KiB), so the copy is bounded by
   construction.

Both sides of the copy are double-checked (kernel checked rights, driver
checks size — Phase 2 §5.2 rule 4), and the driver never computes a pointer
from client-supplied data: `lba` is only used to index its own storage region
after the range check.

---

## 8. Error and teardown paths

### 8.1 Cross-document consistency checks (all deliberate)

| Behavior | Why it is correct |
|---|---|
| `RD_MAP` grant dies with the driver (lineage) | Deep revocation, capability spec §3.4 — matches VFS drop-on-stale (respawn decision §6) |
| First message must be `RD_INFO` | Matches POSIX boot handshake (Phase 2 §5.2) and respawn re-attach (respawn decision §5 step 9a); costs the client nothing |
| Endpoint-agnostic serving | Respawn decision §7: the driver never distinguishes boot wiring from respawn wiring |
| Storage rights decide `RD_ERR_RO` | Capability discipline: the driver's authority is its cap, not a policy flag (§1) |
| Console `rw` for `NEW_CHANNEL` | Transport spec §5: path-2 injections are delivered on the kernel↔driver channel |
| No timers in the driver | Backoff/respawn is the POSIX core's state machine (respawn decision §4–5); a passive server has no deadlines to miss |

### 8.2 Teardown — why the driver leaks nothing

- **Client dies or times out mid-request:** the client closes the endpoint;
  the kernel delivers `CLOSE` to the driver and **reaps the transient grant
  bound to that request** (capability spec §4.3 rule 5 — grants are
  `(endpoint, tag)`-bound). The driver drops the endpoint's `Endpoint` entry.
  No driver-side cleanup exists because the driver holds no grant state
  between iterations (§4.3).
- **Driver dies:** the kernel revokes its table (§6.3 of the capability spec),
  which unmaps/kills every grant it issued (including the client's `RD_MAP`
  view, §6.5) and delivers `CLOSE_PEER_DEAD` to the client — which is the
  respawn decision's trigger event.
- **Malformed frame:** close with `CLOSE_PROTO`, detail = the offending code.
  The client sees a protocol close and reacts per its own policy (respawn
  decision §4.3 escalates backoff on `CLOSE_PROTO`).
- **Storage cap missing/bad at boot:** `assert!` fails closed in `rust_entry`
  — a driver without storage is useless and must not start serving.

---

## 9. Testing plan

Host-side harness (per repo convention — the protocol and server code compile
and run on the host with a fake channel layer in place of the kernel):

1. **Handshake:** first message not `RD_INFO` → endpoint closed with
   `CLOSE_PROTO`; `RD_INFO` first → served; a second `RD_INFO` is a normal
   (valid) request.
2. **Rights enforcement:** `RD_READ` with a grant lacking `W` → `RD_ERR_CAP`
   reply, buffer untouched (contents must be byte-identical after the
   attempt). Same for `RD_WRITE` without `R`.
3. **Size enforcement:** grant shorter than `count × 512` → `RD_ERR_CAP`,
   no copy. `count == 0`, `count > MAX_IO`, payload length ≠ 12 → `RD_ERR_INVAL`.
4. **Range:** `lba + count > blocks` → `RD_ERR_RANGE`; boundary `lba+count ==
   blocks` succeeds.
5. **Read-only storage:** `RD_WRITE` on the §1 manifest → `RD_ERR_RO`;
   `RD_INFO.flags` bit0 = 1.
6. **Malformed frames:** bad magic, bad version, cap_count > 1 → endpoint
   closed, no reply, no partial mints (verified via the fake channel layer's
   mint log).
7. **Multi-endpoint interleaving:** two clients with independent windows;
   a large copy on client A does not delay client B beyond the copy itself
   (single-threaded; verify the wait loop yields correctly — in the MVP
   semantics, B's request is queued and processed after A's reply, which is
   correct per window=1).
8. **`RD_MAP`:** reply carries one `PERSIST|MAP` grant covering the storage
   region with R rights; in the fake layer, simulate deep revocation on
   driver "death" and verify the client's grant dies with it.
9. **Aliasing:** a grant region overlapping the storage region — the memmove
   copy still yields correct bytes for `RD_READ` (this is the shared-space
   adversarial case; the test pins the memmove choice).

---

## 10. Open questions

1. **`MAX_IO` tuning:** 64 blocks is a guess. If the POSIX block cache ever
   wants larger readahead, raise `MAX_IO` (and the client's grant size)
   together — the two are coupled by `grant.len >= count × 512`.
2. **Writable driver:** the plan is one manifest letter (`"r"` → `"rw"` on
   storage) plus the existing `RD_WRITE`/`RD_FLUSH` handlers, but a writable
   ramdisk with the VFS writing files has crash-consistency questions that
   this read-only plan deliberately does not touch (respawn decision §9 open
   question 5 already defers those).
3. **Console MSG policy:** the driver drops-and-logs any MSG on console. If
   the kernel ever legitimately needs to push control *data* (not events) to
   the driver, this needs a defined message type — defer until a workload
   asks.
4. **`RD_MAP` rights refinement:** v1 grants the client whatever the storage
   cap allows (R, or RW for a writable manifest). Should a client be able to
   request *less* (an R-only request on a writable device)? Cheap to add (a
   rights field on the `RD_MAP` request) — add it only when a writable
   backend exists and a client wants a read-only view.
