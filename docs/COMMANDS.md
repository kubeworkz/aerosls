# AeroSLS Command Reference

AeroSLS exposes four command surfaces:

1. **Serial Shell** — interactive shell on COM1, reached via a USB-UART adapter on real hardware or the QEMU `-serial` flag.
2. **REST API** — HTTP/JSON served by the kernel on port **3000**. Under `make x86-run` QEMU forwards host **3001** → guest 3000 (`Makefile:144`), so from your own machine the address is `localhost:3001`.
3. **`aeroslsctl`** — a command-line client over that REST API. See below.
4. **Build Commands** — Makefile targets for compile, test, and hardware bundle generation.

---

## aeroslsctl

`tools/aeroslsctl` — stdlib Python 3, no dependencies, same conventions as `utils/*.py` (`--host localhost:3001`, `--token`, DB_ADMIN by default).

```bash
tools/aeroslsctl cluster status
tools/aeroslsctl nodes
tools/aeroslsctl workloads
tools/aeroslsctl workload declare --name api --partition 1 --restart on-failure
tools/aeroslsctl shell partition migrate 1 2
```

It holds no state and caches nothing — every answer comes from the node named by `--host`. There is no `aeroslsctl` view of the cluster that could drift from the kernel's, because the tool has no view of its own. To ask a different node, point `--host` at it.

### Exit codes

| Code | Meaning |
| ---- | ------- |
| `0`  | the node did it |
| `1`  | the node was unreachable, or returned non-2xx / non-JSON |
| `2`  | the node answered and **refused** |

The 1/2 split is the point: a script can tell "the node is down" from "the node said no." This matters more than it looks, because **the kernel returns HTTP 200 for refusals**, carrying `{"ok":"false"}` — and that `"false"` is a JSON *string*, not a boolean. Both of the obvious client checks are therefore wrong:

```python
if resp.status == 200:   # true even when the call was refused
if data.get("ok"):       # "false" is a non-empty str -> truthy
```

Either one reports success on every rejection. Any other client written against this API needs the same guard (`_refused()` in the CLI; `tests/aeroslsctl_host_test.py` scenario 1 is the regression).

### Coverage

First-class verbs cover the orchestration surface: `cluster`, `nodes`, `services`, `workloads`, `workload declare`, `mesh`, `partitions`, `reconcile`, `health`. The kernel serves ~146 routes in total; the rest are reached through two passthroughs that cannot fall out of sync because they don't wrap anything:

- `aeroslsctl shell <any shell command>` → `POST /api/shell/exec`
- `aeroslsctl raw GET|POST <path> [--body JSON]` → any route

**`raw` takes the method as a positional argument, before the path.** There is no `--method` flag. This is spelled out with worked examples because the line above, read quickly, invites `raw /api/streams` — which fails with an argparse error rather than doing anything useful:

```bash
# right
tools/aeroslsctl --host localhost:3001 raw GET /api/streams
tools/aeroslsctl --host localhost:3001 raw POST /api/stream/create \
  --body '{"name":"report.pdf","mime":"application/pdf"}'

# wrong -- argparse rejects both
tools/aeroslsctl raw /api/streams                       # method missing
tools/aeroslsctl raw /api/streams --method GET          # no such flag
```

**There is no `stream` shell command.** Streams are created, written and listed over REST only — `GET /api/streams`, `POST /api/stream/create`, `POST /api/stream/upload`. Nothing in `user/shell.c`'s dispatch begins with `stream`, so `shell "stream list"` is refused as an unrecognised command.

**Body field names are the parser's, not the obvious ones.** `POST /api/stream/upload` reads `hex` — hex-encoded bytes, up to `UPLOAD_CHUNK_MAX` (16 KiB binary, 32 KiB of hex) per request — plus optional `offset` and `last`. A body using `data` gets `{"ok":"false","error":"name and hex required"}`:

```bash
# 8 KiB of 0xAB -> two 4 KiB pages, so a later migration has something to move
HEX=$(printf 'ab%.0s' $(seq 1 8192))
tools/aeroslsctl --host localhost:3001 raw POST /api/stream/upload \
  --body "{\"name\":\"payload.bin\",\"hex\":\"$HEX\",\"offset\":0,\"last\":1}"
```

