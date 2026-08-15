An advanced RISC-V 64-bit server/AI chip like the XuanTie C930/C950 class would look like a modern server SoC: multiple wide out-of-order RV64 cores, large coherent caches, vector/matrix engines, DDR5/HBM, PCIe/CXL, virtualization, security, and RAS.

I don’t have T-Head’s internal RTL or confidential implementation details, but the architecture-level features are mostly public/standardized. You can reuse the standards-based features in your own design without copying the proprietary microarchitecture.

## What an advanced RISC-V64 server/AI chip would include

### 1. Core microarchitecture

- **RVA22/RVA23 profile compliance**
  - RV64I + M/A/F/D/C + Bitmanip + Vector + Hypervisor + cache-management ops
  - RVA23 is the right target for a new server-grade design
- **Wide superscalar out-of-order**
  - 6–8 wide decode/issue
  - 12–15 stage pipeline
  - Large ROB, physical register files, deep load/store queues
  - Aggressive branch prediction: TAGE, perceptron, RAS, indirect predictors
- **Execution units**
  - Multiple integer ALUs/AGUs
  - 2–4 load/store units
  - FPU and vector units
- **RVV 1.0 vector engine**
  - VLEN 256–512 bits
  - Multiple vector pipes, chaining, masked operations
  - Vector crypto: Zvbb, Zvbc, Zvkg, Zvkned, etc.
- **Matrix/tensor acceleration**
  - Especially for the AI-focused C950 class
  - Could be a RISC-V Matrix extension or a vendor-specific tensor unit
  - Fused vector/matrix datapath for transformer/LLM operators

### 2. Cache and memory hierarchy

- L1 I/D: 32–64 KB each
- L2: 256 KB–1 MB per core or cluster
- L3/LLC: 16–64 MB shared, multi-banked
- Coherent interconnect: AMBA CHI or AXI5 with directory/snoop filter
- Memory controllers:
  - Server: 4–12 channels DDR5/LPDDR5X
  - AI: HBM3e for high bandwidth
- PCIe Gen5/Gen6
- CXL 2.0/3.0 for memory expansion/coherent accelerators
- UCIe for chiplet-based designs

### 3. Virtualization, interrupts, and platform

- Hypervisor extension **H**
- RISC-V **IOMMU**
- **AIA**: APLIC + IMSIC for MSI and low-latency interrupts
- Sstc, Sscofpmf, Svpbmt, Svadu, Svinval
- PCIe MSI-X, IOMMU-based device isolation

### 4. Security

- ePMP / PMP
- IOPMP for I/O isolation
- Secure boot, TEE, root of trust
- Crypto extensions: scalar Zk and vector crypto
- Side-channel hardening

### 5. AI/agentic-AI features

For an “agentic AI” chip like C950, you would add:

- Matrix/tensor engine or tightly coupled NPU
- Shared virtual memory between CPU and accelerators
- Coherent accelerator interface
- High-bandwidth memory for model weights
- Low-latency interrupt handling for tool calling/RAG/agent loops
- Possibly custom instructions for AI runtime operations

### 6. RAS, debug, and power

- ECC/parity on caches, TLBs, register files
- Machine check architecture and error records
- Sdext, Smtrace for debug/trace
- DVFS, power gating, per-core power states
- Telemetry and thermal management

## What is known publicly about C930/C950

- **XuanTie C930**
  - Marketed as server-grade
  - Wide out-of-order RISC-V core
  - RVV 1.0 vector
  - Virtualization, IOMMU, AIA
  - DDR5, PCIe Gen5, CXL
  - Large shared L3 cache
  - Targets cloud/server/HPC
- **XuanTie C950**
  - Described as AI/agentic-AI focused
  - Likely adds matrix/tensor acceleration
  - Larger vector width, higher memory bandwidth, possibly HBM
  - Optimized for LLM inference, RAG, agentic workloads

## Features you can reuse in your own RISC-V design

Yes. The following are standards-based or industry-standard and are not proprietary:

- **RVA22/RVA23 ISA profiles**
- **RVV 1.0 vector extension**
- **RISC-V Matrix extension** if available, or a custom tensor unit
- **Hypervisor extension**
- **AIA interrupt architecture**
- **RISC-V IOMMU**
- **AMBA CHI/AXI** coherent fabric
- **PCIe, CXL, DDR5/LPDDR5X/HBM**
- **Security extensions**: ePMP, IOPMP, crypto
- **RAS, debug, trace specs**

The proprietary part is the specific microarchitecture/RTL. You can legally implement the same standard features with your own design.

## Practical recommendation

If you are building a new advanced RISC-V64 chip:

1. Target **RVA23**.
2. Implement **RVV 1.0 with at least 256-bit VLEN**, plus vector crypto.
3. Add a **matrix/tensor engine** for AI.
4. Use **CHI/AXI coherent interconnect** and large shared L3.
5. Include **IOMMU + AIA** from day one for server use.
6. Plan for **DDR5/HBM** and **PCIe/CXL**.
7. Add **security and RAS** early, not as an afterthought.

That would give you a C930/C950-class architecture without copying T-Head’s proprietary RTL.

---

# GRX-930 Architecture & Microarchitecture Specification

**Gridworkz RISC-V Server/AI SoC — C930-class** Revision 0.1 · 2026-08-15 · Status: Draft for review

---

## 0. Document scope and legal position

This specification defines a clean-room, standards-based RV64 server/AI SoC targeting the same market segment and feature set as the Alibaba T-Head XuanTie C930/C950 class of cores.

