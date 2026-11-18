# E7 gate live arm after the hold-gap regression (2026-11-18)

This records the plain gate arm (`E7G_DRILL=0`) against the tree carrying the
deterministic `env_service_pour_hold_gap()` host regression. The image was
rebuilt from this tree; no test seam is linked into the kernel.

Run from the worktree root in a WSL login shell.

## Commands

The exact build-and-live invocation:

```sh
E7G_DRILL=0 \
E7G_BUILD=1 \
E7G_ISO=sls_operating_system_e3_gate_hold2.iso \
E7G_ARTIFACTS=/tmp/e7_gate_hold_live5 \
    bash tests/e7_gate_check.sh --live
```

The build command that invocation ran:

```sh
make x86-iso-e3 E3_INIT_FEATURES=e3_envs,e7_gate E7_SLOW_BOOT=0 \
    E7_TOOTH= X86_E3_ISO=sls_operating_system_e3_gate_hold2.iso
```

To rerun against the saved image without rebuilding:

```sh
E7G_DRILL=0 \
E7G_BUILD=0 \
E7G_ISO=sls_operating_system_e3_gate_hold2.iso \
E7G_ARTIFACTS=/tmp/e7_gate_hold_live5 \
    bash tests/e7_gate_check.sh --live
```

## Image

`sls_operating_system_e3_gate_hold2.iso`

SHA-256 (`iso.sha256`):

```text
5aec99b9f641c061769751b679fc773611e6edb62e6b28b8e1888767a00cf0d4  sls_operating_system_e3_gate_hold2.iso
```

Verify from the worktree root:

```sh
sha256sum -c evidence/e7_gate_hold_20261118/iso.sha256
```

## Result

**PASS** — harness exit 0, all gate clauses green.

```text
payloads=1
payload_skips=0
B1-B7 pass
D1 drill clauses not applicable (drill=0)
[ENV_PAYLOAD] pour hold: partition 1 frozen with the parked posture verified
[ENV_PAYLOAD] pour hold released: partition 1 resumed
e7_gate_check: the gate run passes
```

`restore.json`:

```json
{"ok":"true","pending":1,"replayed":1,"refused":0,"remaining":0,"resumed":0,"repaused":0,"payloads":1,"payload_skips":0,"tooth":"none"}
```

The built kernel's `env_service_pour_hold_gap` is `FUNC WEAK DEFAULT` and
disassembles to `endbr64; ret`, so the host seam is inert in this image.

## Artifacts

- `boot1.log` — capture boot, checkpoint, and power-off.
- `boot2.log` — adoption, replay, payload pour, and resumed workload.
- `facts` — `drill=0`, `payloads=1`, `payload_skips=0`, `tooth=none`.
- `restore.json` — the REST restore report.
- `iso.sha256` — the image digest above.
