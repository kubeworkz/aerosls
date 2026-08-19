# AeroSLS sidecars (Rust)

Cargo workspace for the capability-based sidecars designed in `docs/`:

| Crate | Personality | Spec |
|---|---|---|
| `proto/` | — (shared wire format + kernel ABI) | `docs/AeroSLS-Sidecar-Channels-Transport-Spec-v0.1.md`, `docs/AeroSLS-POSIX-Sidecar-Phase2-Design-v0.1.md` §5 |
| `ramdisk/` | `aerosls.ramdisk.v1` | `docs/AeroSLS-Ramdisk-Driver-Implementation-Plan-v0.1.md` |
| `blockcache/` | — (POSIX sidecar component) | respawn decision §4.1 (device states), Phase 2 §3.4/§5.2 |
| `vfs/` | — (POSIX sidecar component) | Phase 2 §3.2/§3.4/§6.3, respawn decision §4.2/§5.9/§6 |
| `procmgr/` | — (POSIX sidecar component) | Phase 2 §3.2/§3.3/§4 (fork/exec) |
| `kernel-sim/` | — (host-only test fake) | capability-layer spec §3–§4, transport spec §3–§6 |

## Layout

```
proto/            # no_std: RD_* frames + channel envelope + the kernel ABI
                  # (Kernel trait, cap types, error codes, the real extern
                  # "C" ABI behind feature `target`) in kabi.rs; plus the
                  # kernel ↔ sidecar contracts: bootinfo.rs (Boot Info Block,
                  # Phase 2 §6.1) and manifest.rs (the packed AERSLSM1
                  # manifest, §2.2 — bounded TLV parse + build)
ramdisk/          # the aerosls.ramdisk.v1 driver sidecar (dumb RD_* server)
  src/kapi.rs     # re-exports proto::kabi (incl. RealKernel under `target`)
  src/heap.rs     # bump allocator over the budget region (reserved)
  src/endpoints.rs# RD_* endpoint set: adoption + handshake state
  src/server.rs   # the RD_* dispatch and handlers
  src/copy.rs     # the driver's entire unsafe surface (memmove block copy)
  src/entry.rs    # rust_entry (feature `target`)
  src/crt0.S      # _start stub (RISC-V + x86-64), linked by the image build
  manifest.json   # sidecar manifest source (genmanifest → packed TLV)
blockcache/       # the POSIX sidecar's block cache — the ramdisk protocol client
  src/cache.rs    # BlockCache: handshake, RD_READ/RD_WRITE/RD_FLUSH/RD_MAP,
                  # direct-mapped read cache, stale-on-close (device state),
                  # poll_dead (observe a queued close without a request)
  src/copy.rs     # the client's entire unsafe surface (raw memory copies)
vfs/              # the POSIX sidecar's VFS — the layer above the block cache
  src/aerofs.rs   # aerofs-lite on-disk format (superblock/inode/dirent, CRC-32,
                  # 11 direct + 1 indirect block) + the genrootfs image builder
  src/ramfs.rs    # in-memory /tmp filesystem (never stale)
  src/fileobj.rs  # file-like objects: PipeNode (ends counted by the VFS, so
                  # EOF/EPIPE/EAGAIN are exact), CharNode (/dev/console — the
                  # in-memory ChanDev the kernel channel plugs into — and
                  # /dev/null)
  src/vfs.rs      # mounts, longest-prefix path resolution, per-task fd tables
                  # (pooled: fork copies a table sharing FileNodes; CLONE_FILES
                  # shares the table object), shared-offset FileNode, the
                  # syscall surface, pipe(), devfs (/dev), fd dispatch over
                  # FileObj (file/pipe/device), stale→EIO, fail-permanently
  src/errno.rs    # shared POSIX errno set
procmgr/          # the POSIX sidecar's proc manager — cooperative tasks over
                  # the VFS (Phase 2 §3.2–§3.3, §4)
  src/procmgr.rs  # ProcManager: run queue, Program/step model, Ctx, fork
                  # (fd-table copy, shared offsets), fork_thread (CLONE_FILES),
                  # exit/zombie/wait with orphan reparenting, exec (applet
                  # registry through the mount chain, argv per design §6.2's
                  # ExecSpec), shell pipelines
                  # (pipe fds dup2'd onto stdio, inherited by fork), and
                  # blocking reads (park on an empty pipe/console, wake on
                  # data/EOF/input via the read-wake drain)
sidecar/          # the POSIX sidecar itself (aerosls.posix.v1)
  src/applets.rs  # the built-in registry: init (the /etc/init.rc boot-script
                  # runner — one forked child per command, sharing sh's word
                  # parser: quotes, backslash escapes, < and > redirects),
                  # cat, grep (glob patterns: * any-run, . any-char, \
                  # escape; exit 0/1 on match/no-match), echo, sh (minimal
                  # interactive shell — console
                  # commands, | pipelines, $? last-exit-status,
                  # '...'/"..." quoting, backslash escapes, $PATH/$HOME
                  # variable expansion, export/setenv/unset/unsetenv
                  # builtins + NAME=value scoped assignments that mutate
                  # the env, < and > redirects), true/false
                  # + register_default_applets
  src/boot.rs     # BootCaps (initial caps by manifest name, from the BIB)
                  # and boot(): handshake with the ramdisk driver, mount /
                  # (aerofs), /dev (console + null), /tmp (ramfs), install
                  # the applet registry, then spawn init with console stdio
                  # (Phase 2 §6.2 steps 4–9)
  src/allocator.rs# BudgetAlloc: request buffers carved from the budget MEM cap
  src/heap.rs     # bump allocator over the budget region (reserved)
  src/entry.rs    # rust_entry over the real ABI (feature `target`): BIB →
                  # heap → boot() → scheduler, parking on the console
                  # channel when quiesced
kernel-sim/       # host fake kernel: driver-side Kernel + client-side Kernel
```

