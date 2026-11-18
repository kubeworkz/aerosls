# M2a dynamic-linking live proof (2026-11-18)

M2a runs a dynamically linked musl ET_EXEC (`m2a_hello.elf`) through the
sidecar-supplied musl loader (`ld-musl-x86_64.so.1`). The manifest is version 2
with exactly one `INTERP` record; the kernel maps the loader at
`LINUX_INTERP_BASE` (0x7f0000000000), sets `AT_BASE`, keeps `AT_ENTRY` as the
main image's `e_entry` (0x40108f), and enters the loader (0x7f000007bf0e). The
hello proves itself with `write(1, "M2A-OK\n", 7)` and `_exit(0)` — no stdio
flush, no `writev`.

The demo is opt-in (`M2A_INTERP=1`) and one-shot per boot: the spawn site reads
the kernel's boot generation, so init's watchdog respawn after the E3 irqtest's
driver-death phase logs `[M2A] one-shot skip gen=00000001` instead of re-running
the spawn into a full task table (`CAP_ETABLEFULL`, rc=-4).

## Commands

Image build and boot:

```sh
make x86-iso-e3 M2A_INTERP=1 X86_E3_ISO=m2a_once.iso SIDECAR_E3_CPIO=m2a_once.cpio
timeout 90 qemu-system-x86_64 -cdrom m2a_once.iso -m 4G -smp 4 -accel tcg \
    -display none -monitor none -no-reboot -boot d -serial file:boot.log
```

Guard and smoke, from the worktree root:

```sh
bash tests/linux_personality_check.sh
bash tests/linux_personality_check_smoke.sh
```

## Result

**PASS.** The loader mapped, the hello wrote its marker and exited cleanly:

```text
[CS] dynamic image: main entry=0x40108f interp base=0x7f0000000000 entry=0x7f000007bf0e path='/lib/ld-musl-x86_64.so.1'
[M2A] hello: spawned ch_00000013
[E7] linux: num=00000001 child=m2a.linux.hello.0 write ok len=7 |M2A-OK.|
M2A-OK
[E7] linux: num=000000E7 child=m2a.linux.hello.0 exit_group status=0
```

There was exactly one `[M2A] hello: spawned` and zero `[M2A] hello: spawn FAILED`.
After the E3 irqtest killed the POSIX sidecar and init restarted it (`restart 1`),
the respawn skipped the one-shot demo:

```text
[INIT] POSIX sidecar died; respawning (restart 1, backoff 100000000 ns)...
[M2A] one-shot skip gen=00000001
```

The loader's RELRO re-protections were accepted (`re-protected` in `boot.log`).
The QEMU run ends by the harness's 90 s timeout, not a guest failure.

Guard and smoke:

```text
linux_personality_check: all clauses hold
linux_personality_check_smoke: 123 passed, 0 failed
```

## Artifacts

- `boot.log` — the full serial log of the passing boot.
- `markers.txt` — the numbered key lines quoted above.
- `guard.log` — `tests/linux_personality_check.sh` output (O9 included).
- `smoke.log` — the smoke run, including the `m2a-oneshot-gone` tooth.
- `facts` — compact result fields.
