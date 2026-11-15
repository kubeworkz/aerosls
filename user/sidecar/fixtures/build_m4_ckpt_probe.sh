#!/usr/bin/env bash
# build_m4_ckpt_probe.sh — reproduce fixtures/m4_ckpt_probe.flat from
# fixtures/m4_ckpt_probe.S. Run from any directory; the committed .flat is
# this exact output and the live checkpoint probe includes it in the archive.
set -euo pipefail
cd "$(dirname "$0")"
gcc -nostdlib -static -no-pie -Wl,-Ttext=0x1000 -Wl,-e,_start \
    -o /tmp/m4_ckpt_probe.elf m4_ckpt_probe.S
objcopy -O binary -j .text /tmp/m4_ckpt_probe.elf m4_ckpt_probe.flat
echo "built: m4_ckpt_probe.flat ($(wc -c < m4_ckpt_probe.flat) bytes)"
