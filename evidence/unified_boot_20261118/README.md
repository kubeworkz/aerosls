# Default unified boot after the sidecar archive repack (2026-11-18)

This is the runtime proof for the rebuilt default `sls_operating_system.iso`.
The tracked `sidecars.cpio` and its stamps were repacked from the current
`user/` sources, `make x86-iso` accepted them through the stale-archive guard,
and the resulting image was booted once through GRUB's unified entry (entry 3)
with `tests/unified_boot_check.sh`. No disk was attached, matching the check's
own design.

The boot was not rerun to produce this directory; `unified_boot.log` is the
serial log from that passing run and `unified_boot_check.log` is its captured
check output.

## Commands

```sh
make selfhost-bootimage
make x86-iso
bash tests/unified_boot_check.sh
```

`tests/unified_boot_check.sh` boots exactly once and checks the unified boot
shape, CPU sharing, HTTP health, and console ownership. It exits 0.

## Image

`sls_operating_system.iso`

SHA-256 (`iso.sha256`):

```text
665b4b961ff71a78c52355cbef245d429405b0272407c45795f0b36c2e5eb03f  sls_operating_system.iso
```

Verify from the worktree root:

```sh
sha256sum -c evidence/unified_boot_20261118/iso.sha256
```

## Sidecar archive

The evidence directory also carries copies of the tracked archive's stamps:

- `sidecars.cpio.digest` — source digest
  `0bb19f57d9f2fafdcc6958e8c5afb8b5ff46cc2a4769980dd32605c703bcfdec`
- `sidecars.cpio.sha256` — archive bytes
  `2b4472dce5795d97354a39bc4a1c154eaacab9c2c2623781bd14c01dea0b57bf`
- `sidecars.cpio.stamps` — the one-run record with both fields and all six
  packed binaries.

`sidecar_stamp_check.log` is the passing `tests/sidecar_stamp_check.sh` output
for that archive (including the E3 tuple check).

## Result

**PASS** — `tests/unified_boot_check.sh` exited 0. The check observed:

```text
ok:   the kernel parsed unified=1 from the command line
ok:   the Ring-0 control plane was planted as a schedulable pseudo-process
ok:   the init sidecar was created in this boot
ok:   init was LEFT RUNNABLE instead of being entered (the kernel kept the boot)
ok:   init was released by the sidecar creator (PROC_HELD -> PROC_SUSPENDED)
ok:   the kernel stamped the BIB's UNIFIED flag into every sidecar's boot info block
ok:   the kernel owns the console in this boot (input forwarding off)
ok:   the software half of the chain ran (init's own ramdisk driver was created)
ok:   no system POSIX sidecar was created (it would fight the kernel for the NIC and COM1)
ok:   no network driver sidecar was created (the kernel drives the NIC in this boot)
ok:   no e1000 user-driver sidecar was created (no NIC was handed off)
ok:   the boot reached the unified markers (heartbeats and the HTTP listener)
ok:   the kernel yielded to Ring-3 work 16 times and init reported 7 heartbeats
ok:   the CPU alternated (heartbeats after yields: 7; yields after heartbeats: 9)
ok:   /api/health answered twice while init was making progress (heartbeats 7 -> 81)
ok:   /api/health body is a live kernel (status=ok, uptime_ticks>0)
ok:   typed input was answered by the kernel shell (the control plane owns the console)
OK: the unified boot ran the Ring-0 control plane (HTTP + shell) and the Ring-3 sidecar world in one boot
```

The default sidecar chain came up as:

- `aerosls.init.0` — PID 100
- `drv.device_manager.0` — PID 101
- `drv.ramdisk.0` — PID 102

The hardware-owned `aerosls.posix.0`, `drv.network.0`, and `drv.e1000.0`
sidecars stayed unspawned, as required for the unified boot. No panic,
triple-fault, assertion, or sidecar startup failure marker appeared. The only
non-marker chatter was the normal single-node consensus election/term timeout
output and the kernel's `Fault Isolation Daemon` startup line.

## Boundary

This proves the default unified image's control plane and default software
sidecar chain after the archive repack. It does not claim the hardware-handoff
sidecars, NVMe persistence, or a tenant/checkpoint workload; those have their
own arms.

## Artifacts

- `unified_boot.log` — the passing serial log (normalized to LF).
- `unified_boot_check.log` — the check's 17 passing assertions and final `OK`.
- `sidecar_stamp_check.log` — the passing archive/stamp guard output.
- `iso.sha256` — the image digest above.
- `sidecars.cpio.digest`, `sidecars.cpio.sha256`, `sidecars.cpio.stamps` —
  copies of the tracked archive stamps.
- `facts` — the compact result fields.
