# AeroSLS "Polyglot Nexus" — Phase 3 Design v0.1

**Status:** Draft for review.
**Scope:** Cross-language, cross-sidecar function calling with zero serialization
overhead. Multiple language runtimes (WebAssembly, Common Lisp, Python, etc.)
interoperate as if linked into the same address space, with isolation boundaries
defined by capabilities. Builds on Phase 1 (capability primitives) and Phase 2
(POSIX sidecar, channel transport, shared-memory arena).

---

## 0. Executive summary

Phase 3 answers one question: **can two sidecars running different language
runtimes call each other's functions with the same cost as a local function
call?** The answer is yes, under the right conditions, and the mechanism is:

1. **AeroIDL** — an interface definition language that describes functions,
   argument types, memory ownership, and error contracts.
2. **Channel calling convention** — a binary protocol layered on Phase 2's
   channel transport that maps IDL signatures to SEND_CAP/RECV_CAP sequences.
3. **Shared arena** — a memory region visible to both sidecars via MEM caps,
   where large data lives in-place instead of being copied into channel
   messages.
4. **Code generation** — compiler-produced stubs that handle serialization,
   capability management, and direct memory access, so application code
   never sees the channel protocol.
5. **Trampoline capability** — for trusted same-ring callers, a hardware-
   protected direct-branch mechanism that bypasses the kernel entirely.

The result: a call from a Wasm sidecar to a Lisp sidecar's `add(i32, i32) → i32`
is two kernel channel operations (~200 ns), and a call that passes a 1 MiB
buffer via MEM cap is two channel operations plus a direct memory read (~200 ns +
memory latency, zero copies).

---

## 1. Deliverable 1 — the AeroIDL

### 1.1 Design rationale

We chose a textual IDL (not binary) for three reasons:
- **Human readability:** developers debug cross-language calls by reading `.aeroidl`
  files, not hex dumps.
- **Tooling:** existing parser-generator ecosystems produce validators, linters,
  and documentation generators cheaply.
- **Extensibility:** new types and annotations add grammar rules without changing
  a binary schema.

The IDL is *not* a general-purpose language. It describes interfaces and types
only — no control flow, no implementations. The compiler reads the IDL and
produces stubs in the target language; the stubs contain the actual
marshalling logic.

### 1.2 Grammar (ABNF)

```abnf
aeroidl        = *header *declaration

header         = "@version" "(" string ")" ";"
               / "@namespace" "(" string ")" ";"

declaration    = enum_decl / struct_decl / interface_decl

; ─── Enum ───────────────────────────────────────────────────────────────────

enum_decl      = "enum" identifier "{" enum_body "}"
enum_body      = *enum_field
enum_field     = identifier "=" integer ","

; ─── Struct ─────────────────────────────────────────────────────────────────

struct_decl    = "struct" identifier "{" struct_body "}"
struct_body    = *struct_field
struct_field   = identifier ":" type [ownership] ","

; ─── Interface ──────────────────────────────────────────────────────────────

interface_decl = "interface" identifier "{" *method_decl "}"
method_decl    = [annotation] identifier "(" param_list ")" "->" return_type ";"
param_list     = [param *( "," param )]
param          = identifier ":" type [ownership]
return_type    = "Result" "<" type "," type ">"
               / "Option" "<" type ">"
               / type

; ─── Annotations ────────────────────────────────────────────────────────────

annotation     = "@" identifier ["(" arg_list ")"]
               / "@async"

; ─── Ownership ──────────────────────────────────────────────────────────────

ownership      = "@borrowed" / "@owned" / "@arena"

; ─── Types ──────────────────────────────────────────────────────────────────

type           = primitive / identifier / array_type / map_type
primitive      = "i8" / "i16" / "i32" / "i64"
               / "u8" / "u16" / "u32" / "u64"
               / "f32" / "f64"
               / "bool" / "string" / "bytes"
array_type     = type "[" "]"
map_type       = "map" "<" type "," type ">"

; ─── Terminals ──────────────────────────────────────────────────────────────

identifier     = ALPHA *(ALPHA / DIGIT / "_")
integer        = ["-"] 1*DIGIT
string         = DQUOTE *(%x20-21 / %x23-5B / %x5D-10FFFF) DQUOTE
```

### 1.3 Memory layout rules

| Type | Inline bytes | When inline | Otherwise |
|------|-------------|-------------|-----------|
| `i8..u64, f32, f64, bool` | 1–8 | Always | — |
| `string` | 8 (cap handle + len) | Always (data in arena) | — |
| `bytes` | 8 (cap handle + len) | Always (data in arena) | — |
| `T[]` | 8 (cap handle + count) | Always (data in arena) | — |
| `struct` | sum of fields | ≤ 128 bytes total | MEM cap to arena |
| `enum` | discriminant (4 bytes) + largest variant | ≤ 128 bytes | MEM cap to arena |

The 128-byte threshold matches the Phase 2 channel transport's 4 KiB payload
limit with headroom: an 8-field method with struct arguments must not exceed
the message payload. The compiler enforces this at compile time and emits an
error if a method signature would overflow.

### 1.4 Example: logging service

```aeroidl
// idl/logging.aeroidl
@version("aeroidl/1.0")
@namespace("aerosls.logging")

enum LogLevel {
    TRACE = 0, DEBUG = 1, INFO = 2,
    WARN  = 3, ERROR = 4, FATAL = 5,
}

struct LogError {
    code:    u32,
    message: string @borrowed,
}

struct LogKV {
    key:   string @borrowed,
    value: string @borrowed,
}

struct LogRecord {
    timestamp_ms: u64,
    level:        LogLevel,
    facility:     string @borrowed,
    message:      string @borrowed,
    kvs:          LogKV[],
}

interface LoggingService {
    emit(record: LogRecord @borrowed) -> Result<(), LogError>
    emit_batch(records: LogRecord[] @arena) -> Result<u32, LogError>
    flush() -> Result<(), LogError>
    set_level(level: LogLevel) -> Result<(), LogError>
}
```

### 1.5 Example: calculator service

```aeroidl
// idl/calculator.aeroidl
@version("aeroidl/1.0")
@namespace("aerosls.calculator")

enum CalcErrorKind {
    DIVISION_BY_ZERO = 1, OVERFLOW = 2, UNDERFLOW = 3,
    INVALID_INPUT = 4,    INTERNAL = 5,
}

struct CalcError {
    kind:    CalcErrorKind,
    message: string @borrowed,
}

struct Vec2 {
    x: f64,
    y: f64,
}

struct MatrixData {
    rows:     u32,
    cols:     u32,
    elements: f64[] @arena,
}

interface CalculatorService {
    add(a: i32, b: i32) -> Result<i32, CalcError>
    div(a: i64, b: i64) -> Result<i64, CalcError>
    dot(a: Vec2 @borrowed, b: Vec2 @borrowed) -> Result<f64, CalcError>
    matmul_vec(m: MatrixData @arena, v: Vec2 @borrowed) -> Result<Vec2, CalcError>
    sqrt_batch(input: f64[] @borrowed) -> Result<f64[] @arena, CalcError>
    @async heavy_reduce(input: f64[] @arena) -> Result<u32, CalcError>
}
```

