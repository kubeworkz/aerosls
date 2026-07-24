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
| `seal <name> <password>`       | Derive+store a password-based key for an object (does NOT encrypt its data — see kernel/secure_api.c). Was an interactive two-line prompt; changed to a single line by the Kernel-Side Shell Refactor (docs/AeroSLS-Web-Terminal-Plan-v0.1.md §10.1) so the command works over both the serial console and the new `/api/shell/exec` HTTP route. |


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
| `proc hold <pid>`     | Hold a runnable process (Navigator-Parity Phase 4) — fails if not found, already running, already held, or a zombie |
| `proc release <pid>`  | Release a held process back to runnable                 |
| `proc priority <pid> <high\|low\|normal>` | Set a process's scheduling priority tier (unrecognized value → `normal`) |


---

### Web Assets

Dynamic web asset store (served alongside the compiled-in Navigator bundle).


| Command                          | Description                                      |
| -------------------------------- | ------------------------------------------------ |
| `webapp set <obj> <path> <html>` | Store an asset at a URL path (replaces existing) |
| `webapp append <obj> <path> <s>` | Append content to an existing asset              |
| `webapp list [<obj>]`            | List all assets — use `*` to list all objects    |


---

### SIMI Introspection


| Command             | Description                                                                             |
| -------------------- | ---------------------------------------------------------------------------------------- |
| `simi info <object>` | Dump a SIMI-format object's header: instruction/literal/entry/name counts, symbol table, activation-cache status |


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
| `agent schedule <name> <ticks> <message...>` | Run this agent automatically every `<ticks>` scheduler ticks with a fixed message |
| `agent unschedule <name>`                    | Cancel a scheduled run                                  |


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

### Databases (Database Namespace & Access Roadmap)

A `database_id` groups tables under a named, grantable namespace — distinct from a `partition_id` (resource/execution isolation, see Partitions below). `perm` strings below follow the object-permission convention (`r`, `w`, `x`, or combinations like `rw`).


| Command                                              | Description                                         |
| ----------------------------------------------------- | ---------------------------------------------------- |
| `database create <name>`                              | Create a database namespace                         |
| `database list`                                       | List all databases                                   |
| `database grant uid <db_name> <uid> <perm>`           | Grant a uid direct access                            |
| `database grant group <db_name> <group_name> <perm>`  | Grant a group access                                 |
| `database revoke uid <db_name> <uid>`                 | Revoke a uid's grant                                 |
| `database revoke group <db_name> <group_name>`        | Revoke a group's grant                               |
| `database check <db_name> <uid> <perm>`               | Test whether uid has `perm` on the database (via any grant path) — prints GRANTED or the reason it isn't |


---

### Group Profiles (Navigator-Parity Gap Roadmap Phase 3)

Named uid groups with an attached default role, usable as a grantee alongside individual uids everywhere a grant exists (databases, authorization lists).


| Command                          | Description                                                          |
| ---------------------------------- | ----------------------------------------------------------------------- |
| `group create <name> <role>`       | Create a group with a default role — `role` is one of `SYSTEM_KERNEL`/`DB_ADMIN`/`APP_USER`/`GUEST` (unrecognized → `GUEST`) |
| `group add <name> <uid>`           | Add a uid as a member                                                    |
| `group list`                       | List all groups with role and member uids                               |


---

### Authorization Lists (Navigator-Parity Gap Roadmap Phase 3)

A reusable, named bundle of permission grants over one or more objects — distinct from the bearer-token **Auth** system below (that one issues session tokens; this one is IBM-i-style object authorization lists). Grantees can be individual uids or whole groups.


| Command                                                | Description                                              |
| --------------------------------------------------------- | ------------------------------------------------------------ |
| `authlist create <name>`                                   | Create an authorization list                                 |
| `authlist grant obj <list_name> <object_name> <perm>`      | Attach an object + permission mask to the list                |
| `authlist grant uid <list_name> <uid>`                     | Add a uid as a grantee of the list                            |
| `authlist grant group <list_name> <group_name>`            | Add a group as a grantee of the list                          |
| `authlist check <uid> <object_name> <perm>`                | Test whether uid has `perm` on the object via any list that grants both — prints GRANTED or "no matching list" |
| `authlist list`                                            | List all authorization lists                                  |


