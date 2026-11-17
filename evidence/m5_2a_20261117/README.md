# M5.2a ordinary-tenant Linux-task CAT_EXEC — evidence (2026-11-17)

This directory records the **M5.2a tenant test-profile arm**. An ordinary tenant
environment (index 3, no `FLAG_GATE`) spawned the existing M5.2 catalog probe as
`m5.tenant.catalog.0`, and the probe reached the same `CAT_EXEC` service through
`SYS_SLS_CATALOG_EXEC` (334).

The profile is a build-time test profile:

```text
E3_INIT_FEATURES=e3_envs,m5_tenant_probe M5_CATALOG_PROBE=1
```

It is **not** a runtime-selectable tenant feature. The probe is the existing
short-lived fixture and is deliberately not registered as a checkpoint task.
This evidence therefore does **not** claim arbitrary tenant Linux payloads,
tenant Linux checkpoint/restore, or a production tenant Linux profile.

## Provenance

- Branch: `m1/linux-network-bridge`
- HEAD at run: `6aff1b9` plus the uncommitted M5.2a change set in this worktree.
- Packed sidecar source digest at run:
  `d836d417b1afa9669dd81cd8bd28be154673c19b48e6a0853982a69f30a29a90`.
- Packed sidecar archive bytes at run:
  `a26ce8ce43e5358e9a878de0fe21c1c302540ec0a91b5751fde5e28a6ad04c14`.
- Tenant image: `sls_operating_system_e3_m52a.iso` (ignored build artifact,
  not stored here).
- The worktree also contained unrelated dirty files; none changed the M5.2
  catalog/syscall path or this profile selection.
- Run directory: `evidence/m5_2a_20261117/`
- The QEMU harness ran under the project's WSL2 Ubuntu toolchain because the
  Windows shell has no QEMU. Its FIFOs and 10 GiB disk image were kept in WSL's
  native `/tmp`; only the captured logs are stored here.

## Commands

Build and boot through the named tenant wrapper:

```sh
M52_BUILD=1 \
M52_ISO=sls_operating_system_e3_m52a.iso \
M52_ARTIFACTS=/tmp/m52a_evidence \
    bash tests/m5_tenant_catalog_live_check.sh
```

The wrapper forces `M52_PROFILE=tenant`; the shared harness builds with
`E3_INIT_FEATURES=e3_envs,m5_tenant_probe M5_CATALOG_PROBE=1`, boots GRUB entry
3, creates a partition plus dave-owned, bob-owned and foreign tables, then
creates ordinary tenant environment index 3 **as bob**.

## Result

**PASS** — exit status `0` (`exit_status.txt`).

```text
tenant env=1 (index 3, owner bob)
ok:   the tenant probe ran without the gate profile (no [E7-GATE])
ok:   the Linux task issued 334 and got the promised answers (M52-CATALOG-OK)
ok:   native observer sees bob's committed UPDATE ('updated')
ok:   dave's row is unchanged ('untouched', no 'denied') — the EPERM refusal did not execute
ok:   the foreign table exists in the catalog; its ENOENT was the partition boundary
M5.2 live tenant profile: PASS (partition=1 env=1)
```

The serial log carries `[M5.2] tenant probe: sp...`, `M52-CATALOG-OK`, and the
sidecar's Linux reply line naming `m5.tenant.catalog.0`. The native observer is
dave's ordinary shell, outside the mediated path. The tenant arm asserts that
no `[E7-GATE]` marker and no gate environment were produced.

## Focused checks run

| Check | Result |
|---|---|
| `cargo test --manifest-path user/Cargo.toml -p aerosls-proto -p aerosls-init` | PASS — 52 init + 59 proto tests |
| `cargo test --manifest-path user/Cargo.toml -p aerosls-init --features m5_tenant_probe --lib` | PASS — 52 tests, including the tenant profile selection and no-checkpoint-task test |
| `cargo test --manifest-path user/Cargo.toml -p aerosls-sidecar --features m5_catalog_probe --lib` | PASS — 100 tests |
| `tests/m5_tenant_catalog_live_check.sh` | PASS — this directory's live log |
| gate arm rerun against the same final source (`M52_PROFILE=gate`) | PASS — `M5.2 live gate profile: PASS` in `gate_regression.log` |

The build emits the tree's existing warnings; both live runs exited 0.

## Artifacts

| File | Contents | SHA-256 |
|---|---|---|
| `live_check.log` | Complete build plus live stdout/stderr, including the four catalog assertions | `f8d04631fa4de05ec607d2400541a2690e238603c8de1c9b29ccea88a08a25ce` |
| `boot.log` | QEMU serial log from the passing tenant boot | `2bf1b2f468c72c687c548aa1880ed705fde4442d5a5522e3006ddd717eece756` |
| `qemu.err` | QEMU stderr; only the harness's normal SIGTERM on cleanup | `44f5b081a77ae04f9595a42f0abac6a6a4d4b61e82f0820104e37b4a10bb8d33` |
| `exit_status.txt` | Harness exit status (`0`) | `9a271f2a916b0b6ee6cecb2426f0b3206ef074578be55d9bc94f6f3fe3ab86aa` |
| `gate_regression.log` | Gate-profile rerun against the same final source; PASS, confirming the shared tracking path still serves the gate arm | `0c5c889a4407e34ee8c1e74f522514e632a0becc504bb16aadfd0f2de27ef4e7` |

## What this proves

- A non-gate tenant manifest selected the new `FLAG_TENANT_LINUX` profile bit;
  the kernel copied it into the BIB as `BOOT_INFO_FLAG_TENANT_LINUX`.
- The tenant sidecar spawned the existing catalog probe under the separate
  `m5.tenant.catalog.0` identity, with zero catalog caps on the child.
- The probe's `SYS_SLS_CATALOG_EXEC` (334) forwarded through the tenant's one
  sidecar channel and reused the environment's `CatalogOps` client.
- The owner INSERT/UPDATE committed, dave's native read saw bob's `updated`
  row, dave's own row stayed `untouched`, and the foreign name answered
  `ENOENT` while remaining present in `GET /api/objects`.
- The tenant arm produced no gate profile marker or gate environment.

## Claim boundary

The profile is compiled in only for `m5_tenant_probe` and selected only for
environment index 3. The manifest has the same five tenant caps and no
hardware caps. The catalog probe has exactly the messenger channel.

This is **not** evidence for:

- a runtime-selectable or shipped tenant Linux profile;
- arbitrary tenant-supplied Linux ELF execution;
- a persistent tenant Linux task;
- tenant Linux checkpoint/restore (the short-lived probe is not registered as a
  third checkpoint task);
- per-task credentials, IFS-like files, native SIMI reach, row-level ACLs, DDL,
  or multi-statement transactions.

The M5.2 gate-profile evidence remains in `evidence/m5_2_20261117/`; this run
does not replace it. A fresh gate-profile boot against the same final source
also passed (`gate_regression.log`), so the shared console-tracking change did
not make the gate arm depend on the tenant profile.
