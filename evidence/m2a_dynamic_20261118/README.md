# M2a dynamic-linking live proof (2026-11-18)

M2a runs a dynamically linked musl ET_EXEC (`m2a_hello.elf`) through the
sidecar-supplied musl loader (`ld-musl-x86_64.so.1`). The manifest is version 2
with exactly one `INTERP` record; the kernel maps the loader at
`LINUX_INTERP_BASE` (0x7f0000000000), sets `AT_BASE`, keeps `AT_ENTRY` as the
main image's `e_entry` (0x40106b), and enters the loader (0x7f000007bf0e).

The hello is now an ordinary buffered-stdio program:

```c
#include <stdio.h>
int main(void) { puts("M2A-OK"); return 0; }
```

Returning from `main` runs musl's real exit path (`exit` → `__stdio_exit` →
`__stdio_write`), which flushes the buffered line with **`writev`** (syscall
20) and then calls `exit_group`. No `_exit` shortcut and no direct `write`.

Two seams this proof exercises:

- **`writev` (20)** was absent from `linux_syscall_table` and the sidecar, so
  the flush was refused `-ENOSYS` and the marker never appeared (the child
  still exited 0, silently). The kernel now owns the iovec walk: the first
  trap copies the caller's array (bounded by `LINUX_IOV_MAX` = 8 entries),
  `linux_writev_plan()` validates each segment's **clamped** span with write's
  canonical rules and drops zero-length entries, and the attach gathers the
  pieces into one payload no larger than `LINUX_FD_WRITE_CHUNK` (1024). The
  gathered total is written into `args[2]`, so the description the sidecar
  sees is exactly write's shape and `expected_req_len` derives the same
  length. The sidecar routes 20 to `answer_file`'s write arm; the console
  write performs the bytes and returns the count. An over-cap `iovcnt` is
  `-EINVAL`; a bad array/segment is `-EFAULT`; both are first-trap refusals
  with no post.
- **The loader's own `brk`** is served by the existing mem-span arm: the two
  `brk` calls musl makes during loader/startup (`num=0000000C`) are forwarded
  and answered `ok` before the flush, inside the child's stamped span. No
  `-ENOMEM`, no unmapped heap, no page fault. (The two `mmap`/`mprotect`
  pairs are the loader's own segment protections.)

The demo is opt-in (`M2A_INTERP=1`) and one-shot per boot: the spawn site reads
the kernel's boot generation, so init's watchdog respawn after the E3 irqtest's
driver-death phase logs `[M2A] one-shot skip gen=00000001` instead of re-running
the spawn into a full task table (`CAP_ETABLEFULL`, rc=-4).

## Commands

Image build and boot:

```sh
make x86-iso-e3 M2A_INTERP=1 X86_E3_ISO=m2a_stdio.iso SIDECAR_E3_CPIO=m2a_stdio.cpio
timeout 50 qemu-system-x86_64 -cdrom m2a_stdio.iso -m 4G -smp 4 -accel tcg \
    -display none -monitor none -no-reboot -boot d -serial file:boot.log
```

Guard and smoke, from the worktree root:

```sh
bash tests/linux_personality_check.sh
bash tests/linux_personality_check_smoke.sh
```

## Result

**PASS.** The loader mapped, the loader's `brk` calls were granted, `puts`
buffered, `main` returned, the exit flush crossed as a 7-byte `writev`, the
marker printed, and the child exited cleanly:

```text
[CS] dynamic image: main entry=0x40106b interp base=0x7f0000000000 entry=0x7f000007bf0e path='/lib/ld-musl-x86_64.so.1'
[M2A] hello: spawned ch_00000013
[E7] linux: num=0000000C child=m2a.linux.hello.0 ok
[E7] linux: num=0000000C child=m2a.linux.hello.0 ok
[E7] linux: num=00000010 child=m2a.linux.hello.0 -> ENOSYS
[E7] linux: num=00000014 child=m2a.linux.hello.0 write ok len=7 |M2A-OK.|
M2A-OK
[E7] linux: num=000000E7 child=m2a.linux.hello.0 exit_group status=0
[E7] linux: num=000000E7 child=m2a.linux.hello.0 group swept=1 dead=1 live=0
```

`ioctl` (16) is musl's `TIOCGWINSZ` tty-size query on the stdout path; it is
still `-ENOSYS` by design and does **not** block the flush — the very next
line is the 7-byte `writev`. The `|M2A-OK.|` rendering is the sidecar's
length-tagged console line; the unadorned `M2A-OK` on the next line is the
actual serial output.

There was exactly one `[M2A] hello: spawned` and zero `[M2A] hello: spawn
FAILED`. After the E3 irqtest killed the POSIX sidecar and init restarted it
(`restart 1`), the respawn skipped the one-shot demo:

```text
[INIT] POSIX sidecar died; respawning (restart 1, backoff 100000000 ns)...
[M2A] one-shot skip gen=00000001
```

The loader's RELRO re-protections were accepted (`re-protected` in `boot.log`).
The QEMU run ends by the harness's 50 s timeout, not a guest failure; the M2a
child itself has exited by then.

Guard and smoke:

```text
linux_personality_check: all clauses hold
linux_personality_check_smoke: 129 passed, 0 failed
```

The writev guard clauses Y1–Y4 are themselves held by four smoke mutations,
so each seam is proven to redden by name when its pinned line drifts:

- t134 `writev-table-row-gone` — deletes the `{ 20, LSC_FD, 0 }` table row;
  the guard must name **Y1**.
- t135 `writev-iovec-cap-widened` — widens `LINUX_IOV_MAX` from `8u` to `64u`;
  the guard must name **Y2**.
- t136 `writev-count-rewrite-gone` — drops `largs[2] = wv.total;`;
  the guard must name **Y3**.
- t137 `writev-sidecar-number-drift` — renumbers the sidecar's
  `const LINUX_WRITEV: u64 = 20;` to `21`; the guard must name **Y4**.
- t138 `m2a-fixture-elf-drift` — replaces the committed `m2a_hello.elf`
  with different bytes; the guard must name **Y5** from the SHA-256 alone.
- t139 `m2a-fixture-recipe-drift` — drops `-nostartfiles` from
  `build_m2a.sh`'s loader-as-libc link line; the ELF still hashes, so only
  the recipe half catches it, and the guard must name **Y5**.

## Artifacts

- `boot.log` — the full serial log of the passing stdio boot.
- `markers.txt` — the numbered key lines quoted above.
- `guard.log` — `tests/linux_personality_check.sh` output (O9 included).
- `smoke.log` — the smoke run, including the `m2a-oneshot-gone` tooth, the
  four writev teeth t134–t137, and the fixture teeth t138 (hash) and t139
  (recipe).
- `facts` — compact result fields.
