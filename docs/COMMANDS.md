# AeroSLS Command Reference

AeroSLS exposes three command surfaces:

1. **Serial Shell** — interactive shell on COM1, reached via a USB-UART adapter on real hardware or the QEMU `-serial` flag.
2. **REST API** — HTTP/JSON served on port 3000 from the running kernel.
3. **Build Commands** — Makefile targets for compile, test, and hardware bundle generation.

---

## Serial Shell

Connect at 38400 baud on COM1. The prompt shows `uid:<id>[tx:<n>]>`  when a transaction is open, otherwise `uid:<id>>` .

```
uid:0> help
```

### Session


| Command             | Description                                                 |
| ------------------- | ----------------------------------------------------------- |
| `login <uid> <gid>` | Switch session credentials (no password — capability model) |
| `help`              | Print the full command list                                 |


---

### Object Catalog

The SLS object catalog is the kernel's persistent namespace. Every piece of data is a named object.


| Command                        | Description                                                         |
| ------------------------------ | ------------------------------------------------------------------- |
| `valloc <name> <type> <pages>` | Allocate a named persistent object                                  |
| `vfree <name>`                 | Release a named object and its pages                                |
| `ls objects` / `ls`            | List all catalog entries with type, pages, and access bits          |
| `stat <name>`                  | Show full details for one object (type, owner, ACL, schema, fields) |


**Object types for `valloc`:**


| Code | Name              | Purpose                                                                                                                                                                                                                         |
| ---- | ----------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `0`  | `SYSTEM_META`     | Kernel metadata, config blobs                                                                                                                                                                                                   |
| `1`  | `DB_TABLE`        | Relational table (keyed records)                                                                                                                                                                                                |
| `2`  | `DB_INDEX`        | B-tree index over a table                                                                                                                                                                                                       |
| `3`  | `HEAP_BLOB`       | Unstructured byte heap                                                                                                                                                                                                          |
| `4`  | `SERVICE_PROCESS` | Ring-3 executable (internal kernel services)                                                                                                                                                                                    |
| `5`  | `WEB_APP`         | HTML/JS/CSS asset store                                                                                                                                                                                                         |
| `6`  | `JOURNAL`         | IBM i-style journal object                                                                                                                                                                                                      |
| `7`  | `PROGRAM`         | Named executable — SLS-native replacement for a filesystem binary. Journaled, indexed, MQT-tracked. Use `/api/program/*` endpoints.                                                                                             |
| `8`  | `STREAM`          | Raw byte-stream "file" object. Up to 1 MiB per stream (256 × 4 KiB frames, lazily allocated from the physical frame pool — no static BSS cost). Readable by `GUEST` role; no execute permission. Use `/api/stream/*` endpoints. |


**Example:**

```
uid:0> valloc employees DB_TABLE 4
uid:0> stat employees
```

---

### Records (DB)

Key-value records within a `DB_TABLE` or `HEAP_BLOB` object.


| Command                         | Description                                             |
| ------------------------------- | ------------------------------------------------------- |
| `insert <object> <key> <value>` | Add a new record field                                  |
| `update <object> <key> <value>` | Modify a field (staged to WAL if a transaction is open) |
| `select <object> [<key>]`       | Read one field, or all fields if key is omitted         |
| `delete <object> <key>`         | Remove a field (blocked on append-only objects)         |
| `write <name> <payload>`        | Direct heap write — no transaction, no WAL (legacy)     |


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


| Command                            | Description                                    |
| ---------------------------------- | ---------------------------------------------- |
| `schema set <object> <key> <type>` | Define a field's type                          |
| `schema show <object>`             | Dump schema definition and current live values |


**Field types:** `STRING` · `UINT64` · `FLOAT` · `BOOL`

**Example:**

```
uid:0> schema set employees salary UINT64
uid:0> schema show employees
```

---

### Transactions

AeroSLS uses Write-Ahead Logging (WAL) for ACID durability.


| Command       | Description                                                                     |
| ------------- | ------------------------------------------------------------------------------- |
| `tx begin`    | Open an ACID transaction (all subsequent `update`/`insert`/`delete` are staged) |
| `tx commit`   | Flush staged writes to WAL and apply to catalog                                 |
| `tx rollback` | Discard all staged writes without touching the WAL                              |
| `wal dump`    | Print all WAL entries (LSN, type, object, key, value)                           |
| `wal recover` | Replay WAL from the beginning — used after a simulated crash                    |


