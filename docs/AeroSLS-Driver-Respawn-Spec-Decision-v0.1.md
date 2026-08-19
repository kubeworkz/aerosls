# AeroSLS Driver Respawn — Spec Decision v0.1

**Status:** Draft for review. Decision document.
**Scope:** What happens when the ramdisk driver sidecar dies and is replaced:
the channel question (same-endpoint rewire vs fresh channel), the authority
question (who respawns, with what cap), and the VFS stale-to-remount state
machine — including exactly what the POSIX sidecar observes at each step.
Closes open question #2 of
`docs/AeroSLS-Sidecar-Channels-Transport-Spec-v0.1.md` (§10) and refines
Phase 2's "init may re-spawn the driver" (`docs/AeroSLS-POSIX-Sidecar-Phase2-Design-v0.1.md`
§5.4). Depends on the capability-layer spec
(`docs/AeroSLS-Kernel-Capability-Layer-Spec-v0.1.md`) and the channel transport
spec; this document amends both, and the deltas are called out in §3.

---

## 0. The decision, in one paragraph

**A dead driver is replaced via a fresh channel, and the fresh channel comes
from `create_sidecar` itself, not from `k_chan_create` and not from a kernel
rewire.** The POSIX sidecar core holds a new cap type, `CAP_SPAWN`, naming the
driver manifest; on driver death it calls `create_sidecar`, and — per the
amendment in §3.1 — `create_sidecar` *always* returns a CHAN cap to the child,
so the core holds a channel to the replacement the moment it exists. The block
cache adopts that cap, revalidates the device (geometry + superblock), flips
the mounts from STALE back to ACTIVE, and I/O resumes. Same-endpoint rewire is
rejected (§1). Open fds on the dead device fail permanently with `EIO`; they do
not transparently reconnect. Recovery is an explicit, observable, bounded
sequence: close → stale → backoff → spawn → attach → revalidate → live.

---

## 1. Why not same-endpoint rewire

The two candidate mechanisms, restated:

- **Rewire:** the kernel keeps the POSIX sidecar's existing ramdisk CHAN cap
  valid across the replacement, repointing its endpoint payload to the new
  driver's counterpart. The handle survives; nothing on the POSIX side needs
  re-creating.
- **Fresh channel (chosen):** the old cap dies with the driver; a new channel
  is established to the replacement by the respawner.

Rewire is rejected on five grounds:

1. **Identity.** A rewire makes the POSIX sidecar's cap change meaning *under
   it* — a different principal behind the same handle, with no act on the
   holder's part. The whole capability model in this project is built on the
   opposite rule: a principal's authority is exactly the set of caps it holds,
   and a cap names one thing. "New driver = new cap" keeps the trust story
   simple and auditable; rewire makes every holder of a channel to a dying
   sidecar silently acquire a relationship to an unknown replacement.
2. **Kernel complexity.** Rewire requires the kernel to (a) retain "named
   endpoint slots" after the peer dies — extra state with a lifetime of its
   own; (b) decide automatically *which* holders get rewired (a driver may
   have several channels to several sidecars, or several channels to one
   sidecar); (c) handle the case where the replacement's manifest does not
   grant the channel the slot expects. Fresh channel adds zero kernel state:
   it reuses the exact `create_sidecar` path that already exists.
3. **Surprise semantics.** With rewire, a stale mount could silently "come
   back" — the VFS would need a new event type (`REWIRED`) and a policy for
   whether to trust the new peer behind an old handle. With fresh channel,
   recovery is driven by the POSIX core and observed as an explicit sequence;
   nothing recovers by accident.
4. **It doesn't save the expensive part.** The costly work in recovery is
   revalidation and cache invalidation (a different driver *might* serve
   different storage — the VFS cannot assume continuity). Both options pay
   that. Rewire's only saving is one syscall worth of channel creation.
5. **The problem rewire was solving doesn't exist.** The original motivation
   was circularity: after the only channel to the driver died, how do we get a
   new one? That circularity is dissolved by the §3.1 amendment —
   `create_sidecar` returns a channel to the child, so the respawner (the POSIX
   core, which is exactly the party that needs the channel) holds one before
   the replacement runs its first instruction. `k_chan_create` cannot do this
   (it requires a live channel to a live peer, which is precisely what is
   missing), which is why the respawn path is **create_sidecar**, not
   `k_chan_create` — `k_chan_create` remains for runtime topology between
   *live* sidecars (e.g., connecting to a freshly started service), where it
   was always the right tool.

