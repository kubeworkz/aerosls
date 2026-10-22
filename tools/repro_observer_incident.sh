#!/usr/bin/env bash
# One-shot repro of the 3-node idle-observer incident: fresh 3-node cluster,
# then the env-migrate confirm guard (which blocks the source node's sweep
# during env create + the synchronous migrate send). Captures all three node
# serial logs + the guard log, then stops the cluster.
#
# run-cluster.sh SUPERVISES in the foreground (killing it kills the nodes),
# so it is backgrounded here and the wait-for-ports loop below replaces it
# as the readiness gate.
set -u
cd /mnt/c/Users/kubew/aerosls2/.freebuff/worktrees/a8eecbdc-bebf-42d0-8d27-072bf8097d40

./run-cluster.sh --stop >/dev/null 2>&1
rm -f cluster/node*.img cluster/node*.iso cluster/cluster.pids cluster/node*.log

# --force: the capacity gate counts WSL "available" memory, which is low
# right now; three 1 GiB TCG nodes commit only what they touch (the same
# shape booted fine earlier today).
./run-cluster.sh --nodes 3 --force > /tmp/repro_cl.log 2>&1 &
CLPID=$!

up=0
deadline=$(( $(date +%s) + 600 ))
while :; do
    up=0
    for p in 3001 3002 3003; do
        c="$(curl -s -o /dev/null -w '%{http_code}' --max-time 3 http://127.0.0.1:$p/api/cluster 2>/dev/null)"
        [ "$c" = "401" ] || { up=0; break; }
        up=1
    done
    [ "$up" = "1" ] && break
    [ "$(date +%s)" -ge "$deadline" ] && break
    sleep 5
done
[ "$up" = "1" ] || { echo "cluster never came up" >> /tmp/repro_all.log; kill $CLPID 2>/dev/null; exit 2; }
echo "cluster up at $(date +%T)" >> /tmp/repro_all.log

AEROSLS_TOKEN=deadbeef01234567cafebabe76543210 bash tests/env_migrate_confirm_live_check.sh > /tmp/repro_guard.log 2>&1
echo "guard exit=$? at $(date +%T)" >> /tmp/repro_all.log

cp cluster/node1.log /tmp/inc_n1.log 2>/dev/null
cp cluster/node2.log /tmp/inc_n2.log 2>/dev/null
cp cluster/node3.log /tmp/inc_n3.log 2>/dev/null
./run-cluster.sh --stop >/dev/null 2>&1
kill $CLPID 2>/dev/null
echo "done" >> /tmp/repro_all.log
