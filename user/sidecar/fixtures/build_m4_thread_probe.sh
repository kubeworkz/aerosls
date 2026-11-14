#!/usr/bin/env bash
# build_m4_thread_probe.sh — reproduce fixtures/m4_thread_probe.flat from
# fixtures/m4_thread_probe.S. Run from any directory; the committed .flat is
# this script's byte-exact output:
#
#   bash build_m4_thread_probe.sh
#   git diff --exit-code m4_thread_probe.flat
set -eu
cd "$(dirname "$0")"
gcc -nostdlib -static -no-pie -Wl,-Ttext=0x1000 -Wl,-e,_start \
    -o /tmp/m4_thread_probe.elf m4_thread_probe.S
objcopy -O binary -j .text /tmp/m4_thread_probe.elf m4_thread_probe.flat
echo "built: m4_thread_probe.flat ($(wc -c < m4_thread_probe.flat) bytes)"
