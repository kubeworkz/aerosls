#!/usr/bin/env python3
"""polyglot_drift_check.py — compare this run's bench latency against the
previous run's baseline and warn on drift.

Single source of truth for the polyglot-e2e job's soft-drift step. Both the
CI workflow (.github/workflows/ci.yml, step "Warn on latency drift vs
previous baseline") and the gate-teeth smoke (tests/polyglot_latency_gate_
smoke.sh, drift tooth) invoke this script, so a warning the teeth prove is
the exact warning CI emits.

Warnings are printed as GitHub Actions annotations (``::warning::...``),
which are no-ops outside a runner, and never fail the build — this is
advisory only. The hard 5ms gate in the e2e test is the failure boundary;
this surfaces drift (median, compute, or transport growing >= ``threshold``x
between runs) well before that.

Usage:
    python3 tests/polyglot_drift_check.py CURRENT.jsonl PREVIOUS.jsonl [threshold]

Both files are the raw e2e output (or the archived subset) containing
``BENCH_JSON`` lines, one per bench leg, e.g.::

    BENCH_JSON {"leg":"add","transport":"shm","n":1000,"median_ns":8000, ...}

Exit 0 always (advisory); prints the comparison log and any warnings.
"""

import json
import os
import sys


def load(path):
    """Key by (transport, leg) so tcp and shm never cross-compare — a shm
    baseline is ~350x faster on the add leg, so mixing them would spam
    false drift warnings."""
    out = {}
    try:
        fh = open(path)
    except OSError as e:
        print(f"info: cannot open {path}: {e} — skipping drift check")
        return out
    with fh:
        for ln in fh:
            ln = ln.strip()
            if not ln or "BENCH_JSON" not in ln:
                continue
            try:
                j = json.loads(ln.split("BENCH_JSON ", 1)[1])
            except Exception:
                continue
            if not j.get("leg"):
                continue
            out[f"{j.get('transport', 'tcp')}/{j.get('leg')}"] = j
    return out


def warn(what, leg, cval, pval, ratio):
    print(
        f"::warning::latency drift: {leg} {what} {cval} ns is "
        f"{ratio:.2f}x the previous run's {pval} ns — investigate "
        f"before the hard 5ms gate trips"
    )


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 1
    cur_path, prev_path = sys.argv[1], sys.argv[2]
    threshold = float(sys.argv[3]) if len(sys.argv) > 3 else 2.0

    cur = load(cur_path)
    prev = load(prev_path)
    if not prev:
        print("no previous baseline legs — nothing to compare (first run)")
        return 0
    if not cur:
        print("info: current bench file has no BENCH_JSON lines — nothing to compare")
        return 0

    for key in sorted(cur):
        c, p = cur[key], prev.get(key)
        leg = f"{c.get('transport', 'tcp')}/{c.get('leg')}"
        if not p or not p.get("median_ns"):
            print(f"info: {leg}: no previous baseline — skipping")
            continue

        # Total median.
        cmed, pmed = c["median_ns"], p["median_ns"]
        ratio = cmed / pmed if pmed else float("inf")
        print(f"info: {leg}: median {cmed} ns vs previous {pmed} ns ({ratio:.2f}x)")
        if ratio >= threshold:
            warn("median", leg, cmed, pmed, ratio)

        # Split components (compute + derived transport). Old baselines
        # lack compute_ns — skip the split check when either side misses
        # it, so a first-baseline-after-upgrade run is clean.
        ccomp = c.get("compute_ns")
        pcomp = p.get("compute_ns")
        if ccomp is None or pcomp is None:
            print(f"info: {leg}: no compute_ns in one side — split check skipped")
            continue
        # transport is derived as total - compute (the sidecar only emits
        # it in the human line, not the JSON), so recompute it here
        # consistently for both runs.
        ctrans = max(0, cmed - ccomp)
        ptrans = max(0, pmed - pcomp)
        cratio = ccomp / pcomp if pcomp else float("inf")
        tratio = ctrans / ptrans if ptrans else float("inf")
        print(
            f"info: {leg}: compute {ccomp} ns vs previous {pcomp} ns ({cratio:.2f}x); "
            f"transport {ctrans} ns vs previous {ptrans} ns ({tratio:.2f}x)"
        )
        if cratio >= threshold:
            warn("compute", leg, ccomp, pcomp, cratio)
        if ptrans and tratio >= threshold:
            warn("transport", leg, ctrans, ptrans, tratio)

    return 0


if __name__ == "__main__":
    sys.exit(main())