Everything specified here derives from **public, open standards**: RISC-V ratified and in-flight extensions, RVA23 profile, AMBA CHI/AXI, PCIe/CXL, JEDEC DDR5/HBM. No T-Head RTL, microarchitectural trade secrets, or confidential documentation is used or referenced. Where public C930/C950 datasheet figures are cited, they are cited only as *market positioning targets* — the microarchitecture that meets them here is our own.

Reference implementation baseline: the open-source `MuhammadMajiid/RV64IMAC` core (Alexandria University, SystemVerilog, 5-stage in-order RV64IMAC + L1 caches + branch prediction + privilege modes). It is used as an ISA/decode/privilege reference and as an integration harness for early bring-up, **not** as the microarchitectural target.

---

## 1. Product definition

### 1.1 Positioning

<table>
  <tr><th></th><th>GRX-930</th></tr>
  <tr><td>Segment</td><td>Cloud server, HPC, AI inference host</td></tr>
  <tr><td>ISA profile</td><td>RVA23U64 / RVA23S64</td></tr>
  <tr><td>Core type</td><td>Wide superscalar out-of-order RV64</td></tr>
  <tr><td>Peak core count</td><td>64 (8 clusters × 8 cores)</td></tr>
  <tr><td>Target frequency</td><td>2.8 GHz nominal / 3.2 GHz boost</td></tr>
  <tr><td>Target performance</td><td>≥ 20 SPECint2006-base/GHz, ≥ 9.5 SPECint2017-rate-base/core-GHz</td></tr>
  <tr><td>Process class</td><td>5 nm-class FinFET or 3 nm-class GAA</td></tr>
  <tr><td>AI capability</td><td>RVV 1.0 (VLEN 256, dual datapath) + IME matrix + optional attached TPE</td></tr>
</table>

Public reference points used as targets (not as design inputs): the XuanTie C950 datasheet advertises 8-wide decode, up to 3.2 GHz, ~22 SPECint2006/GHz, VLEN 256 dual datapath, 64 KB 4-way L1D at 4-cycle load-to-use, 256 KB–3 MB private L2, 0–8 MB shared L3, CHI.E/CHI.F at 256-bit, AIA v1.0, Sv57/48/39 with PA48, and RERI RAS.

### 1.2 Design principles

1. **Standards first.** Every architectural surface is a ratified or public-review RISC-V, ARM AMBA, PCI-SIG, CXL, or JEDEC specification. No custom ISA where a standard exists.
2. **The vector register file is the AI datapath.** Matrix acceleration reuses `v0–v31` (see §5). This is not a cost-saving compromise — it is what makes the matrix engine context-switchable, virtualizable, and binary-portable.
3. **Decoupled, re-targetable blocks.** Every major block presents a specified interface and is verifiable standalone. Blocks built against the in-order bring-up harness must drop into the out-of-order core without a rewrite.
4. **RAS, security, and virtualization are day-one architecture,** not a later revision.

---

## 2. SoC topology

```plaintext
                          ┌────────────────────────────────────────────┐
                          │            GRX-930 SoC (up to 64C)         │
   DDR5 ×8 ch ────────────┤                                            │
   (or HBM3e ×4 stacks)   │   ┌──────────┐  ┌──────────┐               │
                          │   │ Cluster0 │  │ Cluster1 │  ... ×8       │
   PCIe Gen5 ×64 ─────────┤   │  8 cores │  │  8 cores │               │
   CXL 2.0 (.mem/.cache)  │   │  L2 priv │  │  L2 priv │               │
                          │   └────┬─────┘  └────┬─────┘               │
   UCIe (chiplet, opt) ───┤        │             │                     │
                          │   ═════╪═════════════╪═══════ CHI.E mesh   │
                          │        │             │        256-bit      │
                          │   ┌────┴─────────────┴────┐                │
                          │   │  L3 / LLC 32 MB       │                │
                          │   │  16 banks + SF/dir    │                │
                          │   └───────────────────────┘                │
                          │   IOMMU · APLIC · IMSIC · RoT · RERI       │
                          └────────────────────────────────────────────┘


```

### 2.1 Cluster

- 8 × GRX-930 cores.
- Private per-core L2, 1 MB, 16-way, strictly inclusive of L1 (config 256 KB … 2 MB).
- Cluster CHI.E Request Node bridge, 256-bit data.
- Cluster-level DVFS domain with per-core power gating and per-core clock gating.
- Optional per-cluster attached TPE (§5.4) sharing the cluster L2 port.

### 2.2 Interconnect

<table>
  <tr><th>Parameter</th><th>Value</th></tr>
  <tr><td>Protocol</td><td>AMBA CHI Issue E (CHI.E), optional CHI.F</td></tr>
  <tr><td>Topology</td><td>2D mesh, 4×4 for 64-core</td></tr>
  <tr><td>Data width</td><td>256-bit per link direction</td></tr>
  <tr><td>Coherence</td><td>MOESI-equivalent CHI states, home-node directory + snoop filter</td></tr>
  <tr><td>SF coverage</td><td>1.5× aggregate L2 tags, 16-way, per home node</td></tr>
  <tr><td>Ordering</td><td>CHI ordered/unordered write, DMT/DCT enabled</td></tr>
  <tr><td>Multi-socket</td><td>CCIX-style CHI-over-link or CXL 3.0 back-invalidate</td></tr>
</table>

### 2.3 Memory and I/O