---

### Tenants (Multitenant Isolation Gap Analysis §5 item 1)

The single operation that atomically provisions both halves of a tenant: a partition (see below) and a database, linked together and never re-derived from either half independently.


| Command                | Description                                                                                          |
| ------------------------- | -------------------------------------------------------------------------------------------------------- |
| `tenant create <name>`     | Create a tenant — provisions a partition + a database, links them, and assigns the caller into the new partition (skipped for uid 0/kernel) |
| `tenant list`              | List all tenants with their linked `partition_id`/`database_id`/`owner_uid`                               |


---

### Partitions / LPAR (Phases 8/13/14, Gap Remediation Phase F, Multitenant Isolation Gap Analysis, Multi-Node Partition Scaling Roadmap)

Real resource and execution isolation within one kernel — a `partition_id` tag checked at catalog access, process spawn, IPC ports, scheduling, and frame quotas. `partition create`/`tenant create` over the REST API require `DB_ADMIN` or higher (see REST API section below); the shell has no such gate today, since the serial console is inherently a trusted, single-operator surface.


| Command                                                  | Description                                                                                                          |
| ----------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------ |
| `partition create <name>`                                    | Define a new partition                                                                                                    |
| `partition list`                                             | List all defined partitions                                                                                              |
| `partition assign <uid> <partition_id>`                      | Assign a uid to a partition                                                                                              |
| `partition destroy <partition_id>`                           | Tear down a partition (kills its processes, vfrees its objects)                                                          |
| `partition pause <partition_id>`                             | Stop scheduling this partition                                                                                           |
| `partition resume <partition_id>`                            | Resume scheduling this partition                                                                                         |
| `partition quota <partition_id> <frames>`                    | Set a physical RAM frame quota (0 = unlimited)                                                                            |
| `partition quotas`                                           | List per-partition frame usage/quota                                                                                     |
| `partition cpuweight set <id> <weight>`                      | Set CPU scheduling weight — a partition at weight 3 gets 3 consecutive scheduler turns per round vs. a sibling at weight 1 (0 = default 1) |
| `partition cpuweights`                                       | List per-partition CPU weights                                                                                           |
| `partition storagequota set <id> <pages>`                    | Set an on-disk page quota, rowstore+vecstore combined (0 = unlimited) — sits beneath a second, hard, physical per-partition disk sub-range that a quota set above simply never reaches |
| `partition storagequotas`                                    | List per-partition on-disk page usage/quota                                                                              |
| `partition connquota set <id> <quota>`                       | Set a max **concurrent** inbound connection count for a partition (0 = unlimited) — distinct from any request-rate limit; a slow client holding a connection open still counts |
| `partition connquotas`                                       | List per-partition connection usage/quota                                                                                |
| `partition migrate <partition_id> <dest_node_id>`             | Cold-migrate a partition's ownership (and, for stream/blob data, the actual bytes) to another cluster node — pauses, hands off the lease, moves stream data over the real DSPP wire protocol if `cluster init` has been run on this boot (same-disk relocate otherwise), reclaims frames, leaves the partition **paused** on success |


---

### Cluster / Cross-Node Identity (Multi-Node Partition Scaling Roadmap Phase 7 addendum)

Sets this boot's real node identity for distributed operation — required before `partition migrate` will take the real cross-node wire path instead of the default same-disk relocate path. Not reachable over the REST API today (shell/syscall only) — see `run-two-nodes.sh` at the repo root for a script that boots two real, networked instances to test this for real.


| Command                    | Description                                                                                       |
| ----------------------------- | ------------------------------------------------------------------------------------------------------ |
| `cluster init <node_id>`       | Set this boot's real node identity (0 is reserved/rejected — the "uninitialized" default every deployment starts at) |
| `cluster status`               | Print `node_id`/role/term/active-node-count/roster-size to the serial console; also warns if `node_id` is still 0 |


---

### Network & Disk Status (Navigator-Parity Gap Roadmap Phase 5c, Storage Isolation Roadmap)


