# M5.2b ordinary-tenant Linux-task registration — evidence (2026-11-17)

This directory records **M5.2b**: the M5.2a ordinary-tenant test probe is now
the environment checkpoint record's third task, and the kernel reconciles its
task facts and stack bounds at registration. This is **registration only**; it
does not capture or restore the tenant, and it does not make the tenant profile
runtime-selectable.

## What changed

- `user/proto/src/env_proto.rs` adds `TENANT_LINUX_TASK_NAME`
  (`m5.tenant.catalog.0`). `user/sidecar/src/m5_catalog_probe.rs`'s
  `TENANT_PROBE_NAME` aliases it, so init's registration and the sidecar's
  spawn cannot drift.
- `user/init/src/env_manager.rs` carries `tenant_linux` on `Environment` and,
  for the `m5_tenant_probe` profile's designated index 3, names the Linux task
  in registration slot 2. The gate branch keeps precedence.
- `kernel/cap.c` stamps `linux_stack_base`/`linux_stack_pages` for **every**
  Linux task, not only the gate profile, and stores the create's BIB cap count
  on the descriptor. The gate poison/scan fields remain gate-only. The poison
  identity (`linux_stack_phys`/`linux_stack_poison`) is cleared by a non-gate
  Linux create rather than at slot reuse, so a gate task's teardown report
  still has it; `proc_clear_resume_state()` leaves those two fields alone.
  The clone and restore-thread creates clear them too, so a reused slot cannot
  hand a thread a stale gate scan.
- `kernel/env_service.c` replaces the entry-only lookup with
  `env_service_task_facts()` (entry, stack bottom/top and cap count). After a
  successful `env_ckpt_register_from()`, it re-resolves each named Linux task
  and stamps it, closing the window where the sidecar's child spawned before
  the record existed. The spawn-time stamp remains the other side of the race.
- Host tests: `tests/env_ckpt_host_test.c` (tenant slot/stamp/identity),
  `tests/env_register_host_test.c` (three-task wire and record), and
  `tests/env_service_wait_host_test.c` (a live Linux child that beat
  registration is reconciled into entry/stack/caps). Guards:
  `tests/env_register_pin_check.sh`, its smoke, and
  `tests/linux_personality_check.sh`'s W5b clause.

## Provenance

- Branch: `m1/linux-network-bridge`
- HEAD at run: `b90e91c` plus the uncommitted M5.2b change set in this
  worktree.
- Build: `E3_INIT_FEATURES=e3_envs,m5_tenant_probe M5_CATALOG_PROBE=1`
  (no `e7_gate`).
- Tenant image: `sls_operating_system_e3_m52a.iso` (ignored build artifact,
  not stored here).
- The QEMU harness ran under the project's WSL2 Ubuntu toolchain because the
  Windows shell has no QEMU. Its disk image and FIFOs stayed in WSL's native
  `/tmp`; only the captured logs are stored here.
- Run directory: `evidence/m5_2b_20261117/`

## Commands

Build and boot the tenant arm:

```sh
M52_PROFILE=tenant \
M52_BUILD=1 \
M52_ISO=sls_operating_system_e3_m52a.iso \
M52_ARTIFACTS=/tmp/m52b_evidence \
    bash tests/m5_catalog_live_check.sh
```

The harness boots GRUB entry 3, creates a partition plus dave-owned, bob-owned
and foreign tables, then creates ordinary tenant environment index 3 **as
bob**. The tenant sidecar spawns `m5.tenant.catalog.0`; the probe issues
`SYS_SLS_CATALOG_EXEC` (334) and prints `M52-CATALOG-OK`.

## Result

**PASS** — exit status `0` (`exit_status.txt`).

The live assertion added in this increment is the stamp line, and it landed:

```text
[ENV_CKPT] linux task 'm5.tenant.catalog.0' stamped into partition=1 index=3 entry=0x400000000000 stack=0x400000002000..0x400000006000 caps=2
```

The stamp only succeeds when the stored record has both the named POSIX parent
and the named Linux slot, so it also demonstrates that registration named the
tenant task. The `[ENV] checkpoint record registered ... tasks=3` line was not
captured on this boot's serial stream (the same capture gap seen in the M5.2
and M5.2a logs); the stamp is the durable proof, and no claim here depends on
the missing line.

The harness's other tenant checks are unchanged and green:

```text
tenant env=1 (index 3, owner bob)
ok:   the tenant Linux task is registered and its stamp landed (registration only; no tenant checkpoint/restore)
ok:   the tenant probe ran without the gate profile (no [E7-GATE])
ok:   the Linux task issued 334 and got the promised answers (M52-CATALOG-OK)
ok:   native observer sees bob's committed UPDATE ('updated')
ok:   dave's row is unchanged ('untouched', no 'denied') — the EPERM refusal did not execute
ok:   the foreign table exists in the catalog; its ENOENT was the partition boundary
M5.2 live tenant profile: PASS (partition=1 env=1)
```

`qemu.err` records `terminating on signal 15`, which is the harness's own
cleanup of the QEMU process after the PASS.

## Gate-profile regression after the poison-clear move

The gate's poison identity is now left intact by
`proc_clear_resume_state()`: a gate task's teardown report reads
`linux_stack_phys`/`linux_stack_poison`, and a non-gate Linux create clears
them instead. The full E7 gate live arm was rebuilt and rerun after this move
(plain gate image, drill off):

```sh
E7G_DRILL=0 E7G_ARTIFACTS=/tmp/e7_gate_m52b \
    bash tests/e7_gate_check.sh --live
```

**PASS** — exit status `0` (`gate_exit_status.txt`). The gate stamp is still
present in both boots:

```text
[ENV_CKPT] linux task 'e7.gate.user.0' stamped into partition=1 index=1 entry=0x800ab76 stack=0x40000009b000..0x4000000ab000 caps=2
```

The stop and exit poison reports still match B7's arithmetic:

```text
[E7-GATE] stack hwm (stop): alloc=65536 poison-span=60912 headroom=58264 deepest=2648 lowest=0x4000000a9398
[E7-GATE] stack hwm (exit): alloc=65536 poison-span=60912 headroom=57424 deepest=3488 lowest=0x4000000a9050
```

B7 passed: the resumed tail went 840 bytes deeper than the stop (3,488 bytes
total), 57,424 of 65,536 bytes stayed untouched, and the run cleared the
32,768-byte floor. The drill clauses are inapplicable (`drill=0`); the poison,
stop and exit measurements are not. Logs: `gate_boot1.log`, `gate_boot2.log`,
`gate_live_check.log`, `gate_facts.txt`.

## Host verification

Run in WSL2 Ubuntu against this worktree:

| Check | Result |
|---|---|
| `env_ckpt_host_test` (real `kernel/env_ckpt.c`) | 122 passed, 0 failed |
| `env_register_host_test` (real `kernel/env_ckpt.c`) | 76 passed, 0 failed |
| `env_service_wait_host_test` (real `kernel/env_service.c` + `kernel/env_ckpt.c`) | 41 passed, 0 failed |
| `cargo test --manifest-path user/proto/Cargo.toml --lib` | 59 passed |
| `cargo test --manifest-path user/init/Cargo.toml --lib` | 52 passed |
| `cargo test --manifest-path user/init/Cargo.toml --lib --features m5_tenant_probe` | 52 passed |
| `cargo test --manifest-path user/sidecar/Cargo.toml --lib --features m5_catalog_probe` | 100 passed |
| `tests/env_register_pin_check.sh` | green |
| `tests/env_register_pin_check_smoke.sh` | 25 teeth passed |
| `cap_create_sidecar_host_test` (real `kernel/cap.c`; self-contained documented build line, with its own two inert M4 restore/rollback stubs) | ALL PASS |
| `linux_initial_stack_host_test` | 46 passed, 0 failed |
| `linux_thread_group_host_test` | 178 passed, 0 failed |
| `tests/linux_personality_check.sh` (incl. W5b) | green |
| `tests/e7_gate_check.sh` (source only) | green |

## Boundary

This is the build-time `m5_tenant_probe` **test profile**, selected only for
environment index 3. M5.2b proves that the tenant task is named in the
checkpoint record and that the kernel fills its entry, stack bounds and cap
count — including when the child spawned before registration. It does **not**
claim tenant Linux checkpoint/restore, a runtime-selectable tenant profile, or
arbitrary tenant Linux payload support. The probe remains a short-lived
fixture. It also does not claim IFS-like files, native SIMI reach, per-task
credentials, row-level ACLs, DDL or multi-statement transactions.
