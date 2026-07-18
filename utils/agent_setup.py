#!/usr/bin/env python3
"""
AeroSLS AI Agent & Workflow Setup
==================================
Creates a curated set of IBM-style AIOps agents and multi-step workflows
directly in the live AeroSLS kernel via the REST API.

Agents created
--------------
  tier_optimizer    — Autonomous storage tier manager (promote hot objects)
  data_auditor      — Data quality auditor (scan tables, flag anomalies)
  security_monitor  — Security policy enforcer (scan permissions, log violations)
  kernel_analyst    — Kernel metrics analyst (collect stats, generate reports)

Workflows created
-----------------
  health_check_wf       — kernel_analyst → tier_optimizer (2-step AIOps loop)
  data_governance_wf    — data_auditor   → security_monitor (compliance audit)

Demo data tables created
------------------------
  sensor_data      — sample IoT readings for the auditor to inspect
  audit_log        — destination for audit results
  metrics_log      — destination for analyst reports

Usage
-----
  # Create everything (idempotent — skips existing resources)
  python3 utils/agent_setup.py

  # Point at a different LLM endpoint (default: 10.0.2.2:11434 = Ollama on host)
  python3 utils/agent_setup.py --endpoint 10.0.2.2:11434 --model llama3.2

  # Run a quick demo (trigger each agent with a starter prompt)
  python3 utils/agent_setup.py --run-demo

  # Drop all managed agents and reset demo data, then recreate
  python3 utils/agent_setup.py --reset
"""

import argparse
import json
import sys
import time
import urllib.error
import urllib.request

# ── Defaults ──────────────────────────────────────────────────────────────────

DEFAULT_HOST     = "localhost:3001"
DEFAULT_TOKEN    = "deadbeef01234567cafebabe76543210"   # dave@gridworkz — DB_ADMIN
DEFAULT_ENDPOINT = "10.0.2.2:11434"                    # Ollama on QEMU host
DEFAULT_MODEL    = "llama3.2"

# ── Agent definitions ──────────────────────────────────────────────────────────

def make_agents(endpoint: str, model: str) -> list:
    return [
        {
            "name":    "tier_optimizer",
            "endpoint": endpoint,
            "model":    model,
            "system_prompt": (
                "You are a storage tier optimizer running inside the AeroSLS kernel. "
                "Your job is to monitor the object catalog and automatically promote "
                "frequently-accessed objects to L1 cache (the fastest tier). "
                "Use db_query to inspect the current tier distribution, then call "
                "tier_promote for each object that should move to a faster tier. "
                "After promoting, use db_insert to write a summary of your actions "
                "to the 'metrics_log' table under the key 'last_tier_action'. "
                "Be concise and action-oriented. Never ask clarifying questions."
            ),
            "tools": "db_query,tier_promote,db_insert",
            "description": "Autonomous storage tier manager — promotes hot objects to L1 cache",
        },
        {
            "name":    "data_auditor",
            "endpoint": endpoint,
            "model":    model,
            "system_prompt": (
                "You are a data quality auditor for the AeroSLS object store. "
                "Use db_select to read records from tables such as 'sensor_data', "
                "then evaluate the data for completeness and anomalies "
                "(missing fields, out-of-range values, stale timestamps). "
                "Use db_insert to write your findings to the 'audit_log' table "
                "with keys like 'status', 'anomalies_found', 'recommendation'. "
                "Be thorough, specific, and professional. Format findings as "
                "structured summaries. Never fabricate data you have not read."
            ),
            "tools": "db_select,db_insert,db_query",
            "description": "Data quality auditor — scans tables and logs findings to audit_log",
        },
        {
            "name":    "security_monitor",
            "endpoint": endpoint,
            "model":    model,
            "system_prompt": (
                "You are a security policy monitor for the AeroSLS kernel. "
                "Use db_query to list all active objects in the catalog. "
                "Check whether sensitive tables (those with 'log', 'audit', or "
                "'secure' in their name) have appropriate access controls. "
                "Insert a 'policy_status' record into 'audit_log' summarising any "
                "violations or confirming compliance. "
                "Keep your analysis factual and grounded in the data you retrieve."
            ),
            "tools": "db_query,db_insert",
            "description": "Security policy enforcer — scans object permissions, logs policy status",
        },
        {
            "name":    "kernel_analyst",
            "endpoint": endpoint,
            "model":    model,
            "system_prompt": (
                "You are a kernel performance analyst for AeroSLS. "
                "Use db_query to gather live metrics: object count, tier distribution "
                "(how many objects are in L1_CACHE, L2_DRAM, L3_SSD), and IPC stats. "
                "Synthesize the data into a concise performance summary. "
                "Store your summary in the 'metrics_log' table under the keys "
                "'kernel_health', 'tier_distribution', and 'recommendation'. "
                "Always base your conclusions strictly on data retrieved by your tools."
            ),
            "tools": "db_query,db_insert",
            "description": "Kernel metrics analyst — collects live stats and generates health reports",
        },
    ]