### 1.6 Example: image processing service (complex types)

```aeroidl
// idl/imageproc.aeroidl
@version("aeroidl/1.0")
@namespace("aerosls.imageproc")

struct ImageDescriptor {
    width:  u32,
    height: u32,
    stride: u32,          // bytes per row
    format: PixelFormat,
    data:   bytes @arena,  // raw pixel data in shared arena
}

enum PixelFormat {
    RGB8  = 0,
    RGBA8 = 1,
    YUV420 = 2,
}

struct FilterParams {
    sigma:  f32,
    radius: u32,
}

interface ImageProcessingService {
    /// Apply Gaussian blur. Input image is borrowed (read-only);
    /// output is a new arena allocation returned to caller.
    blur(image: ImageDescriptor @borrowed, params: FilterParams @borrowed)
        -> Result<ImageDescriptor @arena, string @borrowed>

    /// Resize in-place: the callee writes into the caller's arena buffer.
    /// The cap must be @borrowed with W rights.
    resize_inplace(image: ImageDescriptor @borrowed, new_w: u32, new_h: u32)
        -> Result<(), string @borrowed>
}
```

---

## 2. Deliverable 2 — calling convention over channels

### 2.1 Channel topology

Each interface maps to a pair of channel endpoints. The calling sidecar holds
the CHAN_W (send) and CHAN_R (recv) ends; the serving sidecar holds the
reverse. A sidecar may host multiple interfaces, each on a separate channel
pair, or multiplex them on one channel with opcode routing.

For Phase 3, we define **one channel per interface** as the default. The
channel is created at sidecar bootstrap via manifest wiring (Path 2) or
dynamic creation (Path 4). The IDL compiler assigns stable opcode numbers to
each method, and both sides agree on the mapping.

### 2.2 Synchronous call sequence

```
  Caller (Wasm sidecar)                     Callee (Lisp sidecar)
       │                                          │
       │  ─── SEND_CAP ─────────────────────────▶ │
       │      opcode | request_id | flags          │
       │      [payload bytes]                      │
       │      [cap descriptors: MEM bufs]          │
       │                                          │
       │         ... callee executes method ...    │
       │                                          │
       │  ◀── RECV_CAP ──────────────────────────  │
       │      request_id | status                  │
       │      [reply payload]                      │
       │      [cap descriptors: MEM results]       │
       │                                          │
```

The request_id (u32) is chosen by the caller and echoed in the reply. This
enables multiplexing multiple in-flight requests on a single channel (window > 1
in a future phase; Phase 3 keeps window = 1 for simplicity).

### 2.3 Message frame layout

Built on top of the Phase 2 kernel envelope (§3.1 of the Channels Transport
Spec). The kernel envelope carries the payload; the IDL payload sits inside it.

#### Kernel envelope (already defined in Phase 2)

```
offset  size  field
0       8     magic        "AEROSCH\x01"
8       2     version      = 1
10      2     kind         MSG=0 | CLOSE=1 | NEW_CHANNEL=2
12      2     flags        bit0 REPLY | bit1 NO_REPLY
14      2     cap_count
16      4     tag          request id
20      2     payload_len
22      2     reserved
24      4     reserved
28      n     payload      ← IDL payload lives here
28+n    16m   caps[]       ← MEM cap descriptors
```

#### IDL request payload (inside kernel MSG payload)

```
offset  size  field
0       2     method_id    opcode assigned by aeroidl-cc
2       2     flags        bit0 = async (NO_REPLY expected)
                           bit1 = streaming (more messages coming)
4       4     request_id   caller-chosen correlation ID
8       ...   args         method arguments, packed per IDL layout rules
```

Argument packing rules:
- Scalar arguments (i32, f64, etc.) are packed in declaration order, each
  occupying its natural size, aligned to its own alignment.
- Struct arguments ≤ 128 bytes are packed inline (field-by-field in order).
- Struct arguments > 128 bytes are passed as a MEM cap handle (u16) in the
  payload, with the actual data in the shared arena.
- Array arguments (`T[]`) are always a MEM cap handle (u16) + count (u32) = 6
  bytes in the payload; the array data is in the shared arena.
- String arguments are a MEM cap handle (u16) + byte length (u32) = 6 bytes
  in the payload; the string data is in the shared arena.
- Ownership annotations determine whether the MEM cap is borrowed (R-only,
  no transfer), owned (transfer to callee), or arena (managed by refcount).

#### IDL reply payload

```
offset  size  field
0       1     ok           1 = success, 0 = error
1       3     reserved
4       ...   result       on success: return value (inline or MEM cap)
                           on error: CalcError { code:u32, msg_cap:u16 }
```

### 2.4 Asynchronous call sequence

An `@async` method sends a `NO_REPLY` message. The callee acknowledges
receipt immediately (no reply on the original channel). The result is
delivered later on a **dedicated result channel** created at bootstrap.

```
  Caller                          Callee
   │                                │
   │  ─── SEND_CAP (NO_REPLY) ───▶ │
   │      opcode | request_id |     │
   │      ASYNC_FLAG | [args]       │
   │      [caps: owned arena buf]   │
   │                                │
   │  ◀── (immediate ACK on ────   │
   │       result channel)          │
   │                                │
   │         ... callee works ...   │
   │                                │
   │  ◀── RECV_CAP on result ────  │
   │      channel                   │
   │      request_id | result       │
   │      [caps: MEM result buf]    │
   │                                │
```

The result channel is a separate CHAN pair created via `k_chan_create` during
bootstrap, carrying only result messages tagged with the original request_id.
This avoids head-of-line blocking on the main request channel.

### 2.5 Error handling

Errors use the IDL `Result<T, E>` type. On the wire:
- The `ok` byte distinguishes success from error.
- On error, the reply payload carries a 4-byte error code (from the IDL enum)
  and optionally a MEM cap to a UTF-8 error message string in the arena.
- The channel transport's CLOSE event (Phase 2 §5) handles fatal errors:
  if the callee crashes or the channel is revoked, the caller receives a
  CLOSE control event with `CLOSE_PEER_DEAD` and can retry on a fresh channel.

### 2.6 Cap descriptor format (reused from Phase 2)

Each MEM cap argument uses the 16-byte descriptor from the capability-layer
spec:

```
offset  size  field
0       4     slot        sender's cap-table handle
4       4     offset      byte offset into the arena region
8       4     len         bytes granted (≤ region − offset)
12      1     rights      R=0x1, W=0x2 (never X over channels)
13      1     flags       bit0 = persist (survives request)
                          bit1 = move (revoke sender)
                          bit2 = borrowed (no ownership change)
14      2     pad
```