| Command         | Description                                                                                    |
| ------------------ | -------------------------------------------------------------------------------------------------- |
| `net status`         | Print IP/gateway/subnet/MAC/DHCP-bound state and TCP connection pool occupancy (by connection state) |
| `disk status`        | Print system-wide storage tier byte totals plus per-partition on-disk byte usage/quota (Storage Isolation Phase 2) |


---

### Usage Metering & Audit (Multitenant Isolation Gap Analysis §5 items 6/8)


| Command        | Description                                                                     |
| ----------------- | ---------------------------------------------------------------------------------- |
| `usage report`      | Print per-partition cumulative HTTP request count, frame-tick total, and live frame gauge |
| `audit list`        | Print the security audit log (uid, action, detail, granted/denied, tick)             |


---

### Vector Store (VectorStore Roadmap/Interface/Gap-Analysis)

HNSW-indexed and brute-force nearest-neighbor search over named vector collections, with optional Ollama embed-on-insert/embed-on-search. Some capabilities (`embed-search`, index rebuild, delete-by-VecId) are REST-only — no shell equivalent exists for those; see the REST API section below.


| Command                                                          | Description                                                                                   |
| --------------------------------------------------------------------- | --------------------------------------------------------------------------------------------------- |
| `vec create <collection> <dimension>`                                   | Create a vector collection (the backing catalog object must already exist via `valloc`)              |
| `vec collection unique <collection> <on\|off>`                          | Toggle `external_id` dedup-on-insert for a collection                                                |
| `vec insert <collection> <external_id> <v0> <v1> ...>`                  | Insert a raw vector                                                                                   |
| `vec embed-insert <collection> <external_id> <model> <prompt...>`       | Embed `<prompt>` via Ollama then insert — always targets `10.0.2.2:11434` (the QEMU SLIRP gateway) from the shell |
| `vec search <collection> <l2\|cosine> <k> <v0> <v1> ...>`               | Brute-force top-k nearest neighbor search                                                             |
| `vec list`                                                              | List all vector collections                                                                           |
| `vec index list`                                                        | List all HNSW indexes                                                                                 |
| `vec index create <index_name> <collection> <l2\|cosine>`               | Build an HNSW index over a collection                                                                 |
| `vec index search <index_name> <k> <ef> <v0> <v1> ...>`                 | HNSW approximate nearest-neighbor search                                                              |
| `vec join <collection> <table> <id_column> <l2\|cosine> <k> <v0> ...>`  | Convenience: search then join matches against a row-set table in one command                          |
| `vec schema export`                                                     | Print COLLECTION/INDEX definitions as text                                                            |
| `vec schema import <text>`                                              | Import COLLECTION/INDEX definitions (one line, no `;` separator support)                              |
| `vec data export <collection> [skip]`                                   | Print `VECTOR <collection> <external_id> <v0> ...` lines, resumable via `skip`                         |
| `vec data import <text>`                                                | Import `VECTOR` lines                                                                                 |
| `object set database <object_name> <database_name\|none>`               | Retag any catalog object's database namespace (generic — not vector-specific, but lives in this command block) |


---

### SQL Engine / Row-Set Tables

Autocommit SQL over row-set tables backed by `rowstore.c`.


| Command                       | Description                                                                                       |
| -------------------------------- | ------------------------------------------------------------------------------------------------------ |
| `sql <statement>`                  | Run one autocommit SQL statement; `SELECT` results are fetched via cursor and printed                    |
| `table create <name>`              | Promote a `valloc`'d + `schema set`'d object into a real row-set table — does not `valloc` or set schema for you |
| `schema export`                    | Print `CREATE TABLE`/`CREATE INDEX` SQL text for every table the caller can read                         |
| `schema import <sql>`              | Import one or more `;`-separated statements on one line — continues past individual failures             |


---

### Message Queues (Navigator-Parity Gap Roadmap Phase 4)