# ── Workflow definitions ───────────────────────────────────────────────────────

WORKFLOWS = [
    {
        "name":         "health_check_wf",
        "shared_table": "wf_health_state",
        "description":  "AIOps loop: analyst collects metrics → optimizer tunes tiers",
        "steps": [
            {"agent": "kernel_analyst",  "in": "task",      "out": "analysis"},
            {"agent": "tier_optimizer",  "in": "analysis",  "out": "result"},
        ],
        "starter": "Perform a full kernel health check and optimise storage tiers.",
    },
    {
        "name":         "data_governance_wf",
        "shared_table": "wf_governance_state",
        "description":  "Compliance audit: data auditor scans → security monitor enforces",
        "steps": [
            {"agent": "data_auditor",     "in": "task",   "out": "audit_result"},
            {"agent": "security_monitor", "in": "audit_result", "out": "policy_report"},
        ],
        "starter": "Run a full data governance audit including quality and security checks.",
    },
]


# ── Demo data ──────────────────────────────────────────────────────────────────

DEMO_DATA = [
    # sensor_data — synthetic IoT readings for the auditor to inspect
    ("sensor_data",  "device_id",    "SENSOR-001"),
    ("sensor_data",  "temperature",  "72.4"),
    ("sensor_data",  "humidity",     "45"),
    ("sensor_data",  "status",       "nominal"),
    ("sensor_data",  "last_reading", "2026-07-17T18:00:00Z"),
    ("sensor_data",  "battery_pct",  "87"),
    # audit_log — destination table for auditor and security monitor
    ("audit_log",    "status",       "pending"),
    ("audit_log",    "created_at",   "2026-07-17T18:00:00Z"),
    # metrics_log — destination table for the kernel analyst
    ("metrics_log",  "status",       "pending"),
    ("metrics_log",  "created_at",   "2026-07-17T18:00:00Z"),
]


# ── HTTP helpers ───────────────────────────────────────────────────────────────

def _base(host: str) -> str:
    h = host.rstrip("/")
    return h if h.startswith(("http://", "https://")) else "http://" + h


def _post(base: str, path: str, payload: dict, token: str,
          timeout: int = 60) -> dict:
    url  = base + path
    body = json.dumps(payload).encode()
    req  = urllib.request.Request(url, data=body, method="POST")
    req.add_header("Content-Type",  "application/json")
    req.add_header("Authorization", f"Bearer {token}")
    try:
        with urllib.request.urlopen(req, timeout=timeout) as r:
            return json.loads(r.read())
    except urllib.error.HTTPError as exc:
        return {"ok": "false", "error": f"HTTP {exc.code}: {exc.read().decode()}"}
    except urllib.error.URLError as exc:
        return {"ok": "false", "error": str(exc.reason)}


def _get(base: str, path: str, token: str) -> dict:
    url = base + path
    req = urllib.request.Request(url)
    req.add_header("Authorization", f"Bearer {token}")
    try:
        with urllib.request.urlopen(req, timeout=15) as r:
            return json.loads(r.read())
    except Exception as exc:
        return {"ok": "false", "error": str(exc)}


# ── Create helpers ─────────────────────────────────────────────────────────────

def ensure_table(base: str, token: str, name: str) -> None:
    """Create a DB_TABLE object if it doesn't already exist."""
    resp = _post(base, "/api/valloc",
                 {"name": name, "type": 1, "pages": 4}, token)
    if resp.get("ok") == "true":
        print(f"  [+] table '{name}' created  (id={resp.get('object_id')})")
    else:
        print(f"  [~] table '{name}' already exists — skipping")


