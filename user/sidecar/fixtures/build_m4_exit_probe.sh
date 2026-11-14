#!/usr/bin/env bash
# build_m4_exit_probe.sh — reproduce fixtures/m4_exit_probe.flat from
# fixtures/m4_exit_probe.S. Run from any directory; the committed .flat is
# this exact output and the live gate includes it in the archive.
set -euo pipefail
cd "$(dirname "$0")"
gcc -nostdlib -static -no-pie -Wl,-Ttext=0x1000 -Wl,-e,_start \
    -o /tmp/m4_exit_probe.elf m4_exit_probe.S
objcopy -O binary -j .text /tmp/m4_exit_probe.elf m4_exit_probe.flat
echo "built: m4_exit_probe.flat ($(wc -c < m4_exit_probe.flat) bytes)"