<table>
  <tr><th>Interface</th><th>Configuration</th></tr>
  <tr><td>DDR5</td><td>8 channels, DDR5-6400, ECC (SDDC + patrol scrub), 512 GB/s peak</td></tr>
  <tr><td>HBM3e (AI SKU)</td><td>4 stacks, 1.2 TB/s aggregate</td></tr>
  <tr><td>PCIe</td><td>Gen5 ×64 lanes, bifurcatable to ×16/×8/×4</td></tr>
  <tr><td>CXL</td><td>2.0 .io/.cache/.mem; type-2 accelerator and memory expander support</td></tr>
  <tr><td>UCIe</td><td>Optional, for chiplet disaggregation of the I/O die</td></tr>
  <tr><td>Boot/mgmt</td><td>eMMC/SPI-NOR, RoT-gated, UART, I2C, JTAG (DM-gated)</td></tr>
</table>

---

## 3. Core microarchitecture

### 3.1 ISA

**Profile: RVA23S64.** Base and mandatory extensions:

```plaintext
RV64I  M  A  F  D  C  B(Zba/Zbb/Zbs)  V(RVV 1.0)  H(hypervisor)
Zicsr Zifencei Zicntr Zihpm Zihintpause Zicbom Zicbop Zicboz
Zfhmin Zfa Zawrs Zacas Zama16b Zcb Zcmop Zimop
Sscofpmf Sstc Svnapot Svpbmt Svinval Svadu Sha
Ssccptr Sscounterenw Ssstateen Ssstrict Ssu64xl Supm
Zkt (constant-time) + Zk* scalar crypto
Zvbb Zvbc Zvkg Zvkned Zvknhb Zvksed Zvksh Zvkt   (vector crypto)
Zvfh Zvfhmin Zvfbfmin Zvfbfwma                    (vector FP16/BF16)


```

**Planned addition:** `Zime` — RISC-V Integrated Matrix Extension. Per the RISC-V IME task group ratification plan, IME freezes/public-reviews 2026-06-27 and targets ratification 2026-08-27. IME adds **no new architected state**: matrix tiles live in the RVV vector registers. GRX-930 targets IME as its matrix ISA rather than a vendor-proprietary AME (see §5.1 for the rationale and the risk-management plan).

### 3.2 Pipeline

13 stages nominal, front-end decoupled from back-end by a fetch target queue.

```plaintext
 F1  F2  F3 │ D1  D2 │ RN │ DIS │ IS │ RF │ EX1..EXn │ WB │ CMT
 ───────────┼────────┼────┼─────┼────┼────┼──────────┼────┼─────
 fetch/BP   │ decode │rnm │ disp│sel │read│ execute  │ wb │retire


```

<table>
  <tr><th>Stage group</th><th>Width</th><th>Notes</th></tr>
  <tr><td>Fetch</td><td>32 B/cycle</td><td>2 sequential blocks, 8 instructions max</td></tr>
  <tr><td>Decode</td><td>6</td><td>Full RVC expansion, 4 macro-op fusion pairs</td></tr>
  <tr><td>Rename</td><td>6</td><td>Move elimination, zero-idiom elimination</td></tr>
  <tr><td>Dispatch</td><td>6</td><td>To 5 issue queues</td></tr>
  <tr><td>Issue</td><td>10</td><td>4 INT, 2 LD, 2 ST/AGU, 2 FP/VEC dispatch</td></tr>
  <tr><td>Commit</td><td>6</td><td>Up to 2 stores drained per cycle</td></tr>
</table>

### 3.3 Front end

<table>
  <tr><th>Structure</th><th>Size</th></tr>
  <tr><td>L1 I-cache</td><td>64 KB, 4-way, 64 B line, parity, 8-entry stream prefetch</td></tr>
  <tr><td>ITLB</td><td>64-entry fully-assoc L1, shared 2048-entry L2 TLB</td></tr>
  <tr><td>Branch predictor</td><td>TAGE-SC-L: 4 K bimodal + 6 tagged tables (8 K entries), statistical corrector, 64-entry loop predictor</td></tr>
  <tr><td>Indirect predictor</td><td>ITTAGE, 2 K targets</td></tr>
  <tr><td>RAS</td><td>32-entry, checkpointed and repaired on misprediction</td></tr>
  <tr><td>BTB</td><td>L0 32-entry (0-bubble), L1 4 K-entry, L2 16 K-entry</td></tr>
  <tr><td>FTQ</td><td>32 fetch targets, decouples BP from fetch</td></tr>
  <tr><td>Mispredict penalty</td><td>13 cycles min</td></tr>
</table>

### 3.4 Execution back end

<table>
  <tr><th>Resource</th><th>Count / size</th></tr>
  <tr><td>ROB</td><td>320 entries, 6-wide commit</td></tr>
  <tr><td>Integer PRF</td><td>256 × 64 b, 12R/6W banked</td></tr>
  <tr><td>FP/Vector PRF</td><td>64 × VLEN (256 b), 6R/3W banked (§4.3)</td></tr>
  <tr><td>Integer ALU</td><td>4 (2 with branch, 2 with mul/div)</td></tr>
  <tr><td>Integer multiply</td><td>2 × 3-cycle pipelined 64×64</td></tr>
  <tr><td>Integer divide</td><td>1 × radix-16, 12–18 cycles, early-out</td></tr>
  <tr><td>AGU</td><td>4 (2 load, 2 store)</td></tr>
  <tr><td>FPU</td><td>2 × FMA (4-cycle FP64), 1 × div/sqrt</td></tr>
  <tr><td>Vector</td><td>See §4</td></tr>
  <tr><td>Load queue</td><td>96 entries</td></tr>
  <tr><td>Store queue</td><td>64 entries + 32-entry store buffer</td></tr>
  <tr><td>Issue queues</td><td>INT 64, LD/ST 48, FP 48, VEC 32, BR 24</td></tr>
</table>