| Command                     | Description                                          |
| ------------------------------ | ------------------------------------------------------- |
| `mq create <name>`               | Create a message queue                                    |
| `mq send <name> <text...>`       | Send a text message (records the sender's uid)             |
| `mq receive <name>`              | Pop and print the oldest message, if any                    |
| `mq list`                        | List all message queues                                     |


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


#### Shell

Note: for a while this REST API section predated several routes that only ever got documented elsewhere (`/api/sql`, `/api/tables`, `/api/vec/*`, `/api/partitions`, ...) or not at all — a full route-by-route audit against `net/http.c` closed that gap; every real route as of this pass now has a section below.

| Method | Path              | Auth        | Description                                                                                                                                                                                                                          |
| ------ | ----------------- | ----------- | -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `POST` | `/api/shell/exec` | `APP_USER+` | `{"command":"…"}` — runs the one command string through the *entire* serial-console dispatch (`sls_shell_execute()`, see docs/AeroSLS-Web-Terminal-Plan-v0.1.md §10). Returns `{"ok":bool,"output":"…"}`; `ok` means the command was recognized, not that the underlying operation succeeded — read `output` the same way you'd read the serial console. Shares session state (`current_session_uid`/`gid`/`current_tx_id`) with the physical serial console — one simulated machine, one live session. |


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
| `POST`   | `/api/agent/schedule`   | `APP_USER+` | Run an agent automatically every N ticks — `{"name":"…","ticks":N,"message":"…"}`                                                                                              |
| `POST`   | `/api/agent/unschedule` | `APP_USER+` | Cancel a scheduled run — `{"name":"…"}`                                                                                                                                        |

**Available tool names for `tools` field:** `db_select` · `db_insert` · `db_query` · `stream_read` · `stream_write` · `tier_promote`

**Demo admin token (fixed at boot):** `deadbeef01234567cafebabe76543210` (uid=1000, role=`DB_ADMIN`)


#### Partitions / LPAR

`POST /api/partitions` and `POST /api/tenants` require `DB_ADMIN` or higher (`{"ok":"false","error":"requires DB_ADMIN or higher"}` otherwise) — every other partition write route below only requires `APP_USER+`, unchanged.

| Method   | Path                              | Auth         | Description                                                                                                     |
| -------- | ---------------------------------- | -------------- | -------------------------------------------------------------------------------------------------------------- |
| `GET`    | `/api/partitions`                    | `APP_USER+`      | List partitions — `{partitions:[{id, name, frame_usage, frame_quota, quota_unlimited}]}`                          |
| `POST`   | `/api/partitions`                    | **`DB_ADMIN+`**  | Create a partition — `{"name":"…"}` → `{ok, partition_id}`                                                        |
| `POST`   | `/api/partition/assign`              | `APP_USER+`      | `{"uid":N,"partition_id":N}`                                                                                       |
| `POST`   | `/api/partition/destroy`             | `APP_USER+`      | `{"partition_id":N}`                                                                                               |
| `POST`   | `/api/partition/pause`               | `APP_USER+`      | `{"partition_id":N}`                                                                                               |
| `POST`   | `/api/partition/resume`              | `APP_USER+`      | `{"partition_id":N}`                                                                                               |
| `GET`    | `/api/partition/quotas`              | `APP_USER+`      | List frame quotas — `{quotas:[{partition_id, usage, quota, unlimited}]}`                                          |
| `POST`   | `/api/partition/quota`               | `APP_USER+`      | `{"partition_id":N,"frame_quota":N}` (0 = unlimited)                                                              |
| `GET`    | `/api/partition/cpuweights`          | `APP_USER+`      | List CPU weights — `{cpuweights:[{partition_id, weight}]}` (skips default weight 1)                              |
| `POST`   | `/api/partition/cpuweight`           | `APP_USER+`      | `{"partition_id":N,"weight":N}`                                                                                    |
| `GET`    | `/api/partition/storagequotas`       | `APP_USER+`      | List on-disk page quotas — `{storagequotas:[{partition_id, page_usage, page_quota}]}`                             |
| `POST`   | `/api/partition/storagequota`        | `APP_USER+`      | `{"partition_id":N,"page_quota":N}`                                                                                |
| `GET`    | `/api/partition/connquotas`          | `APP_USER+`      | List concurrent-connection quotas — `{connquotas:[{partition_id, conn_usage, conn_quota}]}`                       |
| `POST`   | `/api/partition/connquota`           | `APP_USER+`      | `{"partition_id":N,"quota":N}`                                                                                     |

**Not reachable over HTTP:** `partition migrate` (shell/syscall only — see the Serial Shell section above) and `cluster init`/`cluster status` (same, and see `run-two-nodes.sh` at the repo root for real two-node testing).

#### Tenants

| Method | Path            | Auth              | Description                                                       |
| ------ | ---------------- | ------------------- | -------------------------------------------------------------------- |
| `GET`  | `/api/tenants`     | `APP_USER+`           | List tenants — `{tenants:[{id, name, partition_id, database_id, owner_uid}]}` |
| `POST` | `/api/tenants`     | **`DB_ADMIN+`**       | Create a tenant — `{"name":"…"}` → `{ok, tenant_id}` (calls `partition_create()` internally, hence the same gate as partition creation) |

#### Security / RBAC (read-only over HTTP)

Databases, group profiles, and authorization lists are all **mutated via shell/syscall only today** — these three routes are read-only views for the dashboard. There is no `POST`/`DELETE` HTTP surface for `database create/grant/revoke`, `group create/add`, or `authlist create/grant` (confirmed absent from `net/http.c`); use the Serial Shell commands above, or `POST /api/shell/exec` to run one through the same dispatch remotely.

| Method | Path                       | Auth | Description                                                                       |
| ------ | ---------------------------- | ------ | -------------------------------------------------------------------------------------- |
| `GET`  | `/api/security/databases`      | None     | Databases with grant summaries                                                          |
| `GET`  | `/api/security/groups`         | None     | Groups with role and member uids                                                         |
| `GET`  | `/api/security/authlists`      | None     | Authorization lists with grantee summaries                                               |
| `GET`  | `/api/security/audit`          | None     | Security audit log — `{count, capacity, entries:[{id, tick, uid, action, detail, granted}]}` |

#### Usage Metering / Network / Disk Status

| Method | Path                    | Auth | Description                                                                                                                    |
| ------ | ------------------------- | ------ | -------------------------------------------------------------------------------------------------------------------------------- |
| `GET`  | `/api/usage`                | `APP_USER+` | Per-partition cumulative usage — `{partitions:[{partition_id, name, http_requests_total, frame_ticks_total, frames_now}]}`         |
| `GET`  | `/api/network/status`       | `APP_USER+` | IP/gateway/subnet/MAC/DHCP state + TCP pool occupancy — `{ip, gateway, subnet_mask, mac, dhcp_bound, tcp_pool:{active, capacity, by_state}}` |
| `GET`  | `/api/disk`                 | `APP_USER+` | Storage tier totals + per-partition byte usage/quota — `{capacity_bytes, tiers:{l1_cache, l2_dram, l3_ssd}, partitions:[{partition_id, disk_bytes_used, disk_bytes_quota}]}` |

Every `/api/*` route except `/api/health` also runs through `http_partition_rate_check()` (429 if the caller's partition has exceeded its request-rate window) and records against the usage counters above — this applies to literally every route documented in this file, not just the ones in this section.

#### Vector Store

| Method   | Path                                   | Auth        | Description                                                                                                                     |
| -------- | ----------------------------------------- | ------------- | -------------------------------------------------------------------------------------------------------------------------------- |
| `GET`    | `/api/vec/collections`                       | None            | List collections — `{collections:[{name, dimension, entry_count, page_count}]}`                                                    |
| `POST`   | `/api/vec/collections`                       | `APP_USER+`     | Create — `{"name":"…","dimension":N}` (backing catalog object must already exist via `POST /api/valloc`)                            |
| `DELETE` | `/api/vec/collections`                       | `APP_USER+`     | Drop — `{"name":"…"}`                                                                                                                |
| `POST`   | `/api/vec/collections/unique`                | `APP_USER+`     | Toggle `external_id` dedup — `{"name":"…","enabled":0\|1}` (integer, not a JSON bool)                                                |
| `GET`    | `/api/vec/indexes`                           | None            | List HNSW indexes — `{indexes:[{name, collection, metric, active_count, node_count}]}`                                             |
| `POST`   | `/api/vec/indexes`                           | `APP_USER+`     | Build an index — `{"name":"…","collection":"…","metric":"cosine"\|"l2"}` (default `cosine`)                                        |
| `DELETE` | `/api/vec/indexes`                           | `APP_USER+`     | Drop an index — `{"name":"…"}`                                                                                                      |
| `POST`   | `/api/vec/insert`                            | `APP_USER+`     | `{"collection":"…","external_id":N,"values":[f0,f1,...]}` → `{ok, status, page_id, slot_index}`                                    |
| `POST`   | `/api/vec/embed-insert`                      | `APP_USER+`     | Embed via Ollama then insert — `{"collection","external_id","endpoint_ip","port","model","prompt"}` (`endpoint_ip`/`port` default `10.0.2.2:11434`, `model` defaults `nomic-embed-text`) |
| `POST`   | `/api/vec/search`                            | `APP_USER+`     | Brute-force top-k — `{"collection","query":[...],"metric":"cosine"\|"l2","k":N}` (`k` defaults 10)                                  |
| `POST`   | `/api/vec/index/search`                      | `APP_USER+`     | HNSW top-k — `{"index","query":[...],"k":N,"ef":N}` (`ef` defaults to `k`)                                                          |
| `POST`   | `/api/vec/embed-search`                      | `APP_USER+`     | Embed then brute-force search, no shell equivalent — same fields as `embed-insert` plus `metric`/`k`                                |
| `POST`   | `/api/vec/index/embed-search`                | `APP_USER+`     | Embed then HNSW search, no shell equivalent — same as above plus `ef`, no `metric` (fixed at index creation)                        |
| `POST`   | `/api/vec/index/rebuild`                     | `APP_USER+`     | Clear + repopulate an HNSW index from current collection state, no shell equivalent — `{"index":"…"}`                              |
| `POST`   | `/api/vec/join`                              | `APP_USER+`     | Join prior search matches against a row-set table — `{"table","id_column","matches":[{external_id,page_id,slot_index,distance},...]}` |
| `DELETE` | `/api/vec/vector`                            | `APP_USER+`     | Delete by physical VecId (not `external_id`) — `{"collection","page_id":N,"slot_index":N}`                                          |
| `GET`    | `/api/vec/schema/export`                     | None            | COLLECTION/INDEX definitions as text                                                                                                |
| `POST`   | `/api/vec/schema/import`                     | `APP_USER+`     | `{"text":"<newline-separated lines>"}` → `{total, succeeded, failed, lines:[...]}`                                                  |
| `GET`    | `/api/vec/data/export/<collection>[/skip/<N>]` | None          | Vector data dump, resumable via the path-segment `skip`                                                                             |
| `POST`   | `/api/vec/data/import`                       | `APP_USER+`     | `{"text":"<newline-separated VECTOR lines>"}`                                                                                       |

#### SQL Engine / Row-Set Tables

| Method | Path                     | Auth        | Description                                                                                          |
| ------ | -------------------------- | ------------- | --------------------------------------------------------------------------------------------------------- |
| `POST` | `/api/sql`                    | `APP_USER+`     | `{"query":"…"}` — on error `{ok:false, error_code, error}`; `SELECT` → `{ok, row_count, truncated, columns, rows}` (NULLs round-trip as JSON `null`); DML → `{ok, affected_rows}` |
| `GET`  | `/api/tables`                 | None            | List all row-set tables                                                                                    |
| `GET`  | `/api/tables/<name>/schema`   | None            | Single table's schema detail                                                                                |
| `POST` | `/api/tables`                 | `APP_USER+`     | `{"name":"…"}` — same "promote an existing valloc'd + schema'd object" semantics as `table create`, does not `valloc`/`schema set` for you |
| `GET`  | `/api/schema/export`          | None            | `CREATE TABLE`/`CREATE INDEX` SQL text dump                                                                 |
| `POST` | `/api/schema/import`          | `APP_USER+`     | `{"sql":"<';'-separated statements>"}` → `{total, succeeded, failed, statements:[...]}`                     |


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
| `./run-two-nodes.sh` | Build the ISO once, then boot **two** real QEMU instances with their e1000 NICs socket-connected directly to each other (no bridge/tap needed) — for testing real cross-node data movement (Multi-Node Partition Scaling Roadmap Phase 7), see `cluster init`/`cluster status` above. Prints the exact commands to run in each instance's console. |
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

