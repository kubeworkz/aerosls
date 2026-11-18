# Phase 5 initrd boot sidecar verification (2026-11-18)

This directory records the Phase 5 (initrd) boot verification for the default
`sls_operating_system.iso`. GRUB auto-selected the Phase 5 entry; the unified
boot was not run.

Two Phase 5 layouts were needed because the default one-NIC boot keeps its only
NIC kernel-owned, so it cannot spawn the e1000 user-driver sidecar:

1. **Single-NIC default layout** — proves the system POSIX and network sidecars
   start and run their applets.
2. **Two-NIC e1000 handoff layout** — hands a second NIC to the Device Manager
   so `drv.e1000.0` starts and proves its PHY loopback.

No boot was rerun to produce this directory. The serial logs and check outputs
were copied from the passing runs; the serial logs are normalized to LF with
trailing whitespace stripped.

## Commands

Single-NIC Phase 5 boot:

```sh
WINDOW_S=180 bash tests/phase5_boot_smoke.sh
```

Two-NIC e1000 handoff boot:

```sh
bash tests/phase5_e1000_driver_smoke.sh
```

## Image

`sls_operating_system.iso`

SHA-256 (`iso.sha256`):

```text
665b4b961ff71a78c52355cbef245d429405b0272407c45795f0b36c2e5eb03f  sls_operating_system.iso
```

Verify from the worktree root:

```sh
sha256sum -c evidence/phase5_boot_20261118/iso.sha256
```

## Boot 1 — single-NIC Phase 5 entry

`tests/phase5_boot_smoke.sh` booted the default Phase 5 initrd entry, exited 0,
and finished in 64 s. The serial log is `phase5_boot.log`.

The system chain started as:

- `aerosls.init.0` — PID 100
- `drv.device_manager.0` — PID 101
- `drv.ramdisk.0` — PID 102
- `drv.network.0` — PID 103
- `aerosls.posix.0` — PID 104

POSIX was wired to the ramdisk and network sidecars and received the timer IRQ
(vector 32), serial IRQ (vector 36), UART I/O cap, and the e1000 BAR0 DEV cap
`nic0.bar0` at `0xfeb80000+131072`.

The applet evidence in the same log:

```text
[nettest] PASS: listener ready on :9999
[nettest] PASS: connect(10.0.2.15:9999)
[nettest] PASS: accept()
[nettest] PASS: shutdown+close
[irqtest] ALL PHASES PASS: timer 50/50, serial loopback 10/10, stuck-driver 3/3, driver-death respawn OK
[devtest] PASS: SYS_DEV_MMAP succeeded
[devtest] e1000 MAC: 52:54:00:12:34:01
[devtest] PASS: CAP_TYPE_DEV + SYS_DEV_MMAP verified on target
```

Marker output (`phase5_boot_markers.log`):

```text
need=phases,prompt phases=1 ready=1 irq=1 dev=1 prompt=1 torn=0 stage=[devtest] PASS: CAP_TYPE_DEV + SYS_DEV_MMAP verified on target tail=b'8 (channel 22)\n[CAP] woken PID 108 (channel 23)\n'
exit=0
```

The guard preset requires `phases` and `prompt`; all reported markers are
present. `phase5_boot_check.log` contains the harness's final `OK` line.

## Boot 2 — two-NIC e1000 handoff Phase 5 entry

`tests/phase5_e1000_driver_smoke.sh` booted the same Phase 5 entry with the
kernel-owned `nic0` plus a second, handed-off `nic1`. The serial log is
`phase5_e1000_boot.log`.

The e1000 sidecar start line:

```text
[SIDECAR] PID 102 'drv.e1000.0': dev cap 'nic0.bar0' phys 0xfeba0000+131072 slot 3
```

The driver's proof:

```text
[e1000] MAC 52:54:00:12:34:02
[e1000] PASS: loopback round-trip 31 bytes (MAC 52:54:00:12:34:02, rx slot 0)
[e1000] entering polled RX-drain serve loop
```

`phase5_e1000_check.log` contains the harness's final `OK` line. The harness
kills QEMU as soon as `[e1000] PASS` appears, so this relatively short log does
not reach the POSIX/network spawn sequence; those lines are in Boot 1's log.

## Notes

### Intentional `irqtest p4` respawn

The `[FAULT]` line in `phase5_boot.log` belongs to the irqtest p4 watchdog
scenario, not to a spontaneous sidecar startup failure. The sequence is:

```text
[irqtest] p4: storming, killing the driver process now
[FAULT] Ring-3 fault  cs=0x23  error=0x0  rip=0x00004000000154d7  — killing process.
[PROC] PID 104 'aerosls.posix.0' exited (code=134).
[INIT] POSIX sidecar died; respawning (restart 1, backoff 100000000 ns)...
[irqtest] p4: boot gen =1 (watchdog respawn)
[irqtest] PASS: watchdog respawn: fresh process re-bound and re-passed all phases
```

