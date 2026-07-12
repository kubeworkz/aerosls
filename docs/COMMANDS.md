# AeroSLS Command Reference

AeroSLS exposes three command surfaces:

1. **Serial Shell** — interactive shell on COM1, reached via a USB-UART adapter on real hardware or the QEMU `-serial` flag.
2. **REST API** — HTTP/JSON served on port 3000 from the running kernel.
3. **Build Commands** — Makefile targets for compile, test, and hardware bundle generation.

---

## Serial Shell

Connect at 38400 baud on COM1. The prompt shows `uid:<id>[tx:<n>]> ` when a transaction is open, otherwise `uid:<id>> `.

```
uid:0> help
```

### Session

| Command | Description |
|---------|-------------|
| `login <uid> <gid>` | Switch session credentials (no password — capability model) |
| `help` | Print the full command list |

---

### Object Catalog

The SLS object catalog is the kernel's persistent namespace. Every piece of data is a named object.

| Command | Description |
|---------|-------------|
| `valloc <name> <type> <pages>` | Allocate a named persistent object |
| `vfree <name>` | Release a named object and its pages |
| `ls objects` / `ls` | List all catalog entries with type, pages, and access bits |
| `stat <name>` | Show full details for one object (type, owner, ACL, schema, fields) |

**Object types for `valloc`:**

| Code | Name | Purpose |
|------|------|---------|
| `0` | `SYSTEM_META` | Kernel metadata, config blobs |
| `1` | `DB_TABLE` | Relational table (keyed records) |
| `2` | `DB_INDEX` | B-tree index over a table |
| `3` | `HEAP_BLOB` | Unstructured byte heap |

**Example:**
```
uid:0> valloc employees DB_TABLE 4
uid:0> stat employees
```

---

### Records (DB)

Key-value records within a `DB_TABLE` or `HEAP_BLOB` object.

| Command | Description |
|---------|-------------|
| `insert <object> <key> <value>` | Add a new record field |
| `update <object> <key> <value>` | Modify a field (staged to WAL if a transaction is open) |
| `select <object> [<key>]` | Read one field, or all fields if key is omitted |
| `delete <object> <key>` | Remove a field (blocked on append-only objects) |
| `write <name> <payload>` | Direct heap write — no transaction, no WAL (legacy) |

**Example:**
```
uid:0> tx begin
uid:0[tx:1]> insert employees name Alice
uid:0[tx:1]> insert employees dept Engineering
uid:0[tx:1]> tx commit
uid:0> select employees
```

---

### Schema

Enforce typed fields on a `DB_TABLE` object.

| Command | Description |
|---------|-------------|
| `schema set <object> <key> <type>` | Define a field's type |
| `schema show <object>` | Dump schema definition and current live values |

**Field types:** `STRING` · `UINT64` · `FLOAT` · `BOOL`

**Example:**
```
uid:0> schema set employees salary UINT64
uid:0> schema show employees
```

---

### Transactions

AeroSLS uses Write-Ahead Logging (WAL) for ACID durability.

| Command | Description |
|---------|-------------|
| `tx begin` | Open an ACID transaction (all subsequent `update`/`insert`/`delete` are staged) |
| `tx commit` | Flush staged writes to WAL and apply to catalog |
| `tx rollback` | Discard all staged writes without touching the WAL |
| `wal dump` | Print all WAL entries (LSN, type, object, key, value) |
| `wal recover` | Replay WAL from the beginning — used after a simulated crash |

---

### Security & Permissions

Capability-based access control. Each object has a per-UID permission bitmask.

| Command | Description |
|---------|-------------|
| `role set <uid> <role>` | Assign a role to a UID |
| `grant <uid> <object> <perm>` | Add permissions: `r`, `w`, `x`, `rw`, `rwx` |
| `revoke <uid> <object> <perm>` | Remove permissions |
| `chmod <name> <mask_hex>` | Set raw permission bitmask directly (legacy) |
| `seal <name>` | Encrypt an object with a password |

**Roles:** `SYSTEM_KERNEL` · `DB_ADMIN` · `APP_USER` · `GUEST`

**Example:**
```
uid:0> role set 42 APP_USER
uid:0> grant 42 employees rw
uid:0> revoke 42 employees x
```

---

### Token Authentication

Issues and manages bearer tokens for the REST API.

| Command | Description |
|---------|-------------|
| `auth create <email> <uid> <role>` | Create a bearer token and bind it to an email |
| `auth list` | Show the full token registry (email · uid · role · token) |
| `auth revoke <email>` | Revoke all tokens for an email address |

**Example:**
```
uid:0> auth create alice@example.com 42 APP_USER
uid:0> auth list
```

---

### Storage Tiers

AeroSLS models three storage tiers: L1 (in-kernel cache), L2 (DRAM), L3 (NVMe SSD).

| Command | Description |
|---------|-------------|
| `tier list` | Show each object's current tier and access frequency |
| `tier promote <name>` | Pull object up one tier: L3 → L2 → L1 |
| `tier demote <name>` | Push object down one tier: L1 → L2 → L3 |

---

### Query Engine

Cognitive scan using natural language over the in-memory object catalog.

| Command | Description |
|---------|-------------|
| `query <text>` | Natural-language direct object scan (e.g. `query show all tables`) |
| `query scan` | Export full catalog as a JSON manifest |

---

### Microkernel Services

Five Ring-0 microkernel services: VirtualMemoryMgr, ObjectSecurityMgr, NativeDbStoreMgr, StorageTierMgr, RecoveryLogVerifier.

| Command | Description |
|---------|-------------|
| `svc list` | Show all service PIDs, ports, states, restart counts |
| `svc crash <name>` | Inject a fault into a named service (fault-isolation test) |
| `svc restart <name>` | Restart a crashed service via the watchdog daemon |
| `ipc stat` | Show IPC queue depths, posted/dispatched/dropped message counts |
| `ipc post <svc> <opcode_hex>` | Post a raw IPC message to a service for testing |