## Build and test (host)

```sh
# All tests: unit tests (proto, aerofs format, ramfs, vfs logic, proc-manager
# scheduler/fork/wait) plus four integration suites against the fake kernel in
# kernel-sim/, each driving the *real* driver end to end:
#   - ramdisk/tests/      the driver (server side)
#   - blockcache/tests/   the block cache (protocol client)
#   - vfs/tests/          the VFS: aerofs-lite mounted on a connected block
#                         cache, incl. driver death → EIO + remount
#   - procmgr/tests/      the proc manager: fork sharing file offsets,
#                         CLONE_FILES threads sharing the fd table, exec,
#                         round-robin interleaving, a real
#                         cat | grep pipeline through pipe fds, and a shell
#                         that parks on an empty pipe until an exec'd
#                         writer delivers data + EOF
#   - sidecar/tests/      the bootstrap: manifest → BIB cap resolution, and
#                         boot() against the real driver — init (the boot
#                         script runner) forks one child per /etc/init.rc
#                         line; cat/echo run from the rootfs, console stdio
#                         carries a rootfs read out, /tmp is writable; and
#                         an interactive sh session (pipeline + $?) typed
#                         at the console
cargo test --workspace
```

The default build has **no kernel ABI** — sidecar cores link against
`aerosls-kernel-sim`, which implements the channel and capability semantics
the specs define (FIFO queues, window=1, transient grants with auto-revoke,
persist grants with lineage revocation on driver death, NEW_CHANNEL
injection, close events waking blocked recv/wait).

## Build the sidecar image

```sh
cargo build -p aerosls-ramdisk --features target   # the driver
cargo build -p aerosls-sidecar --features target   # the POSIX sidecar
```