The p4 phase deliberately kills the driver process to test watchdog recovery.
Exit code 134 is `128 + SIGABRT`. Init respawned POSIX as PID 108, which
re-bound the same caps and passed p4. `sidecar_start_lines.log` includes both
PID 104 and PID 108 startup/wiring lines.

### Missing storage-image backup

The `backup` applet also logs:

```text
[backup] Copying sls_storage.img -> /var/backups/aerosls/hourly
[backup] FAILED: sls_storage.img not found (run from the repo root, or set --storage/STORAGE_IMG)
```

Both smokes boot with `-cdrom` only and attach no storage image, so this is the
expected no-disk result. It is unrelated to POSIX, network, or e1000 startup.

### Post-`System ready` line is the delayed p4 announcement

The `POSIX sidecar died; respawning` text that appears after `System ready`
and the `$` prompt is not a second sidecar exit. It is the p4 watchdog
respawn announcement (`restart 1`) arriving late in the serial stream; the
serial console is not strictly chronological.

Log evidence:

- There is exactly one `POSIX sidecar died; respawning` line, and it says
  `restart 1`; `restart 2` never appears. A second respawn would report
  `restart 2` because the supervisor increments `restarts_b` after each one.
- The real p4 event is earlier: `[FAULT]` and `[PROC] PID 104
  'aerosls.posix.0' exited (code=134)` at lines 601-602, PID 108's start
  lines at 646, `[POSIX] boot` at 654, then p4 `boot gen =1` at 1098 and
  `ALL PHASES PASS` at 1100.
- There is no `[PROC] PID 108 ... exited/terminated`, `[TORE] PID 108`, or
  `[FAULT]` for PID 108. PID 108 receives eight `[CAP] woken PID 108` events
  after the late respawn line (lines 1309-1316), so it is still alive.
- The `System ready`/`$` position is drain order, not event order: the
  adjacent `AeroSLS Phase 5 booting` is drain-ordered boot output from the
  already-running PID 108. The marker check still exits 0 over the complete
  log.

Source evidence:

- `user/vfs/src/fileobj.rs:435-448` — `ConsoleIo::read` returns `EAGAIN`
  when no input is buffered and the console is open; it returns `Ok(0)`
  (EOF) only after `closed` is set.
- `user/procmgr/src/procmgr.rs:1249-1255` — `read_blocking` maps `EAGAIN`
  to `ReadBlock::WouldBlock`, which parks the task.
- `user/sidecar/src/applets.rs:2451-2459` — the shell calls
  `read_blocking(0)` and maps `WouldBlock` to `Step::Blocked(Readable(0))`;
  it exits only on `Data(0)` (EOF) or an error. A headless boot with no
  serial input therefore parks at the prompt; it does not exit or trigger a
  respawn.
- `user/init/src/demo.rs:332-339` — the `[INIT] POSIX sidecar died`
  message is emitted only from the `CH_KIND_CLOSE` arm, and the respawn
  attempt is `restarts_b + 1`; a second close would report `restart 2`.

### Panic scan

Neither serial log contains `PANIC`, `triple fault`, `general protection`, or
`page fault` markers. The only `[FAULT]` is the intentional p4 watchdog kill
described above.

## Result

**PASS** — both harnesses exited 0. The requested sidecars started:

- `drv.network.0` and `aerosls.posix.0` in Boot 1, with `[nettest] PASS`,
  `[irqtest] ALL PHASES PASS`, and `[devtest] PASS`.
- `drv.e1000.0` in Boot 2, with `[e1000] PASS`.

The intentional p4 kill/respawn, the delayed p4 announcement after `System
ready`, and the no-disk `backup` failure are documented above so they are not
mistaken for startup regressions or a second sidecar exit.

## Artifacts

- `phase5_boot.log` — Boot 1 serial log (single NIC, LF-normalized).
- `phase5_e1000_boot.log` — Boot 2 serial log (two NICs, LF-normalized).
- `phase5_boot_check.log` — Boot 1 harness output.
- `phase5_e1000_check.log` — Boot 2 harness output.
- `phase5_boot_markers.log` — `phase5_boot_markers.py --check --preset guard`
  output and exit status.
- `sidecar_start_lines.log` — the `[SIDECAR]` startup/wiring lines for
  `aerosls.init.0`, `drv.device_manager.0`, `drv.ramdisk.0`,
  `drv.network.0`, `aerosls.posix.0` (including the PID 108 respawn), and
  `drv.e1000.0`.
- `iso.sha256` — the image digest above.
- `facts` — compact result fields.

## Boundary

This proves Phase 5 sidecar startup and the IRQ, network, DEV, and e1000
loopback paths on the default ISO. It does not claim the unified boot, NVMe
persistence, or a tenant/checkpoint workload; those have separate evidence.
