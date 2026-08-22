# AeroSLS Capability ABI Layouts — v0.1

The capability syscall ABI is a set of opaque request structs passed by raw
pointer through `do_syscall`. Every layer that builds or consumes one — the
kernel, the C SDK (`aerocap.h`, `aerosls_cap.h`), the Rust runtime
(`user/aerosls`), the compile-only mocks, and the AeroIDL-generated code —
must agree on its C layout byte-for-byte. This document is the prose
reference for that contract. It is **enforced** by two tests:

- `tools/aeroidl/tests/cross_lang_constants.rs` — parses every definition
  from source, computes C layouts, anchors the kernel's against the tables
  below, and compares all other definitions field-by-field (offset + size).
- `user/aerosls/src/req.rs` (`layout_tests`) — pins the Rust runtime's own
  structs with the *compiled* `size_of!` / `offset_of!` / `align_of!`, so a
  definition and its compiled layout can never disagree.

Layout rules: C alignment, 64-bit pointers (8 bytes, align 8), natural
(no `packed`) alignment on both sides, little-endian targets.

---

## 1. The capability descriptor (`SLSCapDesc`) — 16 bytes, align 4

The IDL layer's view of a moved MEM cap. On send, `slot` is the *sender's*
table slot; on recv the kernel rewrites it to the *receiver's* new slot.

| field   | type                | offset | size |
|---------|---------------------|--------|------|
| `slot`  | `uint16_t`          | 0      | 2    |
| `offset`| `uint32_t`          | 4      | 4    |
| `len`   | `uint32_t`          | 8      | 4    |
| `rights`| `uint8_t`           | 12     | 1    |
| `flags` | `uint8_t`           | 13     | 1    |
|         | (tail padding)      | 14     | 2    |

**Known divergence (deliberate, documented):** the generated/mock Rust
`CapDescriptor` declares `slot: u32` (offset 0) with an explicit `pad: u16`
at 14, keeping the total at 16 bytes and every other field at the same
offset. The runtime converts through `desc_to_kernel`, truncating the high
half of `slot`. Offsets must match everywhere; the slot *width* is exempt.

Definitions: kernel `SLSCapDesc`, SDK `aerosls_cap_desc_t`,
runtime `CapDesc`, mocks `aerosls_cap_desc_t` / `CapDescriptor`,
generated `CapDescriptor`.

## 2. Message-path request structs (syscalls 302–304)

### `SLSCapSendMsgRequest` — 96 bytes, align 8 (SYS_SLS_CAP_SEND_MSG 302)

| field         | type                          | offset | size |
|---------------|-------------------------------|--------|------|
| `ch_w_idx`    | `uint16_t`                    | 0      | 2    |
| `n_caps`      | `uint16_t`                    | 2      | 2    |
| `_pad`        | `uint8_t[4]`                  | 4      | 4    |
| `tag`         | `uint32_t`                    | 8      | 4    |
| `flags`       | `uint32_t` (bit0 = NO_REPLY)  | 12     | 4    |
| `payload_len` | `uint32_t` (≤ 4096)           | 16     | 4    |
| `_pad2`       | `uint8_t[4]`                  | 20     | 4    |
| `payload`     | `void*`                       | 24     | 8    |
| `caps`        | `SLSCapDesc[4]`               | 32     | 64   |

### `SLSCapRecvMsgRequest` — 104 bytes, align 8 (SYS_SLS_CAP_RECV_MSG 303)