The IDL compiler sets the `borrowed`/`move`/`persist` flags based on the
ownership annotation:
- `@borrowed` → rights = R, flags = borrowed. Caller retains the cap; the
  kernel mints a transient R-only derived cap in the receiver's table.
- `@owned` → rights = R|W, flags = move. The cap is moved (revoked from
  sender) and minted into the receiver's table. The receiver must free it.
- `@arena` → rights = R|W, flags = persist. The cap persists beyond the
  request; both sides hold derived caps to the same arena object. Lifetime
  is managed by reference counting (§4).

---

## 3. Deliverable 3 — code generation

### 3.1 Codegen architecture

```
  .aeroidl source
       │
       ▼
  ┌──────────────────┐
  │  AeroIDL Parser   │  recursive descent, ~2500 LOC
  │  + Type Checker   │  ownership validation, size checks
  └────────┬─────────┘
           │ typed AST
           ▼
  ┌──────────────────┐
  │  Code Generator   │  template-driven, per-target
  ├──────┬───────────┤
  │  C   │  CLISP    │
  └──┬───┴─────┬─────┘
     │         │
     ▼         ▼
  .h/.c       .lisp
```

Each codegen target produces:
1. **Type definitions** — struct/enum in the target language
2. **Inline stub functions** — one per method, with marshalling logic
3. **Dispatch table** (callee only) — opcode → function pointer
4. **Arena helpers** — alloc/free wrappers with ownership semantics

### 3.2 Generated C header (caller side — Wasm sidecar)

Full source: `tools/aeroidl/gen/calculator.h`

Key snippet — the `add(i32, i32) → Result<i32>` stub:

```c
static inline CalcResult
calculator_add(int32_t a, int32_t b)
{
    /* Both args fit in the 8-byte payload (no arena, no caps). */
    uint8_t payload[8];
    *(int32_t *)(payload + 0) = a;
    *(int32_t *)(payload + 4) = b;

    uint32_t req_id = aerosls_next_request_id();
    aerosls_chan_send(g_calculator_chan_w,
                     CALC_OP_ADD, req_id,
                     payload, sizeof(payload),
                     NULL, 0,    /* no cap arguments */
                     0);         /* blocking sync call */

    /* Receive reply — blocking recv */
    uint8_t reply_buf[16];
    uint16_t cap_slots[8];
    uint32_t reply_kind, reply_tag;
    size_t   reply_len;
    uint16_t n_caps;

    aerosls_chan_recv(g_calculator_chan_r,
                     reply_buf, sizeof(reply_buf),
                     cap_slots, 8,
                     &reply_kind, &reply_tag, &reply_len, &n_caps);

    /* Deserialize */
    CalcResult result;
    result.ok = reply_buf[0];
    if (result.ok)
        result.val_i32 = *(int32_t *)(reply_buf + 4);
    else {
        result.err.kind    = *(CalcErrorKind *)(reply_buf + 4);
        result.err.message = NULL;
    }
    return result;
}
```

**Zero-copy for large data** — the `sqrt_batch` stub:

```c
static inline CalcResult
calculator_sqrt_batch(uint16_t input_cap, uint32_t count)
{
    uint8_t payload[4];
    *(uint32_t *)payload = count;

    /* Attach input as borrowed MEM cap — callee reads directly from arena */
    aerosls_cap_desc_t input_desc = {
        .slot   = input_cap,
        .offset = 0,
        .len    = count * sizeof(double),
        .rights = AEROSLS_CAP_PERM_R,
        .flags  = AEROSLS_CAP_FLAG_BORROWED,
    };

    uint32_t req_id = aerosls_next_request_id();
    aerosls_chan_send(g_calculator_chan_w,
                     CALC_OP_SQRT_BATCH, req_id,
                     payload, sizeof(payload),
                     &input_desc, 1, 0);

    /* Reply carries a MEM cap to the output arena buffer */
    uint8_t reply_buf[16];
    uint16_t cap_slots[8];
    uint32_t rk, rt; size_t rl; uint16_t nc;
    aerosls_chan_recv(g_calculator_chan_r,
                     reply_buf, sizeof(reply_buf), cap_slots, 8,
                     &rk, &rt, &rl, &nc);

    CalcResult result;
    result.ok = reply_buf[0];
    if (result.ok)
        result.val_cap = (nc > 0) ? cap_slots[0] : AEROSLS_CAP_NONE;
    else { /* error path */ }
    return result;
}
```

### 3.3 Generated Common Lisp FFI (callee side — Lisp sidecar)

Full source: `tools/aeroidl/gen/calculator.lisp`

Key snippet — the dispatch table and `add` handler:

```lisp
(defun calculator-dispatch (opcode payload payload-len cap-slots n-caps)
  "Main entry point called by the Lisp sidecar's channel event loop."
  (flet ((encode-result (result) ...))  ; serialize reply

    (ecase opcode
      (#.+op-add+
       (let ((a (sb-sys:sap-ref-32 (sb-sys:sap payload) 0))
             (b (sb-sys:sap-ref-32 (sb-sys:sap payload) 4)))
         (encode-result (calculator-add a b))))

      ;; ... other opcodes ...

      )))

(defun calculator-add (a b)
  "add(a: i32, b: i32) -> Result<i32, CalcError>"
  (declare (type (unsigned-byte 32) a b))
  (handler-case
      (calc-success (ldb (byte 32 0) (+ a b)))
    (arithmetic-error (e)
      (calc-failure :overflow (format nil "add overflow: ~A" e)))))
```

**Zero-copy for arena data** — the `sqrt_batch` handler reads the input
buffer directly from the shared arena via the MEM cap handle:

```lisp
(defun calculator-sqrt-batch (input-cap count)
  (let* ((in-ptr  (aerosls:arena-mem input-cap))    ; direct memory access
         (out-cap (aerosls:arena-alloc (* count 8)   ; allocate from arena
                    :rights '(:read :write)))
         (out-ptr (aerosls:arena-mem out-cap)))
    (dotimes (i count)
      (let ((val (cffi:mem-aref in-ptr :double i)))
        (setf (cffi:mem-aref out-ptr :double i)
              (if (minusp val) 0.0d0 (sqrt val)))))
    (calc-success out-cap)))   ; return the output MEM cap to the caller
```

### 3.4 What the stubs handle (and what application code never sees)

| Concern | Handled by stubs | Application code sees |
|---------|------------------|-----------------------|
| Opcode assignment | ✅ generated constants | function name |
| Payload serialization | ✅ inline byte packing | typed arguments |
| Arena allocation | ✅ `aerosls_arena_alloc` | `(arena-alloc size)` |
| MEM cap creation | ✅ cap descriptor setup | ownership annotation |
| Channel send/recv | ✅ `chan_send` / `chan_recv` | synchronous return |
| Error unwrapping | ✅ `ok` byte → union decode | `Result<T, E>` |
| Ownership semantics | ✅ cap flag bits | `@borrowed` / `@owned` / `@arena` |