---

## 2. Authority: `CAP_SPAWN` and the manifest registry

Respawn requires the POSIX sidecar to create a sidecar, and authority must
flow through a cap. This adds a third cap type.

### 2.1 The cap

```
CAP_SPAWN = 3
entry payload: { manifest_name_len u16, manifest_name }   // e.g. "drv.ramdisk.0"
```

- `CAP_SPAWN` names a manifest **in the kernel's validated registry** — the
  same registry `create_sidecar` uses at boot. The holder may call
  `create_sidecar(manifest_name)` for exactly that manifest and no other.
- This is the load-bearing security property: **a compromised POSIX core can
  respawn the driver but cannot spawn a different sidecar, and cannot fabricate
  a manifest granting itself more authority.** The kernel spawns only from its
  own validated copy of the manifest. Escalation through respawn is
  structurally impossible.
- Rights on `CAP_SPAWN`: a single bit, `SPAWN=0x1`, plus an instance-count
  flag `ONE_AT_A_TIME=0x2` (see §2.2). The registry enforces at most one live
  instance per manifest name, so a spawn storm fails `CAP_ERR_TARGET` — a
  hard kernel-side bound on churn (§8).

### 2.2 Registry rules for respawn

- The kernel registry keys instances by **manifest name** (`drv.ramdisk.0`).
  The name is stable across respawns; a dead instance's name is reusable. This
  stability is what lets other manifests pin channels to the driver by name
  (transport spec §2.2) across generations.
- At most one **live** instance per manifest name. `create_sidecar` while the
  previous instance is alive (even mid-teardown) fails `CAP_ERR_TARGET`.
- The replacement driver's budget, caps, and limits come entirely from its own
  manifest — the kernel's standard `create_sidecar` machinery. The POSIX
  sidecar's `CAP_SPAWN` grants no additional authority over the child beyond
  the control channel (§3.1).

### 2.3 Manifest change (Phase 2 sidecar)

The POSIX sidecar's manifest gains one entry:

```json
{ "name": "spawn.ramdisk", "type": "spawn",
  "manifest": "drv.ramdisk.0", "rights": "spawn|one_at_a_time" }
```

The ramdisk driver's own manifest is **unchanged**: it does not declare the
POSIX-facing channel at all (see §7 — the driver is connection-agnostic, and
both boot wiring and respawn wiring come from the *creator's* side).

---

## 3. Amendments to the prior specs

### 3.1 Transport spec §2.3 — `create_sidecar` always returns a control channel

Amendment: **every `create_sidecar` call establishes a parent–child control
channel.** The child's end is minted into its initial table (positioned and
rights-pinned if the child's manifest declares `peer: "parent"`, otherwise
appended with `SEND|RECV|CLOSE`); the parent's end is the syscall's return
value. The old wording ("the child's manifest *may* name `parent`") becomes:
the manifest's declaration is optional and only *pins*; the channel exists
regardless.

Consequences:

- Respawn works without circularity: the POSIX core's `create_sidecar` call
  returns the very channel the block cache needs.
- Tier-2 fork (Phase 2 §4.4) is unchanged — it just relies on the now-guaranteed
  channel instead of a manifest declaration.
- A child that does not want the control channel revokes it; revocation
  delivers `CLOSE_REVOKED` to the parent, which the parent treats per its
  policy (§5 step 7b).

### 3.2 Capability-layer spec — new cap type

`CAP_SPAWN = 3` with the payload and registry rules of §2. The capability
spec's "types are a registry" rule (§1.1) absorbs this without further change.

### 3.3 Phase 2 doc §5.4 — who respawns

Refinement: the **POSIX core** auto-respawns the driver (bounded backoff,
§5); `init` (a user program) does not hold spawn authority and cannot touch
kernel caps — it triggers or observes respawn through two new sidecar-ABI
entries (not kernel syscalls): `sidecar_spawn(name)` (force a respawn attempt
from the DEAD state) and `sidecar_status(name)` (report LIVE/STALE/SPAWNING/
ATTACHING/DEAD for observability).

---