`tests/commands_doc_check.sh` verifies this section against `user/shell.c`'s dispatch, `tools/aeroslsctl`'s argparse definitions and the `raw` usage form, so the three mistakes above become a test failure rather than an operator's afternoon.

`workloads scale`, `workloads logs`, `workloads exec` and `nodes drain` are described in `docs/AeroSLS-Control-Plane.md` but **were never built** — there is no replica count, no log ring, no per-workload exec and no drain protocol. The CLI does not stub them; asking for one prints what is missing and what to use instead.

### run-cluster.sh — N nodes

```bash
./run-cluster.sh --nodes 4           # boot a 4-node cluster
./run-cluster.sh --nodes auto        # as many as this host holds
./run-cluster.sh --nodes 3 --dry-run # print the plan and each node's QEMU argv
./run-cluster.sh --stop              # tear it down
```

It reports the host's capacity on every run and names the binding constraint:

```
==> Host capacity
      cores              4
      memory available   15000 MiB   (reserving 2048 for the host)
      free disk          109000 MiB
==> Fits at 1024 MiB / 1 vCPU per node
      by memory          12
      by free disk       10   (10 GiB virtual each; sparse, so pessimistic)
      roster cap         8
      => capacity 8, bound by the roster cap (CLUSTER_NODE_MAX)
      CPU (advisory)     3 busy nodes at once; idle nodes halt and cost
                         almost nothing, so this is not a hard limit
```

**CPU is advisory, not a bound.** An idle node halts — with `-smp 1` there is no AP, and the AP's loop was the one that spun rather than halting. So a mostly-idle cluster is limited by memory. A node under load still wants a core, which is what that figure is for.

An explicit `--nodes` over capacity is **refused, not clamped** (`--force` overrides). Exceeding `CLUSTER_NODE_MAX` is refused separately and cannot be forced — that is a protocol limit, not this host's.

Replaces the retired `run-two-nodes.sh`, which its point-to-point netdev capped at exactly two. Nodes are numbered 1..N and **self-identify at boot** — no `cluster init` step — because each gets its own ISO carrying `node=<i>` on the kernel command line.

| | |
| --- | --- |
| Node cap | Read from `CLUSTER_NODE_MAX` in `net/consensus.h` (8), not hardcoded. A higher id would be refused by `cluster_init()` and the node would boot STANDALONE, looking healthy. |
| Segment | One shared `-netdev socket,mcast=239.192.152.40:12340`. No launch ordering — nodes join independently. |
| Per node | 1 GiB RAM, 1 vCPU, 10 G sparse disk, console on `12340 + i` |
| Artefacts | `cluster/node<i>.{iso,img,log}`, `cluster/cluster.pids` |
| **REST API** | **`http://localhost:3000+i`** — node 4 is `http://localhost:3004` |
| NICs per node | Two: management (slirp NAT + `hostfwd`, carries HTTP/ARP/TCP) and cluster (the multicast segment, carries DSPP only) |

A partial failure is not left half-up: if any node fails to start, every node that *did* come up is torn down and the launcher exits non-zero, naming each failure with QEMU's own stderr.

**The dashboard can follow the cluster.** `run-cluster.sh` prints the exact `AEROSLS_NODES` line to export before `npm run dev` in `slsos-sim`; the node selector in the Cluster panel then points every other panel at whichever node you pick. An id that is not in that list is **refused**, not served by node 1 — a panel showing one machine's numbers under another's name is worse than one that says it cannot reach the node.

**Each node has a URL.** `aeroslsctl --host localhost:3004` drives node 4, and `curl localhost:3004/api/cluster` works. The management NIC sits on its own isolated slirp NAT, which also runs a DHCP server — so `net/dhcp.c` gets a real lease there instead of timing out to the compiled-in `10.0.2.15`.

Note that `make x86-run` forwards host **3001**, which is node 1's REST port. The launcher refuses rather than colliding; set `AEROSLS_HTTP_BASE` to move the range.

Consoles are interactive: attach and you get a shell prompt. Look for `[BOOT] node identity <i> taken from the command line` to confirm each node came up as itself, then drive it with `cluster status`, `partition migrate` and the rest.

---

### Reaching a two-node cluster

