#!/usr/bin/env bash
# 2-node regression for the confirmation guard after the idle-observer arm:
# fresh 2-node cluster (the guard's original, safe shape), same capture and
# cleanup contract as tools/repro_observer_incident.sh.
set -u
cd /mnt/c/Users/kubew/aerosls2/.freebuff/worktrees/a8eecbdc-bebf-42d0-8d27-072bf8097d40

./run-cluster.sh --stop >/dev/null 2>&1
rm -f cluster/node*.img cluster/node*.iso cluster/cluster.pids cluster/node*.log

./run-cluster.sh --nodes 2 --force > /tmp/reg2_cl.log 2>&1 &
CLPID=$!

up=0
deadline=$(( $(date +%s) + 600 ))
while :; do
    up=0
    for p in 3001 3002; do
        c="$(curl -s -o /dev/null -w '%{http_code}' --max-time 3 http://127.0.0.1:$p/api/cluster 2>/dev/null)"
        [ "$c" = "401" ] || { up=0; break; }
        up=1
    done
    [ "$up" = "1" ] && break
    [ "$(date +%s)" -ge "$deadline" ] && break
    sleep 5
done
[ "$up" = "1" ] || { echo "cluster never came up" >> /tmp/reg2_all.log; kill $CLPID 2>/dev/null; exit 2; }
echo "cluster up at $(date +%T)" >> /tmp/reg2_all.log

AEROSLS_TOKEN=deadbeef01234567cafebabe76543210 bash tests/env_migrate_confirm_live_check.sh > /tmp/reg2_guard.log 2>&1
echo "guard exit=$? at $(date +%T)" >> /tmp/reg2_all.log

cp cluster/node1.log /tmp/reg2_n1.log 2>/dev/null
cp cluster/node2.log /tmp/reg2_n2.log 2>/dev/null
./run-cluster.sh --stop >/dev/null 2>&1
kill $CLPID 2>/dev/null
echo "done" >> /tmp/reg2_all.log