### 3.5 L1 data cache and memory pipeline

<table>
  <tr><th>Parameter</th><th>Value</th></tr>
  <tr><td>Capacity</td><td>64 KB, 4-way, 64 B line</td></tr>
  <tr><td>Load-to-use</td><td>4 cycles</td></tr>
  <tr><td>Ports</td><td>2 load (64 b each, 256 b for vector) + 2 store</td></tr>
  <tr><td>Write policy</td><td>Write-back, write-allocate, SECDED ECC on data, parity on tags</td></tr>
  <tr><td>MSHRs</td><td>24</td></tr>
  <tr><td>Prefetch</td><td>Stride + IP-correlated + next-line, throttled by L2 feedback</td></tr>
  <tr><td>DTLB</td><td>64-entry L1 + shared 2048-entry L2 TLB, Svadu hardware A/D update</td></tr>
  <tr><td>Disambiguation</td><td>Predictive store-to-load forwarding with 2 K-entry memory dependence predictor</td></tr>
</table>

---

## 4. Vector unit (GRX-VPU) — RVV 1.0

**This is the first block to be implemented in RTL.** §4.3–§4.9 are written as an implementation contract, not a summary.

### 4.1 Architectural parameters

<table>
  <tr><th>Parameter</th><th>Value</th><th>Notes</th></tr>
  <tr><td>VLEN</td><td>256 bits</td><td>Architected vector register length</td></tr>
  <tr><td>ELEN</td><td>64 bits</td><td>Max element width</td></tr>
  <tr><td>DLEN</td><td>2 × 128 bits</td><td>Physical datapath width (&quot;dual datapath&quot;)</td></tr>
  <tr><td>Vector registers</td><td>32 (v0–v31)</td><td>Architected; 64 physical in the OoO core</td></tr>
  <tr><td>SEW</td><td>8/16/32/64</td><td>Zve64d capable</td></tr>
  <tr><td>LMUL</td><td>1/8, 1/4, 1/2, 1, 2, 4, 8</td><td>Fractional LMUL supported</td></tr>
  <tr><td>VLMAX</td><td>VLEN/SEW × LMUL</td><td>32 elements at SEW=8, LMUL=1</td></tr>
  <tr><td>Element data types</td><td>INT8/16/32/64, FP16, BF16, FP32, FP64</td><td>INT4 packed via Zvbb helpers in v2</td></tr>
  <tr><td>Tail/mask policy</td><td>ta/tu, ma/mu</td><td>Both implemented</td></tr>
</table>

### 4.2 Interface to the scalar core — VCIF

The VPU is a **decoupled coprocessor**. It does not sit inside the scalar pipeline. This is the key structural decision that lets it be validated today against the in-order bring-up harness and dropped into the out-of-order core unchanged.

The **Vector Command Interface (VCIF)** has four independent channels:

**1. Issue channel (core → VPU)** — ready/valid

<table>
  <tr><th>Signal</th><th>Width</th><th>Meaning</th></tr>
  <tr><td>iss_valid / iss_ready</td><td>1</td><td>Handshake</td></tr>
  <tr><td>iss_instr</td><td>32</td><td>Raw RVV instruction word</td></tr>
  <tr><td>iss_rs1</td><td>64</td><td>Scalar operand 1 (.vx operand, base address, AVL)</td></tr>
  <tr><td>iss_rs2</td><td>64</td><td>Scalar operand 2 (stride, vsetvl vtype)</td></tr>
  <tr><td>iss_tag</td><td>6</td><td>Core-assigned tag (ROB index in the OoO core)</td></tr>
  <tr><td>iss_vstart</td><td>8</td><td>Architectural vstart on resume from trap</td></tr>
</table>

**2. Writeback channel (VPU → core)** — for vector→scalar results

<table>
  <tr><th>Signal</th><th>Width</th><th>Meaning</th></tr>
  <tr><td>wb_valid / wb_ready</td><td>1</td><td>Handshake</td></tr>
  <tr><td>wb_tag</td><td>6</td><td>Matching issue tag</td></tr>
  <tr><td>wb_data</td><td>64</td><td>vsetvl* new vl, vmv.x.s, vcpop.m, vfirst.m, CSR reads</td></tr>
  <tr><td>wb_wen</td><td>1</td><td>Write rd in the scalar RF</td></tr>
</table>

**3. Completion channel (VPU → core)** — retirement and traps

<table>
  <tr><th>Signal</th><th>Width</th><th>Meaning</th></tr>
  <tr><td>cmt_valid</td><td>1</td><td>Instruction complete</td></tr>
  <tr><td>cmt_tag</td><td>6</td><td>Matching issue tag</td></tr>
  <tr><td>cmt_trap</td><td>1</td><td>Trap taken inside the VPU</td></tr>
  <tr><td>cmt_trap_cause</td><td>5</td><td>RISC-V exception cause</td></tr>
  <tr><td>cmt_trap_tval</td><td>64</td><td>Faulting address</td></tr>
  <tr><td>cmt_vstart</td><td>8</td><td>Element index to resume at</td></tr>
</table>

**4. Memory channel (VPU ↔ L1D / L2)** — independent 256-bit port

<table>
  <tr><th>Signal</th><th>Width</th><th>Meaning</th></tr>
  <tr><td>mem_req_valid / _ready</td><td>1</td><td>Handshake</td></tr>
  <tr><td>mem_req_we</td><td>1</td><td>0 = load, 1 = store</td></tr>
  <tr><td>mem_req_addr</td><td>64</td><td>Byte address, naturally aligned to access size</td></tr>
  <tr><td>mem_req_wdata</td><td>256</td><td>Store data</td></tr>
  <tr><td>mem_req_wstrb</td><td>32</td><td>Byte enables (mask + tail folded in)</td></tr>
  <tr><td>mem_rsp_valid</td><td>1</td><td>Response</td></tr>
  <tr><td>mem_rsp_rdata</td><td>256</td><td>Load data</td></tr>
  <tr><td>mem_rsp_err</td><td>1</td><td>Access/page fault</td></tr>
