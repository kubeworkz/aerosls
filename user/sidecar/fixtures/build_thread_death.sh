#!/usr/bin/env bash
# build_thread_death.sh — reproduce the two I5b death-arm flats from their
# sources. The committed .flat files are this script's byte-exact output (run
# from this directory):
#
#   bash build_thread_death.sh && git diff --stat
set -eu
cd "$(dirname "$0")"
for p in e7_thread_fault_probe e7_thread_groupexit_probe; do
    gcc -nostdlib -static -no-pie -Wl,-Ttext=0x1000 -Wl,-e,_start \
        -o "/tmp/$p.elf" "$p.S"
    objcopy -O binary -j .text "/tmp/$p.elf" "$p.flat"
    echo "built: $p.flat ($(wc -c < "$p.flat") bytes)"
done
