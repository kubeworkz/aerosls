# AeroSLS POSIX Compatibility Sidecar — Phase 2 Design v0.1

**Status:** Draft for review.
**Scope:** Design of the POSIX compatibility sidecar built on the Phase 1 capability
primitives (capability tables, MEM caps, channel endpoints, `create_sidecar()`).
The kernel never implements `open()`, `read()`, `fork()`, etc. natively — every
POSIX operation is synthesized inside the sidecar from capabilities and channel
messages. Deliverables covered: (1) manifest format, (2) architecture + `open()`
walkthrough, (3) fork/exec semantics, (4) ramdisk driver channel protocol,
(5) bootstrap sequence, (6) security analysis.

---

## 0. Executive summary

- A **sidecar** is the unit of kernel-enforced isolation: one address space, one
  kernel capability table, one schedulable entity, one manifest. Everything the
  kernel does for the POSIX world is: load a manifest, build a capability table,
  run the sidecar, and move capabilities across channels. That's it.
- Inside the POSIX sidecar, a **trusted core** (Rust, single thread, cooperative
  scheduling) implements the POSIX *process model* (tasks, fds, pathnames,
  permissions) and owns **all** of the sidecar's kernel capabilities. User
  programs (shell, BusyBox applets) never hold or even see a kernel capability
  handle: they interact exclusively through a sidecar-local syscall ABI.
- **File descriptors are sidecar-local capabilities.** An fd is an index into a
  per-task table of `{node, rights, flags}` entries owned by the core. `fork()`
  copies the table (refcounted nodes); `exec()` trims it by `FD_CLOEXEC`;
  `dup2()` copies one entry. Rights are fixed at `open()` time
  (`O_RDONLY` ⇒ read-only fd) and checked on every operation — no amplification.
- **`fork()` is vfork-class memory sharing in the MVP** (BusyBox/ash is designed
  for exactly this; it is how uClinux runs the same toolchain). Exec replaces
  the task context. Three escalation tiers are defined, ending at
  "process = separate sidecar" for true kernel-enforced process isolation.
- **The ramdisk driver is a dumb block server.** It understands one thing: raw
  blocks, exchanged via a fixed channel protocol (`RD_INFO` / `RD_READ` /
  `RD_WRITE` / `RD_FLUSH` / `RD_MAP`). Buffers are passed as transient MEM-cap
  grants with least-privilege rights; the driver has zero filesystem knowledge
  and cannot become a confused deputy.
- **Threat model honesty:** within one sidecar (one unprotected address space),
  capabilities and Rust memory safety fully contain *buggy-but-not-malicious*
  code. A *malicious* program in the same address space can corrupt the sidecar's
  own core state — so the sidecar is a single trust domain. Malicious process
  isolation requires the Phase 4 per-sidecar (or per-task) MMU isolation model,
  whose kernel-side prerequisite (mapping MEM caps into a target address space)
  is flagged as the one Phase 1 extension this design needs.

---

## 1. Phase 1 contract — the primitives we build on

This design assumes Phase 1 delivered exactly the following, and nothing more:

| Primitive | Semantics assumed |
|---|---|
| **Capability table** | Per-sidecar kernel-managed array of entries `{type, rights, payload}`. The kernel is the only authority that can mint, derive, revoke, or transfer entries. A sidecar refers to its caps by table index (handle). Types used here: `MEM`, `CHAN`. |
| **MEM cap** | Names a region `{base, len}` with rights `r/w/x`. Owner can request a **derived** cap (rights ⊆ held, region ⊆ held) or **transfer** a cap to another sidecar's table via a channel. *Phase 1 gap:* for isolation mode (Phase 4) the kernel must also support **mapping** a MEM cap into a *target sidecar's* address space with per-principal page permissions. In the shared-address-space MVP this mapping is identity — see §7. |
| **Channel endpoints** | Bidirectional message channels between two sides (kernel↔sidecar or sidecar↔sidecar). `send`/`recv` of messages, each of which may carry **capability arguments** (handles). Bounded queues (`chan_queue_depth`), blocking with kernel wakeup, and a **close event** delivered to the peer when the other side dies — the crash-notification primitive. |
| **`create_sidecar(manifest_ref)`** | Kernel loads a manifest, builds the initial capability table, maps the sidecar image, allocates the memory budget, and starts the sidecar at its entry point with a pointer to a Boot Info Block. |
| **Sidecar** | One address space + one capability table + one kernel schedulable entity + one manifest. The kernel's scheduler runs sidecars cooperatively/preemptively per `cpu.share`; the sidecar itself decides how to multiplex its own tasks. |

Two consequences of this contract that shape everything below:

1. **All authority flows through caps; there is no ambient namespace.** A sidecar
   can name only what its table holds. The POSIX sidecar gets *exactly* the caps
   in its manifest: a memory budget, a console/debug channel, and a channel to
   the ramdisk driver. Nothing else.
2. **The kernel exposes no POSIX syscalls.** The only kernel entry points are
   capability-table operations and channel send/recv. `open("/etc/passwd")` is a
   journey through sidecar code, not a syscall.

---

## 2. Deliverable 1 — the sidecar manifest

The manifest is the entire contract between the kernel and a sidecar: identity,
initial authority, resources, and debug plumbing. It exists in two forms:

- **Source form** — JSON, for humans and tooling (`genmanifest`).
- **Packed form** — a fixed TLV binary blob that the kernel parses at
  `create_sidecar()` time with zero string parsing and no allocation.

### 2.1 Source form

```json
{
  "format": "aerosls/sidecar-manifest",
  "version": "1.0",
  "personality": "aerosls.posix.v1",

  "image": { "kind": "elf", "offset": 0x4000, "size": 0xC000, "entry": "_start" },

  "budget": {
    "mem_bytes": 33554432,
    "stack_bytes": 262144,
    "heap_initial_bytes": 8388608
  },

  "cpu": { "share": 200, "preemptible": true },

  "limits": {
    "max_tasks": 64,
    "max_fds": 4096,
    "max_channels": 128,
    "max_open_files": 512,
    "chan_queue_depth": 64
  },

  "caps": [
    { "name": "budget",   "type": "mem",  "size": 33554432, "rights": "rw" },
    { "name": "img.ro",   "type": "mem",  "size": 1048576,  "rights": "rx" },
    { "name": "console",  "type": "chan", "peer": "kernel.debug.console", "rights": "rw" },
    { "name": "ramdisk",  "type": "chan", "peer": "drv.ramdisk.0",        "rights": "rw" }
  ],

  "bootstrap": {
    "console_channel": "console",
    "debug_channel": null,
    "log_level": "info"
  },

  "flags": { "isolated": false, "wx_policy": "w^x" }
}
```