**Example:**
```
uid:0> svc crash NativeDbStoreMgr
uid:0> svc list
uid:0> svc restart NativeDbStoreMgr
```

---

### Process Isolation (Ring-3)

Spawn ELF64 or flat-binary services in isolated Ring-3 address spaces.

| Command | Description |
|---------|-------------|
| `upload <name> <hex>` | Write hex-encoded bytes to the binary store |
| `demo <name>` | Load the built-in AeroSLS test binary |
| `loader list` | Show all binaries in the service binary store |
| `load <name>` | Spawn a Ring-3 process from an uploaded binary |
| `proc list` | Show all running Ring-3 processes (PID, name, state) |
| `proc spawn <object>` | Create a process from a `SERVICE_PROCESS` catalog object |
| `proc kill <pid>` | Terminate a running process |

---

### Web Assets

Dynamic web asset store (served alongside the compiled-in Navigator bundle).

| Command | Description |
|---------|-------------|
| `webapp set <obj> <path> <html>` | Store an asset at a URL path (replaces existing) |
| `webapp append <obj> <path> <s>` | Append content to an existing asset |
| `webapp list [<obj>]` | List all assets — use `*` to list all objects |

---

## REST API

The kernel HTTP server listens on port 3000. All endpoints return JSON. CORS is open (`*`).

### Authentication

Protected endpoints require:
```
Authorization: Bearer <token>
```

Get a token:
```bash
curl -X POST http://<ip>:3000/auth/token \
     -H "Content-Type: application/json" \
     -d '{"email":"dave@gridworkz.com","password":"any"}'
```

Demo accounts:

| Email | Role | Token |
|-------|------|-------|
| `dave@gridworkz.com` | `DB_ADMIN` | `deadbeef01234567cafebabe76543210` |
| `bob@vance.com` | `APP_USER` | `cafebabe7654321089abcdef01234567` |
| `carol@gridworkz.com` | `DB_ADMIN` | `feedf00dabcdef0112345678deadc0de` |
| `guest@sandbox.com` | `GUEST` | `deadc0de9988776655443322aabbccdd` |

---

### Endpoints

#### System

| Method | Path | Auth | Description |
|--------|------|------|-------------|
| `GET` | `/api/health` | None | Liveness probe — returns `{"status":"ok","system":"AeroSLS 4.0"}` |
| `GET` | `/api/scan` | None | Object catalog, WAL stats, service summary |

#### Objects

| Method | Path | Auth | Description |
|--------|------|------|-------------|
| `GET` | `/api/objects` | None | All SLS objects |
| `GET` | `/api/objects/<name>` | None | Single object detail |
| `POST` | `/api/valloc` | `APP_USER+` | Allocate a new object — body: `{"name":"…","type":1,"pages":4}` |
| `POST` | `/api/record` | `APP_USER+` | Write a record — body: `{"object":"…","key":"…","value":"…"}` |

#### Transactions

| Method | Path | Auth | Description |
|--------|------|------|-------------|
| `POST` | `/api/tx/begin` | `APP_USER+` | Open a transaction |
| `POST` | `/api/tx/commit` | `APP_USER+` | Commit |
| `POST` | `/api/tx/rollback` | `APP_USER+` | Rollback |

#### Observability

| Method | Path | Auth | Description |
|--------|------|------|-------------|
| `GET` | `/api/services` | None | Microkernel service list + IPC stats |
| `GET` | `/api/wal` | None | Write-Ahead Log entries |
| `GET` | `/api/tiers` | None | Storage tier contents (L1 / L2 / L3) |
| `GET` | `/api/processes` | None | Ring-3 process table |
| `GET` | `/api/query?q=<text>` | None | Natural-language object scan |

#### Auth

| Method | Path | Auth | Description |
|--------|------|------|-------------|
| `POST` | `/auth/token` | None | Issue a bearer token |
| `GET` | `/auth/verify` | Bearer | Validate a token — returns uid, role, email |

#### Navigator SPA

| Method | Path | Auth | Description |
|--------|------|------|-------------|
| `GET` | `/` | None | Navigator SPA root (index.html embedded in kernel) |
| `GET` | `/assets/*` | None | JS / CSS bundles |

---

## Build Commands

Run from the repository root.

| Command | Description |
|---------|-------------|
| `make x86-iso` | Compile kernel + link + generate UEFI/BIOS bootable ISO |
| `make x86-run` | Build ISO and boot in QEMU with display (interactive) |
| `make bundle` | Rebuild `slsos-sim` UI and regenerate `kernel/webapp_bundle.c` |
| `make riscv-elf` | Compile RISC-V kernel ELF |
| `make riscv-run` | Build RISC-V ELF and boot in QEMU virt |
| `make clean` | Remove all build artifacts (`.o`, `.bin`, `.iso`, `.elf`, `.log`) |
| `make all` | Build x86 ISO + RISC-V ELF (default target) |

### Real hardware

```bash
# Flash ISO to USB
sudo dd if=sls_operating_system.iso of=/dev/sdX bs=4M status=progress

# Override static IP (edit before building)
# include/config.h:
#   #define KERNEL_STATIC_IP  0x6401A8C0UL   // 192.168.1.100
#   #define KERNEL_STATIC_GW  0x0101A8C0UL   // 192.168.1.1
```

### Workflow: update the UI and rebuild

```bash
# 1. Edit slsos-sim/src/ ...
# 2. Regenerate the embedded bundle
make bundle
# 3. Rebuild ISO with new bundle
make x86-iso
# 4. Flash to USB or boot in QEMU
```