| field             | type                  | offset | size |
|-------------------|-----------------------|--------|------|
| `ch_r_idx`        | `uint16_t`            | 0      | 2    |
| `block`           | `uint8_t`             | 2      | 1    |
| `_pad`            | `uint8_t[1]`          | 3      | 1    |
| `max_caps`        | `uint16_t` (≤ 4)      | 4      | 2    |
| `_pad2`           | `uint8_t[2]`          | 6      | 2    |
| `buf`             | `void*`               | 8      | 8    |
| `buf_len`         | `uint32_t`            | 16     | 4    |
| `_pad3`           | `uint8_t[4]`          | 20     | 4    |
| `out_tag`         | `uint32_t` [out]      | 24     | 4    |
| `out_flags`       | `uint32_t` [out]      | 28     | 4    |
| `out_payload_len` | `uint32_t` [out]      | 32     | 4    |
| `out_n_caps`      | `uint16_t` [out]      | 36     | 2    |
| `_pad4`           | `uint8_t[2]`          | 38     | 2    |
| `out_caps`        | `SLSCapDesc[4]` [out] | 40     | 64   |

### `SLSCapArenaFreeRequest` — 8 bytes, align 2 (SYS_SLS_CAP_ARENA_FREE 304)

| field     | type         | offset | size |
|-----------|--------------|--------|------|
| `cap_idx` | `uint16_t`   | 0      | 2    |
| `_pad`    | `uint8_t[6]` | 2      | 6    |

## 3. Legacy single-cap request structs (syscalls 290–297)

| syscall | struct (kernel)              | size | align | fields (offset: type) |
|---------|------------------------------|------|-------|-----------------------|
| 290 | `SLSCapArenaAllocRequest`    | 8    | 4     | `npages` 0:u32, `perm` 4:u32 |
| 291 | `SLSCapChanCreateRequest`    | 20   | 4     | `far_pid` 0:u32, `out_rd` 4:u16, `out_wr` 6:u16, `out_far_rd` 8:u16, `out_far_wr` 10:u16, `_pad` 12:u8[6] |
| 292 | `SLSCapSendRequest`          | 16   | 8     | `ch_w_idx` 0:u16, `cap_idx` 2:u16, `_pad` 4:u8[4], `cookie` 8:u64 |
| 293 | `SLSCapRecvRequest`          | 16   | 8     | `ch_r_idx` 0:u16, `block` 2:u8, `_pad` 3:u8[1], `release_pid` 4:u32, `cookie` 8:u64 |
| 294 | `SLSCapRevokeRequest`        | 8    | 2     | `cap_idx` 0:u16, `_pad` 2:u8[6] |
| 295 | `SLSCapMapRequest`           | 24   | 8     | `cap_idx` 0:u16, `_pad` 2:u8[6], `vaddr` 8:u64, `flags` 16:u32, `_pad2` 20:u8[4] |
| 296 | `SLSCapUnmapRequest`         | 16   | 8     | `cap_idx` 0:u16, `_pad` 2:u8[6], `vaddr` 8:u64 |
| —   | `SLSCapCreateMemRequest`     | 16   | 8     | `phys_base` 0:u64, `npages` 8:u32, `perm` 12:u32 |

(The `SLSCapCreateMemRequest` syscall is 289 in `kernel/cap.h`; 297 is
`SYS_SLS_CAP_LIST`, which takes no struct.)

## 4. Cross-layer name mapping

| wire id | kernel/cap.h | C SDK aerocap.h | C SDK aerosls_cap.h | Rust runtime req.rs | mocks / generated |
|---------|--------------|-----------------|---------------------|---------------------|-------------------|
| cap descriptor | `SLSCapDesc` | — | `aerosls_cap_desc_t` | `CapDesc` | `aerosls_cap_desc_t`, `CapDescriptor` |
| send-msg | `SLSCapSendMsgRequest` | — | `sls_cap_send_msg_req` | `SendMsgReq` | — |
| recv-msg | `SLSCapRecvMsgRequest` | — | `sls_cap_recv_msg_req` | `RecvMsgReq` | — |
| arena-free | `SLSCapArenaFreeRequest` | — | `sls_cap_arena_free_req` | `ArenaFreeReq` | — |
| create-mem | `SLSCapCreateMemRequest` | `sls_cap_create_mem_req` | — | — | — |
| arena-alloc | `SLSCapArenaAllocRequest` | `sls_cap_arena_alloc_req` | — | `ArenaAllocReq` | — |
| chan-create | `SLSCapChanCreateRequest` | `sls_cap_chan_create_req` | — | — | — |
| send | `SLSCapSendRequest` | `sls_cap_send_req` | — | — | — |
| recv | `SLSCapRecvRequest` | `sls_cap_recv_req` | — | — | — |
| revoke | `SLSCapRevokeRequest` | `sls_cap_revoke_req` | — | — | — |
| map | `SLSCapMapRequest` | `sls_cap_map_req` | — | `MapReq` | — |
| unmap | `SLSCapUnmapRequest` | `sls_cap_unmap_req` | — | `UnmapReq` | — |

