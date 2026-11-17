# M5.1 mediated POSIX write path — evidence (2026-11-17)

Scope: `CAT_EXEC` (verb 6), bound-owner and `PERM_WRITE` gates, explicit
`sql_tx_begin` / `sql_execute_tx` / `sql_tx_commit` handling. M5.2 remains the
named Linux-syscall follow-up and is not implemented here.

All commands ran in the `m1-linux-network` worktree unless the table names the
clean `/tmp/m51-clean2` tree. Logs in this directory are the captured output of
the runs below.

| Check | Command | Result |
|---|---|---|
| Host build | `gcc -no-pie -Wall -Wextra -std=c11 -I . -I kernel -I net -I arch/x86 -include tests/kernel_io_panic_port.h -o /tmp/env_catalog_host_test tests/env_catalog_host_test.c kernel/cap.c kernel/env_catalog.c kernel/kernel_io.c kernel/console.c kernel/linux_abi.c kernel/elf64_load.c kernel/entropy.c kernel/sha256.c kernel/sql_parser.c kernel/predicate.c` | exit 0 |
| Host CAT_EXEC cases | `/tmp/env_catalog_host_test` | exit 0 — **240 passed, 0 failed** (`host_test.log`) |
| HTTP JSON decoder | `tests/http_json_str_host_test.c` (build line in its header) | exit 0 — **4 escape cases held** |
| Source guard | `bash tests/env_catalog_broker_check.sh` | exit 0 — **all clauses held** (`broker_guard.log`) |
| Guard teeth | `bash tests/env_catalog_broker_check_smoke.sh` | exit 0 — **58 teeth held, 0 lost** (`broker_smoke.log`) |
| Sidecar stamps | `(cd /tmp/m51-clean2 && bash tests/sidecar_stamp_check.sh)` | exit 0 — clauses A–N held (`sidecar_stamp.log`) |
| Proto tests | `cargo test -p aerosls-proto --lib` | exit 0 — **58 passed** (`proto_tests.log`) |
| Sidecar tests | `cargo test -p aerosls-sidecar --lib` in `/tmp/m51-clean2/user` | exit 0 — **92 passed** (`sidecar_tests.log`) |
| Live surface | `AEROSLS_TOKEN=<test-bearer> bash tests/env_catalog_live_check.sh` | exit 0 — full §9.1 surface plus M5.1, with a text value (`live_check.log`) |

The sidecar archive for this change was rebuilt in a clean HEAD copy with only
the four M5 `user/` files applied, so its source digest covers the committed
tree rather than the unrelated local M4/E7 edits. `sidecar_stamp.log` is from
that clean tree. Its source digest is
`08154622aced7eebdfcfb07fcee6bb08453e7db8434b927a4798b74a929d4860` and the
archive sha256 is
`a08426adaefa64c091314dfbb55b4e570590f4b13479dae71cc2c228d6888d20`.

The text arm required one kernel transport fix: `net/http.c`'s `json_str()`
used to copy JSON bytes raw, so a client's correctly escaped backslash (`\`
in JSON) reached `sls_shell_execute()` as two characters. The environment shell
then saw `\'text\'`, not the single backslash in `\'text\'`, and stripped the
SQL quotes before `catalog exec` ran. `json_str()` now decodes the standard
one-character escapes (`\`, `\"`, `\/`, `\b`, `\f`, `\n`, `\r`, `\t`) and
copies other escapes through unchanged. The live arm exercises this through
`/api/shell/exec`; `tests/env_catalog_live_check.sh` now sends `VALUES (\'text\')`
as the command text.

The transport fix also has a host-only regression test:
`tests/http_json_str_host_test.c` includes the real `net/http.c`, links only
`json_str`, and checks `\\`, `\"`, `\n`, and an unknown `\q`. It needs no cluster.

The live run started a fresh three-node cluster with
`./run-cluster.sh --nodes 3 --force` on ports 3001–3003, after rebuilding the
kernel with the `json_str()` change and the working-tree sidecars. The cluster
was stopped after the run.

## What the live M5.1 arm proves

- The owner's `catalog exec INSERT` inserted a **text** value (letters, not a
  number: `e91w-<run>`), returned `1 row(s) affected`, and native SQL observed
  the committed row byte for byte. A `CAT_OK` written before commit would leave
  this read empty.
- `catalog exec UPDATE` set a second text value and `catalog exec DELETE`
  committed in turn; native SQL observed the updated text and then its absence.
- Dave's table was pinned to `perm_mask=0x4` (read only). Bob's `catalog exec
  DELETE` was then refused by name with `PERM_WRITE`, no affected-row body was
  sent, and native SQL still saw Dave's original row.
- `catalog exec DROP TABLE` was refused by name as DDL, not executed.
- A write naming the existing foreign-partition object answered as absence
  (`NOENT`), with no row created and no PERM that confirmed the object.

M5.2 (a Linux-personality task reaching `CAT_EXEC` through a project-private
syscall, working name `SYS_SLS_CATALOG_EXEC`) is not implemented or evidenced
here.
