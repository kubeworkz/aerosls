# M5.1 mediated POSIX write path — evidence (2026-11-17)

Scope: `CAT_EXEC` (verb 6), bound-owner and `PERM_WRITE` gates, explicit
`sql_tx_begin` / `sql_execute_tx` / `sql_tx_commit` handling. M5.2 remains the
named Linux-syscall follow-up and is not implemented here.

All commands ran in the `m1-linux-network` worktree. Logs in this directory are
the captured output of the runs below.

| Check | Command | Result |
|---|---|---|
| Host build | `gcc -no-pie -Wall -Wextra -std=c11 -I . -I kernel -I net -I arch/x86 -include tests/kernel_io_panic_port.h -o /tmp/env_catalog_host_test tests/env_catalog_host_test.c kernel/cap.c kernel/env_catalog.c kernel/kernel_io.c kernel/console.c kernel/linux_abi.c kernel/elf64_load.c kernel/entropy.c kernel/sha256.c kernel/sql_parser.c kernel/predicate.c` | exit 0 |
| Host CAT_EXEC cases | `/tmp/env_catalog_host_test` | exit 0 — **240 passed, 0 failed** (`host_test.log`) |
| Source guard | `bash tests/env_catalog_broker_check.sh` | exit 0 — **all clauses held** (`broker_guard.log`) |
| Guard teeth | `bash tests/env_catalog_broker_check_smoke.sh` | exit 0 — **58 teeth held, 0 lost** (`broker_smoke.log`) |
| Sidecar stamps | `bash tests/sidecar_stamp_check.sh` | exit 0 — clauses A–N held (`sidecar_stamp.log`) |
| Proto tests | `cargo test -p aerosls-proto --lib` | exit 0 — **58 passed** (`proto_tests.log`) |
| Sidecar tests | `cargo test -p aerosls-sidecar --lib` | exit 0 — **93 passed** (`sidecar_tests.log`) |
| Live surface | `AEROSLS_TOKEN=<test-bearer> bash tests/env_catalog_live_check.sh` | exit 0 — full §9.1 surface plus M5.1 (`live_check.log`) |

The sidecar archive in this commit was rebuilt in a clean HEAD copy with only
the four M5 `user/` files applied, so its source digest covers the committed
tree rather than the unrelated local M4/E7 edits. `sidecar_stamp.log` is from
that clean tree. The live arm ran against the existing working-tree cluster
(which also carried those unrelated edits); the M5.1 kernel, client and applet
code exercised is the same.

The live run used the existing three-node cluster on ports 3001–3003. Its
`cluster/cluster.pids` had been removed by an earlier stop attempt while the
nodes remained healthy; the test only reads node IDs from that file, so the run
used a temporary ID-only file (`AEROSLS_CLUSTER_DIR=/tmp/m51-live-cluster`)
against the live endpoints. No node was restarted or rebuilt for the run.

## What the live M5.1 arm proves

- The owner's `catalog exec INSERT` returned `1 row(s) affected`, and native SQL
  observed the committed row. A `CAT_OK` written before commit would leave this
  read empty.
- `catalog exec UPDATE` and `catalog exec DELETE` committed in turn; native SQL
  observed the updated value and then its absence.
- Dave's table was pinned to `perm_mask=0x4` (read only). Bob's `catalog exec
  DELETE` was then refused by name with `PERM_WRITE`, no affected-row body was
  sent, and native SQL still saw Dave's original row.
- `catalog exec DROP TABLE` was refused by name as DDL, not executed.
- A write naming the existing foreign-partition object answered as absence
  (`NOENT`), with no row created and no PERM that confirmed the object.

M5.2 (a Linux-personality task reaching `CAT_EXEC` through a project-private
syscall, working name `SYS_SLS_CATALOG_EXEC`) is not implemented or evidenced
here.
