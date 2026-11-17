#!/usr/bin/env bash
# build_catalog_probe.sh — reproduce fixtures/e7_catalog_probe.flat from
# fixtures/e7_catalog_probe.S. The committed .flat is this script's
# byte-exact output (run from this directory):
#
#   bash build_catalog_probe.sh && cmp e7_catalog_probe.flat <committed>.flat
set -eu
cd "$(dirname "$0")"
gcc -nostdlib -static -no-pie -Wl,-Ttext=0x1000 -Wl,-e,_start \
    -o /tmp/e7_catalog_probe.elf e7_catalog_probe.S
objcopy -O binary -j .text /tmp/e7_catalog_probe.elf e7_catalog_probe.flat
echo "built: e7_catalog_probe.flat ($(wc -c < e7_catalog_probe.flat) bytes)"
