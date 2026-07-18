## **AI Agent & Workflow Showcase Plan**

### **What the kernel already supports**

The agent engine is a first-class kernel subsystem with:

- **ReAct loop** (up to 8 steps per `agent_run`) — Reason, Act, Observe cycle
- **Tools the agent can call**: `db_select`, `db_insert`, `db_query`, `tier_promote`, `stream_read`, `agent_run` (sub-agent chaining)
- **Persistent memory** — `last_answer` stored in a DB_TABLE, survives reboots
- **Conversation history** — stored in a STREAM object
- **Workflow chaining** — pipes `last_answer` from step N → input of step N+1
- **LLM inference** — any OpenAI-compatible HTTP endpoint (can point at local Ollama or cloud)

### **Proposed Agents (IBM AIOps parallels)**

**1.** `StorageTierOptimizer`

> *IBM Turbonomic analogue* — resource optimization

```
System prompt: "You are a storage tier optimizer for AeroSLS. Query the
object catalog for access patterns. Promote objects with high access
frequency to L1_CACHE. Report objects that should be demoted."

Tools: db_query, tier_promote
Memory table: tier_optimizer_memory
```

- On each run: scans `idle` ticks from `/api/tiers`, promotes the hottest object
- Demonstrates: autonomous infrastructure management without human intervention

**2.** `DataQualityAuditor`

> *IBM DataStage / OpenPages analogue* — data governance

```
System prompt: "You are a data quality auditor. Read DB_TABLE records,
detect missing or malformed fields, insert corrected values, and write
an audit summary to the audit_log table."

Tools: db_select, db_insert, db_query
Memory table: audit_memory
```

- On each run: queries a table, finds anomalies, inserts `audit_status` field
- Demonstrates: AI-driven data remediation

**3.** `SecurityPolicyEnforcer`

> *IBM Guardium analogue* — data security monitoring

```
System prompt: "You are a security policy monitor for AeroSLS. Query all
DB_TABLE objects and verify that sensitive tables have appropriate role
restrictions. Insert policy_status records flagging violations."

Tools: db_query, db_insert
Memory table: security_mem
```

- On each run: scans object permissions, flags guest-accessible sensitive tables
- Demonstrates: continuous compliance enforcement

**4.** `KernelMetricsAnalyst`

> *IBM Instana analogue* — observability

```
System prompt: "You are a kernel performance analyst. Read live metrics
(uptime, object count, tier distribution) and insert an analysis summary
into the metrics_log table."

Tools: db_insert, db_query
Memory table: metrics_mem
```

- On each run: reads `/api/metrics` + `/api/tiers` data (via db_query), writes trend analysis
- Demonstrates: AI-powered observability

---

### **Proposed Workflows (IBM Automation Platform analogue)**



**Workflow 1:** `DataOnboarding`

> *IBM DataPower / Integration analogue*


| Step | Agent             | Input             | Output                      |
| ---- | ----------------- | ----------------- | --------------------------- |
| 1    | `SchemaInspector` | stream name       | detected schema             |
| 2    | `DataValidator`   | schema + stream   | validation report           |
| 3    | `TierPlanner`     | validation result | tier assignments + promotes |




**Workflow 2:** `AutonomousHealthCheck`

> *IBM Watson AIOps analogue*


| Step | Agent              | Input           | Output                    |
| ---- | ------------------ | --------------- | ------------------------- |
| 1    | `MetricsCollector` | "collect now"   | metrics summary           |
| 2    | `AnomalyDetector`  | metrics summary | anomaly list              |
| 3    | `RemediationBot`   | anomaly list    | remediation actions taken |




**Workflow 3:** `ComplianceAudit`

> *IBM OpenPages analogue*


| Step | Agent             | Input               | Output                  |
| ---- | ----------------- | ------------------- | ----------------------- |
| 1    | `PolicyScanner`   | "audit all objects" | violations found        |
| 2    | `ReportGenerator` | violations          | compliance report       |
| 3    | `ArchiveBot`      | compliance report   | report stored as STREAM |


---

### **Implementation Approach**



**Phase A — Kernel pre-configured agents (REST API)**  
Create agents and workflows via `POST /api/agent/create` and `POST /api/workflow/create` from a Python setup script that runs once. Agents persist across reboots via the persist layer.

```
python3 utils/agent_setup.py \
  --endpoint "10.0.2.2:11434"  # Ollama on host, or cloud endpoint
  --model "llama3.1:8b"
```



**Phase B — Frontend wiring**  
The [SlsAgentManager.tsx](vscode-file://vscode-app/c:/Users/kubew/AppData/Local/Programs/Microsoft%20VS%20Code/125df4672b/resources/app/out/vs/code/electron-browser/workbench/workbench.html) and [SlsWorkflowBuilder.tsx](vscode-file://vscode-app/c:/Users/kubew/AppData/Local/Programs/Microsoft%20VS%20Code/125df4672b/resources/app/out/vs/code/electron-browser/workbench/workbench.html) tabs currently show hardcoded demo data. Wire them to `GET /api/agents` and `GET /api/workflows` (real kernel endpoints) and add **Run** buttons that call `POST /api/agent/run` with a one-line prompt.



**Phase C — Live demo data**  
Pre-load 2-3 DB_TABLE objects (e.g., `customer_records`, `sensor_data`, `audit_log`) so agents have something real to read and modify — making the demo self-contained.

---

### **Priority order**

1. `utils/agent_setup.py` — CLI that creates all agents + workflows via the kernel API
2. **Kernel** `GET /api/workflows` — currently missing endpoint (create is done, list is not)
3. **Frontend agent panel** — replace hardcoded demo with live `/api/agents` data + Run buttons
4. **Frontend workflow panel** — same, wire to `/api/workflows` + Run buttons
5. **Demo data loader** — `utils/demo_data.py` that populates sample tables

Want to start with the agent_setup.py script, or with the frontend wiring first?



**Usage summary:**

```
# Default setup (idempotent — safe to re-run)
python3 utils/agent_setup.py

# Point at a different LLM
python3 utils/agent_setup.py --endpoint 192.168.1.100:11434 --model mistral

# Trigger all 4 agents with starter prompts (requires LLM reachable)
python3 utils/agent_setup.py --run-demo

# Run a specific workflow
python3 utils/agent_setup.py --run-workflow health_check_wf

# Hard reset (drop + recreate everything)
python3 utils/agent_setup.py --reset
```



**Next steps** from the plan:

1. **Frontend wiring** — replace hardcoded demo agents in [SlsAgentManager.tsx](vscode-file://vscode-app/c:/Users/kubew/AppData/Local/Programs/Microsoft%20VS%20Code/125df4672b/resources/app/out/vs/code/electron-browser/workbench/workbench.html) and [SlsWorkflowBuilder.tsx](vscode-file://vscode-app/c:/Users/kubew/AppData/Local/Programs/Microsoft%20VS%20Code/125df4672b/resources/app/out/vs/code/electron-browser/workbench/workbench.html) with live `/api/agents` and `/api/workflows` data + Run buttons
2. `utils/demo_data.py` — richer sample datasets for agents to reason over