Field semantics:

| Field | Meaning |
|---|---|
| `personality` | Registered name of the compatibility layer the sidecar implements. `aerosls.posix.v1` selects the POSIX core. The kernel treats it as opaque; a personality registry maps it to expected image ABI. |
| `image` | Where the sidecar binary lives (offset/size inside the kernel image or boot module for MVP) and its entry point. The kernel maps it `rx` and maps data `rw` per `wx_policy`. |
| `budget.mem_bytes` | **Kernel-enforced hard ceiling.** The kernel grants one MEM cap for the whole budget and accounts every subsequent grant/derivation against it. The sidecar carves heap, stacks, cache pool, and mmap region from it. Exceeding it fails at the allocator, not in the kernel. |
| `cpu.share` | Scheduling hint: weight relative to a baseline of 100. 200 ≈ twice the default CPU share. The kernel may clamp it; `preemptible` tells the kernel whether it may preempt mid-sidecar (see §3.3). |
| `limits.*` | Resource ceilings enforced *inside* the sidecar core (task count, fd count, channel count, open files, per-channel queue depth). A fork bomb hits `max_tasks` and gets `EAGAIN` — the sidecar fails safe, the kernel budget is never touched. |
| `caps[]` | The **complete initial authority**. Named entries the sidecar looks up at boot (names are only meaningful inside the sidecar; the kernel just builds the table). Rights are the least privilege needed: budget `rw`, image `rx` (W^X), channels `rw`. |
| `bootstrap` | Debug plumbing. `console_channel` names a channel cap used for logging and `/dev/console`; `debug_channel` may name an optional kernel **tracer** channel that receives a copy of every internal message (syscall trace, cap transfers) — the Phase 2 debugging hook. |

### 2.2 Packed (binary) form

The kernel parses this. Layout is little-endian, fixed-width where possible:

```
offset  size  field
0       8     magic          "AERSLSM1"
8       2     version_major  = 1
10      2     version_minor  = 0
12      2     record_count
14      2     flags          bit0 tolerate_unknown | bit1 strict_caps
16      4     total_len      (whole blob, for bounds checking)
20      4     body_crc32
24      ...   records        (record_count × TLV)
```

Record: `{ tag: u16, len: u16, payload: bytes }` — 4-byte header, payload `len`
bytes, records packed back-to-back.

| Tag | Name | Payload |
|---|---|---|
| 0x0001 | `PERSONALITY` | `name_len u16` + name bytes (UTF-8) |
| 0x0002 | `IMAGE` | `offset u32, size u32, entry u64` |
| 0x0003 | `BUDGET` | `mem_bytes u64, stack_bytes u32, heap_initial u32` |
| 0x0004 | `CPU` | `share u16, flags u8` |
| 0x0005 | `LIMITS` | `max_tasks u16, max_fds u16, max_channels u16, max_open_files u16, chan_queue_depth u16` |
| 0x0006 | `CAP_MEM` | `name_len u16, name, base u64, size u64, rights u8` |
| 0x0007 | `CAP_CHAN` | `name_len u16, name, peer_len u16, peer, rights u8, flags u8` |
| 0x0008 | `BOOTSTRAP` | `console_name_len u16, console_name, dbg_name_len u16, dbg_name, log_level u8` |
| 0x0009 | `FLAGS` | `flags u32` (`isolated`, `wx_policy`) |
| 0x7F00 | `SIGNATURE` | reserved slot for Phase 3 (signed manifests); ignored by v1 kernel |

`CAP_MEM`/`CAP_CHAN` map 1:1 to entries in the initial capability table, in
record order — the kernel fills the sidecar's table left-to-right and the
sidecar's Boot Info Block (see §6) reports handle indices in the same order, so
the packed manifest and the runtime view can never disagree.

### 2.3 Versioning and extensibility

- **Major version = breaking.** The kernel supports `[1, N]`. A manifest with
  `version_major > N` is refused outright; one with `version_major` below the
  kernel's floor is refused (the kernel no longer knows how to build its caps).
  Major bumps are for changes to cap semantics, entry ABI, or budget model.
- **Minor version = additive.** A newer minor adds records the kernel can skip.
  `tolerate_unknown` (header flag bit 0) tells the kernel whether unknown *tags*
  are skippable or fatal. `strict_caps` (bit 1) makes any unrecognized cap
  record fatal — used for security-sensitive sidecars that must know exactly
  what they hold.
- **Extensibility rules:** new cap types add new tags (never overload old ones);
  personalities are a registry, so a `aerosls.posix.v2` can redefine the syscall
  ABI without touching the manifest format; the `SIGNATURE` slot is reserved now
  so v1 images don't need re-layout later; all reserved fields must be written
  as zero and ignored on read.
- The CRC covers the whole blob so a corrupted manifest fails at
  `create_sidecar()` time, before any cap is minted.

---

## 3. Deliverable 2 — architecture and the `open()` path

### 3.1 Block diagram