---

## 4. Deliverable 4 — zero-copy argument passing

### 4.1 The shared arena

The shared arena is a kernel-managed physical memory region visible to both
sidecars via MEM capabilities. It was introduced in Phase 1 (CAP_ARENA_SIZE =
64 MiB) and used by the POSIX sidecar's block cache. In Phase 3, the arena
is extended to be **shared between language sidecars**:

```
  ┌──────────────────────────────────────────────────────────────────┐
  │                    Shared Arena (64 MiB)                         │
  │                                                                  │
  │  ┌──────────────┐  ┌──────────────┐  ┌──────────────┐          │
  │  │ Wasm sidecar │  │ Lisp sidecar │  │ Python sidecar│  ...    │
  │  │   objects     │  │   objects    │  │   objects     │          │
  │  └──────────────┘  └──────────────┘  └──────────────┘          │
  │                                                                  │
  │  Each sidecar holds a MEM cap to the arena with R|W|MAP rights.  │
  │  Objects within the arena are addressed by (offset, len) pairs   │
  │  that are transmitted as MEM cap descriptors on channel msgs.    │
  └──────────────────────────────────────────────────────────────────┘
```

The arena is set up at boot: the kernel grants each sidecar a MEM cap covering
the full arena. Objects within the arena are allocated by a sidecar-local
allocator (or a shared arena allocator for truly shared objects).

### 4.2 How a large byte array travels

**Scenario:** Wasm sidecar calls `calculator.sqrt_batch(input: f64[] @borrowed)`
with a 1 MiB buffer (125,000 doubles).

