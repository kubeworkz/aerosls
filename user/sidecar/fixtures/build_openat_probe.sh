#!/usr/bin/env bash
# build_openat_probe.sh — reproduce fixtures/e7_openat_probe.flat from
# fixtures/e7_openat_probe.S. The committed .flat is this script's byte-exact
# output (the .gitattributes marks *.flat binary).
#
#   bash build_openat_probe.sh && cmp e7_openat_probe.flat <committed>.flat
set -eu
cd "$(dirname "$0")"
gcc -nostdlib -static -no-pie -Wl,-Ttext=0x1000 -Wl,-e,_start \
    -o /tmp/e7_openat_probe.elf e7_openat_probe.S
objcopy -O binary -j .text /tmp/e7_openat_probe.elf e7_openat_probe.flat
wc -c e7_openat_probe.flat