---

### Journaling

IBM i-style before/after-image journal.  Each journal captures every INSERT, UPDATE, and DELETE on attached tables.  Journal entries survive beyond a single transaction (unlike the WAL) and are used for audit trails and change-data capture.


| Command                            | Description                                 |
| ---------------------------------- | ------------------------------------------- |
| `journal create <name>`            | Create a journal object (type=6)            |
| `journal attach <journal> <table>` | Start capturing DML changes for a table     |
| `journal detach <journal> <table>` | Stop capturing                              |
| `journal list`                     | Show all active journal attachments         |
| `journal dump <name> [<seq>]`      | Print entries (optionally from sequence N)  |
| `journal purge <name>`             | Remove rolled-back entries to reclaim space |


**Entry types** (IBM i codes):


| Type | Meaning                      |
| ---- | ---------------------------- |
| `PT` | Put — INSERT                 |
| `UP` | Update (after-image)         |
| `UB` | Update Before (before-image) |
| `DL` | Delete                       |
| `CM` | Commit marker                |
| `RB` | Rollback marker              |


---

### Row Locking

Exclusive (X) row locks — Read-Committed isolation.  Locks are acquired before WAL staging and released on commit or rollback.


| Command     | Description                                  |
| ----------- | -------------------------------------------- |
| `lock list` | Show all active row locks (tx_id, type, key) |


Conflict behaviour: a second transaction trying to write the same key is immediately rejected (no wait/deadlock).

---

### Secondary Indexes

Sorted keyed access paths over DB_TABLE fields (IBM i logical file / keyed access path).  Indexes are auto-maintained on every INSERT, UPDATE, and DELETE.


| Command                               | Description                                |
| ------------------------------------- | ------------------------------------------ |
| `index create <name> <table> <field>` | Build a sorted index on a field suffix     |
| `index list`                          | Show all indexes                           |
| `index rebuild <name>`                | Re-scan the parent table and rebuild       |
| `index drop <name>`                   | Remove an index                            |
| `index scan <name> [<start_value>]`   | O(log n) lookup or range scan from a value |


Field matching uses suffix rules: `field="dept"` captures `alice_dept`, `bob_dept`, etc.

---

### Constraints

Data integrity enforced at the kernel boundary, before the lock and WAL stage.


| Command                                                | Description             |
| ------------------------------------------------------ | ----------------------- |
| `constraint add <table> <field> UNIQUE`                | Reject duplicate values |
| `constraint add <table> <field> NOT_NULL`              | Reject empty values     |
| `constraint add <table> <field> RANGE <min> <max>`     | Numeric range check     |
| `constraint add <table> <field> REFERENCE <ref_table>` | Foreign-key integrity   |
| `constraint list [<table>]`                            | Show active constraints |
| `constraint remove <table> <field> <type>`             | Drop a constraint       |


**Violation codes** returned by DML on constraint failure:


| Code | Constraint |
| ---- | ---------- |
| `1`  | UNIQUE     |
| `2`  | NOT_NULL   |
| `3`  | RANGE      |
| `4`  | REFERENCE  |


---

### Cursors

Server-side iterators that hold scan position across multiple FETCH calls (IBM i `DECLARE CURSOR / OPEN / FETCH / CLOSE`).


| Command                                                       | Description                                     |
| ------------------------------------------------------------- | ----------------------------------------------- |
| `cursor open <table> [where <field>=<value>] [order <index>]` | Open a cursor, optionally filtered and ordered  |
| `cursor fetch <id> [<n>]`                                     | Fetch next N rows (default 5)                   |
| `cursor close <id>`                                           | Close cursor and free slot                      |
| `cursor list`                                                 | List all open cursors with position/done status |


`cursor fetch` returns `{"id":N,"rows":[...],"fetched":N,"done":bool}`.  Keep calling fetch until `done=true`.

---

### Aggregates & ORDER BY

Analytics queries in a single pass over the table (IBM i `OPNQRYF / GROUP BY / ORDER BY`).