def insert_record(base: str, token: str, table: str, key: str, val: str) -> None:
    _post(base, "/api/record", {"object": table, "key": key, "value": val}, token)


def create_agent(base: str, token: str, ag: dict) -> bool:
    resp = _post(base, "/api/agent/create", {
        "name":          ag["name"],
        "endpoint":      ag["endpoint"],
        "model":         ag["model"],
        "system_prompt": ag["system_prompt"],
        "tools":         ag["tools"],
    }, token)
    ok = resp.get("ok") == "true"
    mask = resp.get("tool_mask", 0)
    if ok:
        print(f"  [+] agent '{ag['name']}'  tools=0x{mask:02x}  — {ag['description']}")
    else:
        err = resp.get("error", "unknown error")
        if "already" in err.lower() or "exists" in err.lower():
            print(f"  [~] agent '{ag['name']}' already exists — skipping")
        else:
            print(f"  [!] agent '{ag['name']}' FAILED: {err}", file=sys.stderr)
    return ok


def create_workflow(base: str, token: str, wf: dict) -> bool:
    payload: dict = {
        "name":         wf["name"],
        "shared_table": wf["shared_table"],
        "step_count":   len(wf["steps"]),
    }
    for i, step in enumerate(wf["steps"]):
        payload[f"step{i}_agent"] = step["agent"]
        payload[f"step{i}_in"]    = step["in"]
        payload[f"step{i}_out"]   = step["out"]

    # also create the shared state table
    ensure_table(base, token, wf["shared_table"])

    resp = _post(base, "/api/workflow/create", payload, token)
    ok = resp.get("ok") == "true"
    if ok:
        steps_str = " → ".join(s["agent"] for s in wf["steps"])
        print(f"  [+] workflow '{wf['name']}'  [{steps_str}]  — {wf['description']}")
    else:
        err = resp.get("error", "unknown error")
        if "already" in err.lower() or "exists" in err.lower():
            print(f"  [~] workflow '{wf['name']}' already exists — skipping")
        else:
            print(f"  [!] workflow '{wf['name']}' FAILED: {err}", file=sys.stderr)
    return ok


def drop_agent(base: str, token: str, name: str) -> None:
    resp = _post(base, "/api/agent/drop", {"name": name}, token)
    if resp.get("ok") == "true":
        print(f"  [-] dropped agent '{name}'")
    else:
        print(f"  [?] drop agent '{name}': {resp.get('error', 'not found')}")


def run_agent(base: str, token: str, name: str, message: str) -> dict:
    print(f"\n  Running agent '{name}'...")
    print(f"  Message: {message[:80]}{'...' if len(message) > 80 else ''}")
    t0 = time.time()
    resp = _post(base, "/api/agent/run", {"name": name, "message": message},
                 token, timeout=120)
    elapsed = time.time() - t0
    if resp.get("ok") == "true":
        steps = resp.get("steps", "?")
        print(f"  ✓ completed in {elapsed:.1f}s  ({steps} ReAct step(s))")
    else:
        print(f"  ✗ failed: {resp.get('error', 'unknown')}", file=sys.stderr)
    return resp


def run_workflow(base: str, token: str, name: str, input_msg: str) -> dict:
    print(f"\n  Running workflow '{name}'...")
    print(f"  Input: {input_msg[:80]}{'...' if len(input_msg) > 80 else ''}")
    t0 = time.time()
    resp = _post(base, "/api/workflow/run", {"name": name, "input": input_msg},
                 token, timeout=300)
    elapsed = time.time() - t0
    if resp.get("ok") == "true":
        print(f"  ✓ workflow completed in {elapsed:.1f}s")
    else:
        print(f"  ✗ failed: {resp.get('error', 'unknown')}", file=sys.stderr)
    return resp


# ── Main ───────────────────────────────────────────────────────────────────────

