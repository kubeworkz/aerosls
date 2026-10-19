#!/usr/bin/env bash
# build_probe.sh — reproduce fixtures/e7_linux_probe.flat from
# fixtures/e7_linux_probe.S. The committed .flat is this script's byte-exact
# output (run from this directory):
#
#   bash build_probe.sh && cmp e7_linux_probe.flat <committed>.flat
set -eu
cd "$(dirname "$0")"
gcc -nostdlib -static -no-pie -Wl,-Ttext=0x1000 -Wl,-e,_start \
    -o /tmp/e7_probe.elf e7_linux_probe.S
objcopy -O binary -j .text /tmp/e7_probe.elf e7_linux_probe.flat
echo "built: e7_linux_probe.flat ($(wc -c < e7_linux_probe.flat) bytes)"