| Command                                                                                          | Description                             |
| ------------------------------------------------------------------------------------------------ | --------------------------------------- |
| `aggregate <table> COUNT [field] [where <f>=<v>] [group <f>] [having <n>]`                       | Count matching rows, optionally grouped |
| `aggregate <table> SUM&#124;AVG&#124;MIN&#124;MAX <field> [where <f>=<v>] [order ASC&#124;DESC]` | Numeric aggregate                       |
| `select <table> [where <f>=<v>] [order <f> ASC&#124;DESC]`                                       | ORDER BY with no aggregation            |


**Examples:**

```
aggregate employees COUNT
aggregate employees SUM score where dept=Engineering
aggregate employees COUNT group dept having 2
select employees where dept=Engineering order score DESC
```

---

### Materialized Query Tables

Pre-computed aggregate tables that auto-refresh on every committed INSERT, UPDATE, or DELETE to the base table (IBM i summary tables / `CREATE TABLE … AS SELECT …`).


| Command                                                                                                  | Description                        |
| -------------------------------------------------------------------------------------------------------- | ---------------------------------- |
| `mqt create <name> <base> COUNT&#124;SUM&#124;AVG&#124;MIN&#124;MAX [field] [group <f>] [where <f>=<v>]` | Create an MQT with initial refresh |
| `mqt list`                                                                                               | Show all MQTs                      |
| `mqt refresh <name>`                                                                                     | Force a re-computation             |
| `mqt drop <name>`                                                                                        | Remove MQT and free result table   |
| `mqt scan <name>`                                                                                        | Show current result records        |


MQT results are stored as regular `DB_TABLE` records — readable via `select`, indexable via DB3, and queryable via DB5/DB6.  The `refreshed_tick` key records the kernel tick at last refresh.

---

### Security & Permissions

Capability-based access control. Each object has a per-UID permission bitmask.


| Command                        | Description                                  |
| ------------------------------ | -------------------------------------------- |
| `role set <uid> <role>`        | Assign a role to a UID                       |
| `grant <uid> <object> <perm>`  | Add permissions: `r`, `w`, `x`, `rw`, `rwx`  |
| `revoke <uid> <object> <perm>` | Remove permissions                           |
| `chmod <name> <mask_hex>`      | Set raw permission bitmask directly (legacy) |
| `seal <name>`                  | Encrypt an object with a password            |


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


| Command                            | Description                                               |
| ---------------------------------- | --------------------------------------------------------- |
| `auth create <email> <uid> <role>` | Create a bearer token and bind it to an email             |
| `auth list`                        | Show the full token registry (email · uid · role · token) |
| `auth revoke <email>`              | Revoke all tokens for an email address                    |


**Example:**

```
uid:0> auth create alice@example.com 42 APP_USER
uid:0> auth list
```

---

### Storage Tiers

AeroSLS models three storage tiers: L1 (in-kernel cache), L2 (DRAM), L3 (NVMe SSD).


| Command               | Description                                          |
| --------------------- | ---------------------------------------------------- |
| `tier list`           | Show each object's current tier and access frequency |
| `tier promote <name>` | Pull object up one tier: L3 → L2 → L1                |
| `tier demote <name>`  | Push object down one tier: L1 → L2 → L3              |


---

### Query Engine

Cognitive scan using natural language over the in-memory object catalog.


| Command        | Description                                                        |
| -------------- | ------------------------------------------------------------------ |
| `query <text>` | Natural-language direct object scan (e.g. `query show all tables`) |
| `query scan`   | Export full catalog as a JSON manifest                             |


---

### Microkernel Services

Five Ring-0 microkernel services: VirtualMemoryMgr, ObjectSecurityMgr, NativeDbStoreMgr, StorageTierMgr, RecoveryLogVerifier.


| Command                       | Description                                                     |
| ----------------------------- | --------------------------------------------------------------- |
| `svc list`                    | Show all service PIDs, ports, states, restart counts            |
| `svc crash <name>`            | Inject a fault into a named service (fault-isolation test)      |
| `svc restart <name>`          | Restart a crashed service via the watchdog daemon               |
| `ipc stat`                    | Show IPC queue depths, posted/dispatched/dropped message counts |
| `ipc post <svc> <opcode_hex>` | Post a raw IPC message to a service for testing                 |


**Example:**