def main() -> None:
    parser = argparse.ArgumentParser(
        description="AeroSLS AI Agent & Workflow Setup",
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument("--host",     default=DEFAULT_HOST,     metavar="HOST:PORT")
    parser.add_argument("--token",    default=DEFAULT_TOKEN,    metavar="TOKEN")
    parser.add_argument("--endpoint", default=DEFAULT_ENDPOINT, metavar="HOST:PORT",
                        help="LLM inference endpoint (OpenAI-compatible)")
    parser.add_argument("--model",    default=DEFAULT_MODEL,    metavar="MODEL",
                        help="Model name passed to the inference server")
    parser.add_argument("--run-demo", action="store_true",
                        help="After setup, trigger each agent with a starter prompt")
    parser.add_argument("--run-workflow", metavar="NAME",
                        help="Run a specific workflow by name")
    parser.add_argument("--reset",    action="store_true",
                        help="Drop all managed agents before recreating them")
    args = parser.parse_args()

    base = _base(args.host)

    # ── Verify kernel is reachable ─────────────────────────────────────────────
    health = _get(base, "/api/health", args.token)
    if health.get("status") != "ok":
        print(f"✗ Kernel not reachable at {base}: {health}", file=sys.stderr)
        sys.exit(1)
    ticks = health.get("uptime_ticks", 0)
    print(f"✓ Kernel online  uptime={ticks//100}s  objects={health.get('object_count', '?')}")
    print(f"  LLM endpoint : {args.endpoint}  model={args.model}\n")

    agents = make_agents(args.endpoint, args.model)

    # ── Reset: drop agents ─────────────────────────────────────────────────────
    if args.reset:
        print("── Reset: dropping managed agents ───────────────────────────────")
        for ag in agents:
            drop_agent(base, args.token, ag["name"])
        print()

    # ── Demo data ──────────────────────────────────────────────────────────────
    print("── Demo data tables ─────────────────────────────────────────────────")
    for table_name in dict.fromkeys(row[0] for row in DEMO_DATA):
        ensure_table(base, args.token, table_name)
    print(f"  Seeding {len(DEMO_DATA)} records...")
    for table, key, val in DEMO_DATA:
        insert_record(base, args.token, table, key, val)
    print()

    # ── Agents ────────────────────────────────────────────────────────────────
    print("── AI Agents ────────────────────────────────────────────────────────")
    for ag in agents:
        create_agent(base, args.token, ag)
    print()

    # ── Workflows ─────────────────────────────────────────────────────────────
    print("── Workflows ────────────────────────────────────────────────────────")
    for wf in WORKFLOWS:
        create_workflow(base, args.token, wf)
    print()

    # ── Summary ───────────────────────────────────────────────────────────────
    alive = _get(base, "/api/agents", args.token)
    wf_list = _get(base, "/api/workflows", args.token)
    n_agents = alive.get("count", len(agents))
    n_wf     = wf_list.get("count", len(WORKFLOWS))
    print(f"── Setup complete: {n_agents} agent(s), {n_wf} workflow(s) registered ─")
    print()
    print("  Run an agent:     POST /api/agent/run    {\"name\":\"tier_optimizer\",\"message\":\"...\"}")
    print("  Run a workflow:   POST /api/workflow/run  {\"name\":\"health_check_wf\",\"input\":\"...\"}")
    print()
    print("  Or re-run this script with --run-demo to trigger all agents automatically.")

    # ── Optional demo run ─────────────────────────────────────────────────────
    if args.run_demo:
        print("\n── Demo run ─────────────────────────────────────────────────────────")
        starters = {
            "tier_optimizer":  "Check the current tier distribution and promote any objects "
                               "that belong in L1 cache based on their access patterns.",
            "data_auditor":    "Audit the sensor_data table for data quality issues. "
                               "Check for missing fields, stale timestamps, or anomalous values.",
            "security_monitor":"Scan all objects in the catalog and verify that audit_log "
                               "and metrics_log have appropriate access restrictions.",
            "kernel_analyst":  "Collect the current kernel metrics including object count, "
                               "tier distribution, and IPC throughput. Generate a health report.",
        }
        for ag in agents:
            run_agent(base, args.token, ag["name"], starters[ag["name"]])

    if args.run_workflow:
        wf_cfg = next((w for w in WORKFLOWS if w["name"] == args.run_workflow), None)
        if not wf_cfg:
            print(f"✗ Unknown workflow '{args.run_workflow}'", file=sys.stderr)
            sys.exit(1)
        run_workflow(base, args.token, wf_cfg["name"], wf_cfg["starter"])


if __name__ == "__main__":
    main()