**Topology note.** Nodes now share one L2 segment (`-netdev socket,mcast=`) instead of a point-to-point `listen`/`connect` pair, so the cluster is no longer capped at two. QEMU forces multicast loopback on for that mode, so each node also receives its own frames; `net_rx_dispatch()` drops them by source MAC and counts them in `net_self_echo_dropped`. A steady non-zero count is normal and healthy — a flat zero means the node is not actually on the segment with the others.


`aeroslsctl` works against a single node under `make x86-run` (host 3001 → guest 3000). It **cannot** reach either node launched by `run-cluster.sh`, and this is structural rather than an oversight in that script: those nodes use `-netdev socket` so they can exchange raw Ethernet frames for DSPP, and there is no host port forward. Adding a second NIC would not fix it — `net/e1000.c` keeps one global tx/rx ring pair and a single `e1000_pci_slot`, so the driver binds exactly one NIC. Giving a node a host-facing NIC would cost it the DSPP link the script exists to demonstrate.

**Resolved (was: a live blocker).** A networked node now has a working console — `http_server_run()` polls the serial port between sweeps and runs the same shell dispatch, against the same session, that the physical console uses. Attach with `telnet 127.0.0.1 <port>` and you get a prompt. The analysis below explains why this needed building at all.

**Original note.** An earlier revision of this section said to drive the two-node walkthrough from each node's serial console. That is wrong, and the reason is worth stating because it affects any networked boot:

```c
/* kernel.c's final dispatch */
if (e1000_mmio_base) {
    http_server_run();  // does not return — serves REST API on port 3000
}
sls_shell_loop();       // only reached when there is NO NIC
```

**`sls_shell_loop()` is never called on a node that has a NIC** — which is why the console had to be driven from the HTTP loop instead. `run-cluster.sh` gives every node an e1000 — that is the whole point of it — so the console shows boot output and then no prompt, ever. Combined with the single-NIC driver (no host port forward possible) and the absence of a keyboard driver, **those nodes currently have no control path at all**, and the two-node walkthrough has never been executable.

The consoles are still worth attaching to read boot output:

```
telnet 127.0.0.1 12341     # node A   (Ctrl-] then "quit" to detach)
telnet 127.0.0.1 12342     # node B
```

Treat the port as equivalent to a root login should a shell ever appear on it: it binds 127.0.0.1 only, and on a shared or internet-facing box should be tunnelled over SSH rather than firewalled open.

The fix is nodes that self-identify at boot rather than being told to over a console — see `docs/AeroSLS-N-Node-Launcher-Plan-v0.1.md` §0. Single-node work under `make x86-run` is unaffected: there the REST API is reachable on `localhost:3001` and `aeroslsctl` works normally.

---

## Serial Shell

Connect at 38400 baud on COM1. The prompt shows `uid:<id>[tx:<n>]>`  when a transaction is open, otherwise `uid:<id>>` .

**This shell is serial-only.** `read_line()` (`kernel/kernel_io.c`) polls `inb(SERIAL_COM1_BASE)` and there is no PS/2 keyboard driver in the tree, so a QEMU graphics window renders VGA output but cannot accept a keystroke. Under QEMU the console must therefore be something you can *write* to — `-serial mon:stdio`, or a socket chardev as `run-cluster.sh` uses. `-serial file:` is output-only and gives you no way in.

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
| `database drop <name>`                                | Remove a database namespace                          |
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

### Checkpointing (Core Backup Strategies)

All four are exact-match commands — `sh_eq`, not prefix — so they take no arguments and a trailing word makes them unrecognised.

| Command | What it does |
| --- | --- |
| `checkpoint` | Triggers a checkpoint immediately via `checkpoint_trigger()`. Direct call, not IPC. |
| `checkpoint status` | Posts `CKPT_OP_STATUS` to `IPC_PORT_CKPTMGR`. Output arrives on the serial console from the manager task, not as a return value. |
| `checkpoint list` | Posts `CKPT_OP_LIST` — the checkpoints on record. |
| `checkpoint tree` | Posts `CKPT_OP_TREE` — the checkpoint lineage. |

Because the three `status`/`list`/`tree` forms go through IPC, the shell returns before the manager has answered. Over `aeroslsctl shell` the captured output may therefore be empty even on success; read the node's serial log for the result.

### QEMU-SLS guest execution

