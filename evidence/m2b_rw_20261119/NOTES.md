# M2b — RW file-backed DSO maps (2026-11-19)

## Slice
Serve the loader's file-backed RW map at the fixed address with the fill rounds:
`mmap addr=0x400000054000 len=8192 prot=3 flags=0x12 fd=3 off=0x2000`.

Changes: the file ladder admits READ, READ|WRITE and READ|EXEC (W|X stays EPERM
before the gate, write-only stays ENOSYS); the verdict takes `prot_write` from
the protection word so the RW pages get a writable PTE; the sidecar's twin admits
0x1/0x3/0x5 as file shapes.

## Boot (quiet init profile: e3_envs,e7_gate — no irqtest self-kill)
All four maps served, 13 fill rounds (20K=6, 4K RX=2, 4K R=2, 8K RW=3), then
`close(3)`:

    [E7] file mmap addr=0x0 len=20480 prot=1 flags=0x2 fd=3 off=0x0
    [E7] file mmap addr=0x400000052000 len=4096 prot=5 flags=0x12 fd=3 off=0x1000
    [E7] file mmap addr=0x400000053000 len=4096 prot=1 flags=0x12 fd=3 off=0x2000
    [E7] file mmap addr=0x400000054000 len=8192 prot=3 flags=0x12 fd=3 off=0x2000
    ... 6 + 2 + 2 + 3 fill rounds ...
    [E7] linux: num=00000003 child=m2a.linux.hello.0 ok          (close fd 3)

On the plain `e3_envs` profile the demo `irqtest` applet's p4 phase deliberately
#UDs the sidecar at ~tick 50 and the respawn re-binds the E7 tenants but not the
M2a tenant; that boot cut the probe off after `close(3)` (see
boot_irqtest_cutoff.log). The gate-profile init.rc omits irqtest, so the run
above reaches the loader's own next demand.

## Next break — RELRO protection over the loader/program images
After the maps and close(3), the loader calls mprotect for RELRO:

    [E7] linux mprotect apply addr=0x400000054000 len=4096 prot=0x1 reply=0x0        (DSO RELRO: OK)
    [E7] linux mprotect apply addr=0x7f00000b2000 len=4096 prot=0x1 reply=-22       (ld-musl image)
    [E7] linux mprotect apply addr=0x403000      len=4096 prot=0x1 reply=-22       (program image)

    Error relocating /lib/ld-musl-x86_64.so.1: RELRO protection failed ...
    Error relocating m2a.linux.hello.0: RELRO protection failed ...
    [E7] linux: num=000000E7 child=m2a.linux.hello.0 exit_group status=127

The kernel's first trap admits the whole stamped span
(`linux_mprotect_span` = min(image_lo, heap_base) .. max(image_hi, heap_limit)),
so both requests are forwarded; the sidecar's mprotect arm checks its own twin
ledger `groups()[gid].brk_base .. brk_limit`, which carries only the heap stamp
and not the task's image pages, and answers EINVAL. The DSO page (inside the
heap stamp) succeeds. No loader-level refusal remains before this point.
