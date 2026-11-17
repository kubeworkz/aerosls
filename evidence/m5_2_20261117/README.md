# M5.2 gate-profile Linux-task CAT_EXEC — evidence (2026-11-17)

This directory records the **gate-profile arm only** of M5.2. A Linux-personality
task in the `e7_gate` profile reached the existing `CAT_EXEC` service through
`SYS_SLS_CATALOG_EXEC` (334). Normal tenant environments still do not spawn a
Linux personality; that surface is **M5.2a** and remains open.

## Provenance

- Branch: `m1/linux-network-bridge`
- HEAD at run: `5e9400fecc26392ec4e371f9c2a8af60a9d2d514`
- The worktree already contained unrelated dirty changes when the run started;
  none of them changed the M5.2 syscall or catalog path. No source change was
  made for this evidence run.
- Run directory: `evidence/m5_2_20261117/`

## Command

```sh
M52_ARTIFACTS=/tmp/m52_catalog_evidence_5e9400f \
    bash tests/m5_catalog_live_check.sh
```

The harness built the E3 image with
`E3_INIT_FEATURES=e3_envs,e7_gate M5_CATALOG_PROBE=1`, booted GRUB entry 3, and
created a partition plus dave-owned, bob-owned and foreign tables. It then
created gate-profile environment index 1 as bob.

## Result

**PASS** — exit status `0` (`exit_status.txt`).

```text
ok:   the Linux task issued 334 and got the promised answers (M52-CATALOG-OK)
ok:   native observer sees bob's committed UPDATE ('updated')
ok:   dave's row is unchanged ('untouched', no 'denied') — the EPERM refusal did not execute
ok:   the foreign table exists in the catalog; its ENOENT was the partition boundary
M5.2 live gate profile: PASS (partition=1 env=1)
```

The serial boot log carries `[M5.2] catalog probe: sp...` and
`M52-CATALOG-OK`, followed by the sidecar's Linux reply line. The native
observer is dave's ordinary shell, outside the mediated path.

## Artifacts

| File | Contents | SHA-256 |
|---|---|---|
| `live_check.log` | Complete stdout/stderr of `tests/m5_catalog_live_check.sh`, including the build and live assertions | `27ff03640452eb21e03f04f2ddf4f23dc29bbbd1f949c660998d166197c1d476` |
| `boot.log` | QEMU serial log from the passing boot | `ac53c09bfc08707a0c688ad399feb9bdba3af7a9154efb49aae655fb7b41e538` |
| `qemu.err` | QEMU stderr; only the harness's normal SIGTERM on cleanup | `c251770037e70f1e93b83b5c4a025db86a3fabf1bb97001ac1fb798d693b8f70` |
| `exit_status.txt` | Harness exit status (`0`) | `9a271f2a916b0b6ee6cecb2426f0b3206ef074578be55d9bc94f6f3fe3ab86aa` |

## What this proves

- The kernel accepted `SYS_SLS_CATALOG_EXEC` (334) from the gate-profile Linux
  task and forwarded the bounded SQL request through the task's one sidecar
  channel.
- The probe's owner INSERT/UPDATE succeeded, and dave's native SQL read saw
  bob's committed `updated` row; the EPERM refusal against dave's table did not
  execute and left dave's `untouched` row unchanged.
- The foreign table existed in `GET /api/objects`, so its `ENOENT` was the
  partition boundary rather than a missing object.
- The task's identity remained the environment's channel-bound owner; the
  syscall carried no uid.

## Claim boundary

This is **not** M5.2a evidence. It does not demonstrate a normal tenant spawning
a Linux personality. It also does not claim IFS-like files, native SIMI reach,
per-task credentials, row-level ACLs, DDL or multi-statement transactions.
