# M2b ioctl rerun — the ENOTTY sidecar

Rebuilt `m2b_ioctl2.iso` from the tree at commit `1261180` (TCGETS now
answers ENOTTY) and reran the quiet gate two-DSO probe to prove the sidecar
image change did not disturb the loader's path.

- ISO sha256: `7a53e538890b39384025574e1c03c044c459e12293fc18cce63d8aacd925a210`
- QEMU: `timeout 70 qemu-system-x86_64 -cdrom m2b_ioctl2.iso -m 4G -smp 4
  -accel tcg -display none -monitor none -no-reboot -boot d -serial file:...`
  (exit 124 = the timeout).
- The child asks `TIOCGWINSZ` (`req=0x5413`) — never TCGETS — so the new
  ENOTTY arm is not on its path, and the answer is unchanged.

The child's trace is identical to the previous run: four file-shape mmaps
served, both RELRO mprotects 0, `ioctl fd=1 req=0x5413` ok, `M2A-OK.`
written, `exit_group status=0`, group swept. `grep 'm2a.linux.hello.0 ->'`
is empty (0 refusals).
