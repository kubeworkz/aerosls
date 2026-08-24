#!/usr/bin/env python3
"""Determinism check for the Phase 5 init<->DM handshake.

Reads a serial transcript (stdin), reports interleave-tolerant markers.
Markers are passed as exact-substring booleans; the caller greps a saved log.
"""
import sys

raw = sys.stdin.read()
# Reconstruct the logical serial stream: the kernel writes whole lines to
# serial, and sidecar out-of-line output can interleave mid-line when two
# CPUs write concurrently. Fragments like "all devices r" + "eady." are split.
# We just do substring searches on a version with the kernel [CAP]/[PROC]
# noise removed line-by-line so interleave false-negatives are minimized.

def strip_noise(s):
    keep = []
    for ln in s.splitlines():
        if "[CAP]" in ln:
            continue
        if "[PROC]" in ln:
            continue
        if "[SIDECAR]" in ln:
            continue
        keep.append(ln)
    return "\n".join(keep)

clean = strip_noise(raw)

def has(frag):
    return frag in raw or frag in clean

result = {
    "spawn_dm": has("spawning Device Manager") or has("spawning Device M"),
    "adopted": has("all devices ready") or has("all devices r") or has("adopted"),
    "spawn_posix": has("spawning POSIX sidecar") or has("POSIX sidecar"),
    "phase5": has("Phase 5 init sidecar complete"),
    "unexpected_tag": has("unexpected message tag"),
    "server_error": has("server error"),
    "sig_fault": ("code=" in raw) and ("unexpected" in raw and "PF" in raw),
}
print(result)