The Rust runtime has twins only for the structs its arena machinery issues
(map, unmap, arena-alloc, arena-free, and the message path); the
send/recv/revoke/chan-create legacy path is C-only, so those entries pin
kernel ↔ C SDK.

## 5. Constants (summary)

| constant | value | meaning |
|----------|-------|---------|
| `CAP_PERM_R` / `AEROSLS_CAP_PERM_R` | `0x01` | readable mapping / CHAN can send |
| `CAP_PERM_W` / `AEROSLS_CAP_PERM_W` | `0x02` | writable mapping / CHAN can recv |
| `CAP_PERM_X` | `0x04` | executable mapping |
| `CAP_PERM_MAP` | `0x08` | holder may map the range |
| `CAP_NONE` / `AEROSLS_CAP_NONE` | `0xFFFF` | "no capability" sentinel |
| `CHAN_FLAG_NO_REPLY` (all languages) | `0x0001` | `ChanMsg.flags` bit0 = async |
| `CAP_MSG_MAX_CAPS` / `AEROSLS_MSG_MAX_CAPS` / `MSG_MAX_CAPS` | `4` | moved caps per message |
| `CAP_MSG_MAX_PAYLOAD` / `AEROSLS_MSG_MAX_PAYLOAD` / `MSG_MAX_PAYLOAD` | `4096` | staged payload ceiling (8 × 4 KiB pool) |

### Syscall numbers

Naming across layers: kernel `SYS_SLS_CAP_*`, C SDK `SLS_SYS_CAP_*`, Rust
runtime `SYS_CAP_*`. The Rust runtime issues only the syscalls its arena and
message machinery uses; the legacy send/recv/revoke/chan-create path is C-only.

| syscall | number | syscall | number |
|---------|--------|---------|--------|
| `SYS_SLS_CAP_CREATE_MEM` | 289 | `SYS_SLS_CAP_SEND_MSG` | 302 |
| `SYS_SLS_CAP_ARENA_ALLOC` | 290 | `SYS_SLS_CAP_RECV_MSG` | 303 |
| `SYS_SLS_CHAN_CREATE` | 291 | `SYS_SLS_CAP_ARENA_FREE` | 304 |
| `SYS_SLS_CAP_SEND` | 292 | | |
| `SYS_SLS_CAP_RECV` | 293 | | |
| `SYS_SLS_CAP_REVOKE` | 294 | | |
| `SYS_SLS_CAP_MAP` | 295 | | |
| `SYS_SLS_CAP_UNMAP` | 296 | | |
| `SYS_SLS_CAP_LIST` | 297 | | |

### Error codes

Negative values, returned widened to `int64`. Identical names in the kernel,
the C SDK (`aerocap.h`), and the Rust runtime (`req.rs`).

| code | value | code | value |
|------|-------|------|-------|
| `CAP_EINVAL` | -1 | `CAP_ENOMEM` | -7 |
| `CAP_EBADF` | -2 | `CAP_ENOSPC` | -8 |
| `CAP_EAGAIN` | -3 | `CAP_ERANGE` | -9 |
| `CAP_ETABLEFULL` | -4 | `CAP_ENOSYS` | -10 |
| `CAP_ECAPREVOKED` | -5 | `CAP_ECONFLICT` | -11 |
| `CAP_EALREADY` | -6 | | |