</table>

**Ordering contract.** The core issues vector instructions in program order and the VPU completes them in program order on the completion channel. The VPU may execute internally out of order but must not report completion out of order — this keeps precise traps trivial in the in-order harness and lets the OoO core's ROB do the reordering. `vstart` is written on every trap so a resumed instruction restarts mid-vector, as RVV 1.0 requires.

### 4.3 Vector register file

- 32 architected registers × 256 bits = 1 KB architected state.
- Organised as **8 banks × 32 bits per element-slice**, allowing one full `VLEN` read per cycle without a 256-bit-wide monolithic array.
- Ports: **4 read** (`vs1`, `vs2`, `vd`-old for tail/mask-undisturbed merge, `v0` mask), **1 write**.
- `v0` is dual-ported for its mask role: mask reads never contend with data reads.
- SECDED ECC per 64-bit element in the server SKU; parity in the bring-up config.
- In the OoO core this becomes a 64-entry physical file with a rename map; the *architected* view specified here is unchanged, which is exactly the decoupling §1.2.3 requires.

### 4.4 Instruction set coverage — implementation phases

**Phase 1 (this RTL drop):**

<table>
  <tr><th>Group</th><th>Instructions</th></tr>
  <tr><td>Configuration</td><td>vsetvli, vsetivli, vsetvl — full vtype decode, vill, AVL rules</td></tr>
  <tr><td>Integer arithmetic</td><td>vadd, vsub, vrsub (VV/VX/VI)</td></tr>
  <tr><td>Logical</td><td>vand, vor, vxor (VV/VX/VI)</td></tr>
  <tr><td>Shift</td><td>vsll, vsrl, vsra (VV/VX/VI)</td></tr>
  <tr><td>Min/max</td><td>vminu, vmin, vmaxu, vmax (VV/VX)</td></tr>
  <tr><td>Multiply</td><td>vmul, vmulh, vmulhu, vmulhsu (VV/VX)</td></tr>
  <tr><td>Multiply-add</td><td>vmacc, vnmsac, vmadd, vnmsub (VV/VX)</td></tr>
  <tr><td>Merge/move</td><td>vmerge.vvm/.vxm/.vim, vmv.v.v/.v.x/.v.i</td></tr>
  <tr><td>Mask-producing compare</td><td>vmseq, vmsne, vmsltu, vmslt, vmsleu, vmsle, vmsgtu, vmsgt</td></tr>
  <tr><td>Load/store</td><td>vle8/16/32/64.v, vse8/16/32/64.v (unit-stride), vlm.v/vsm.v</td></tr>
</table>

**Phase 2:** strided and indexed load/store, segment load/store, widening/narrowing ops, `vrgather`/`vslide*`, reductions, fixed-point saturating ops, `vfirst`/`vcpop`/`vid`/`viota`.

**Phase 3:** vector floating point (FP16/BF16/FP32/FP64 FMA), vector crypto (`Zvbb`, `Zvbc`, `Zvkg`, `Zvkned`, `Zvknhb`), `Zvfbfwma` BF16 widening FMA — the operator that matters most for transformer inference.

**Phase 4:** IME matrix instructions (§5).

### 4.5 Microarchitecture

```plaintext
        VCIF issue
             │
    ┌────────▼─────────┐
    │     Decoder      │  RVV opcode/funct6/funct3 → uop descriptor
    └────────┬─────────┘
             │
    ┌────────▼─────────┐      ┌──────────────┐
    │   vtype/CSR unit │◄────►│ vl, vtype,   │
    │  (vsetvl* logic) │      │ vstart, vcsr │
    └────────┬─────────┘      └──────────────┘
             │
    ┌────────▼─────────┐
    │    Sequencer     │  LMUL group expansion + DLEN pass counter
    │                  │  emits (vreg_idx, elem_base, active_mask) per pass
    └───┬─────────┬────┘
        │         │
  ┌─────▼───┐ ┌───▼──────────┐
  │  VRF    │ │     VLSU     │──► 256-bit memory port
  │ 4R / 1W │ │ unit-stride  │
  └─────┬───┘ └───┬──────────┘
        │         │
  ┌─────▼─────────▼───┐
  │   Lane datapath   │  DLEN=128 b × 2 passes per VLEN=256 b
  │  16×8b / 8×16b /  │
  │  4×32b / 2×64b    │
  └─────────┬─────────┘
            │
  ┌─────────▼─────────┐
  │  Mask + tail merge│  ma/mu, ta/tu policy applied here
  └─────────┬─────────┘
            │  VRF write / VCIF writeback / completion


```

### 4.6 Sequencer

The sequencer is the piece that makes RVV tractable. It converts one architectural instruction into a stream of fixed-width passes:

```plaintext
passes_total = ceil( (vl - vstart) elements × SEW bits / DLEN )   , per LMUL register
regs_in_group = max(1, LMUL)                                       , fractional LMUL ⇒ 1


```

Per pass it emits: source register indices (`vs1+r`, `vs2+r`, `vd+r`), element base index, a per-element active mask (from `v0` when `vm=0`), and prefix/tail element masks derived from `vstart` and `vl`. Elements before `vstart` are never written. Elements at or beyond `vl` follow the `ta`/`tu` policy. Inactive elements follow `ma`/`mu`.

