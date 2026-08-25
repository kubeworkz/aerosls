#!/bin/bash
# 10-boot determinism check
set -e
ISO=/mnt/c/Users/kubew/aerosls2/.freebuff/worktrees/65bb8ebf-765e-484b-92da-3c64b34f9067/sls_operating_system.iso
DET=/mnt/c/Users/kubew/aerosls2/.freebuff/worktrees/65bb8ebf-765e-484b-92da-3c64b34f9067/tools/boot_det.py

clean=0
total=0
for n in $(seq 1 10); do
  timeout 90 qemu-system-x86_64 -cdrom "$ISO" -m 4G -smp 4 -accel tcg -nographic -serial mon:stdio > /tmp/pdet_$n.log 2>/dev/null || true
  result=$(python3 "$DET" < /tmp/pdet_$n.log)
  ok=$(echo "$result" | python3 -c "
import sys, json
d = json.load(sys.stdin)
ok = d['adopted'] and d['phase5'] and not d['unexpected_tag'] and not d['server_error']
print(1 if ok else 0)
" 2>/dev/null || echo 0)
  clean=$((clean + ok))
  total=$((total + 1))
  echo "boot$n: clean=$ok $result"
done
echo "=== RESULT: $clean/$total clean ==="
