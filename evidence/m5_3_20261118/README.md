# M5.3 ordinary-tenant Linux checkpoint/restore (2026-11-18)

This records the first successful checkpoint/restore of a registered
ordinary-tenant Linux task. The profile is build-time only: environment index
3 carries the ordinary tenant manifest bit (`FLAG_TENANT_LINUX`) and **no**
`FLAG_GATE`. It reuses the committed M4.6 two-thread fixture under
`m5.tenant.ckpt.0`, the task name init writes into the checkpoint record and
the tenant sidecar spawns through `spawn_tenant()`.

## Commands

The passing two-boot run, reusing the image built by the same harness:

```sh
M53_BUILD=0 \
M53_ISO=sls_operating_system_e3_m53b.iso \
M53_ARTIFACTS=/tmp/m53_tenant_ckpt_live3 \
    bash tests/m5_tenant_ckpt_probe_check.sh
```

The image was built by the harness's own build step:

```sh
make x86-iso-e3 E3_INIT_FEATURES=e3_envs,m5_tenant_probe,m5_tenant_ckpt \
    M5_TENANT_CKPT=1 X86_E3_ISO=sls_operating_system_e3_m53b.iso
```

To rebuild and run in one invocation:

```sh
M53_ISO=sls_operating_system_e3_m53b.iso \
M53_ARTIFACTS=/tmp/m53_tenant_ckpt_live3 \
    bash tests/m5_tenant_ckpt_probe_check.sh
```

## Image

`sls_operating_system_e3_m53b.iso`

SHA-256 (`iso.sha256`):

```text
1185d7f1c5b971c92affc5dbee1e84c2c710be6c762939acfbcc82de30da502b  sls_operating_system_e3_m53b.iso
```

Verify from the worktree root:

```sh
sha256sum -c evidence/m5_3_20261118/iso.sha256
```

## Result

**PASS** — harness exit 0. Boot 1 created environment index 3, the tenant
sidecar spawned `m5.tenant.ckpt.0`, the fixture printed
`M4.6-CKPT-READY`, and both members parked in `wait (futex)`. The checkpoint
answered:

```json
{"status":0,"seq":1,"count":1}
```

and the capture recorded:

```text
[ENV_PAYLOAD] linux group 'm5.tenant.ckpt.0' threads=2 tids=105..106 forms=1/1
```

Boot 2 replayed the ordinary tenant, poured the payload, and answered:

```json
{"ok":"true","pending":1,"replayed":1,"refused":0,"remaining":0,"resumed":0,"repaused":0,"payloads":1,"payload_skips":0,"tooth":"none"}
```

Both restored tasks retained their parked forms:

```text
[ENV_PAYLOAD] restored group for 'm5.tenant.ckpt.0': 2 held task(s), leader pid=105
[ENV_PAYLOAD] thread 'm5.tenant.ckpt.0' restored PARKED on channel 14 (expect num=202)
[ENV_PAYLOAD] thread 'm5.tenant.ckpt.0.t106' restored PARKED on channel 15 (expect num=202)
[PARTITION] partition 1 resumed.
[ENV_PAYLOAD] pour hold released: partition 1 resumed
```

The restored workload then printed:

```text
M4.6-FUTEX-PARENT-WAKE
M4.6-FUTEX-WOKE
M4.6-RESTORED-OK
```

There was no `[E7-GATE]` marker in either boot.

The parent's captured timed futex wait is expected to expire after the reboot;
the fixture uses that timeout to issue the wake. The proof is the child's
restored **untimed** waiter: it was still `PARKED` after restore, the parent's
`FUTEX_WAKE` released it, and it wrote `M4.6-FUTEX-WOKE` before the parent
wrote `M4.6-RESTORED-OK`. A lost parked waiter or a mismatch between init's
record name and the sidecar's spawn name leaves the harness red.

## Artifacts

- `boot1.log` — ordinary tenant creation, fixture spawn, both futex waits,
  checkpoint, and power-off.
- `boot2.log` — replay, payload pour, restored PARKED forms, partition resume,
  and the wake markers.
- `harness.log` — the passing harness output, including the source checks,
  checkpoint/restore JSON, and final PASS lines.
- `checkpoint.json` / `restore.json` — the exact API answers.
- `create.json` / `pcreate.json` — the environment and partition creation
  answers.
- `facts` — the compact result fields.
- `iso.sha256` — the image digest above.