### 4.7 Lane datapath

`DLEN = 128` bits per pass. One SEW-agnostic datapath slice, replicated:

<table>
  <tr><th>SEW</th><th>Elements per pass</th><th>Lane slices used</th></tr>
  <tr><td>8</td><td>16</td><td>16 × 8-bit</td></tr>
  <tr><td>16</td><td>8</td><td>8 × 16-bit</td></tr>
  <tr><td>32</td><td>4</td><td>4 × 32-bit</td></tr>
  <tr><td>64</td><td>2</td><td>2 × 64-bit</td></tr>
</table>

Adders are built as segmented 64-bit carry chains with carry-break at SEW boundaries, so one physical adder serves all four SEWs. Multipliers are 4 × 32×32 arrays with partial-product recombination for SEW=64 and splitting for SEW≤16 — a standard SWAR multiplier structure.

Latency: 1 cycle for logical/add/shift/min/max/compare, 3 cycles pipelined for multiply/multiply-add. Back-to-back dependent passes on the same register group are forwarded, so a full `LMUL=8` `vadd` at SEW=8 costs 16 passes ≈ 16 cycles.

### 4.8 Vector load/store unit

Phase 1 handles unit-stride only. The address generator produces naturally-aligned 256-bit requests; the first and last request of an access are byte-masked for sub-block starts and `vl` tails. `wstrb` folds together: byte-enables from the element size, the active mask, and the tail. Loads that fault report `mem_rsp_err`, and the VLSU converts the failing element index into `vstart` before raising `cmt_trap`.

### 4.9 CSRs

<table>
  <tr><th>CSR</th><th>Addr</th><th>Notes</th></tr>
  <tr><td>vstart</td><td>0x008</td><td>Written on trap, cleared on successful completion</td></tr>
  <tr><td>vxsat</td><td>0x009</td><td>Fixed-point saturation flag (phase 2)</td></tr>
  <tr><td>vxrm</td><td>0x00A</td><td>Fixed-point rounding mode (phase 2)</td></tr>
  <tr><td>vcsr</td><td>0x00F</td><td>{vxrm, vxsat}</td></tr>
  <tr><td>vl</td><td>0xC20</td><td>Read-only, written by vsetvl*</td></tr>
  <tr><td>vtype</td><td>0xC21</td><td>{vill, 0, vma, vta, vsew[2:0], vlmul[2:0]}</td></tr>
  <tr><td>vlenb</td><td>0xC22</td><td>VLEN/8 = 32</td></tr>
</table>

`vsetvl*` AVL rules implemented exactly per RVV 1.0 §6.2:

- `rs1 ≠ x0` → `AVL = x[rs1]`, `vl = min(AVL, VLMAX)`
- `rs1 = x0`, `rd ≠ x0` → `AVL = ~0`, `vl = VLMAX`
- `rs1 = x0`, `rd = x0` → keep current `vl`, trap if `VLMAX` would change
- Unsupported `vtype` → `vill = 1`, all other `vtype` fields zeroed, `vl = 0`

---

## 5. Matrix / tensor acceleration

### 5.1 ISA choice: IME over a proprietary AME

The XuanTie parts use **AME** (Attached Matrix Extension), a vendor extension with its own architected tile state. GRX-930 targets **IME** (Integrated Matrix Extension) instead.

<table>
  <tr><th></th><th>AME-style (attached tiles)</th><th>IME (this design)</th></tr>
  <tr><td>Architected state</td><td>New tile register file</td><td>None — reuses v0–v31</td></tr>
  <tr><td>Context switch</td><td>New save/restore path, new xstatus bits</td><td>Existing vector context switch</td></tr>
  <tr><td>Virtualization</td><td>New state to trap/migrate</td><td>Free</td></tr>
  <tr><td>Binary portability</td><td>Per-vendor</td><td>Portable across VLEN</td></tr>
  <tr><td>Silicon cost</td><td>Separate large RF</td><td>Reuses VPU RF and ports</td></tr>
  <tr><td>Standardization</td><td>Vendor-specific</td><td>RISC-V TG, ratification targeted 2026-08-27</td></tr>
</table>

IME defines matrix-level operations — tile multiply and outer product — over the vector registers, with fp64/fp32/bf16/int8/fp8 data types. Because the tiles *are* vector registers, the matrix engine is a new set of functional units hanging off the VPU register file rather than a new coprocessor with its own state. That is a very large reduction in verification and software-enablement cost, and it is the single strongest argument for building the vector unit first.

**Risk management.** IME is not ratified as of this revision (target 2026-08-27, ~12 days out). The RTL therefore isolates all IME opcode decode in `grx_vpu_decoder` behind a `GRX_IME_EN` parameter, and the matrix functional units attach through the same internal operand-issue interface the ALU lanes use. If the ratified encoding differs from the public review draft, only the decoder table changes.

### 5.2 Matrix engine (GRX-MXU) — planned

<table>
  <tr><th>Parameter</th><th>Value</th></tr>
  <tr><td>Structure</td><td>32 × 32 outer-product MAC array per core</td></tr>
  <tr><td>Native types</td><td>INT8×INT8→INT32, BF16×BF16→FP32, FP16×FP16→FP32, FP8 (E4M3/E5M2)→FP32</td></tr>
  <tr><td>Throughput</td><td>1024 MACs/cycle INT8 = 5.7 TOPS/core at 2.8 GHz</td></tr>
  <tr><td>Accumulator</td><td>FP32/INT32, held in vector registers per IME</td></tr>
  <tr><td>Operand feed</td><td>Direct from VRF, 2 × 256-bit reads/cycle + accumulator read-modify-write</td></tr>
  <tr><td>Sparsity</td><td>2:4 structured sparsity decode (phase 5)</td></tr>