```
uid:0> svc crash NativeDbStoreMgr
uid:0> svc list
uid:0> svc restart NativeDbStoreMgr
```

---

### Process Isolation (Ring-3)

Spawn ELF64 or flat-binary services in isolated Ring-3 address spaces.


| Command               | Description                                              |
| --------------------- | -------------------------------------------------------- |
| `upload <name> <hex>` | Write hex-encoded bytes to the binary store              |
| `demo <name>`         | Load the built-in AeroSLS test binary                    |
| `loader list`         | Show all binaries in the service binary store            |
| `load <name>`         | Spawn a Ring-3 process from an uploaded binary           |
| `proc list`           | Show all running Ring-3 processes (PID, name, state)     |
| `proc spawn <object>` | Create a process from a `SERVICE_PROCESS` catalog object |
| `proc kill <pid>`     | Terminate a running process                              |


---

### Web Assets

Dynamic web asset store (served alongside the compiled-in Navigator bundle).


| Command                          | Description                                      |
| -------------------------------- | ------------------------------------------------ |
| `webapp set <obj> <path> <html>` | Store an asset at a URL path (replaces existing) |
| `webapp append <obj> <path> <s>` | Append content to an existing asset              |
| `webapp list [<obj>]`            | List all assets — use `*` to list all objects    |


---

### AI Agents (Phase H)

First-class OS objects wrapping an LLM inference endpoint, a permitted tool set, and a persistent memory table.  Agents execute a **ReAct** (Reason + Act) loop: the LLM reasons, calls kernel tools (DB read/write, tier promote, stream I/O), then produces a final text answer — all without leaving the kernel address space.


| Command                                      | Description                                            |
| -------------------------------------------- | ------------------------------------------------------ |
| `agent create <name> <endpoint> <model>`     | Create an agent (no tools; use REST to set tool mask)  |
| `agent run <name> <message…>`                | Run one ReAct loop; answer printed to serial log       |
| `agent list`                                 | List all agents with state, steps, and tool mask       |
| `agent status <name>`                        | Show full descriptor including last answer             |
| `agent kill <name>`                          | Stop and remove agent from catalog                     |


### Workflows (Phase H)

Ordered sequences of agent invocations.  Each step reads its input from a shared `DB_TABLE` and writes its output back — creating a data pipeline entirely within the OS.


| Command                                             | Description                                   |
| --------------------------------------------------- | --------------------------------------------- |
| `workflow create <name> <shared_table> <step_count>` | Define an empty workflow                     |
| `workflow addstep <name> <agent> <in_key> <out_key>` | Append a step to the pipeline                |
| `workflow run <name> <input…>`                       | Execute all steps sequentially               |
| `workflow list`                                      | List all workflows with progress bars        |
| `workflow status <name>`                             | Show step detail and current step index      |


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


| Email                 | Role       | Token                              |
| --------------------- | ---------- | ---------------------------------- |
| `dave@gridworkz.com`  | `DB_ADMIN` | `deadbeef01234567cafebabe76543210` |
| `bob@vance.com`       | `APP_USER` | `cafebabe7654321089abcdef01234567` |
| `carol@gridworkz.com` | `DB_ADMIN` | `feedf00dabcdef0112345678deadc0de` |
| `guest@sandbox.com`   | `GUEST`    | `deadc0de9988776655443322aabbccdd` |


---

### Endpoints

#### System


| Method | Path          | Auth | Description                                                       |
| ------ | ------------- | ---- | ----------------------------------------------------------------- |
| `GET`  | `/api/health` | None | Liveness probe — returns `{"status":"ok","system":"AeroSLS 4.0"}` |
| `GET`  | `/api/scan`   | None | Object catalog, WAL stats, service summary                        |


#### Objects


| Method | Path                  | Auth        | Description                                                     |
| ------ | --------------------- | ----------- | --------------------------------------------------------------- |
| `GET`  | `/api/objects`        | None        | All SLS objects                                                 |
| `GET`  | `/api/objects/<name>` | None        | Single object detail                                            |
| `POST` | `/api/valloc`         | `APP_USER+` | Allocate a new object — body: `{"name":"…","type":1,"pages":4}` |
| `POST` | `/api/record`         | `APP_USER+` | Write a record — body: `{"object":"…","key":"…","value":"…"}`   |


