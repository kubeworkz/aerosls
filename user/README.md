# AeroSLS sidecars (Rust)

Cargo workspace for the capability-based sidecars designed in `docs/`:

| Crate | Personality | Spec |
|---|---|---|
| `proto/` | — (shared wire format + kernel ABI) | `docs/AeroSLS-Sidecar-Channels-Transport-Spec-v0.1.md`, `docs/AeroSLS-POSIX-Sidecar-Phase2-Design-v0.1.md` §5 |
| `ramdisk/` | `aerosls.ramdisk.v1` | `docs/AeroSLS-Ramdisk-Driver-Implementation-Plan-v0.1.md` |
| `blockcache/` | — (POSIX sidecar component) | respawn decision §4.1 (device states), Phase 2 §3.4/§5.2 |
| `kernel-sim/` | — (host-only test fake) | capability-layer spec §3–§4, transport spec §3–§6 |

## Layout

```
proto/            # no_std: RD_* frames + channel envelope + the kernel ABI
                  # (Kernel trait, cap types, error codes) in kabi.rs
ramdisk/          # the aerosls.ramdisk.v1 driver sidecar (dumb RD_* server)
  src/kapi.rs     # re-exports proto::kabi + the real extern "C" ABI (feature `target`)
  src/bootinfo.rs # Boot Info Block parsing (Phase 2 §6.1)
  src/heap.rs     # bump allocator over the budget region (reserved)
  src/endpoints.rs# RD_* endpoint set: adoption + handshake state
  src/server.rs   # the RD_* dispatch and handlers
  src/copy.rs     # the driver's entire unsafe surface (memmove block copy)
  src/entry.rs    # rust_entry (feature `target`)
  src/crt0.S      # _start stub (RISC-V + x86-64), linked by the image build
  manifest.json   # sidecar manifest source (genmanifest → packed TLV)
blockcache/       # the POSIX sidecar's block cache — the ramdisk protocol client
  src/cache.rs    # BlockCache: handshake, RD_READ/RD_WRITE/RD_FLUSH/RD_MAP,
                  # direct-mapped read cache, stale-on-close (device state)
  src/copy.rs     # the client's entire unsafe surface (raw memory copies)
kernel-sim/       # host fake kernel: driver-side Kernel + client-side Kernel
```

## Build and test (host)

```sh
# All tests: unit tests (proto, heap, copy, bootinfo, endpoints, blockcache)
# plus two integration suites against the fake kernel in kernel-sim/:
#   - ramdisk/tests/      the driver (server side)
#   - blockcache/tests/   the block cache, run end to end against the *real*
#                         driver on the fake kernel (client side)
cargo test --workspace
```

The default build has **no kernel ABI** — sidecar cores link against
`aerosls-kernel-sim`, which implements the channel and capability semantics
the specs define (FIFO queues, window=1, transient grants with auto-revoke,
persist grants with lineage revocation on driver death, NEW_CHANNEL
injection, close events waking blocked recv/wait).

## Build the sidecar image

```sh
cargo build -p aerosls-ramdisk --features target
```

`--features target` selects the real kernel ABI (`extern "C"` syscalls:
`k_chan_wait/recv/send/close`, `k_cap_info`) and the `rust_entry` bootstrap.
The kernel does not exist yet — those symbols are forward declarations to be
implemented in `kernel/cap.c` and `kernel/chan.c` per the capability-layer
and transport specs. Image assembly (crt0 + linker script + the manifest's
`image` record) is the sidecar build step; see `src/crt0.S`.

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
  respawn layer's job (respawn decision §4–§6).
- `RD_MAP` grants are durable views of the storage cap, so they die with the
  driver through lineage revocation — exactly what the POSIX VFS's
  drop-on-stale policy expects (respawn decision §6).