</table>

### 5.3 Why this matters for transformer inference

The operators that dominate LLM decode are (a) GEMV/GEMM against weight matrices, (b) softmax, (c) RMSNorm/LayerNorm, (d) RoPE, (e) KV-cache gather. IME covers (a). RVV covers (b)–(d) well *if* `Zvfbfwma` (BF16 widening FMA) and reductions are present — hence their priority in phase 3. (e) is a `vrgather`/indexed-load problem, phase 2. A matrix engine without those vector operators leaves most of decode-phase time on the table, which is the second reason the vector unit is built first.

### 5.4 Optional attached TPE

For the AI SKU, an optional cluster-level Tensor Processing Engine attaches as a CHI Request Node with:

- Shared virtual memory with the cores via the RISC-V IOMMU (same page tables, `Sv48`).
- Hardware coherence — the TPE participates in CHI, so no software cache maintenance.
- Its own DMA descriptors and a low-latency doorbell through IMSIC MSI, so an agent loop dispatching work sees interrupt latency in the hundreds of nanoseconds rather than microseconds.

The coherent-shared-virtual-memory property is what makes an "agentic" workload — many small dependent dispatches interleaved with host-side control flow — perform acceptably. A DMA-copy-based accelerator interface loses on that workload regardless of its peak TOPS.

---

## 6. Virtualization, interrupts, platform

<table>
  <tr><th>Feature</th><th>Specification</th></tr>
  <tr><td>Hypervisor</td><td>RISC-V H extension, two-stage translation, hgatp Sv48x4</td></tr>
  <tr><td>MMU</td><td>Sv39/Sv48/Sv57, PA48, Svpbmt, Svnapot, Svinval, Svadu hardware A/D</td></tr>
  <tr><td>Interrupts</td><td>AIA v1.0: APLIC (wired) + IMSIC (MSI), per-hart 2 supervisor domains × 5 interrupt files</td></tr>
  <tr><td>Timers</td><td>Sstc — supervisor-mode timer compare, no M-mode trap</td></tr>
  <tr><td>Perf counters</td><td>Sscofpmf — count overflow and mode filtering, 29 HPM counters</td></tr>
  <tr><td>IOMMU</td><td>RISC-V IOMMU v1.0, PCIe ATS/PRI, device contexts, MSI translation</td></tr>
  <tr><td>Device isolation</td><td>IOMMU device-context per BDF + IOPMP for non-PCIe masters</td></tr>
</table>

---

## 7. Security

<table>
  <tr><th>Layer</th><th>Mechanism</th></tr>
  <tr><td>Physical memory</td><td>ePMP (Smepmp), 64 regions</td></tr>
  <tr><td>I/O</td><td>IOPMP for DMA masters outside the IOMMU domain</td></tr>
  <tr><td>Boot</td><td>Immutable ROM RoT → signed FSBL → measured boot, ECDSA-P384 + SHA-384</td></tr>
  <tr><td>Key storage</td><td>OTP fuses + hardware key ladder, no software read path</td></tr>
  <tr><td>Crypto</td><td>Scalar Zk* and vector Zvk*, all constant-time (Zkt)</td></tr>
  <tr><td>Confidential compute</td><td>Memory encryption engine (AES-XTS-256) with per-VM keys, CXL IDE</td></tr>
  <tr><td>Side channels</td><td>Way-partitioned LLC (Smcdeleg-style QoS), branch predictor flush on domain switch, Zkt timing guarantees, speculative-load hardening on S→U boundary</td></tr>
  <tr><td>Debug lockout</td><td>JTAG disabled by fuse in production, re-enable only via signed challenge</td></tr>
</table>

---

## 8. RAS, debug, power

### 8.1 RAS

<table>
  <tr><th>Structure</th><th>Protection</th></tr>
  <tr><td>L1I / L1D data</td><td>SECDED ECC (L1D), parity + invalidate-on-error (L1I)</td></tr>
  <tr><td>L1 tags</td><td>Parity, way-disable on persistent error</td></tr>
  <tr><td>L2 / L3</td><td>SECDED ECC, DECTED on L3 data, cache line delete</td></tr>
  <tr><td>TLBs</td><td>Parity, flush-and-refill on error</td></tr>
  <tr><td>PRF / VRF</td><td>SECDED ECC per 64-bit element</td></tr>
  <tr><td>DRAM</td><td>SDDC, patrol scrub, post-package repair, ADDDC</td></tr>
  <tr><td>Interconnect</td><td>Link CRC with retry (CHI.E)</td></tr>
  <tr><td>Reporting</td><td>RERI (RAS Error Record Interface) — memory-mapped error records, per-component banks</td></tr>
  <tr><td>Containment</td><td>Poison propagation, precise machine-check on consumption, no silent data corruption</td></tr>
</table>

### 8.2 Debug and trace

- RISC-V Debug Specification v1.0 — Debug Module, abstract commands, system bus access.
- `Sdext` external debug, `Sdtrig` with 8 triggers (address/data/instruction-count).
- **RISC-V Nexus Trace v1.0** — branch trace, data trace, timestamped, per-hart funnel to a shared trace buffer and off-chip via PCIe or dedicated trace port.

### 8.3 Power