| Command | What it does |
| --- | --- |
| `qemu run <hex>` | Decodes `<hex>` as a guest binary and runs it through the TCG engine via `sls_launch_guest(bin, len, entry_gpa=0, max_insns=100000)`. |
| `qemu paging` | Runs an end-to-end guest-paging test: the launcher builds four-level guest page tables mapping GVA `0x400000` to GPA `0x5000`, then a guest loads `CR3`, sets `CR0.PG`, and reads through that GVA. Prints PASS only if the magic value arrives, which requires the shadow walker to have resolved through the guest's own tables — the identity mapping would return 0, so a pass and a failure are distinguishable rather than both looking like "the guest halted". |
| `qemu bench [loads]` | Builds an unrolled straight-line guest program of `loads` (default 4096) `MOV EAX,[EBX+disp32]` instructions striding 64 bytes, runs it once, and reports cycles via `rdtsc`. |

`qemu bench` exists to measure the **one** thing the shadow-page-table work changes: the cost of a guest memory load through generated code. Three things about the number:

- The program is **straight-line, not a loop**. Every control-flow instruction in the frontend exits the TB to the C dispatcher, so a loop would measure the interpreter rather than the generated code.
- The cycle count **includes translation**, because the frontend re-translates on every launch. With a large `loads` count that is amortised and roughly equal in both configurations, so the *ratio* between softmmu-on and softmmu-off is dominated by execution; the absolute cycles/load is an upper bound.
- It is a **microbenchmark of the load path**, not a workload. A ratio taken from it describes the load path and should be described that way.

The output states `softmmu=ON|OFF`, read from the same `tcg_use_softmmu` the backend compiled with, so a run cannot be attributed to the wrong configuration.

Three limits worth knowing before using it: the decode buffer is a fixed **4096 bytes**, so anything longer is silently truncated; the hex parser accepts spaces between byte pairs but does **no validation**, so a non-hex character decodes to garbage rather than an error; and execution stops after 100,000 instructions whether or not the guest halted.


### Two rules that are not guessable, and cost real time when missed

**A stream's partition is stamped when the stream is created, and never changes.** `stream_create()` calls `partition_get_for_uid(caller_uid)` once and stores the result. `partition assign` is **not retroactive** — reassigning a uid moves nothing that already exists. So the order is fixed:

```
partition create   ->   partition assign <uid> <id>   ->   create the stream
```

Get that backwards and the stream sits in whatever partition the uid mapped to at the time (partition 0 by default), while the partition you meant to fill stays empty. `partition migrate` then succeeds having moved nothing, and reports `0 stream(s)`. Check with `GET /api/streams` and compare each stream's `partition_id` against the partition you intend to migrate — that field is the authority, not the most recent `partition assign`.

**`partition migrate` needs the owner node, so look before you migrate.** It refuses a destination that already owns the partition, and only the owning node holds data to send. `partition list` and `GET /api/partitions` both report `owner_node` (they did not until this was added — the value used to be discoverable only by attempting a migration and reading the error):

```bash
tools/aeroslsctl --host localhost:3001 partitions        # id, owner_node, name, ...
tools/aeroslsctl --host localhost:3001 shell "partition list"
```

Partition 0 (`system`) can never be migrated — `PARTITION_SYSTEM can never be migrated` is a hard refusal, not a quota.

---

### Cluster / Cross-Node Identity (Multi-Node Partition Scaling Roadmap Phase 7 addendum)

Sets this boot's real node identity for distributed operation — required before `partition migrate` will take the real cross-node wire path instead of the default same-disk relocate path. Not reachable over the REST API today (shell/syscall only) — see `run-cluster.sh` at the repo root for a script that boots real, networked instances to test this for real.


| Command                    | Description                                                                                       |
| ----------------------------- | ------------------------------------------------------------------------------------------------------ |
| `cluster init <node_id>`       | Set this boot's real node identity (0 is reserved/rejected — the "uninitialized" default every deployment starts at) |
| `cluster status`               | Print `node_id`/role/term/active-node-count/roster-size to the serial console; also warns if `node_id` is still 0 |


---

### Cluster View (control-plane surface)

Served by **any** node — every node holds the roster and the replicated service registry, so there is no control-plane node to point at.

