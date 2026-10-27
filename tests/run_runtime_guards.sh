#!/usr/bin/env bash
# One-shot: run the runtime guards that run_checks.sh owed, against the
# already-running 3-node cluster. Sequential (they share the cluster), token
# supplied, each exit status recorded.
set -u
cd "$(dirname "$0")/.."
export AEROSLS_TOKEN=deadbeef01234567cafebabe76543210

# failover_2node_live_check.sh is NOT here: it refuses a cluster whose
# cluster.pids names anything but exactly 2 nodes. It runs last, against its
# own 2-node cluster (see the caller's notes), after these finish.
GUARDS="
entropy_boot_diversity_check.sh
partition_reannounce_live_check.sh
partition_ownedset_gc_live_check.sh
env_migrate_confirm_live_check.sh
e3_multi_env_boot_check.sh
failover_adoption_live_check.sh
env_catalog_live_check.sh
"

overall=0
for g in $GUARDS; do
    echo "=== $g"
    if ./tests/$g; then
        echo "OK   $g"
    else
        rc=$?
        echo "BAD  $g (rc=$rc)"
        overall=1
    fi
done
echo "RUNTIME-GUARDS overall: $([ $overall -eq 0 ] && echo PASS || echo FAIL)"
exit $overall