## 4. The respawn state machine

Two related state machines: the **device** (the block cache's channel object)
and the **mount** (the VFS's mount-table entry referencing that device).

### 4.1 Device states (block cache)

```
        close event (terminal)
LIVE ───────────────────────────────▶ STALE
  ▲                                      │
  │  revalidate ok                      │ backoff timer fires
  │  (reset retry counter)              ▼
ATTACHING ◀─────────────── create_sidecar ok ── SPAWNING
  │                                            │ create_sidecar fails
  │ revalidate fails                          │ (retry counter++)
  └──────────────▶ STALE ◀────────────────────┘
                        │ retries exhausted
                        ▼
                       DEAD   (sidecar_spawn restarts from STALE)
```

| State | Meaning | I/O on this device |
|---|---|---|
| `LIVE` | channel up, validated | requests flow |
| `STALE` | device gone, or between attempts | all requests fail `EIO` immediately |
| `SPAWNING` | `create_sidecar` in flight (or backoff timer pending) | fail `EIO` |
| `ATTACHING` | new endpoint opened; `RD_INFO` + revalidation in progress | fail `EIO` (no requests until `LIVE`) |
| `DEAD` | retry budget exhausted, or death was non-respawnable (§5 step 1) | fail `EIO`; only `sidecar_spawn` recovers |

### 4.2 Mount states (VFS)

| State | Meaning | Path resolution | `open`/`read`/`write` |
|---|---|---|---|
| `ACTIVE` | device `LIVE` | normal | normal |
| `STALE` | device `STALE`/`SPAWNING`/`ATTACHING`/`DEAD` | **resolves** to the stale mount (does **not** fall through to `ENOENT`) | fail `EIO` |

Mounts stay in the table while stale — the path exists, the device is down.
This is deliberate: `open("/etc/passwd")` on a dead driver must fail `EIO`
(device error), not `ENOENT` (as if the file never existed). A stale mount only
transitions back to `ACTIVE` via device `LIVE` (§5 step 10); a device in `DEAD`
leaves its mounts stale indefinitely until `sidecar_spawn` restarts the cycle.

### 4.3 The transition table (one row per event)

| Event | Device | Mount | Side effects |
|---|---|---|---|
| `CLOSE_PEER_DEAD` / `CLOSE_PEER` on device channel | `LIVE→STALE` | `ACTIVE→STALE` | abort in-flight requests with `EIO`; detach endpoint; **drop device caches**; log fault kind from `detail`; arm backoff (n=0) |
| `CLOSE_PROTO` on device channel | `LIVE→STALE` | `ACTIVE→STALE` | same, but backoff starts at an escalated level (the driver broke the protocol; assume instability) |
| `CLOSE_REVOKED` / `CLOSE_ADMIN` | `LIVE→STALE→DEAD` | `STALE` | no auto-respawn (we revoked it, or the kernel removed it on purpose) |
| backoff timer fires | `STALE→SPAWNING` | `STALE` | `create_sidecar(spawn.ramdisk)` |
| `create_sidecar` returns control channel | `SPAWNING→ATTACHING` | `STALE` | open endpoint; `RD_INFO` |
| `create_sidecar` fails | `SPAWNING→STALE` | `STALE` | retry counter++; schedule next backoff; counter ≥ max → `DEAD` |
| `RD_INFO` + revalidation OK | `ATTACHING→LIVE` | `STALE→ACTIVE` | reset retry counter; recovery complete |
| revalidation fails | `ATTACHING→STALE` | `STALE` | log loudly (different device behind the name); retry counter++; backoff |
| any terminal close while `SPAWNING`/`ATTACHING` | → `STALE` | `STALE` | (the replacement died too) backoff continues |

---

## 5. Step-by-step: what the POSIX sidecar sees

Everything below is observable from the POSIX sidecar's event loop; nothing
happens silently.

**Death → detection**

1. The driver faults, exits, or (pathologically) closes its channel. The
   kernel teardown (capability-layer spec §6.3) revokes the driver's table and
   delivers a close event on every endpoint it held: the POSIX sidecar's
   `k_chan_wait` returns the ramdisk endpoint with `kind=CLOSE`,
   `reason=CLOSE_PEER_DEAD` (or `CLOSE_PEER`/`CLOSE_PROTO`), `detail` = the
   driver's exit code or fault kind. A blocked `recv`/`wait` is always woken —
   in-flight requests cannot hang.
2. The `chan_runtime` marks the channel object `CLOSED`, logs the reason, and
   wakes the block cache's waiters.
3. The block cache aborts every outstanding `RD_*` request with `EIO`, drops
   the endpoint, and flips the device to `STALE`.
4. The VFS flips every mount on that device to `STALE`. From this instant:
   `open`/`read`/`write` on the mount fail `EIO`; path resolution still finds
   the mount. **Open fds are unaffected in structure** (their `FileNode`s stay
   alive) but every subsequent I/O returns `EIO` — permanently, per §6.

**Respawn**

5. The core's driver-supervisor policy arms a backoff timer (exponential with
   jitter: 100 ms → 200 → 400 → … capped at 5 s; retry budget, default 8
   attempts). The timer runs in the event loop via `k_chan_wait` timeouts — the
   POSIX sidecar keeps running; only I/O to the stale device fails.
6. On timer expiry the core calls `create_sidecar("drv.ramdisk.0")` using its
   `CAP_SPAWN`. The kernel validates the cap and the registry
   (one-live-instance rule, §2.2), creates the replacement from the *same
   validated manifest*, and returns a CHAN cap — the control channel (§3.1).
   The new driver's initial table contains its end of that channel, plus its
   storage MEM cap (the same ramdisk region) and console.
7. Failure handling: if `create_sidecar` fails (`CAP_ERR_TARGET`: registry
   busy, budget, …), the core stays in `STALE`, increments the retry counter,
   and reschedules. If the returned channel immediately closes
   (`CLOSE_REVOKED` — the child revoked it, or it died during boot), the same:
   counter, backoff. Counter exhausted → `DEAD`, console warning, wait for
   `sidecar_spawn`.

**Re-attach and remount**

8. The core hands the control-channel cap to the block cache. The block cache
   opens it (device → `ATTACHING`).
9. Revalidation, in order:
   a. `RD_INFO` → compare `{block_size, blocks}` against the geometry
      recorded at first mount. Mismatch ⇒ this is a *different device* behind
      the same name: log loudly, fail the attach (§4.3 revalidation-fails row).
   b. `RD_MAP` (or begin `RD_READ`s) → re-read the aerofs-lite superblock →
      compare `{magic, block_size, block_count, superblock_crc}` against the
      record from first mount.
   c. Caches were dropped at step 3, so nothing from the old driver is trusted
      implicitly: every inode/dentry/page is re-read from the new driver.
10. On success the device flips `LIVE`, mounts flip `ACTIVE`, the retry counter
    resets, and the driver-supervisor logs "device recovered (attempt n)".
    New opens and continued I/O work normally. Previously-open fds remain dead
    (§6).
11. On failure at any revalidation step: device → `STALE`, backoff continues
    from the incremented counter.

---

## 6. VFS semantics: stale mounts, open fds, errno, caches

- **Stale mount, new open:** `EIO` (not `ENOENT`). The mount table entry is
  retained so the namespace stays honest (§4.2).
- **Stale mount, path traversal through it:** `EIO` at the first operation
  that needs the device. `stat`/`access` on a stale mount also fail `EIO`.
- **Open fds across the death: fail permanently.** A `FileNode` opened before
  the death keeps its offset and structure, but every `read`/`write`/`lseek`
  hits the block cache and gets `EIO`. The fd does **not** transparently
  reconnect after recovery — the process must close and re-open. Rationale:
  transparent reconnection would let an fd silently change content if the
  replacement driver serves different storage; a hard `EIO` is honest, matches
  how real systems treat a device that went away, and costs nothing.
- **Cache policy: drop on stale, always.** The block cache, page cache, and
  dentry/inode caches for the device are dropped at step 3, unconditionally.
  Revalidation (§5 step 9) is a sanity gate for flipping the mount live, *not*
  a cache-retention decision. Keeping cache on a matching image hash is a
  Phase-later optimization (§9 open question 1).
- **`/tmp` (ramfs) and `/dev` are sidecar-internal** and never go stale —
  only mounts backed by the ramdisk device. Multi-device VFS generalizes this
  per-device (each device tracks its own state machine).

---

## 7. The driver's side: connection-agnostic by construction

The full implementation plan is `docs/AeroSLS-Ramdisk-Driver-Implementation-Plan-v0.1.md`
(manifest, entry point, server loop, grant handling, testing). The properties
this decision relies on, restated:

The ramdisk driver never needs to know about respawn, rewire, or names:

- It does **not** declare the POSIX-facing channel in its manifest (§2.3).
  At boot, the kernel injects the counterpart endpoint (path 2, transport
  spec §2.2) and the driver sees a `NEW_CHANNEL` event; at respawn, the
  endpoint is in its initial table (path 3 control channel, §3.1). Either way
  it just has a CHAN cap.
- Its server loop is **endpoint-agnostic**: serve `RD_*` on every CHAN cap it
  holds with `RECV` right, one window per endpoint. It never authenticates
  peers, never checks names, never distinguishes boot wiring from respawn
  wiring. (The kernel already validated that whoever reaches it holds a channel
  cap — the driver's trust boundary is the protocol, not the peer.)
- Its storage is fixed by its manifest (the same kernel-owned ramdisk region),
  which is what makes revalidation geometry checks meaningful: a replacement
  driver *can* serve different storage only if its manifest differs — and
  manifests are kernel-validated, so that is an operator decision, not an
  attacker's.

---

## 8. Security notes

1. **No escalation through respawn.** `CAP_SPAWN` names a kernel-registry
   manifest; `create_sidecar` spawns only from the kernel's validated copy. A
   compromised POSIX core can restart the driver but cannot spawn a sidecar
   with more authority, cannot alter the driver's manifest, and cannot inject
   caps into the replacement (no amplification, capability-layer spec §1).
2. **New principal = new cap.** The replacement is reachable only through the
   control channel the spawner holds; nothing else in the system has a
   relationship to it, and no stale cap from the old instance works. Stale fds
   cannot silently attach to the new device (§6).
3. **Bounded churn.** Three independent bounds: the kernel registry's
   one-live-instance-per-name rule (a spawn storm fails `CAP_ERR_TARGET`
   immediately), the core's retry budget + exponential backoff, and the
   replacement's own manifest budget. A crash-looping driver consumes its
   backoff and stops; it cannot spin the kernel.
4. **Observability.** Every transition is an event the POSIX core can log to
   the console/tracer channel, and `sidecar_status` exposes the state machine
   to init. Recovery is never silent.

---

## 9. Open questions

1. **Cache retention on matching image hash:** if revalidation proves the
   replacement serves a byte-identical image (same `superblock_crc`, same
   geometry), the page cache *could* be kept and recovery becomes nearly free
   for read-only mounts. The spec deliberately drops unconditionally (simpler,
   safer); revisit when boot time or recovery latency matters.
2. **`CLOSE_PROTO` policy:** the spec escalates backoff on protocol
   violations, but never distinguishes "the driver is broken" from "we sent a
   malformed request" (our bug — respawning the driver won't fix our
   encoder). A future revision could add a per-protocol "blame" heuristic or
   surface the failing request for inspection.
3. **Kernel-side spawn limits beyond one-live-instance-per-name:** a
   compromised core could still spawn-and-kill the driver in a tight loop
   (each spawn fails if the previous is alive, but a fast kill frees the name).
   A per-manifest spawn *rate* limit in the kernel closes this; decide whether
   the cooperative single-threaded core makes it unnecessary (it likely does —
   the core cannot kill and respawn faster than its own event loop, and the
   backoff is in the same loop).
4. **Control-channel revocation by the child:** a replacement driver that
   revokes its control channel at boot is indistinguishable from one that died
   during boot (both → `CLOSE_REVOKED`/`CLOSE_PEER_DEAD`, both retried). That
   is fine for the driver (it doesn't need the channel to serve), but a
   hostile *spawner* could use the ambiguity to mask churn. Accepted for MVP;
   revisit if `sidecar_status` needs to distinguish "died" from "revoked".
5. **Multi-device staleness:** this state machine is per-device and generalizes
   cleanly (each block-cache device object tracks its own channel + mounts
   list). Confirm the VFS mount-table keying (device → mounts) is the right
   structure when writable block devices arrive — a writable device's stale
   semantics (dirty buffers! crash consistency) will be a separate decision,
   not an extension of this one.