<table>
  <tr><th>Mechanism</th><th>Detail</th></tr>
  <tr><td>DVFS</td><td>Per-cluster voltage domain, per-core DVFS via AVS with on-die droop detectors</td></tr>
  <tr><td>Core C-states</td><td>C0 (active), C1 (clock gated), C6 (power gated, state retained in L2), C7 (cluster off)</td></tr>
  <tr><td>Vector power</td><td>VPU power-gated when vl=0 and no vector instruction in flight for N cycles</td></tr>
  <tr><td>Throttling</td><td>Per-core dynamic power meter, EDP-based frequency capping, thermal sensors per cluster</td></tr>
  <tr><td>Telemetry</td><td>Per-core energy counters exposed via HPM counters and a management block</td></tr>
</table>

---

## 9. RTL implementation roadmap

<table>
  <tr><th>Phase</th><th>Deliverable</th><th>Status</th></tr>
  <tr><td>P0</td><td>Project scaffold, parameter packages, Verilator/Icarus lint+sim flow</td><td>Done</td></tr>
  <tr><td>P1</td><td>GRX-VPU RVV 1.0 core subset (§4.4 phase 1) + self-checking TB</td><td>This drop</td></tr>
  <tr><td>P2</td><td>VLSU strided/indexed/segment, vrgather/vslide, reductions</td><td>Next</td></tr>
  <tr><td>P3</td><td>Vector FP (FP16/BF16/FP32/FP64 FMA), Zvfbfwma, vector crypto</td><td></td></tr>
  <tr><td>P4</td><td>GRX-MXU IME matrix units on the VPU register file</td><td></td></tr>
  <tr><td>P5</td><td>L2 cache + CHI.E request node</td><td></td></tr>
  <tr><td>P6</td><td>OoO core: rename/ROB/issue queues, VPU re-hosted behind VCIF unchanged</td><td></td></tr>
  <tr><td>P7</td><td>L3/LLC + mesh + home node directory</td><td></td></tr>
  <tr><td>P8</td><td>IOMMU, AIA (APLIC + IMSIC), hypervisor extension</td><td></td></tr>
</table>

### 9.1 Bring-up harness

Phase 1–4 blocks are validated against the RV64IMAC reference core acting as the scalar host: its decoder is extended to recognise the RVV major opcodes (`OP-V` = 0x57, `LOAD-FP` = 0x07, `STORE-FP` = 0x27) and forward them over VCIF, stalling on the completion channel. This is deliberately a *harness*, not the product microarchitecture — it exists so the VPU runs real instruction streams years before the OoO core exists.

### 9.2 Verification strategy

<table>
  <tr><th>Level</th><th>Method</th></tr>
  <tr><td>Block</td><td>Self-checking SystemVerilog TB with an element-level golden model, directed + constrained-random</td></tr>
  <tr><td>Coverage</td><td>Functional covergroups on {SEW × LMUL × vm × ta/tu × ma/mu × vstart} cross</td></tr>
  <tr><td>Integration</td><td>RV64IMAC harness running compiled RVV assembly</td></tr>
  <tr><td>Architectural</td><td>RISCV-DV random program generation; Spike as the golden ISS co-simulation reference</td></tr>
  <tr><td>Formal</td><td>Property checks on the sequencer (no write before vstart, no write past vl under ta)</td></tr>
  <tr><td>Performance</td><td>Trace-driven roofline on GEMM/GEMV kernels, checked against §5.2 throughput targets</td></tr>
</table>

---

## 10. Open decisions

1. **IME encoding freeze.** Hold matrix RTL until 2026-08-27 ratification, or start against the public-review draft and accept a decoder respin? Recommend: start now, isolate decode.
2. **VLEN 256 vs 512.** 256 matches the C930/C950 class and halves VRF area and port pressure. 512 doubles per-core vector throughput at significant area/power cost. The RTL parameterizes `VLEN`, so this can be decided after the phase-3 FP numbers are real.
3. **DLEN.** 2 × 128 as specified, or 1 × 256 for lower latency at higher area. Currently parameterized.
4. **L3 capacity.** 32 MB specified; the C950 datasheet caps at 8 MB shared. For a 64-core server part, 32 MB is the defensible number, but it is a large area line item.
5. **Chiplet split.** Monolithic vs. compute-die + I/O-die over UCIe. Affects the DDR5/PCIe placement and adds a die-to-die coherence hop.

---

## Sources

Public references used for market positioning and standards status:

- [Alibaba XuanTie C950 — CNX Software](https://www.cnx-software.com/2026/03/25/alibaba-xuantie-c950-a-powerful-rva2364-bit-risc-v-core-for-edge-ai-computing/)
- [XuanTie C950 spec sheet (PDF)](https://regmedia.co.uk/2026/03/25/supplied_xuantie_c950_spec_sheet.pdf)
- [Alibaba launches server-grade RISC-V CPU — The Register](https://www.theregister.com/2025/03/05/china_alibaba_risc_v_c930/)
- [Alibaba launches RISC-V XuanTie C930 server CPU — Tom's Hardware](https://www.tomshardware.com/pc-components/cpus/alibaba-launches-risc-v-based-xuantie-c930-server-cpu-ai-hpc-chip-ships-this-month-more-designs-to-follow)
- [RISC-V IME Ratification Plan — RISC-V Tech Hub](https://riscv.atlassian.net/wiki/spaces/IMEX/pages/598867969/IME+Ratification+Plan)
- [RISC-V AME Ratification Plan — RISC-V Tech Hub](https://riscv.atlassian.net/wiki/spaces/AMEX/pages/55083420/AME+Ratification+Plan)
- [RISC-V Integrated Matrix Extension repository](https://github.com/riscv/integrated-matrix-extension)
- [RV64IMAC reference core](https://github.com/MuhammadMajiid/RV64IMAC)
