# M5.2c missing-payload restore refusal — evidence (2026-11-17)

This directory records **M5.2c**: a tenant checkpoint record whose Linux payload
capture was skipped is now **refused at restore**, before `env_service_create()`
can spawn a fresh tenant. It is separate from the open runtime-profile work.

## The hole this closes

`persist_environments()` writes the checkpoint record before
`env_payload_capture_all()` runs. Capture skips an environment when one of its
named tasks has no live process; the M5.2 tenant probe exits after its catalog
work, so a checkpoint taken after that exit has a record and no payload. The old
restore created the environment first, then reported the payload as skipped and
left the fresh tenant running — an empty tenant replayed under the old
environment's identity.

The fix is a restore-side preflight in `env_service_restore_pending()`. A record
that names a Linux task is refused with `ENV_CKPT_REFUSE_PAYLOAD` unless
`env_payload_has_sequence(partition, index, record.sequence)` finds a payload
whose directory entry and head sequence match the record. The preflight runs
before `env_ckpt_restore_resume_for_create()`, before the Linux restore hold is
armed, and before `env_service_create()`. `env_payload_has_sequence()` reads only
the slot directory and payload head; it does not pour pages.

## Host evidence

- `host_env_payload.log` — `tests/env_payload_host_test.c` against the real
  `kernel/env_payload.c`: **106 passed, 0 failed**. The new checks accept the
  captured sequence, refuse a stale sequence, and refuse a different identity.
- `host_env_service_wait.log` — `tests/env_service_wait_host_test.c` against the
  real `kernel/env_service.c` and `kernel/env_ckpt.c`: **54 checks passed, 0
  failed**. The M5.2c clause drives a real three-task tenant record at `(7,4)`
  with no matching payload and asserts:
  - `pending=1`, `refused=1`, `replayed=0`, `payload_skips=0`, `remaining=0`;
  - **zero `ENV_CREATE` sends** (`g_sends == 0`);
  - last refusal `ENV_CKPT_REFUSE_PAYLOAD`;
  - the record is marked refused and retained.
  Its positive control flips the preflight to "sequence matches" and asserts the
  same record reaches the create round trip.

Commands:

```sh
gcc -Wall -Wextra -std=c11 -I . -I kernel -I arch/x86 \
    -o /tmp/env_payload_host_test \
    tests/env_payload_host_test.c kernel/env_payload.c kernel/env_ckpt.c
/tmp/env_payload_host_test

gcc -Wall -Wextra -std=c11 -I . -I kernel -I arch/x86 \
    -o /tmp/env_service_wait_host_test \
    tests/env_service_wait_host_test.c kernel/env_service.c kernel/env_ckpt.c
/tmp/env_service_wait_host_test
```

## Live evidence

`live_check.log` is the harness output (**exit 0**). `tooth.txt` names the run:
`tenant-payload-missing`.

Build and run:

```sh
M52_PROFILE=tenant \
M52_TOOTH=tenant-payload-missing \
M52_BUILD=1 \
M52_ISO=sls_operating_system_e3_m52c.iso \
M52_ARTIFACTS=/tmp/m52c-evidence \
    bash tests/m5_catalog_live_check.sh
```

The image was built with
`E3_INIT_FEATURES=e3_envs,m5_tenant_probe M5_CATALOG_PROBE=1` (no `e7_gate`).
The same harness run was replayed against that image with `M52_BUILD=0` to
record `live_check.log`.

The harness creates ordinary tenant environment index 3 as bob, waits for the
tenant probe to print `M52-CATALOG-OK` and exit, confirms the group was swept,
then checkpoints. `boot1.log` carries the skipped capture:

```text
[ENV_PAYLOAD] capture skipped for partition=1 index=3: named task has no live process 'm5.tenant.catalog.0'
[PERSIST] Environment checkpoint snapshot written (1 live record(s), 1 frozen, 0 dropped).
```

It then reboots the **same NVMe image** in place and calls
`POST /api/env/restore`. `restore.json` is the refusal:

```json
{"ok":"true","pending":1,"replayed":0,"refused":1,"remaining":0,"resumed":0,"repaused":0,"payloads":0,"payload_skips":0,"tooth":"none"}
```

`envlist.json` shows the partition has no environment:

```json
{"ok":"true","partition":1,"envs":[],"live":0}
```

`boot2.log` carries the refusal before create:

```text
[ENV_CKPT] Restored 1 environment checkpoint record(s) (0 partition(s) re-paused).
[ENV_RESTORE] record (partition 1, index 3): record 3 names a Linux task but its payload was not captured for this sequence -- refused before create -- no create attempted, so no fresh tenant was spawned
[ENV_RESTORE] last refusal: record 3 names a Linux task but its payload was not captured for this sequence -- refused before create.
[ENV_RESTORE] 1 pending, 0 replayed, 1 refused, 0 still pending (0 partition(s) resumed for a create, 0 re-paused).
```

The harness also asserts no `[M5.2] tenant probe: sp` and no
`[ENV_PAYLOAD] contents not restored` line after the reboot. The old path would
have created a fresh tenant and emitted the latter after the fact; neither
happened.

## Boundary

- This proves the **refusal** for a build-time tenant test profile whose
  short-lived probe had already exited. It does not restore a live tenant, does
  not make the tenant profile runtime-selectable, and does not claim arbitrary
  tenant payloads.
- The preflight applies to every Linux-bearing record, including the E7 gate.
  The gate live arm (`E7G_DRILL=0`) was rebuilt against this tree and **passed
  every gate clause** on its second attempt, with `refused=0` and
  `payloads=1, payload_skips=0`; see `evidence/e7_gate_m52c_20261117/`. The
  first attempt passed the preflight (`replayed=1, refused=0`) and then hit the
  existing payload-park refusal (`payloads=0, payload_skips=1`), so it is kept
  as a separate failed attempt rather than folded into the pass.
- `qemu.err` records `terminating on signal 15`, the harness's cleanup after the
  PASS.
