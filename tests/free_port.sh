#!/usr/bin/env bash
# tests/free_port.sh — print the first free loopback TCP port in 3001..3020,
# for a boot check's QEMU hostfwd.
#
# ─── Why this exists ───────────────────────────────────────────────────────
# run-cluster.sh gives node i its REST API on host port 3000+i, and the deploy
# gate (deploy/deploy.sh) runs tests/run_checks.sh while the live instance
# still serves. Boot checks that hard-coded their hostfwd port therefore
# collided with live nodes: on the 2026-09-13 deploy, a cluster held
# 3001..3003, so partition_teardown_boot_check (3002) and
# simi_teardown_boot_check (3003) could not start QEMU at all — "Could not set
# up host forwarding rule" went to /dev/null and surfaced only as "QEMU exited
# before the grub menu appeared" — while aerocap (3011) and tcache_roundtrip
# (which already scanned for a free port) stayed green. The same class of
# collision as the 2026-08-18 storage-image flock failure, on the port.
#
# A taken port must fail to bind, not silently test the wrong service, so
# probe first and hand back one nothing answers on.
#
# Each probe is bounded (timeout 2): on a host where connect() to a closed
# loopback port hangs instead of refusing — observed on WSL2 with
# networkingMode=mirrored after its loopback state goes stale — an unbounded
# probe hangs the port scan forever. A healthy refusal is <1ms, so 2s never
# changes the answer on a well host; a hung probe is treated as free, and a
# genuinely taken port still makes QEMU's bind fail loudly.
#
# Callers should bind the port on 127.0.0.1 (hostfwd=tcp:127.0.0.1:$PORT-:3000)
# and talk to http://127.0.0.1:$PORT — the probe only proves loopback is free,
# and a deploy host must not expose a test kernel on a public interface.
#
# Usage:
#   PORT=$(bash tests/free_port.sh) || { echo "ABORT: no free port in 3001..3020" >&2; exit 2; }
#
# Exit: 0 with the port on stdout; 1 if every port in the range is taken.
set -u

for p in $(seq 3001 3020); do
    if ! timeout 2 bash -c 'exec 3<>"/dev/tcp/127.0.0.1/$1"' _ "$p" 2>/dev/null; then
        echo "$p"
        exit 0
    fi
done
exit 1