`--features target` selects the real kernel ABI (`extern "C"` syscalls:
`k_chan_wait/recv/send/close`, `k_cap_info` — defined in `proto/src/kabi.rs`,
shared by every sidecar) and the `rust_entry` bootstrap (`ramdisk/src/entry.rs`,
`sidecar/src/entry.rs`). The kernel does not exist yet — those symbols are
forward declarations to be implemented in `kernel/cap.c` and `kernel/chan.c`
per the capability-layer and transport specs. Image assembly (crt0 + linker
script + the manifest's `image` record) is the sidecar build step; see
`ramdisk/src/crt0.S`.

## Design notes

- The driver is **dumb, passive, connection-agnostic**: it serves `RD_*` on
  every channel it holds with `RECV` right and never initiates. First message
  on any endpoint must be `RD_INFO` (the implicit handshake).
- Request buffers travel as **transient MEM grants** (read buffers W-only,
  write buffers R-only). The kernel mints, bounds, and auto-revokes them; the
  driver re-checks rights and size before its one `unsafe` function runs.
- The block cache mirrors the contract on the client side: one request
  outstanding per endpoint (window=1 by construction), driver error replies
  (`RD_ERR_*`) never stale the device, and a close event or failed send flips
  it to `STALE` and drops the mapped view — recovery (respawn) is the
  respawn layer's job (respawn decision §4–§6). `poll_dead()` lets the VFS
  observe a close that arrived with no request in flight (respawn §5 steps
  1–3), so a cache hit is never served from a dead device.
- `RD_MAP` grants are durable views of the storage cap, so they die with the
  driver through lineage revocation — exactly what the POSIX VFS's
  drop-on-stale policy expects (respawn decision §6).
- The VFS pins the respawn semantics: stale mounts fail `EIO` (never
  `ENOENT` — path resolution stays table-only), fds mint rights at `open`
  and re-check on every op, `dup`/`dup2` share the offset via
  `Arc<FileNode>`, and a pre-death fd fails permanently across a remount
  (the node pins the fs generation; `remount_aerofs` revalidates the
  superblock identity before replacing the fs in place).
- The proc manager pins the process semantics: `fork` copies the parent's fd
  table with shared `FileNode`s (correct shared-offset POSIX), `fork_thread`
  shares the table object (CLONE_FILES), an exited task is a zombie until
  its parent reaps it (orphans reparent to init), and a parent that waits on
  a live child blocks until the exit wakes it. `exec` resolves the file
  through the mount chain and dispatches on its first line into an applet
  registry — BusyBox argv[0]-dispatch in miniature.
- Pipes are core objects, not fs objects: `pipe()` mints read/write ends as
  `FileObj` variants sharing an `Arc<PipeNode>`, and the VFS recounts the
  live ends at every fd mutation (open/close/dup/dup2/fork-copy/exit), so a
  reader sees **EOF** once the last writer is gone, a writer fails
  **EPIPE** once the last reader is gone, and an empty/full pipe fails
  **EAGAIN**. Devices live in `/dev` (devfs names them; an fd on a device
  holds `FileObj::Char` and its I/O bypasses the mount layer):
  `/dev/console` is the sidecar's ChanDev, in-memory until the bootstrap
  wires the kernel-console channel (its `close` event — the channel dying
  — makes console reads EOF: buffered input is served first, then
  `Ok(0)`, and parked readers are woken by the drain), and `/dev/null`
  discards.
- Blocking reads park instead of spin: `Ctx::read_blocking` turns a pipe's
  `EAGAIN` into a VFS-registered wait (`Vfs::wait_readable`) and a
  `Step::Blocked(Readable(fd))`; the VFS holds one waiter per task, keyed
  by the object's `Arc` (so the readiness check never touches a freed
  object). The proc manager drains satisfied waits at every scheduler step
  (the unified wake drain) against a pure predicate —
  data present, or the last writer gone (EOF), or console input/close —
  and requeues the task, whose program simply retries the read (re-parking
  on would-block). Because the sidecar is single-threaded and cooperative,
  no per-event notification is needed: a write or writer-exit in any
  task's step, or console input pushed between runs, is visible at the
  next drain. The `cat | grep` and blocking-shell integration tests drive
  the whole path: shell creates the pipe, dup2's an end onto each child's
  stdio, `exec`s the applets, closes its own copies, and reaps both
  stages (in the pipeline interleaving grep never actually hits an empty
  pipe — cat's 16-byte chunks refill it before grep's round-robin turn;
  the blocking-shell test is where a read-park is proven end to end).
- Blocked-task liveness is observable: the proc manager keeps a wake
  trace (`wake_trace`) of `Parked`/`Woken` events with their `BlockReason`
  — every park records why the task left the run queue, every wake
  records the reason it was blocked on (data/EOF/space arrived, the
  console closed, a waited child exited). A park without a later wake is
  a wedged task, so the trace doubles as the deadlock check; the
  blocking-read, blocking-write, console-close, and wait unit/integration
  tests assert the park→wake pairs.
- Blocking writes are the mirror image: `Ctx::write_blocking` turns a
  pipe's `EAGAIN` (full) into a `Vfs::wait_writable` registration and a
  `Step::Blocked(Writable(fd))`, and the same scheduler drain wakes the
  writer when a reader frees space (or the last reader closes — the retry
  observes `EPIPE`). Files and the console never block on write in v1.
  The 8 KiB producer/consumer integration test proves it end to end: the
  producer provably parks with a 4 KiB pipe full, the consumer drains it
  in 512-byte chunks, and every byte arrives in order at `/tmp/out` —
  exercising the chunk-preservation rule too: a program must stash a
  read-but-unwritten chunk in its state before parking, since the step's
  stack dies with the step.