```
                 AeroSLS POSIX sidecar  (personality: aerosls.posix.v1)
  ┌─────────────────────────────────────────────────────────────────────────────────────┐
  │                                                                                     │
  │   ┌───────────┐  ┌───────────┐  ┌───────────┐  ┌───────────┐   ┌────────────────┐   │
  │   │  shell    │  │  busybox  │  │  init     │  │  app N    │   │  libc (stubs   │   │
  │   │  task 3   │  │  task 2   │  │  task 1   │  │  task 4   │   │  + crt0)       │   │
  │   └─────┬─────┘  └─────┬─────┘  └─────┬─────┘  └─────┬─────┘   └───────┬────────┘   │
  │         │              │              │              │                 │            │
  │         └──────── syscall ABI (sidecar trap) ────────┴─────────────────┘            │
  │                                   │                                                │
  │   ┌───────────────────────────────▼──────────────────────────────────────────────┐  │
  │   │                        TRUSTED CORE (Rust, one thread)                       │  │
  │   │   ┌─────────────┐  ┌─────────────┐  ┌─────────────┐  ┌────────────────────┐  │  │
  │   │   │ dispatcher  │  │ proc manager│  │ VFS         │  │ chan runtime       │  │  │
  │   │   │ syscall     │  │ tasks,      │  │ mounts,     │  │ endpoints, cap     │  │  │
  │   │   │ dispatch,   │  │ fork/exec,  │  │ pathname,   │  │ passing, event     │  │  │
  │   │   │ arg checks  │  │ wait, sched │  │ perms       │  │ loop               │  │  │
  │   │   └──────┬──────┘  └──────┬──────┘  └─────┬───────┘  └─────────┬──────────┘  │  │
  │   │          │          internal bus (mailboxes, same envelope     │              │  │
  │   │          │          as channel msgs)                           │              │  │
  │   │          └───────────┬───────────────┬─────────────────────────┘              │  │
  │   │                      │               │                                        │  │
  │   │                      ▼               ▼                                        │  │
  │   │           ┌──────────────────┐  ┌──────────────────┐                          │  │
  │   │           │ block cache      │  │ device layer     │                          │  │
  │   │           │ aerofs-lite      │  │ /dev/console     │                          │  │
  │   │           │ parse, buffer    │  │ /dev/null        │                          │  │
  │   │           │ pool, LRU        │  │ /tmp (ramfs)     │                          │  │
  │   │           └────────┬─────────┘  └──────────────────┘                          │  │
  │   └────────────────────┼──────────────────────────────────────────────────────────┘  │
  │                        │                                        │                    │
  └────────────────────────┼────────────────────────────────────────┼────────────────────┘
                           │ channel (cap)                          │ channel (cap)
                           ▼                                        ▼
            ┌────────────────────────────┐         ┌─────────────────────────────┐
            │  ramdisk driver sidecar    │         │ kernel debug console /      │
            │  raw blocks only (RD_*)    │         │ tracer channel              │
            └────────────────────────────┘         └─────────────────────────────┘
```

The **internal bus** is a deliberate mirror of the external channel protocol:
components (dispatcher, proc manager, VFS, block cache) exchange the *same*
message envelope, with capability arguments, over mailboxes inside the sidecar.
This uniformity is what makes §3.4's walkthrough literal ("libc stub sends a
channel message to the VFS component"), and it means any component can later be
split out into its own sidecar without redesigning its interface.

### 3.2 Mapping the POSIX process model onto AeroSLS

| POSIX concept | AeroSLS implementation |
|---|---|
| Process | Sidecar-internal **task**: `{id, regs, stack, fd_table, cwd, euid/egid, mem_regions, sig_pending, state}` owned by the proc manager. One sidecar = one trust domain; many tasks. |
| Thread | A task sharing an fd_table (clone flags `CLONE_FILES`). MVP runs all tasks cooperatively on the sidecar's single kernel thread (§3.3). |
| Kernel thread / scheduler | The kernel schedules the *sidecar* (one entity, `cpu.share` weight). Inside, the proc manager runs a cooperative round-robin of tasks. |
| File descriptor | Sidecar-local capability: index into per-task `fd_table` of `{node: Arc<dyn FileLike>, rights, flags}`. `FileLike` = `FileNode` (inode+offset+cache), `PipeNode`, `ChanDev` (console), `NullDev`, `MmapRegion`. |
| Open file description (shared offset) | The `FileNode` — shared by `dup`/`fork`, so `dup2`d fds correctly share the file offset. |
| Address space | In the MVP the whole sidecar shares one address space; `mem_regions` is per-task *metadata* (which ranges the task's heap/stack/mmap occupy) used for argument validation and future isolation. |
| Pathname namespace | The VFS's mount table (`/` ← aerofs-lite over ramdisk, `/tmp` ← ramfs, `/dev` ← devfs). No ambient namespace: a path is resolved only through the VFS, which is core code. |
| Permission model | Mode-bit checks (uid/gid from the task) for POSIX compatibility. The *real* authority is capability-based: you can only reach a file if the core gave you an fd for it. |

### 3.3 Scheduling and the syscall ABI

User code enters the core through a **sidecar trap**: a `syscall`-style
instruction whose vector lands in the core's dispatcher (the kernel is not
involved). The dispatcher:

1. Saves the task context, switches to the core stack.
2. Validates the ABI frame (syscall number, argument pointers, lengths — every
   user pointer must lie inside the *calling task's* `mem_regions`).
3. Posts the request as an internal-bus message to the owning component, tagged
   with the task id, and runs the event loop until the reply (or yields to the
   next task).
4. Copies the result out and restores the task context.

Preemption happens only at **safe points**: when a task blocks on a channel
recv, a mailbox reply, or an explicit yield. The core's data structures are
therefore never mutated mid-operation — no data races, no locks needed, which
is a security property as much as a performance one (§7). `cpu.preemptible`
tells the kernel whether it may also preempt the whole sidecar at its own
boundaries (i.e., between messages); a non-preemptible sidecar gets a fixed
quantum between channel operations.

The MVP syscall surface (all core-implemented, none kernel-implemented):
`open, close, read, write, lseek, stat, access, fork, execve, exit,
waitpid, pipe, dup, dup2, mmap, munmap, brk, getpid, getppid, kill,
chdir, getcwd, ioctl, readdir` (plus the `gettimeofday`-class stubs BusyBox
wants). Everything beyond this is `ENOSYS` — the personality grows the surface,
the kernel never does.

### 3.4 `open("/etc/passwd", O_RDONLY)` — full walkthrough

The sidecar's 32 MiB budget is laid out as (illustrative):

```
0x0000_0000  image (rx)                1 MiB
0x0010_0000  core data (rw)            1 MiB
0x0020_0000  heap (rw)                16 MiB   <- allocator
0x0120_0000  task stacks (rw)          2 MiB   <- 8 × 256 KiB
0x0140_0000  block cache pool (rw)     2 MiB   <- 4096 × 512 B slots
0x0160_0000  mmap region (rw)         10 MiB   <- exec'd images
```

Step-by-step (messages shown as `TYPE{payload}; caps[]`):

```
 shell            libc stub         dispatcher        VFS              block cache       ramdisk driver
   │  open(2)          │                  │                │                   │                │
   │───sidecar trap────▶                  │                │                   │                │
   │                   │─ SYS_OPEN        │                │                   │                │
   │                   │  {path:"/etc/    │                │                   │                │
   │                   │   passwd",       │                │                   │                │
   │                   │   flags:O_RDONLY,│                │                   │                │
   │                   │   caller:task3} ─▶ dispatcher     │                   │                │
   │                   │                  │─ OPEN{path,    │                   │                │
   │                   │                  │   flags,       │                   │                │
   │                   │                  │   caller:task3}▶ VFS               │                │
   │                   │                  │                │─ canonicalize;    │                │
   │                   │                  │                │  resolve mount "/"│                │
   │                   │                  │                │  → ramfs(ramdisk) │                │
   │                   │                  │                │─ walk: "etc"      │                │
   │                   │                  │                │  "passwd" (cache  │                │
   │                   │                  │                │  miss on inode)   │                │
   │                   │                  │                │─ READ{ino:2,...} ─▶│                │
   │                   │                  │                │                   │─ RD_READ       │
   │                   │                  │                │                   │  {lba, count}  │
   │                   │                  │                │                   │  caps:[MEM     │
   │                   │                  │                │                   │   grant buf,   │
   │                   │                  │                │                   │   rights=W]    │
   │                   │                  │                │                   │───channel─────▶│
   │                   │                  │                │                   │ (kernel mints  │
   │                   │                  │                │                   │  W-only derived│
   │                   │                  │                │                   │  cap in driver)│
   │                   │                  │                │                   │   write 512 B  │
   │                   │                  │                │                   │◀──RD_OK{1}─────│
   │                   │                  │                │◀─ READ{data}──────│ (grant auto-  │
   │                   │                  │                │                   │  revoked)      │
   │                   │                  │                │─ parse inode 2;   │                │
   │                   │                  │                │  read dir block → │                │
   │                   │                  │                │  find "passwd"    │                │
   │                   │                  │                │  (ino 5, size 512)│                │
   │                   │                  │                │─ data block → cache│               │
   │                   │                  │                │─ permission check:│                │
   │                   │                  │                │  mode bits vs     │                │
   │                   │                  │                │  task3 euid/egid  │                │
   │                   │                  │                │─ alloc FileNode + │                │
   │                   │                  │                │  fd entry         │                │
   │                   │                  │                │  rights={R}       │                │
   │                   │                  │◀─OPEN{fd:5}────│                   │                │
   │                   │◀─ 5──────────────│                  │                   │                │
   │◀─ 5────────────────│                  │                │                   │                │
```

The numbered sequence, with the capability transfers made explicit:

1. **libc stub → dispatcher.** `open("/etc/passwd", O_RDONLY)` traps; the stub
   marshals `SYS_OPEN{path, flags, mode, caller}`. The dispatcher validates the
   `path` pointer lies inside task 3's `mem_regions` and copies the string into
   core memory — after this, no user pointer crosses the core boundary.
2. **dispatcher → VFS (internal bus).** `OPEN{path:"/etc/passwd",
   flags:O_RDONLY, caller:3}`. No capability argument: paths are resolved by
   the VFS, never handed across the bus.
3. **VFS pathname resolution.** Canonicalize (reject `..` above root, cap
   symlink depth); find mount `/` → aerofs-lite over the ramdisk channel;
   walk dentries `etc` → `passwd`. Inode for `etc` is not cached yet.
4. **VFS → block cache.** `READ{ino:2, blk:0, nblk:1, dst:cache_slot}`.
5. **block cache → ramdisk driver (external channel).**
   `RD_READ{lba, count:1}` **with a capability argument**: `caps[0] = MEM
   grant` for the cache slot, **rights requested = W only** (the driver will
   write into it). The kernel validates the POSIX sidecar actually holds
   `rw` on that slot and mints a *derived* W-only cap in the driver's table.
   The driver bounds-checks `len ≥ count × 512` (else `RD_ERR_CAP`, touches
   nothing), copies 512 bytes, replies `RD_OK{1}`.
6. **Grant teardown.** The channel runtime auto-revokes the transient grant
   when the reply is sent. The driver never accumulates memory rights.
7. **VFS assembles the file.** Parses the inode (mode, uid, size, block
   pointers), reads the directory block to find `passwd` → inode 5, size 512.
   Data block read (same RD_READ pattern) lands in the cache; the FileNode
   references the cached page.
8. **Permission check.** Mode bits of inode 5 vs task 3's `euid/egid` (and the
   VFS's own capability view: the mount is only reachable through core code).
9. **fd allocation.** The VFS allocates fd 5 in task 3's table:
   `{node: FileNode(inode 5, offset 0), rights: {R}, flags: 0}` — rights come
   *from the open flags*, so this fd can never write, even to a writable file.
10. **Reply.** `OPEN{fd:5}` up the bus; the syscall returns `5`.

A subsequent `read(5, buf, 200)` follows the same shape minus path resolution:
dispatcher validates `buf` in task 3's regions → `READ{fd:5, buf, 200}` → VFS
copies from the cached page (offset 0, advances the shared FileNode offset) →
`{n:200}`. On a cache miss the block cache repeats steps 5–6 with the cache
slot; the page is then copied into the user buffer. (For the common
read-the-whole-file case, see the `RD_MAP` fast path in §5.2.)

---

## 4. Deliverable 3 — fork/exec without a kernel `fork()`

The kernel offers one way to create execution: `create_sidecar()`. Everything
else is the proc manager synthesizing the POSIX illusion.

### 4.1 The clone operation (sidecar-internal)

`fork()` → trap → `SYS_FORK{caller}` → proc manager:

```
proc_mgr::clone(task) -> Task {
    // 1. New task struct: registers copied (PC = return-from-fork point),
    //    fresh stack carved from the stack region.
    let child = Task {
        id: alloc_pid(task.parent),          // pid table + generation counter
        regs: task.regs.clone(),             // child returns 0 from fork()
        stack: stack_region::alloc(),
        fd_table: task.fd_table.clone(),     // ← the "capability copy"
        cwd: task.cwd.clone(),
        euid: task.euid, egid: task.egid,
        mem_regions: task.mem_regions.clone(),
        state: Runnable,
    };
    // 2. Refcount every fd node (Arc bump) — see 4.2.
    // 3. Register child as a task-group member (for kill/waitpid).
    // 4. Return 0 to the child, child_pid to the parent.
}
```

There is **no kernel involvement**: no cap table is copied (the core owns the
sidecar's one kernel table, and the child's authority is *derived from the
core*, not duplicated), no address space is created (MVP shares it). The two
"copies" that matter are:

- **fd table copy** — the child inherits every fd number the parent had, each
  referring to the same refcounted `FileLike` node. Because the *node* (and
  hence the file offset) is shared, POSIX fork semantics for open file
  descriptions come out right for free.
- **mem_regions copy** — the child's *view* of which memory it owns. In the
  MVP this is advisory metadata used for syscall argument validation; in the
  isolation model (§4.4) it becomes the child's real, kernel-backed regions.

### 4.2 fd inheritance and rights — the rules

- An fd is a sidecar-local capability: an entry in a per-task table. There is
  exactly one way to obtain an fd entry: the core creates one (`open`, `pipe`,
  `dup` of an owned entry). There is exactly one way to share one: `fork`
  (table copy) or `dup2` (single-entry copy, with the source entry owned by the
  caller). There is no ambient way to reference another task's entry.
- Rights are fixed at creation: `O_RDONLY` ⇒ `{R}`, `O_RDWR` ⇒ `{R,W}`,
  `O_APPEND`/`O_TRUNC` are status flags on the node. Every `read`/`write`
  checks the fd's rights mask against the operation — an fd opened read-only
  can never write, even if the underlying file is writable (no amplification).
- `FD_CLOEXEC` is a flag on the *entry*, honored at `exec`.
- Pipes are `PipeNode`s (bounded buffers + waiters). `pipe()` returns two fds
  sharing one node; the read end's rights are `{R}`, the write end's `{W}`.

### 4.3 exec and the pipeline example

`execve(path, argv, envp)` → `SYS_EXEC` → proc manager + VFS + loader:

1. VFS opens `path` (same machinery as §3.4) and hands the loader a read view.
2. The loader parses the ELF, allocates regions in the mmap region, maps
   segments (`rx` code, `rw` data — W^X), builds a fresh stack frame with
   `argv`/`envp` copies.
3. The task's registers are reset to the ELF entry; `mem_regions` now covers
   only the new image (the old program's heap allocations are *not* freed —
   they were shared with the parent under vfork semantics; the allocator
   reclaims them when the last task in the group exits).
4. The fd table is rewritten: every entry with `FD_CLOEXEC` dropped, the rest
   renumbered contiguously — this is the POSIX "fds survive exec" contract.
   Everything else (signal dispositions, task-group membership) resets.

A concrete pipeline, `cat /etc/passwd | grep root`, showing fd tables at each
step (shell = task 3, `{n: node}`):

```
step  shell (task 3)            cat (child A)              grep (child B)
───   ───────────────            ──────────────             ──────────────
pipe  fd3={PipeNode,R}
      fd4={PipeNode,W}
fork  fd3,fd4 (Arc++)    A: fd3={R}, fd4={W}        (B not born yet)
dup2  —                   A: dup2(4→1): fd1={W}     —
close —                   A: close(3),close(4)       —
exec  —                   A: fd0={console,R}, fd1={W}, fd2={console,W}
                          (exec'd /bin/cat; fd table rebuilt, 3/4 gone)
fork  fd3,fd4 (Arc++)    —                          B: fd3={R}, fd4={W}
dup2  —                   —                          B: dup2(3→0): fd0={R}
close shell: close(3,4)  —                          B: close(3),close(4)
exec  —                   —                          B: fd0={R}, fd1={console,W}, fd2={console,W}
```

`cat` writes to fd 1 = the pipe's write end; `grep` reads fd 0 = the pipe's
read end. The pipe node's refcount drops to zero when both children exit, at
which point the core reclaims it. `waitpid` reaps zombie task structs (a task
that exited keeps only its id, exit code, and group id until reaped).

**The vfork caveat, stated plainly.** MVP `fork()` shares memory (it is, in
effect, `vfork` with a fresh stack). A child that *writes* to its heap before
exec corrupts the parent's view. This is acceptable because:

- BusyBox's `ash` uses `vfork` when available and its applets are exec-first;
  this exact combination (BusyBox on uClinux, no-MMU `fork`) is the
  battle-tested precedent we are copying.
- The *guaranteed-safe* child sequence is `dup2`/`close`/`exec`/`_exit` — all
  core-mediated, none touching shared heap — which is precisely the shell
  pipeline shape.

### 4.4 Escalation tiers (when shared-memory fork is not enough)

- **Tier 0 (MVP):** shared memory, vfork semantics — §4.1–4.3. Covers shell +
  coreutils.
- **Tier 1:** allocator-aware heap *clone* (copy the budget region, fix up
  metadata) — requires precise pointer tracking, so only practical for
  region-based/heap-only workloads; documented as the fallback for daemons
  that fork-without-exec but don't share data. Best effort; not a hard
  isolation boundary.
- **Tier 2 (Phase 4):** **process = sidecar.** `fork` becomes
  `create_sidecar(copied_manifest)` + a state handoff over a fresh channel:
  the parent serializes `{regs, fd_table → caps, cwd, env}` to the child
  sidecar, which re-hydrates and execs. Memory is now kernel-isolated between
  processes. Costs: state serialization, no shared memory (pipes stay
  channel-based, which they already are), heavier fork. This is the model the
  manifest's `isolated` flag and §7's isolation mode are designed for.

---

## 5. Deliverable 4 — ramdisk driver channel protocol

### 5.1 Principles

- The driver is **dumb by design**: it serves raw blocks and knows nothing of
  files, permissions, or paths. All policy lives in the POSIX sidecar's VFS.
  This keeps the driver ~300 lines and auditable, and means a compromised
  driver cannot leak "files" — only blocks, and only to a holder of the
  channel cap (kernel-enforced).
- **Buffers travel as MEM-cap grants, not inline payloads.** A 512-byte block
  copied inline is fine; a 4 KiB page is already marginal; a whole-image `map`
  must never be copied. The protocol moves capability arguments, and the kernel
  (not the driver, not the client) validates and mints them.
- **Least privilege on every grant.** A read buffer is granted **W-only** (the
  driver writes into it); a write buffer is granted **R-only** (the driver
  reads from it). Neither side can do more with the buffer than the operation
  requires, and neither side can re-grant stronger than it holds.

### 5.2 Message framing

One frame format, both directions, little-endian:

```
offset  size  field
0       8     magic       "AEROSRD\x01"
8       2     version     = 1
10      2     type        RD_INFO=1 RD_READ=2 RD_WRITE=3 RD_FLUSH=4 RD_MAP=5
12      2     flags       bit0 reply | bit1 error
14      2     req_id      client-chosen, echoed verbatim in the reply
16      2     cap_count
18      2     payload_len
20      n     payload
20+n    m     caps[]      cap_count × 16-byte descriptors
```

Capability descriptor (16 bytes):

```
offset  size  field
0       4     slot        sender's cap-table handle (index) of the MEM cap
4       4     offset      byte offset into the sender's region
8       4     len         bytes granted (≤ region size − offset)
12      1     rights      requested: 0x1=R 0x2=W (never X over channels)
13      1     flags       bit0 persist (survives the request) | bit1 move (revoke sender)
14      2     pad
```

Cap transfer rules (kernel-enforced, both sides):

1. **Derive, never amplify.** The kernel mints in the receiver's table a cap
   with `rights = min(held_by_sender, requested)`. A sidecar cannot pass a
   stronger cap than it holds, and `RD_READ`'s W-only request means the driver
   never even *receives* a readable copy of the client's buffer.
2. **Transient by default.** Grants live for the duration of the request and
   are auto-revoked when the reply is sent (or on error/disconnect). Receivers
   must not cache them. `persist` is reserved for `RD_MAP`'s durable result.
3. **`move` = revocation of the sender's cap** — used when a sidecar
   surrenders a region (e.g., the driver handing over its backing store view).
4. **Bounds are re-validated by the receiver.** The driver must check
   `len ≥ count × block_size` before touching memory; on mismatch it replies
   `RD_ERR_CAP` and touches nothing. The kernel checked *rights*; the driver
   checks *size*; neither trusts the other.

### 5.3 Message types

| Type | Request payload | Reply payload | Cap arguments |
|---|---|---|---|
| `RD_INFO` | — | `block_size u32, blocks u64, flags u32` (bit0 = read-only) | none |
| `RD_READ` | `lba u64, count u32` | `status u16, bytes u64` | `caps[0]` MEM, **W**, `len ≥ count×bs` |
| `RD_WRITE` | `lba u64, count u32` | `status u16, bytes u64` | `caps[0]` MEM, **R**, `len ≥ count×bs` |
| `RD_FLUSH` | — | `status u16` | none |
| `RD_MAP` | — | `status u16` | reply carries `caps[0]` MEM **persist**, rights = disk flags (`R`, or `RW` if writable), covering the whole backing store |

`RD_MAP` is the fast path that makes the POSIX sidecar's root filesystem
nearly free: the ramdisk is already in RAM, so after `RD_MAP` the VFS can map
the whole image read-only and page the aerofs-lite metadata directly instead
of issuing `RD_READ`s (the block cache then fronts only `/tmp` writes and any
future writable mounts). The walkthrough in §3.4 is written for the
`RD_READ` path so the grant mechanics are visible; with `RD_MAP` mounted, steps
5–6 become a direct memory reference.

### 5.4 Error handling

Status codes (`u16`, same namespace both directions):

| Code | Meaning | Action |
|---|---|---|
| `RD_OK` 0 | success | — |
| `RD_ERR_INVAL` 1 | malformed payload / bad params | reply + drop nothing; client bug |
| `RD_ERR_RANGE` 2 | `lba + count` out of bounds | reply |
| `RD_ERR_CAP` 3 | missing/undersized/over-righted grant | reply; **buffer untouched** |
| `RD_ERR_NOMEM` 4 | driver out of internal memory | reply; client may retry |
| `RD_ERR_IO` 5 | backing store failure | reply; VFS marks mount stale |
| `RD_ERR_RO` 6 | write to read-only store | reply |
| `RD_ERR_BUSY` 7 | driver busy (window full) | reply; client backsoff |
| `RD_ERR_PROTO` 8 | frame-level violation | **close the endpoint** |

Protocol-level rules:

- **One in-flight request per endpoint in v1** (`window = 1`). `req_id` exists
  for diagnostics and for a later windowed mode; in v1 it must match the
  outstanding request.
- **Timeouts are client-side.** The POSIX sidecar sets a deadline per request;
  on expiry it tears the endpoint down (kernel delivers close to the driver)
  and re-opens. A wedged driver can never wedge the POSIX sidecar — this is
  why the driver is a *sidecar* and not kernel code.
- **Driver death** surfaces as the kernel's channel close event to the POSIX
  sidecar. The VFS marks every mount on that driver `stale` (reads fail
  `EIO`); init (via the console) may re-spawn the driver sidecar and remount.
- **Malformed frames** are never interpreted: the receiver closes the endpoint
  with `RD_ERR_PROTO` in the close reason. No partial parsing, no retry loops
  on garbage.

---

## 6. Deliverable 5 — bootstrap sequence

### 6.1 Kernel side

1. `create_sidecar(manifest)` → kernel parses the packed manifest (§2.2), CRC
   checks, range-checks the budget against available RAM.
2. Kernel builds the initial capability table in manifest order: `budget`
   (MEM rw), `img.ro` (MEM rx), `console` (CHAN), `ramdisk` (CHAN). It also
   wires the ramdisk channel: `create_sidecar` connects the POSIX sidecar's
   endpoint to the driver sidecar's endpoint (the kernel already holds the
   driver's cap from *its* manifest).
3. Kernel maps the image at a fixed base (`rx`), maps core data (`rw`), sets
   up the boot stack, and jumps to `_start` with a pointer to the **Boot Info
   Block** in `a0` (RISC-V) / `rdi` (x86-64).

Boot Info Block (kernel-filled, immutable):

```
{ magic u64, version u16, cap_count u16,
  caps: [{ name_len u16, name, slot u16, type u8, rights u8,
           base u64, len u64 } × cap_count],
  budget_bytes u64, stack_top u64 }
```

### 6.2 First instructions

```asm
# RISC-V, arch/riscv/sidecar_crt0.S
.section .text._start
_start:
    mv   s0, a0          # 1. save BootInfo pointer before anything else
    la   sp, boot_stack_top   # 2. own stack (a MEM cap from the manifest)
    mv   a0, s0          # 3. pass bib through
    call rust_entry      # 4. jump into Rust; never returns
```

Then Rust, in dependency order (this ordering is the whole bootstrap contract —
no step touches a facility the previous step hasn't stood up):

```rust
// user/posix/src/entry.rs
#[no_mangle]
pub extern "C" fn rust_entry(bib: *const BootInfo) -> ! {
    // 1. Read and validate the BootInfo block.
    let bib = unsafe { &*bib };
    assert_eq!(bib.magic, BOOT_INFO_MAGIC, "bad boot info");

    // 2. Find our initial caps by name (kernel built them in manifest order).
    let budget  = bib.find_cap(CAP_MEM,  "budget").expect("no budget cap");
    let console = bib.find_cap(CAP_CHAN, "console").expect("no console cap");
    let ramdisk = bib.find_cap(CAP_CHAN, "ramdisk").expect("no ramdisk cap");

    // 3. Stand up the heap: the budget region becomes the Rust global allocator.
    //    (bump allocator first, free-list once the core is alive)
    unsafe { HEAP.init(budget.base, budget.len) };
    let _ = alloc::default_alloc_error_handler; // heap usable now

    // 4. Channel runtime: register endpoints, arm kernel wakeups.
    chan_runtime::init();
    let console = chan_runtime::open(console, "console");
    let ramdisk = chan_runtime::open(ramdisk, "drv.ramdisk.0");

    // 5. Logging (console channel, or serial fallback) — debug from here on.
    log::init(console.clone());

    // 6. Handshake with the ramdisk driver.
    let info = ramdisk.request(RD_INFO {})?;          // {block_size:512, blocks:65536}
    let image = if writable(info) { None } else {
        Some(ramdisk.request(RD_MAP {})?)             // durable R MEM cap, whole image
    };

    // 7. Mount root: aerofs-lite superblock at block 0 of the image.
    let root = AeroFsLite::open(image.unwrap_or(ramdisk_block_dev()));
    vfs::mount("/", root);

    // 8. Device layer + writable /tmp.
    devfs::register("console", console.clone());
    devfs::register("null", NullDevice);
    vfs::mount("/tmp", RamFs::new(budget));

    // 9. Spawn init: pid 0 = idle task, pid 1 = init.
    //    BusyBox with argv[0]=="init": mounts, rc scripts, then a shell.
    let init = proc_mgr::spawn(ExecSpec {
        path: "/bin/busybox", argv: ["init"], envp: default_env(),
        stdio: [console, console, console],
    })?;

    // 10. Cooperative scheduler — never returns.
    scheduler::run()
}
```

### 6.3 Root filesystem — aerofs-lite (minimal)

A 512-byte-block read-only FS, just enough for `init` + BusyBox:

```
block 0     superblock  { magic "AFSL", version 1, block_size 512,
                          inode_count, inode_start, data_start, root_inode=2 }
block 1..K  inode table  64 B each: { mode u16, uid u16, gid u16, size u32,
                          mtime u32, blocks[12] u32, indirect u32 }
then        data blocks  directory entry: { name[56], ino u32, type u8, rec_len u8 }
```

The image layout `/bin/busybox`, `/etc/passwd`, `/etc/group`, `/dev/*` is
generated by the same build that produces the ramdisk (a host-side `genrootfs`
tool), mirroring how the repo already generates its boot images. The VFS is
driver-agnostic: aerofs-lite over `RD_MAP`, later ext2-lite or a writable FS
over `RD_READ`/`RD_WRITE`, with no changes above the block cache.

---

## 7. Deliverable 6 — security analysis

### 7.1 Threat model (stated honestly)

| Attacker | Model | Outcome |
|---|---|---|
| Buggy user program (buffer overflow in a C coreutils applet) | Non-malicious, arbitrary-ish memory writes | **Contained** by the core (§7.2) |
| Malicious user program (deliberately hostile, same sidecar) | Can write anywhere in the shared address space | **Not contained within its own sidecar** — the sidecar is one trust domain (§7.4). Contained *across* sidecars by the kernel (§7.3) |
| Compromised ramdisk driver sidecar | Can serve forged blocks / violate protocol | Contained: it holds only its own caps; cannot reach the POSIX sidecar's memory except via granted buffers (§7.3) |
| Malicious manifest | Lies about identity/caps | Kernel validates format, CRC, rights syntax; caps are still only what the kernel mints (§2) |

### 7.2 Why file descriptors and permission checks cannot be forged (within the threat model)

- **Fds are not objects — they are indices the core owns.** A program can pass
  any integer to `read()`; the core looks it up in *that task's* fd table and
  returns `EBADF` for anything absent. There is no way to obtain an entry other
  than the three core-mediated paths (open/pipe, fork inheritance, dup2 of an
  owned entry). A task's fd table is core memory; the only handles to it are
  internal (Rust `Arc`s), never exposed to the program.
- **No amplification.** Rights are minted once at `open()` from the flags and
  re-checked on every operation. An fd opened `O_RDONLY` cannot write even if
  the file is writable; a pipe read-end cannot write; `dup2` copies the entry
  including its rights.
- **The core is the only kernel-cap holder.** User programs cannot reach the
  ramdisk channel, the console channel, or the budget MEM cap — those handles
  live in the core's data structures and are used only by core code on the
  program's behalf. Even a fully compromised user program can only do what the
  syscall ABI expresses: it cannot read raw blocks, cannot mint caps, cannot
  talk to the driver. This is the *authority flows through caps* invariant
  applied at the sidecar boundary.
- **Language-level safety makes the boundary real.** The syscall ABI is the
  only place user pointers enter the core, and the dispatcher validates every
  pointer against the calling task's `mem_regions` before use. The core is
  Rust with `unsafe` confined to a handful of audited modules (ELF loader,
  block-copy loops, the crt0 stub); the single-threaded cooperative model
  means no data races and no torn permission checks. A buffer overflow in a
  C applet can scribble on *its own* heap but cannot reach the fd table, the
  capability table, or the VFS mount table — those live in the Rust core, and
  Rust guarantees the core's own invariants even while the rest of the address
  space is misbehaving.
- **VFS bypass resistance.** Path resolution happens only in core code:
  canonicalization clamps `..` at the root, symlink depth is bounded, and the
  mount table is core-owned. There is no path-oracle to confuse: the ramdisk
  driver serves blocks, not names, so no component can be tricked into
  resolving a path with different permissions than the VFS intended.

### 7.3 What the kernel enforces (the hard boundary)

- **Cap integrity:** only the kernel mints/derives/revokes; a sidecar cannot
  forge a handle or pass a stronger cap than it holds (derive-never-amplify).
- **Channel rights:** a sidecar can only send to endpoints it holds caps for;
  the kernel is the enforcement point for every cap transfer in §5.2.
- **Budget:** total memory granted to the sidecar (including all transfers and
  derivations) is accounted against `budget.mem_bytes`; a fork bomb or fd leak
  dies at the sidecar's own limits or the kernel's budget — never the host.
- **Crash containment:** the ramdisk driver is a sidecar; its death is a
  channel-close event, not a kernel fault. The POSIX sidecar survives, marks
  mounts stale, and can re-spawn the driver.
- **W^X:** even in the shared address space, the kernel maps code regions `rx`
  and data `nx` per MEM cap — so a buggy program can corrupt data but cannot
  *execute* injected code (no JIT-style bypass in the MVP).

### 7.4 The honest limitation, and the escalation path

In the shared-address-space MVP, a *malicious* program can read or corrupt the
core's own data structures (it shares the address space; nothing stops a
deliberate write to the fd table's backing memory, and Rust cannot defend
against deliberate corruption of its own memory). Therefore:

> **Within one sidecar, capabilities are a software-enforced discipline —
> powerful against accidental misuse and auditable by construction, but not a
> malicious-adversary boundary. The unit of kernel-enforced isolation is the
> sidecar, not the process.**

That is the correct engineering posture for Phase 2, for three reasons:

1. It matches the SLS philosophy: AeroSLS's single-level store makes the
   address space the *uniform* medium; isolation is a *mapping* decision, not
   a separate mechanism. The capability layer is designed so that switching a
   sidecar from shared-space to isolated requires changing *mapping*, not
   changing any interface — every MEM cap transfer in §5 already names a
   region and rights.
2. It matches the MVP's actual workloads: shell + BusyBox pipelines are
   buggy-but-not-malicious, single-trust-domain code — exactly what Rust +
   capability discipline contain.
3. It preserves the isolation upgrade path the design has been assuming all
   along — Tier 2 fork (§4.4), `flags.isolated`, and per-sidecar isolation are
   the *same* design with per-sidecar page tables.

**Hardware isolation path (Phase 4), in order:**

1. **Per-sidecar page tables.** The kernel maps each sidecar's budget into its
   own address space; MEM cap transfers become real mappings with per-principal
   permissions (the Phase 1 extension flagged in §1). W-vs-R between sidecars
   becomes kernel-enforced, and a compromised driver can no longer read its
   W-grant as data. This alone upgrades every *sidecar* boundary in this doc
   from software discipline to hardware enforcement.
2. **Per-task page tables within a sidecar** (Tier 2 fork). fd rights and
   permission checks remain core-enforced, but a malicious task can no longer
   corrupt the core or other tasks' memory — Rust then protects *within-task*
   invariants, the kernel protects *between-task* boundaries. This is the
   full POSIX process-isolation story.
3. **Hardening knobs** that become available once mappings are per-principal:
   guard pages around stacks and the cache pool, stack canaries, CFI on the
   core, `RD_MAP` grants mapped read-only in the requester, and (eventually)
   per-sidecar ASLR for the mmap region.

The one hard dependency to carry into Phase 1 follow-up work: **MEM caps must
support map-into-target-space with rights** (seL4-style frame mapping). The
shared-space MVP works without it (mapping is identity); nothing in §7.4 works
without it.

---

## 8. Open questions and Phase 1 follow-ups

1. **MEM cap mapping** (§1, §7.4): the single Phase 1 extension the whole
   isolation story depends on. Specified in
   `docs/AeroSLS-Kernel-Capability-Layer-Spec-v0.1.md` as a transfer mode
   (cap-argument flag `MAP`), with the shared-space no-op and the isolated-mode
   real mapping sharing one semantic contract. Confirm the kernel can map a MEM
   cap into a target sidecar's address space with derived rights before Phase 4
   work starts.
2. **Transient grant auto-revoke** (§5.2): the kernel must revoke a cap
   argument when the request completes. This is a new capability operation
   (or a flag on message completion) that Phase 1 should spec explicitly.
3. **Channel close/`RD_ERR_PROTO` reason codes** (§5.4): confirm the Phase 1
   channel endpoint carries a close *reason* so the VFS can distinguish
   "driver died" from "protocol violation".
4. **CPU share semantics** (§2.1): is `share` a strict weight (proportional
   scheduling) or a hint? The doc assumes hint-with-clamping; a strict
   guarantee needs kernel scheduler work.
5. **Signals under cooperative scheduling** (§3.3): SIGINT delivery from the
   console char device to the foreground task group at safe points only —
   acceptable for MVP, but a blocked `read()` on the console must become
   interruptible without violating the no-data-race rule.
6. **`RD_MAP` for writable stores** (§5.3): a writable mapping breaks the
   "transient by default" rule by design (persist). Decide whether writable
   root filesystems should be block-mediated only, with `RD_MAP` reserved for
   read-only images — the doc's default.
