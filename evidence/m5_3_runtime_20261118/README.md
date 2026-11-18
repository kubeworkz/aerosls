# M5.3 runtime-selectable tenant profile live run (2026-11-18)

This records the two-boot proof for the runtime-selectable replacement of the
M5.3 build-time index-3 arm. The ordinary-tenant checkpoint profile is chosen
by the create request itself:

```json
{"index":4,"profile":"tenant-linux-ckpt"}
```

Index 4 is deliberately not the M5.2a probe-only index 3. The environment
carries the ordinary tenant manifest bit (`FLAG_TENANT_LINUX`) and no
`FLAG_GATE`; init persists the choice as the record's `m5.tenant.ckpt.0`
Linux task name, and boot 2 derives the profile from that name.

## Commands

Image build:

```sh
make x86-iso-e3 E3_INIT_FEATURES=e3_envs,m5_tenant_probe,m5_tenant_ckpt \
    M5_TENANT_CKPT=1 X86_E3_ISO=sls_operating_system_e3_m53r.iso -j2
```

The passing two-boot run, reusing that image:

```sh
M53_BUILD=0 \
M53_ISO=sls_operating_system_e3_m53r.iso \
M53_ARTIFACTS=/tmp/m53r-evidence \
    bash tests/m5_tenant_ckpt_probe_check.sh
```

## Image

`sls_operating_system_e3_m53r.iso`

SHA-256 (`iso.sha256`):

```text
2e4fd2644323c156c76d1377c96e45fe81d10d831a37983e5f722d6b48bec271  sls_operating_system_e3_m53r.iso
```

Verify from the worktree root:

```sh
sha256sum -c evidence/m5_3_runtime_20261118/iso.sha256
```

## Result

**PASS** — harness exit 0. The create route answered HTTP 200 for
`{"index":4,"profile":"tenant-linux-ckpt"}`:

```json
{"ok":"true","env_id":1,"partition":1}
```

Boot 1 stamped the task at the runtime-selected index:

```text
[ENV_CKPT] linux task 'm5.tenant.ckpt.0' stamped into partition=1 index=4 entry=0x400000000000 stack=0x400000002000..0x400000006000 caps=2
```

The fixture printed `M4.6-CKPT-READY`, both members parked in `wait (futex)`,
and the checkpoint answered:

```json
{"status":0,"seq":1,"count":1}
```

with the capture:

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
```

The restored workload then printed:

```text
M4.6-FUTEX-PARENT-WAKE
M4.6-FUTEX-WOKE
M4.6-RESTORED-OK
```

There was no `[E7-GATE]` marker in either boot. The child's restored **untimed**
waiter is the proof: it was still `PARKED` after restore, the parent's captured
timeout issued `FUTEX_WAKE`, and the child wrote `M4.6-FUTEX-WOKE` before the
parent wrote `M4.6-RESTORED-OK`.

## Artifacts

- `boot1.log` — runtime-profile create, fixture spawn, both futex waits,
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