- The bootstrap ties the chain together (`sidecar/`): the kernel ↔ sidecar
  contract lives in `proto` as two checked formats — the packed manifest
  (`manifest.rs`, AERSLSM1, bounded TLV with a CRC verified before any
  field is trusted) and the Boot Info Block (`bootinfo.rs`, which the
  kernel fills from the manifest). `BootCaps::from_bib` resolves the
  initial caps by name (`budget`, `console`, `ramdisk`), and `boot()` runs
  the Phase 2 §6.2 sequence: block-cache handshake → mount `/`, `/dev`,
  `/tmp` → install the applet registry → spawn init with console stdio
  (fds 0,1,2 opened by the bootstrap). Init is no placeholder: it is the
  `applets::init` boot-script  runner, which executes `/etc/init.rc` line by
  line — forking one child per command (parsed with the same word parser
  as `sh`: quotes, backslash escapes, `<`/`>` redirects; a piped line
  fails 127, since init runs one command per line), waiting for each,
  reaping 127-failing children, and exiting when the script is consumed. Applets read their arguments
  from `Ctx::argv`, set by the spawner or the last `exec` (design §6.2's
  `ExecSpec.argv`); exec failure costs the child 127, never init. The
  boot integration test runs the whole thing against the real driver:
  the script's quoted and backslash-escaped echoes reach console stdout,
  its `cat < /etc/passwd` carries a rootfs read out to the console, its
  `echo booted > /tmp/out` lands on the ramfs, a bogus command and a
  piped line each exit 127 without stopping init, and the manifest/BIB
  cap names line up end to end. On top of init sits `applets::sh`, a minimal interactive shell:
  it prompts on the console, parks on empty input via the blocking-read
  machinery,  buffers multi-line input into a batch queue (a pasted chunk
  runs line by line with one prompt around it, nothing dropped), forks
  one child per pipeline stage (resolving bare command names through the
  exported `PATH`, probing each `:`-separated directory — default
  `/bin`), applies per-stage `<` / `>` redirects (which
  override the pipeline connection at fd 0/1, like POSIX), and parses
  single / double quotes (grouping whitespace into one
  argument — `'…'` fully literal, `"…"` still expanding `$?`) plus
  backslash escapes (`\ `, `\$`, `\|`, … are literal; inside double
  quotes only `$`, `"`, `\` are escapable, per POSIX), with  `$VAR` /
  `${VAR}` expansion against a persistent shell environment (`PATH=/bin`,
  `HOME=/root`; unset names expand to nothing — the env region survives
  pipeline waits and batch reaps),  with `export` (list or set `NAME=value`
  entries in place), `setenv NAME value`, `unset NAME...` and
  `unsetenv NAME` builtins that run in the shell itself and mutate that
  region, plus leading `NAME=value` scoped assignments (`PATH=x cmd`
  searches `x` for that command only; a bare `FOO=bar` line persists,
  as does an assignment on a builtin, per the POSIX special-builtin
  rule), waits, and tracks the
  last exit status as `$?` — the boot test drives a full
  `echo hello | cat` pipeline, a `false` → `echo $?` sequence, an
  `echo saved > /tmp/saved` file write, a `cat < /etc/passwd` read,
  a pasted two-line batch, quoted/escaped arguments
  (`echo "hello world" | cat`, `echo '$?'` printed literally,
  `echo hello\ world`,  a quoted double space surviving the fork
  snapshot exactly, `echo $PATH ${HOME}` expanding the shell's
  environment, an `export`/`setenv` session that sets `FOO`, sets a
  quoted `GREETING`, overwrites `HOME` in place, turns a usage error
  into `$? = 2`, and lists the mutated environment, a PATH-driven
  lookup session — `export PATH=/usr/bin:/bin` finds `greet` in
  `/usr/bin` with `/bin` fallback, and a PATH miss turns `false` into
  `$? = 127`, an env-mutation session — `unset GREETING` empties
  its expansion, a bare `FOO=scoped` line persists and `unsetenv FOO`
  removes it, and `PATH=/usr/bin greet hi` resolves through the
  scoped path while the shell's PATH stays put — and grep filter
  pipelines (`echo hello | grep ell` matches, `grep zzz` exits 1 into
  `$?`, `h.llo` and `h*o` wildcards match) through the real driver,
  asserting the console transcript byte for byte. `BudgetAlloc` carves RD request buffers out of the sidecar's own
  budget cap (grants with no amplification), and the `target` entry
  points (ramdisk + sidecar) share the real `extern "C"` ABI in
  `proto::kabi`.
