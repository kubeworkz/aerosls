# AeroIDL Compiler (`aeroidl-cc`)

The AeroIDL compiler generates typed stubs for cross-sidecar function calls.
It takes `.aeroidl` interface definitions and produces language-specific
marshalling code that uses AeroSLS channel messages and MEM capabilities for
zero-copy data transfer.

## Usage

```bash
# Generate C stubs (for C/Rust Wasm sidecars)
aeroidl-cc --target c --output gen/calculator.h idl/calculator.aeroidl

# Generate Common Lisp FFI (for Lisp sidecars)
aeroidl-cl --output gen/calculator.lisp idl/calculator.aeroidl

# Generate both at once
aeroidl-cc --target c --output gen/calculator.h \
           --target clisp --output gen/calculator.lisp \
           idl/calculator.aeroidl
```

## Input format

See `idl/calculator.aeroidl` and `idl/logging.aeroidl` for examples.

### Primitives

| Type   | Size     | Description                          |
|--------|----------|--------------------------------------|
| `i8`   | 1 byte   | Signed 8-bit integer                 |
| `i16`  | 2 bytes  | Signed 16-bit integer                |
| `i32`  | 4 bytes  | Signed 32-bit integer                |
| `i64`  | 8 bytes  | Signed 64-bit integer                |
| `u8`   | 1 byte   | Unsigned 8-bit integer               |
| `u16`  | 2 bytes  | Unsigned 16-bit integer              |
| `u32`  | 4 bytes  | Unsigned 32-bit integer              |
| `u64`  | 8 bytes  | Unsigned 64-bit integer              |
| `f32`  | 4 bytes  | IEEE 754 single-precision float      |
| `f64`  | 8 bytes  | IEEE 754 double-precision float      |
| `bool` | 1 byte   | Boolean                              |

### Compound types

| Type     | Description                                    |
|----------|------------------------------------------------|
| `string` | UTF-8 byte sequence (always carries a MEM cap) |
| `bytes`  | Raw byte sequence (always carries a MEM cap)   |
| `T[]`    | Array of T (carries a MEM cap + count header)  |
| `struct` | Named aggregate (inline if ≤ 128 bytes, else MEM cap) |
| `enum`   | Sum type with explicit integer discriminants   |

### Ownership annotations

| Annotation | Meaning                                         |
|------------|-------------------------------------------------|
| `@borrowed`| Caller retains ownership; callee reads only     |
| `@owned`   | Callee takes ownership; must free or transfer   |
| `@arena`   | Lives in shared arena; lifetime via cap refcount|

### Built-in types

| Type           | Description                                        |
|----------------|----------------------------------------------------|
| `Option<T>`    | `Some(T)` or `None` — maps to nullable cap or flag |
| `Result<T, E>` | `Ok(T)` or `Err(E)` — standard error union         |

## Output

Each target generates:

1. **Type definitions** — C structs/enums or CL defstruct/deftype
2. **Stub functions** — one per interface method, handling:
   - Argument serialization into channel message payloads
   - MEM cap creation and attachment for arena/borrowed/owned data
   - Channel send (SEND_CAP) and receive (RECV_CAP)
   - Reply deserialization and error unwrapping
3. **Dispatch table** (callee side) — opcode-to-function routing

## Architecture

```
  .aeroidl source
       │
       ▼
  ┌─────────────────┐
  │  aeroidl parser  │   (recursive descent, ~2K LOC)
  └────────┬────────┘
           │ AST
           ▼
  ┌─────────────────┐
  │  type checker    │   (ownership, memory layout, channel limits)
  └────────┬────────┘
           │ typed AST
           ▼
  ┌─────────────────┐     ┌─────────────────┐
  │  C codegen       │     │  CL codegen      │
  │  (aeroidl-cc)    │     │  (aeroidl-cl)    │
  └────────┬────────┘     └────────┬────────┘
           │                       │
           ▼                       ▼
     .h / .c files            .lisp files
```

## Constraints enforced by the compiler

- Payload per message ≤ 4096 bytes (kernel channel limit).
- Max 8 MEM cap arguments per message.
- Max 128 bytes for inline struct arguments (larger → automatic MEM cap).
- Ownership annotations must be consistent (no `@owned` on a borrowed channel).
- Async methods cannot return arena-owned data (fire-and-forget = no reply channel).
- Error types must have a u32 code field for the kernel error registry.
