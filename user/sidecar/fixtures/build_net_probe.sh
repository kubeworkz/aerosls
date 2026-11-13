#!/usr/bin/env bash
# build_net_probe.sh — reproduce fixtures/m1_net_probe.flat from
# fixtures/m1_net_probe.S. The committed .flat is this script's byte-exact
# output (run from this directory):
#
#   bash build_net_probe.sh && cmp m1_net_probe.flat <committed>.flat
set -eu
cd "$(dirname "$0")"
gcc -nostdlib -static -no-pie -Wl,-Ttext=0x1000 -Wl,-e,_start \
    -o /tmp/m1_net_probe.elf m1_net_probe.S
objcopy -O binary -j .text /tmp/m1_net_probe.elf m1_net_probe.flat
echo "built: m1_net_probe.flat ($(wc -c < m1_net_probe.flat) bytes)"