#### Transactions


| Method | Path               | Auth        | Description        |
| ------ | ------------------ | ----------- | ------------------ |
| `POST` | `/api/tx/begin`    | `APP_USER+` | Open a transaction |
| `POST` | `/api/tx/commit`   | `APP_USER+` | Commit             |
| `POST` | `/api/tx/rollback` | `APP_USER+` | Rollback           |


#### Observability


| Method | Path                  | Auth        | Description                                                                                                                                                                                                                                                                |
| ------ | --------------------- | ----------- | -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `GET`  | `/api/services`       | None        | Microkernel service list + IPC stats                                                                                                                                                                                                                                       |
| `GET`  | `/api/wal`            | None        | Write-Ahead Log entries                                                                                                                                                                                                                                                    |
| `GET`  | `/api/tiers`          | None        | Storage tier contents (L1 / L2 / L3)                                                                                                                                                                                                                                       |
| `GET`  | `/api/processes`      | None        | Ring-3 process table                                                                                                                                                                                                                                                       |
| `GET`  | `/api/programs`       | None        | List all `PROGRAM` objects — `{programs:[{name, vaddr, pages, tier, binary, binary_bytes, format}]}`. Live metadata (`status`, `last_pid`) visible via `/api/objects/<name>`.                                                                                              |
| `POST` | `/api/program/create` | `APP_USER+` | Create a `PROGRAM` object — `{"name":"…","pages":N}`. Auto-inserts metadata records (`status`, `binary_size`, `format`, `last_pid`) which are journaled, indexed, and MQT-tracked via the DB engine hook chain.                                                            |
| `POST` | `/api/program/upload` | `APP_USER+` | Upload binary hex — `{"name":"…","hex":"deadbeef…","offset":N,"last":0|1}`. Up to 1024 bytes/request; chain calls with `offset`. On `last=1` updates metadata records (`binary_size`, `format`, `status→ready`).                                                           |
| `POST` | `/api/program/spawn`  | `APP_USER+` | Spawn process — `{"name":"…"}`. Maps binary into a fresh PML4 via `program_spawn()`, enters Ring-3. Updates `status→running` and `last_pid`. Returns `{"ok":"true","pid":N}`.                                                                                              |
| `GET`  | `/api/streams`        | None        | List all `STREAM` objects — `{streams:[{name, mime_type, size, frames}]}`.                                                                                                                                                                                                 |
| `GET`  | `/api/stream/<name>`  | None        | Download raw file content. Sets `Content-Disposition: attachment` and the stored MIME type. Data is served frame-by-frame from the physical frame pool.                                                                                                                    |
| `POST` | `/api/stream/create`  | `APP_USER+` | Create a `STREAM` object — `{"name":"…","mime":"text/plain"}`. Auto-inserts metadata records (`status`, `byte_size`, `mime_type`).                                                                                                                                         |
| `POST` | `/api/stream/upload`  | `APP_USER+` | Upload file data as hex — `{"name":"…","hex":"…","offset":N,"last":0|1}`. Same chunked protocol as `/api/program/upload`. Max 1 MiB per stream (256 × 4 KiB frames from the physical frame pool — no static BSS cost). On `last=1` updates `byte_size` and `status→ready`. |
| `GET`  | `/api/query?q=<text>` | None        | Natural-language object scan                                                                                                                                                                                                                                               |
| `GET`  | `/api/locks`          | None        | Active row locks (DB2)                                                                                                                                                                                                                                                     |


#### Journaling


| Method | Path                            | Auth        | Description                                              |
| ------ | ------------------------------- | ----------- | -------------------------------------------------------- |
| `POST` | `/api/journal/attach`           | `APP_USER+` | `{"journal":"…","table":"…"}` — start journaling a table |
| `POST` | `/api/journal/detach`           | `APP_USER+` | `{"journal":"…","table":"…"}` — stop journaling          |
| `GET`  | `/api/journals`                 | None        | List all active journal attachments                      |
| `GET`  | `/api/journal/<name>[?since=N]` | None        | JSON array of journal entries from sequence N            |


#### Indexes


