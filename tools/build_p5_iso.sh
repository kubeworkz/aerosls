#!/bin/bash
set -e
cd "$(dirname "$0")/.."
W=$(pwd)

# Patch grub.cfg: timeout=0, default=2 for instant Phase 5 boot
cp grub.cfg grub.cfg.bak
sed -i 's/^set timeout=.*/set timeout=0/' grub.cfg
sed -i 's/^set default=.*/set default=2/' grub.cfg

# Build ISO (uses my_sls_kernel.bin + sidecars.cpio if present)
make x86-iso

# Restore original grub.cfg
mv grub.cfg.bak grub.cfg

echo "[ISO] built: $W/sls_operating_system.iso"
ls -la sls_operating_system.iso
