# E7 gate live arm with the M5.2c preflight (2026-11-17)

This records the plain gate arm (`E7G_DRILL=0`) against the current tree, built
with the M5.2c missing-payload preflight. The preflight applies to every
Linux-bearing record, so this run is the positive control: the gate's real
captured payload must pass the preflight and pour.

## Command

```sh
E7G_DRILL=0 \
E7G_ISO=sls_operating_system_e3_gate_m52c.iso \
E7G_ARTIFACTS=/tmp/e7_gate_m52c_live2 \
    bash tests/e7_gate_check.sh --live
```

The image was built by the same command without `E7G_BUILD=0` and with
`E3_INIT_FEATURES=e3_envs,e7_gate E7_SLOW_BOOT=0 E7_TOOTH=`. The passing rerun
used `E7G_BUILD=0` against that image.

## Passing run

**PASS** — all gate clauses green; `E7G_RUN_DONE_PASS`.

Gate stamp, boot 1:

```text
[ENV_CKPT] linux task 'e7.gate.user.0' stamped into partition=1 index=1 entry=0x800ab76 stack=0x40000009b000..0x4000000ab000 caps=2
```

Capture, boot 1:

```text
[ENV_PAYLOAD] capture: task 2 'e7.gate.user.0' kind=2 form=3 park_syscall=0 nchans=0 user_rip=0x8057ab7 user_rsp=0x4000000a9778 fs_base=0x807c208
[ENV_PAYLOAD] captured partition=1 index=1 tasks=3 pages=1880 console=25 bytes=7737344 slot=0 seq=0
```

Restore report, boot 2:

```json
{"ok":"true","pending":1,"replayed":1,"refused":0,"remaining":0,"resumed":0,"repaused":0,"payloads":1,"payload_skips":0,"tooth":"none"}
```

`refused=0` is the important field for this increment: the preflight accepted
the gate payload rather than refusing it as a missing-payload record. The pour
then landed (`payloads=1, payload_skips=0`).

Gate clauses:

```text
B1 payloads=1, payload_skips=0
B2 adopted context form=3, stop=spent, fs_base=0x807c208
B3 first resumed read off=0x30000
B4 no second Copying; Done observed
B5 no [FAULT]
B6 address space torn down (frames freed)
B7 deepest 3488, 57424/65536 untouched
D1 drill clauses not applicable (drill=0)
e7_gate_check: the gate run passes
```

## First attempt

The first run used the same freshly built image and command, but with
`E7G_ARTIFACTS=/tmp/e7_gate_m52c_live`. It passed the preflight (`replayed=1,
refused=0`) and then failed in the existing payload-park wait:

```text
[ENV_PAYLOAD] wait: task 0 'drv.ramdisk.1' kind=1 live=yes parked=no syscall=311 nchans=0
[ENV_PAYLOAD] posture: 'drv.ramdisk.1' (task 0) is live but not parked (state=1 waiting=65535 park_syscall=311)
[ENV_PAYLOAD] contents not restored for (partition 1, index 1): a live sidecar is not parked in the recv the capture recorded -- the replay stands; contents are not claimed
```

Its restore report was `replayed=1, refused=0, payloads=0, payload_skips=1`.
This is a post-preflight pour failure, not `ENV_CKPT_REFUSE_PAYLOAD`; the
second attempt passed. Both attempts' boot logs and reports are kept here.

## Artifacts

- `pass/` — the passing boot logs, `gate_live_check.log`, `gate_stamp.txt`,
  `restore.json`/`restore_report.json`, `facts.txt`.
- `first_attempt/` — the first attempt's boot logs, report, and harness output.