| Method | Path                            | Auth        | Description                                                       |
| ------ | ------------------------------- | ----------- | ----------------------------------------------------------------- |
| `GET`  | `/api/indexes`                  | None        | List all indexes                                                  |
| `GET`  | `/api/index/<name>[?q=<value>]` | None        | Dump index; `?q=val` does exact lookup → `{"hit":bool,"key":"…"}` |
| `POST` | `/api/index/create`             | `APP_USER+` | `{"name":"…","table":"…","field":"…"}`                            |
| `POST` | `/api/index/drop`               | `APP_USER+` | `{"name":"…"}`                                                    |
| `POST` | `/api/index/rebuild`            | `APP_USER+` | `{"name":"…"}`                                                    |


#### Constraints


| Method | Path                         | Auth        | Description                                                                                                   |
| ------ | ---------------------------- | ----------- | ------------------------------------------------------------------------------------------------------------- |
| `GET`  | `/api/constraints[?table=T]` | None        | List constraints, optionally filtered by table                                                                |
| `POST` | `/api/constraint/add`        | `APP_USER+` | `{"table":"…","field":"…","type":"UNIQUE&#124;NOT_NULL&#124;RANGE&#124;REFERENCE","min":N,"max":N,"ref":"…"}` |
| `POST` | `/api/constraint/remove`     | `APP_USER+` | `{"table":"…","field":"…","type":"…"}`                                                                        |


#### Cursors


| Method | Path                         | Auth        | Description                                                                     |
| ------ | ---------------------------- | ----------- | ------------------------------------------------------------------------------- |
| `GET`  | `/api/cursors`               | None        | List open cursors                                                               |
| `POST` | `/api/cursor/open`           | `APP_USER+` | `{"table":"…","where":"…","eq":"…","order":"<index_name>"}` → `{"cursor_id":N}` |
| `GET`  | `/api/cursor/fetch?id=N&n=M` | None        | Fetch next M rows → `{"rows":[…],"done":bool}`                                  |
| `GET`  | `/api/cursor/close?id=N`     | None        | Close cursor                                                                    |


#### Aggregates


| Method | Path             | Auth        | Description                                                                                                                                                        |
| ------ | ---------------- | ----------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------ |
| `POST` | `/api/aggregate` | `APP_USER+` | `{"table":"…","fn":"COUNT&#124;SUM&#124;AVG&#124;MIN&#124;MAX","field":"…","where":"…","eq":"…","group_by":"…","having":N,"order_by":"…","order":"ASC&#124;DESC"}` |


`fn` can be empty (or omitted) for a plain ORDER BY without aggregation.

#### Materialized Query Tables


| Method | Path               | Auth        | Description                                                      |
| ------ | ------------------ | ----------- | ---------------------------------------------------------------- |
| `GET`  | `/api/mqts`        | None        | List all MQTs                                                    |
| `GET`  | `/api/mqt/<name>`  | None        | Read current MQT result records                                  |
| `POST` | `/api/mqt/create`  | `APP_USER+` | `{"name":"…","table":"…","fn":"SUM","field":"…","group_by":"…"}` |
| `POST` | `/api/mqt/refresh` | `APP_USER+` | `{"name":"…"}` — force re-computation                            |
| `POST` | `/api/mqt/drop`    | `APP_USER+` | `{"name":"…"}`                                                   |


#### Auth


| Method | Path           | Auth   | Description                                 |
| ------ | -------------- | ------ | ------------------------------------------- |
| `POST` | `/auth/token`  | None   | Issue a bearer token                        |
| `GET`  | `/auth/verify` | Bearer | Validate a token — returns uid, role, email |


#### AI Agents & Workflows (Phase H)

All write routes require `APP_USER+`. Read routes are open.