| Endpoint | Returns |
| --- | --- |
| `GET /api/cluster` | This node's id, role, term, active-node count, quorum threshold, and the roster. `initialised: false` means `cluster init` has not been run — the node is standalone |
| `GET /api/nodes` | Every known node. `detail: "first-hand"` for this one; `"membership-only"` for peers |
| `POST /api/cluster/init` | `{"node_id":N}` — set this node's identity. DB_ADMIN |
| `POST /api/cluster/peer` | `{"node_id":N}` — register a peer into the roster. DB_ADMIN |

**These FORM a cluster; they do not boot one.** A kernel cannot start another kernel, and the dev server deliberately executes no host processes. Start each node yourself (`run-cluster.sh`), then join them from the Cluster panel or these endpoints.

**`cluster/init` is a reset, not a merge.** It clears this node's term, role and roster (`consensus.h`), so calling it on a node already in a working cluster drops it out of that cluster. `cluster/peer` reports its outcome in `detail` — "added", "already a member (no-op)", "re-activated", "invalid node id", "roster full" — because re-registering an existing peer is a successful no-op, not a failure.

**What a peer's entry does and does not contain.** Partition ownership and announced services *are* known cluster-wide — `partition_owner_table[]` is the authority for where a partition lives, and the service registry replicates. A peer's workloads, live contexts, memory and breakers are **not** replicated and are returned absent rather than as zeroes; select that node to read them from the node itself.

The frontend routes per-node calls as `/node/<id>/api/...`. Because the kernel deals in node IDs and not addresses (DSPP is L2 broadcast and needs no address book), the UI is told where each node listens via `AEROSLS_NODES="1=http://host:3001,2=http://host2:3001"`. Unset means the previous single-kernel behaviour.

---

### Service Registry (Orchestration Plan Phase 4)

Name → partition/node/endpoint resolution. The **node is not stored** — it is derived from the partition's current owner on every lookup, so `service resolve` after a `partition migrate` reports the new node with nothing having been updated. Registration and removal require `DB_ADMIN` or higher; resolution is not role-gated. Registrations are dropped automatically when their partition is destroyed.

| Command | Description |
| --- | --- |
| `service register <name> <partition_id> <ipc\|tcp> <port>` | Register (or update in place) a service in a partition. Refuses an undefined partition, port 0, an empty or over-long name |
| `service unregister <name>` | Remove a registration |
| `service resolve <name>` | Print partition, **current** node, endpoint kind/port, and whether it is local |
| `service list` | Print every registration, each with its partition's current owner node |

REST equivalents: `GET /api/services`, `GET /api/service/resolve/<name>`, `POST /api/service` (`{"name","partition_id","endpoint_kind":"tcp"|"ipc","endpoint_port"}`, DB_ADMIN-gated).

---

### Declarative Workloads & Reconciler (Orchestration Plan Phase 5)

Declare desired state; the kernel converges on it. **The reconciler is OFF at boot** — including after a reboot that restores declarations — and must be switched on deliberately. It reconciles exactly four conditions and logs every action.

A partition runs if **any** active workload in it wants to run (union semantics), so two workloads sharing a partition with opposite desired states do not fight. Each workload's own declared service is reconciled independently.

| Command | Description |
| --- | --- |
| `workload declare <name> <partition_id> <running\|stopped> [svc <name> <ipc\|tcp> <port>]` | Declare, or update in place. The `svc …` clause is optional; a workload may declare no service |
| `workload delete <name>` | Remove a declaration |
| `workload list` | Declarations, per-entry action counts and converged flags, plus reconciler state and intent-queue depth/drops |
| `reconcile on` / `reconcile off` | Enable or disable autonomous convergence |
| `workload retry <name>` | Re-arm a workload the reconciler gave up on restarting |
| `context list` | Live execution contexts: pc, retired steps, status |
| `context step <budget>` | Advance every live context by up to `<budget>` instructions |

Add `restart never|on-failure|always` to a declaration to enable **self-healing**. A context that TRAPs is a failure; a context that HALTs *finished*, so `on-failure` leaves it alone and only `always` restarts it — restarting a completed batch job would loop it forever. Default is `never`, so existing declarations are unaffected.

Attempts back off exponentially (~1 s doubling to ~30 s) and stop after 10, after which the workload is marked `[GAVE UP]` and announced once. `workload retry <name>` or re-declaring re-arms it; neither erases the lifetime restart count. A workload that runs stably for long enough gets its budget back.

