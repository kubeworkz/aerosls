#!/usr/bin/env bash
# build_candidate.sh — reproduce fixtures/e7_candidate.elf from
# fixtures/e7_linux_probe.S. The committed .elf is this script's byte-exact
# output (run from this directory):
#
#   bash build_candidate.sh && cmp e7_candidate.elf <committed>.elf
#
# The deltas from build_probe.sh, each load-bearing:
#   -Wl,-Ttext=0x400000 — linked vaddrs for the census candidate's
#     segments (the .flat probe's 0x1000 is a FILE offset after objcopy;
#     this ELF is mapped at its p_vaddrs, §5.1);
#   -Wl,--build-id=none — gcc's default --build-id=sha1 drops a
#     .note.gnu.build-id whose LMA COLLIDES with -Ttext's .text on this ld
#     (observed: "section .note.gnu.build-id LMA overlaps section .text");
#     the note carries nothing this kernel reads;
#   -no-pie -static — ET_EXEC by construction: §5.1's gate accepts linked
#     vaddrs only, and an ET_DYN image is REFUSED by name, never biased.
set -eu
cd "$(dirname "$0")"
gcc -nostdlib -static -no-pie -Wl,--build-id=none -Wl,-Ttext=0x400000 -Wl,-e,_start \
    -o e7_candidate.elf e7_linux_probe.S
echo "built: e7_candidate.elf ($(wc -c < e7_candidate.elf) bytes)"