| Method   | Path                    | Auth        | Description                                                                                                                                                                      |
| -------- | ----------------------- | ----------- | -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `GET`    | `/api/agents`           | None        | List all agents — `{count, agents:[{name, model, endpoint, state, steps, tool_mask, object_id}]}`                                                                               |
| `GET`    | `/api/agent/<name>`     | None        | Single agent descriptor                                                                                                                                                          |
| `POST`   | `/api/agent/create`     | `APP_USER+` | Create an agent — `{"name":"…","endpoint":"ip:port","model":"llama3.2","system_prompt":"…","tools":"db_select,db_query"}`. Tool list is comma-separated.              |
| `POST`   | `/api/agent/run`        | `APP_USER+` | Run one ReAct loop — `{"name":"…","message":"…"}`. **Blocks** until inference completes. Returns `{ok, agent, steps}`. Full answer is on the kernel serial log.              |
| `POST`   | `/api/agent/drop`       | `APP_USER+` | Remove an agent — `{"name":"…"}`                                                                                                                                              |
| `GET`    | `/api/workflows`        | None        | List all workflows — `{count, workflows:[{name, state, step_count, current_step}]}`                                                                                              |
| `GET`    | `/api/workflow/<name>`  | None        | Workflow descriptor with steps array                                                                                                                                             |
| `POST`   | `/api/workflow/create`  | `APP_USER+` | Define pipeline — `{"name":"…","shared_table":"…","step_count":N,"step0_agent":"…","step0_in":"…","step0_out":"…",…}`. Keys follow `stepN_agent/in/out` pattern.        |
| `POST`   | `/api/workflow/run`     | `APP_USER+` | Execute pipeline — `{"name":"…","input":"…"}`. **Blocks** until all steps finish. Each step's output is written to the shared state table under `out_key`.                  |

**Available tool names for `tools` field:** `db_select` · `db_insert` · `db_query` · `stream_read` · `stream_write` · `tier_promote`

**Demo admin token (fixed at boot):** `deadbeef01234567cafebabe76543210` (uid=1000, role=`DB_ADMIN`)


#### Navigator SPA


| Method | Path        | Auth | Description                                        |
| ------ | ----------- | ---- | -------------------------------------------------- |
| `GET`  | `/`         | None | Navigator SPA root (index.html embedded in kernel) |
| `GET`  | `/assets/*` | None | JS / CSS bundles                                   |


---

## Build Commands

Run from the repository root.


| Command          | Description                                                       |
| ---------------- | ----------------------------------------------------------------- |
| `make x86-iso`   | Compile kernel + link + generate UEFI/BIOS bootable ISO           |
| `make x86-run`   | Build ISO and boot in QEMU with display (interactive)             |
| `make bundle`    | Rebuild `slsos-sim` UI and regenerate `kernel/webapp_bundle.c`    |
| `make riscv-elf` | Compile RISC-V kernel ELF                                         |
| `make riscv-run` | Build RISC-V ELF and boot in QEMU virt                            |
| `make clean`     | Remove all build artifacts (`.o`, `.bin`, `.iso`, `.elf`, `.log`) |
| `make all`       | Build x86 ISO + RISC-V ELF (default target)                       |


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

---

## AI Backend

The Navigator's AI Co-Processor (`/api/ai/generate`) supports three backends, configured via `slsos-sim/.env`:


| Variable            | Values                                     | Default                        |
| ------------------- | ------------------------------------------ | ------------------------------ |
| `AI_BACKEND`        | `ollama` · `claude` · `openai`             | `ollama`                       |
| `AI_MODEL`          | any model name                             | `llama3.2` / `claude-opus-4-5` |
| `OLLAMA_BASE_URL`   | Ollama server URL                          | `http://localhost:11434`       |
| `ANTHROPIC_API_KEY` | `sk-ant-...`                               | *(required for claude)*        |
| `OPENAI_BASE_URL`   | base URL of any OpenAI-compat server       | `http://localhost:11434/v1`    |
| `OPENAI_API_KEY`    | API key (use any string for local servers) | `local`                        |


**Ollama - fully local (default):**

```bash
curl -fsSL https://ollama.com/install.sh | sh
ollama pull llama3.2
# .env: AI_BACKEND=ollama  (or just leave defaults)
```

**Claude:**

```bash
# .env
AI_BACKEND=claude
ANTHROPIC_API_KEY=sk-ant-...
AI_MODEL=claude-opus-4-5        # or claude-sonnet-4-5, claude-haiku-4-5
```

**OpenAI-compatible (LM Studio / llama.cpp / vLLM):**

```bash
# .env
AI_BACKEND=openai
OPENAI_BASE_URL=http://localhost:1234/v1
AI_MODEL=your-loaded-model
```

> Memory frame data, WAL entries, and kernel state stay on your machine when using Ollama. Only the formatted prompt is sent — and only to `localhost`.