Add `prog <object> [entry]` to a declaration to make the workload a **running computation**: `workload declare job 3 running prog loopsum main`. The reconciler then instantiates the uploaded SIMI program as a live context and registers it for migration — which is what makes `partition migrate` move real work rather than only data.

REST equivalents: `GET /api/workloads`, `POST /api/workload`, `POST /api/reconcile` (`{"enabled":"true"}`), all DB_ADMIN-gated except the GET.

**Service discovery is cluster-wide.** Registering announces the name over DSPP; other nodes cache it and can resolve it. A local registration always outranks a cached remote one, and cached entries are never persisted (a stale cache restored from disk would resurrect services that moved while this node was down).

**Cached entries expire.** Each node re-announces what it owns every ~5 s. A remote entry reports `fresh` (< 2 heartbeats), `stale` (overdue but **still resolving** — a warning) or `expired` (≥ ~20 s, stops resolving). Local registrations never age.

**Endpoints are probed, and that is reported separately.** `health` says whether the *information* is current; `serving` says whether the *endpoint* is accepting — `up`, `down`, or `unknown`. They are independent: `stale + up` means "the last thing we heard was good, but we haven't heard lately"; `fresh + down` means "we know, and it's broken". Each node probes only its own endpoints (a TCP port must have a LISTEN socket; an IPC port is judged by the microkernel watchdog, where DEGRADED counts as down) and ships the verdict on its heartbeat. A `down` service still resolves — the registry reports rather than hides.

`service resolve` shows all of it: `'api' -> partition 3, node 2, tcp port 8080 (remote, fresh, endpoint up)`.

A wedged handler — process alive, port bound, watchdog happy, but nothing draining its queue — reports `up` to the probe but **trips its circuit breaker** on IPC queue saturation (see below).

---

### Circuit Breakers (Orchestration Plan Phase 6)

Each service carries a three-state breaker: `closed` (calls flow), `open` (calls refused), `half-open` (exactly one trial call admitted). It opens after 5 consecutive failures and admits a trial after ~10 s; a trial that succeeds closes it, one that fails re-opens it immediately.

Failures come from three places, and two need no cooperation: the endpoint probe (process died), IPC queue saturation (handler wedged), and explicit reports from callers for anything the kernel cannot see. `service resolve` shows the breaker state alongside health and serving.

| Command | Description |
| --- | --- |
| `mesh list` | Every breaker: state, consecutive/total failures, successes, trips, calls permitted and refused |
| `mesh reset <name>` | Force a breaker closed without waiting out the cooldown. Clears the state, **not** the recorded history |

REST: `GET /api/mesh`.

Breakers are per-node: a remote entry carries the owning node's endpoint verdict but not its breaker, because a service healthy there may still be unreachable from here.

**Operational note:** the reconciler sweeps on the AP core and queues anything that writes to disk for the BSP, which drains it in the HTTP server loop and on every shell command. On a boot with **no NIC** the HTTP loop never runs, so queued work applies only when an operator types a command — convergence is not autonomous in that configuration.

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

**Correction — everything here IS reachable over HTTP.** An earlier revision of this file said `partition migrate` was "shell/syscall only". That was wrong, and had been for as long as `POST /api/shell/exec` has existed: that route runs the *full* `sls_shell_execute()` dispatch (`user/shell.c:443`), and wraps it in `kernel_serial_capture_start()`, so a command's serial output is captured into the JSON response rather than lost to the console. Anything you can type at the serial prompt you can also POST:

```
aeroslsctl shell partition migrate 1 2
curl -X POST localhost:3001/api/shell/exec -H "Authorization: Bearer $TOK" \
     -d '{"command":"partition migrate 1 2"}'
```

`cluster init` additionally has its own typed route, `POST /api/cluster/init`. See `run-cluster.sh` at the repo root for booting the nodes themselves — that part is still not something the kernel can do for you.

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
| `./run-cluster.sh --nodes N` | Build one ISO per node, then boot **N** real QEMU instances on a shared multicast L2 segment — for testing real cross-node data movement (Multi-Node Partition Scaling Roadmap Phase 7). Each node self-identifies from `node=<i>` on its kernel command line, so no `cluster init` is needed. `--nodes auto` sizes to the host; `--stop` tears it down. Replaces the retired `run-two-nodes.sh`. |
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