**Step 1 — Caller allocates from arena (if data isn't already there):**
```c
// Caller side (Wasm sidecar, generated stub)
uint16_t input_cap = aerosls_arena_alloc(1000000 * 8,
                                         AEROSLS_CAP_PERM_R | AEROSLS_CAP_PERM_W);
double *input = (double *)aerosls_arena_mem(input_cap);
// ... fill input with data (may already be in arena from previous computation) ...
```

**Step 2 — Caller sends the MEM cap (borrowed) with the call:**
```c
aerosls_cap_desc_t desc = {
    .slot   = input_cap,
    .offset = 0,
    .len    = 1000000 * 8,      // 8 MiB
    .rights = AEROSLS_CAP_PERM_R,   // callee reads only
    .flags  = AEROSLS_CAP_FLAG_BORROWED,
};
aerosls_chan_send(chan_w, CALC_OP_SQRT_BATCH, req_id,
                 payload, 4, &desc, 1, 0);
```

**Step 3 — Kernel validates and mints a derived cap:**
- Kernel checks: caller holds `input_cap` with R|W, requested is R (≤ held) ✓
- Kernel mints: derived R-only cap in receiver's (Lisp) cap table
- Kernel does NOT copy the 8 MiB buffer — only the 16-byte cap descriptor
  crosses the channel boundary

**Step 4 — Callee reads directly from arena (zero copy):**
```lisp
;; Lisp sidecar — generated stub
(let ((in-ptr (aerosls:arena-mem input-cap)))   ; direct memory pointer
  ;; in-ptr points to the SAME physical pages the Wasm sidecar wrote to
  ;; No copy. No serialization. Just a pointer dereference.
  (dotimes (i 1000000)
    (setf (cffi:mem-aref out-ptr :double i)
          (sqrt (cffi:mem-aref in-ptr :double i)))))
```

**Step 5 — Callee allocates output and returns a MEM cap (owned):**
```lisp
(let ((out-cap (aerosls:arena-alloc 8000000 :rights '(:read :write))))
  ;; ... write results ...
  (calc-success out-cap))   ; return cap handle in reply
```

**Step 6 — Caller receives the output cap and reads results (zero copy):**
```c
// Caller side
double *output = (double *)aerosls_arena_mem(result.val_cap);
// Directly accessible — same physical pages the Lisp sidecar wrote to
```

### 4.3 Ownership-transfer scheme

The ownership model uses three mechanisms:

#### Borrowed caps (`@borrowed`)
- **Direction:** caller → callee (transient read access)
- **Lifetime:** the cap exists in the receiver's table only for the duration
  of the request. The kernel's transient-grant auto-revoke (Phase 2 §5.2)
  ensures the derived cap is revoked when the reply is sent.
- **Freeing:** the caller retains the original cap and frees it when done.
  No ownership change.

#### Owned caps (`@owned`)
- **Direction:** caller → callee (permanent transfer)
- **Lifetime:** the cap is moved (revoked from sender, minted in receiver's
  table). The receiver becomes the sole owner.
- **Freeing:** the callee must explicitly `arena_free()` the cap, or transfer
  it to another sidecar. If the callee crashes, the kernel's close event
  delivers a `CLOSE` to the caller, and the kernel reaps the cap from the
  dead sidecar's table (refcount drops to 0, arena pages are freed).

#### Arena caps (`@arena`)
- **Direction:** bidirectional (both sides hold a cap to the same arena object)
- **Lifetime:** managed by reference counting on the arena object header.
- **Freeing:** each sidecar that received the cap holds a reference. The
  arena object is freed when the refcount reaches 0.

### 4.4 Arena object header and reference counting

Every object allocated in the arena has a hidden 16-byte header:

```
offset  size  field
0       4     refcount        atomic u32, starts at 1 per transfer
4       4     size            object size in bytes (excl. header)
8       4     owner_sidecar   pid of the allocating sidecar (for diagnostics)
12      4     flags           bit0 = string (UTF-8), bit1 = binary
```

When a MEM cap to an arena object is sent with `@owned` or `@arena` semantics,
the kernel **atomically increments the refcount** in the arena header before
minting the cap in the receiver's table. When a sidecar calls `arena_free(cap)`,
the kernel **atomically decrements** the refcount. When refcount hits 0:

1. The arena pages are marked free in the allocator bitmap.
2. If the original allocator is still alive, it receives a notification.
3. The pages can be reused for future allocations.

This is analogous to `Arc<T>` in Rust or `GCPointer` in Lisp — the arena
object lives as long as at least one sidecar holds a derived cap to it.

**Race condition safety:** the refcount is in the shared arena, which both
sidecars can write to. The kernel performs the increment/decrement atomically
(using `LOCK XADD` on x86-64 or `LR/SC` on RISC-V). Sidecars never directly
modify the refcount — they call `arena_free()` which crosses into the kernel.

### 4.5 Lifetime diagram

```
  Time ──────────────────────────────────────────────────────────────▶

  Wasm sidecar                     Arena Object                  Lisp sidecar
       │                              │                              │
       │  arena_alloc(size)           │                              │
       │  ──kernel──▶ refcount=1      │  ◀── pages allocated ──     │
       │                              │                              │
       │  chan_send(MEM @arena)        │                              │
       │  ──kernel──▶ refcount=2 ──────────────────▶                 │
       │                              │                              │
       │  arena_free(cap)             │                              │
       │  ──kernel──▶ refcount=1      │                              │
       │  (pages still alive)         │                              │
       │                              │                              │
       │                              │     Lisp writes results      │
       │                              │     into arena memory        │
       │                              │                              │
       │                              │  chan_send(MEM @owned)       │
       │  ◀──kernel── refcount=2 ─────────────────────────────────── │
       │                              │                              │
       │                              │  arena_free(cap)             │
       │                              │  ──kernel──▶ refcount=1      │
       │                              │                              │
       │  (processes results)         │                              │
       │                              │                              │
       │  arena_free(cap)             │                              │
       │  ──kernel──▶ refcount=0      │                              │
       │                              │── pages freed ──▶ (reuse)   │
```

---

## 5. Deliverable 5 — same-ring trampoline capability

### 5.1 Motivation

When two sidecars run in the same CPU privilege ring (ring 0 on x86-64, EL1
on AArch64) and are mutually trusted, the kernel-mediated channel send/recv
is unnecessary overhead. The kernel can issue a **trampoline capability** — a
capability that grants the caller permission to directly branch to a
pre-verified entry point in the callee's address space, with hardware-enforced
memory protection ensuring the callee's code cannot be tampered with.

### 5.2 Hardware mechanism: Intel MPK / AMD PKU

Memory Protection Keys (MPK) provide per-thread, per-key access control
without TLB flushes. The kernel assigns a protection key to each trusted
sidecar pair:

```
  Protection Key Assignment:
    Key 0: kernel (always accessible in ring 0)
    Key 1: Wasm sidecar code pages (RX)
    Key 2: Lisp sidecar code pages (RX)
    Key 3: shared arena data (RW for both sidecars)
    Key 4+: untrusted sidecars (restricted)
```

On a trampoline call:
1. The caller sets `PKRU` register to grant itself access to the callee's
   code pages (key 2) but restrict its own data pages (key 1).
2. The caller jumps to the callee's pre-verified entry point.
3. The callee executes and returns.
4. The caller restores `PKRU` to its normal state.

This is faster than a channel call because:
- No kernel context switch (both sidecars run in ring 0)
- No channel queue manipulation
- No cap table lookup
- No message copy
- Just a `WRPKRU` + `JMP` + function body + `RET` + `WRPKRU`

### 5.3 Trampoline capability structure

```c
struct TrampolineCap {
    uint64_t entry_vaddr;       /* callee's verified entry point */
    uint64_t stack_vaddr;       /* callee's pre-allocated call stack */
    uint32_t callee_pkey;       /* MPK protection key for callee */
    uint32_t caller_pkey_mask;  /* PKRU value for the caller during call */
    uint32_t data_pkey;         /* shared arena data key */
    uint32_t flags;             /* bit0 = uses callee stack, bit1 = callee preempts */
    uint64_t max_stack_bytes;   /* stack guard bound */
};
```

### 5.4 Trampoline call sequence

```
  Caller (Wasm sidecar, ring 0)              Callee (Lisp sidecar, ring 0)
       │                                          │
       │ 1. save caller's PKRU                    │
       │ 2. load callee's PKRU (wrpkru)           │
       │    — now has access to callee's code      │
       │    — lost access to own writable data     │
       │ 3. load callee's stack pointer            │
       │ 4. push arguments onto callee's stack     │
       │ 5. JMP trampoline.entry_vaddr ─────────▶ │
       │                                          │ 6. callee executes
       │                                          │ 7. writes result to
       │                                          │    pre-agreed arena slot
       │  ◀── RET ──────────────────────────────  │ 8. return
       │ 9. restore caller's PKRU                  │
       │ 10. read result from arena slot            │
       │                                          │
```

### 5.5 Kernel issuance of trampoline capabilities

The kernel issues a trampoline capability only when **all** of the following
conditions are met:

1. **Same ring:** both sidecars run in ring 0 (or EL1). This is the default
   for AeroSLS sidecars; ring-3 sidecars cannot use trampolines.

2. **Mutual trust:** both sidecar manifests declare `trust: "mutual"` and
   name each other in an allow-list. The kernel verifies the declarations
   match (same as pinning in Path 2 channel creation).

3. **MPK support:** the CPU supports Intel MPK or AMD PKU. The kernel
   detects this at boot and refuses trampoline creation on older CPUs.

4. **Code integrity:** the callee's code pages have been loaded from a
   verified image (signed manifest). The kernel has measured the code hash
   and attested it. The trampoline entry point must be within the measured
   code region.

5. **Stack isolation:** the callee has a dedicated stack that is not
   accessible to the caller (protected by MPK key). The stack guard bound
   prevents stack-smashing attacks from corrupting the callee's control flow.

6. **Maximum stack size:** the callee declares `max_stack_bytes` in its
   manifest. The kernel maps the stack with a guard page below it, and the
   trampoline sets the stack pointer to `stack_top - max_stack_bytes` on
   entry. Stack overflow hits the guard page and faults to the kernel.

**Kiosk syscall:**
```c
int k_create_trampoline(uint32_t callee_pid,
                        uint64_t entry_vaddr,
                        uint32_t max_stack_bytes,
                        struct TrampolineCap *out);
```

The kernel:
1. Validates all six conditions.
2. Allocates a new MPK key (or reuses one from a freed trampoline).
3. Programs the callee's page table entries with the MPK key.
4. Programs the shared arena with the `data_pkey`.
5. Returns the trampoline cap to the caller's cap table as a new cap type
   (`CAP_TYPE_TRAMP`).

### 5.6 When is it safe?

Trampoline calls are safe when:

| Condition | Why |
|-----------|-----|
| Same CPU ring | No privilege escalation possible |
| Mutual trust manifests | Both sides explicitly opted in |
| Verified code hash | Callee code hasn't been tampered with |
| MPK enforcement | Hardware prevents data page access violations |
| Dedicated stacks | Stack smashing cannot corrupt the caller's stack |
| No exception from callee | Callee cannot trap into the kernel on behalf of the caller |
| Callee is single-threaded | No concurrent access to the trampoline entry |

**When it is NOT safe:**
- Cross-ring (ring 3 → ring 0): always use channels
- Untrusted callee: always use channels
- Callee with exception handlers: exceptions during trampoline must be
  caught by the kernel and converted to a channel CLOSE event
- SMP with shared writable data: concurrent writes to the arena require
  the caller and callee to use atomic operations or a spinlock (the arena
  provides this natively)

### 5.7 Performance comparison

| Mechanism | Latency | Copy overhead | Kernel involvement |
|-----------|---------|---------------|-------------------|
| Channel call (Phase 2) | ~200 ns | payload copy | send/recv syscalls |
| Trampoline call (Phase 3) | ~30 ns | zero | one WRPKRU + JMP |
| Local function call (same sidecar) | ~5 ns | zero | none |
| IPC via pipes (Linux) | ~1 μs | buffer copy | kernel pipe buffer |
| IPC via Unix socket | ~2 μs | buffer copy | kernel socket buffer |

The trampoline reduces the cross-sidecar call to approximately 6× the cost
of a local function call, vs. 40× for the channel path.

---

## 6. Deliverable 6 — benchmarking plan

### 6.1 Experiment design

**Goal:** Measure round-trip latency of cross-sidecar calls and compare to
baselines. Isolate the effect of each optimization layer.

#### Test matrix

| Test ID | Call type | Argument size | Data path | Expected latency |
|---------|-----------|---------------|-----------|-----------------|
| T1 | Wasm→Wasm local | 0 bytes (add) | same sidecar | ~5 ns |
| T2 | Wasm→Lisp channel | 0 bytes (add) | channel send/recv | ~200 ns |
| T3 | Wasm→Lisp trampoline | 0 bytes (add) | WRPKRU + JMP | ~30 ns |
| T4 | Wasm→Lisp channel | 1 KiB | MEM cap + arena read | ~250 ns |
| T5 | Wasm→Lisp channel | 64 KiB | MEM cap + arena read | ~300 ns |
| T6 | Wasm→Lisp channel | 1 MiB | MEM cap + arena read | ~500 ns |
| T7 | Wasm→Lisp trampoline | 1 MiB | MPK + direct read | ~200 ns |
| T8 | Wasm→Lisp channel | 8 MiB | MEM cap + arena read | ~3 μs |
| T9 | Linux pipe | 0 bytes | pipe write/read | ~1 μs |
| T10 | Linux pipe | 64 KiB | pipe write/read | ~5 μs |
| T11 | Linux Unix socket | 0 bytes | sendmsg/recvmsg | ~2 μs |
| T12 | Linux Unix socket | 64 KiB | sendmsg/recvmsg | ~10 μs |
| T13 | Wasm→Lisp channel | 0 bytes (async) | send + result channel | ~400 ns |
| T14 | Wasm→Lisp channel | 1 MiB (async) | send + result channel | ~500 ns |

#### Measurement methodology

1. **Clock source:** Use `rdtsc` (x86-64) calibrated to the TSC frequency
   at boot. On RISC-V, use the `time` CSR. Each measurement is:
   ```
   start = rdtsc()
   result = call(...)
   end = rdtsc()
   latency = (end - start) * tsc_period_ns
   ```

2. **Warmup:** 10,000 iterations to warm L1/L2 caches and branch predictors.
   Do not include these in the results.

3. **Measurement window:** 100,000 iterations per test, recorded in a
   pre-allocated array. No allocation during measurement.

4. **Statistical analysis:** Report min, p50, p95, p99, max, mean, stddev.
   The p99 is the most operationally relevant metric (tail latency).

5. **Isolation:** run the benchmark as the sole workload. No other sidecars
   active. Pin the benchmark task to a single core (no migration).

### 6.2 Micro-architectural effects

#### L1 cache
- **T1 (local):** both caller and callee code are in L1I. The call is a
  predicted branch — essentially free. Arguments and return values are in
  registers or L1D.
- **T2 (channel):** the kernel send/recv path touches kernel data structures
  (cap table, channel queue). These are cold on first call but stay hot in
  L1/L2 during the benchmark. The payload bytes are in L1D after the
  channel copy. Expect ~80 ns in L1D-hot state, ~200 ns on first call.
- **T3 (trampoline):** the callee's code is in L1I (pre-verified, hot). The
  `WRPKRU` is a single µop. The JMP is a predicted branch. Minimal cache
  impact.
- **T4–T8 (large data):** the arena read is sequential — L2/L3 streaming.
  For 1 MiB, expect ~100 ns L3 hit + ~400 ns main memory latency.
  Prefetching helps; the compiler-generated stubs issue explicit prefetch
  hints for buffers > 4 KiB.

#### TLB
- **Channel path:** the kernel maps/unmaps channel queues and cap table pages.
  These are always in the TLB during the benchmark (hot path).
- **Arena path:** the shared arena is a large contiguous region. For 64 MiB,
  that's 16,384 pages — far exceeding the L1 TLB (64 entries on x86-64).
  TLB misses dominate for first-time arena access but amortize during the
  benchmark.
- **Trampoline path:** the callee's code and stack are small, well within
  TLB capacity. No TLB pressure expected.

#### Branch prediction
- The channel send/recv path has many branches (validation checks, cap
  table lookup, queue bounds). These predict well after warmup (the branch
  predictor learns the pattern).
- The trampoline path has 2 branches (WRPKRU check, JMP). Prediction is
  trivial.

#### Memory ordering
- The refcount operations in §4.4 use `LOCK XADD` which serializes the
  memory subsystem. For the arena benchmark, each call includes one
  increment and one decrement — ~10 ns each on modern x86.
- The trampoline path avoids refcount entirely (the callee writes directly
  to pre-mapped arena memory) — no serialization overhead.

### 6.3 Expected results (hypotheses)

| Hypothesis | Expected result |
|------------|----------------|
| H1: Trampoline is ≥5× faster than channel for small args | T3 ~30 ns vs T2 ~200 ns |
| H2: Channel overhead is constant for args < 64 bytes | T2 and T4 within 50 ns |
| H3: Arena read is memory-bound, not channel-bound | T6 ~300 ns ≈ T5 + memory latency |
| H4: Channel beats Linux IPC by ≥5× | T2 ~200 ns vs T11 ~2 μs |
| H5: Async adds ~200 ns overhead vs sync | T13 ~400 ns vs T2 ~200 ns |
| H6: 8 MiB transfer is memory-bandwidth bound | T8 ~3 μs ≈ 8 MiB / 25 GB/s |

### 6.4 Benchmark implementation

The benchmark is a standalone sidecar that:
1. Creates a Wasm sidecar (caller) and a Lisp sidecar (callee) via
   `create_sidecar`.
2. Wires channels between them (Path 2 manifest wiring).
3. Optionally creates a trampoline capability (if MPK is available).
4. Runs the test matrix in sequence, recording `rdtsc` timestamps.
5. Prints results in a machine-readable format (JSON) for post-processing.

```c
// bench_polyglot.c — Phase 3 cross-sidecar latency benchmark
#include "aerosls_bench.h"

static void run_sync_channel_test(uint16_t chan_w, uint16_t chan_r,
                                  size_t arg_size, int iterations) {
    uint16_t arena_cap = alloc_arena_buffer(arg_size);
    uint64_t timestamps[100000];

    for (int i = 0; i < 10000 + iterations; i++) {
        uint64_t start = rdtsc();
        calculator_add(1, 2);   // or sqrt_batch for large data
        uint64_t end = rdtsc();
        if (i >= 10000) timestamps[i - 10000] = end - start;
    }

    report_statistics("channel_sync_0B", timestamps, iterations);
    free_arena_buffer(arena_cap);
}

static void run_trampoline_test(struct TrampolineCap *trap, int iterations) {
    uint64_t timestamps[100000];

    for (int i = 0; i < 10000 + iterations; i++) {
        uint64_t start = rdtsc();
        trampoline_call(trap, CALC_OP_ADD, 1, 2);
        uint64_t end = rdtsc();
        if (i >= 10000) timestamps[i - 10000] = end - start;
    }

    report_statistics("trampoline_0B", timestamps, iterations);
}

int main(void) {
    // Setup: create sidecars, wire channels
    uint16_t chan_w, chan_r;
    setup_polyglot_test(&chan_w, &chan_r);

    // T1: local
    run_local_test(100000);

    // T2: channel sync, 0 bytes
    run_sync_channel_test(chan_w, chan_r, 0, 100000);

    // T4-T8: channel sync, various sizes
    for (size_t sz : {1024, 65536, 1048576, 8388608})
        run_sync_channel_test(chan_w, chan_r, sz, 100000);

    // T3, T7: trampoline (if available)
    struct TrampolineCap trap;
    if (k_create_trampoline(lisp_pid, lisp_entry, 65536, &trap) == 0) {
        run_trampoline_test(&trap, 100000);
        run_trampoline_test_large(&trap, 1048576, 100000);
    }

    // Print results
    print_benchmark_report();
    return 0;
}
```

---

## 7. Integration with existing AeroSLS phases

### 7.1 What Phase 3 depends on

| Dependency | Source | Usage in Phase 3 |
|------------|--------|------------------|
| Capability word format | Phase 1 §1 | Cap descriptors in channel messages |
| MEM caps + arena | Phase 1 §1 | Shared memory for zero-copy data |
| Channel endpoints | Phase 1 §1 | Transport for cross-sidecar calls |
| Channel transport framing | Phase 2 §3 | Kernel envelope for SEND_CAP/RECV_CAP |
| Transient-grant auto-revoke | Phase 2 §5 | Borrowed cap cleanup |
| Manifest wiring | Phase 2 §2 | Bootstrap channel creation |
| `k_chan_create` (Path 4) | Phase 2 §2.4 | Result channel for async calls |
| `create_sidecar` | Phase 1 §1 | Creating language sidecars |

### 7.2 What Phase 3 extends

| Extension | New kernel support needed |
|-----------|--------------------------|
| Arena refcounting | Atomic refcount in arena object header; `arena_free()` syscall |
| Trampoline capability | New cap type `CAP_TYPE_TRAMP`; MPK key allocation; `k_create_trampoline` |
| Multi-sidecar arena sharing | Arena MEM caps with `MAP` permission across sidecars |
| Async result channels | `k_chan_create` called at bootstrap; result channel protocol |

### 7.3 What Phase 3 does NOT change

- The channel transport framing (§3.1 of the Channels Transport Spec) is
  unchanged. AeroIDL payloads sit inside the existing MSG payload bytes.
- The cap descriptor format (16 bytes) is unchanged. New flags (bit2 =
  borrowed) are additive.
- The manifest format is unchanged. Language sidecars use the same personality
  mechanism as the POSIX sidecar.
- The scheduler is unchanged. Language sidecars are scheduled identically to
  any other sidecar.

---

## 8. Security analysis

### 8.1 Threat model

| Attacker | Model | Phase 3 containment |
|----------|-------|---------------------|
| Malicious Wasm sidecar | Can corrupt its own arena allocation | Cannot corrupt Lisp sidecar's arena objects (different cap-table entries, different refcounts) |
| Malicious Lisp sidecar | Can corrupt its own arena allocation | Cannot forge MEM cap descriptors (kernel validates on every send) |
| Confused deputy (callee) | Accidentally uses wrong cap | Stubs handle cap management; application code never sees raw caps |
| Callee crash during trampoline | Control flow exception | Kernel catches via MPK guard page fault; delivers CLOSE to caller; reaps trampoline cap |
| Arena refcount race | Two sidecars free same cap concurrently | Kernel atomic XADD; no race possible from user space |
| Stack smash via trampoline | Callee overflows its stack | Guard page below callee stack; MPK prevents writes to caller's stack |

### 8.2 What the IDL compiler guarantees

- **No ambient authority:** stubs can only access data via MEM caps passed in
  the call. There is no way for generated code to reach memory it was not
  explicitly granted.
- **No capability amplification:** the IDL compiler generates cap descriptors
  with rights ≤ what the caller holds. The kernel validates this; the
  compiler makes it structurally impossible to request stronger rights.
- **No serialization bugs:** the compiler generates type-safe marshalling code.
  A `Vec2` always occupies 16 bytes; a `u32` always occupies 4 bytes. The
  compiler rejects methods whose signatures would overflow the 4 KiB payload
  limit.
- **Ownership enforcement:** `@borrowed` caps are auto-revoked by the kernel.
  `@owned` caps are moved. `@arena` caps are refcounted. The compiler never
  generates code that violates these semantics.

---

## 9. Open questions and Phase 4 directions

1. **Streaming and bidirectional channels.** The current design is
   request/reply. A future `@stream` annotation would allow the callee to
   send multiple partial results on a dedicated data channel before the final
   reply. Use case: real-time image processing, progressive rendering.

2. **Multi-ary sidecar calls.** Can a single call fan out to multiple sidecars
   (e.g., Wasm → Lisp + Python simultaneously)? The IDL could express this
   as `@parallel` on a method, with the stub spawning the calls and merging
   results.

3. **Capability revocation during trampoline.** If the caller revokes the
   trampoline cap while the callee is mid-execution, the kernel must ensure
   the callee completes safely (or fault it). The current design requires
   the callee to be single-threaded and non-preemptible during a trampoline
   call; preemption is deferred to the return.

4. **Remote sidecar calls.** The kernel envelope is explicitly designed for
   remote transport. A future `@remote` annotation could route a call over
   the network, with the IDL compiler generating appropriate serialization
   (protobuf or similar) for the cross-node case.

5. **Dynamic interface registration.** Currently interfaces are statically
   declared in manifests. A future `register_interface(aeroidl_bytes)` syscall
   would allow runtime service discovery, building a cross-sidecar service
   mesh.

6. **Integration with the AeroSLS service mesh** (§0 of the Service Mesh doc).
   The IDL and channel protocol are the foundation of a typed service mesh
   where each service is a sidecar and routing is via capability channels.

---

## Appendix A — file inventory

| Path | Description |
|------|-------------|
| `docs/AeroSLS-Polyglot-Nexus-Phase3-Design-v0.1.md` | This design document |
| `idl/logging.aeroidl` | Example IDL: logging service |
| `idl/calculator.aeroidl` | Example IDL: calculator service |
| `idl/imageproc.aeroidl` | Example IDL: image processing service (§1.6) |
| `tools/aeroidl/README.md` | IDL compiler reference |
| `tools/aeroidl/gen/calculator.h` | Generated C header (caller side) |
| `tools/aeroidl/gen/calculator.lisp` | Generated Lisp FFI (callee side) |
| `kernel/cap.h` | Message syscalls 302-304, `SLSCapDesc`, request structs, extended `ChanMsg` |
| `kernel/cap.c` | `cap_send_msg` / `cap_recv_msg` / `cap_arena_free`, payload pool, multi-cap holders |
| `kernel/syscall_dispatch.c` | Dispatch wiring for 302-304 |
| `user/libaerocap/aerosls_cap.h` | Ring-3 channel runtime behind the generated C stubs (real transport) |
| `user/aerosls/` | Ring-3 channel runtime behind the generated RUST stubs: `chan_send`/`chan_recv`/`arena_alloc`/`arena_free`/`arena_mem`/`next_request_id`, same syscalls + framing as the C library (`target` feature = real ABI; `host-fake` = test seam) |
| `tests/cap_msg_host_test.c` | Host test: message round-trip, zero-copy arena read, arena_free, hygiene |
| `tests/aerocap_abi_host_test.c` | ABI pinning extended to the Phase 3 request structs + syscall numbers |
| `tools/aeroidl/tests/e2e_real_transport.rs` | Generated calculator client + dispatcher LINKED against the real `aerosls` runtime, full `add(5,3) → Ok(8)` round trip over an in-process fake kernel |

## Appendix B — capability word layout (reference)

From Phase 1 `kernel/cap.h`:

```
 63    61 60    57 56          40 39       32 31          16 15          4 3  0
+--------+--------+--------------+----------+--------------+-------------+-----+
| type   | state  | object id    | perms    | offset (pg)  | len (pg)    |rsrvd|
| 3 bits | 4 bits | 17 bits      | 8 bits   | 16 bits      | 12 bits     | 0   |
+--------+--------+--------------+----------+--------------+-------------+-----+
```

Phase 3 additions:
- `CAP_TYPE_TRAMP` = 4 (new type for trampoline capabilities)
- Cap descriptor flag bit 2 = `BORROWED` (new flag for `@borrowed` annotations)
- Arena object header at offset −16 from the allocated region (refcount, size,
  owner, flags)

## Appendix C — syscall additions (Phase 3)

### As implemented (v0.2)

| Syscall number | Name | Purpose |
|----------------|------|---------|
| 302 | `SYS_SLS_CAP_SEND_MSG` | Send a channel message: staged payload + up to 4 moved MEM caps (§2.3) |
| 303 | `SYS_SLS_CAP_RECV_MSG` | Receive a message: payload copied out, caps installed into fresh slots |
| 304 | `SYS_SLS_CAP_ARENA_FREE` | Drop ONE MEM reference; arena frames return at refcount 0 |

**Superseded numbers:** the draft's 298-300 collided with Phase 1.5 syscalls
already in the tree (`GETPPID` 298, `PROGRAM_SPAWN_NB` 299, `YIELD` 300,
`PROGRAM_SPAWN_NB_HELD` 301), so the implemented transport moved to 302-304.
The trampoline syscalls (draft 299/300) are NOT yet implemented; when they
land they should take 305-306.

### As designed (v0.1, superseded)

| Syscall number | Name | Purpose |
|----------------|------|---------|
| 298 | `SYS_SLS_ARENA_FREE` | Decrement refcount on arena object; free at 0 |
| 299 | `SYS_SLS_TRAMPOLINE_CREATE` | Create a trampoline cap to a trusted callee |
| 300 | `SYS_SLS_TRAMPOLINE_CALL` | Invoke a trampoline (for non-inline callers) |

The inline trampoline path (§5.4) uses `WRPKRU + JMP` directly from user
space. `SYS_SLS_TRAMPOLINE_CALL` exists for callers that cannot use inline
assembly (e.g., interpreted languages).

**Implementation deltas from the draft** (this is the record of what the
code actually does, for the Appendix A file inventory):

- The kernel message envelope is payload-opaque: it stages the IDL bytes in
a fixed 8 × 4 KiB pool, moves the caps, and echoes the tag. The
`[opcode u16][payload_len u32]` request header is the Ring-3 library's
concern (`user/libaerocap/aerosls_cap.h`), matching what the generated
dispatcher parses — the kernel never interprets it.
- **Flags are bit0-only.** The draft's envelope table (§2.3, "bit0 REPLY |
bit1 NO_REPLY") describes a request/response pairing the implementation
never adopted: replies are a user-level convention (opcode 0 = response),
so the kernel `ChanMsg.flags` field carries a single bit — bit0 =
`NO_REPLY` (async, `0x0001`) — matching the IDL payload table's "bit0 =
async" line. The same value is used verbatim by all three Ring-3
runtimes: C (`AEROSLS_CHAN_FLAG_NO_REPLY`), Rust runtime
(`req::CHAN_FLAG_NO_REPLY`), and the AeroIDL Rust generator's emitted
`CHAN_FLAG_NO_REPLY` constant, so a message's async flag reads the same
in every language. (The separate sidecar-channels transport spec's
`F_NO_REPLY = 0x0002` is a different envelope and is unaffected.)
- Cap descriptors (`struct SLSCapDesc`, 16 bytes) travel with each message:
sender slot, byte offset/len into the arena region, rights, flags. On recv
the kernel installs each cap into a fresh receiver slot and rewrites the
descriptor's `slot` field — the SEND_CAP / RECV_CAP sequence of §2.2.
- Recv is non-blocking (returns `CAP_EAGAIN` on empty); the Ring-3 SDK
retries. The kernel-side park/wake is Phase-1.5 territory (single-cap path)
and is not extended to messages in this milestone. The Rust runtime's
`chan_recv` reports `CAP_EAGAIN` faithfully; the generated Rust client is a
single-shot poll, so a real deployment needs the caller to retry (or the
kernel park, once extended).
- Both Ring-3 runtimes (C: `user/libaerocap/aerosls_cap.h`; Rust:
`user/aerosls`) speak the identical wire format: requests get the
`[opcode u16][payload_len u32]` header, replies (opcode 0) are raw, and the
request structs are byte-for-byte the kernel's. The Rust runtime adds
`CAP_PERM_MAP` to every `arena_alloc` so `arena_mem` can map the buffer
(the C library does the same), and maps caps on demand into a reserved
32 TiB region (64 slots × 16 MiB).
- `SYS_SLS_CAP_ARENA_FREE` (304) is the single-reference analogue of
`cap_revoke`: it drops the caller's own slot holder and frees the object's
arena frames only at refcount 0, so caps already sent on a channel survive.
- Payload pool is 8 buffers; a send whose payload exceeds a free buffer
returns `CAP_ENOSPC` before touching the queue (fail-before-mutate).
