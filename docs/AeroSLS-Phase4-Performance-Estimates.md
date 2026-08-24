# AeroSLS Phase 4 — Performance Estimates

**Version:** 1.0  
**Date:** August 23, 2026  
**Scope:** Performance analysis of the Phase 4 Device Driver SDK, covering interrupt latency, DMA setup costs, zero-copy throughput, and micro-architectural effects.  
**Depends on:** Phase 4 Design v0.1, Phase 4 Security Analysis, Benchmark Harness (benchmarks/phase4/).

---

## Table of Contents

1. [Executive Summary](#1-executive-summary)
2. [System Model and Assumptions](#2-system-model-and-assumptions)
3. [Interrupt Latency Analysis](#3-interrupt-latency-analysis)
4. [DMA Setup and Allocation Costs](#4-dma-setup-and-allocation-costs)
5. [Channel IPC Throughput](#5-channel-ipc-throughput)
6. [Zero-Copy NIC Throughput](#6-zero-copy-nic-throughput)
7. [Capability System Overhead](#7-capability-system-overhead)
8. [Context Switch Cost](#8-context-switch-cost)
9. [IOMMU Performance](#9-iommu-performance)
10. [Micro-Architectural Effects](#10-micro-architectural-effects)
11. [Comparison with Linux In-Kernel Drivers](#11-comparison-with-linux-in-kernel-drivers)
12. [Scalability Analysis](#12-scalability-analysis)
13. [Optimization Opportunities](#13-optimization-opportunities)
14. [Benchmark Plan Summary](#14-benchmark-plan-summary)
15. [Risk Factors and Confidence Intervals](#15-risk-factors-and-confidence-intervals)

---

## 1. Executive Summary

Phase 4 introduces a capability-based sidecar architecture for device drivers, replacing in-kernel driver modules with user-space (or same-ring, memory-isolated) sidecars. This adds overhead compared to Linux's monolithic driver model, but the overhead is bounded, predictable, and compensated by strong isolation guarantees.

### Key Performance Claims

| Metric | AeroSLS Estimate | Linux Baseline | Overhead Factor |
|--------|-----------------|---------------|----------------|
| IRQ-to-handler latency (p50) | 15–25 µs | 3–8 µs | 2–5× |
| IRQ-to-handler latency (p99) | 25–45 µs | 8–15 µs | 2–3× |
| DMA allocation (1 page) | 2–5 µs | 0.5–2 µs (CMA) | 2–3× |
| Channel round-trip (64B) | 4–10 µs | 1–3 µs (futex) | 2–4× |
| TCP throughput (1 flow, 1 Gb/s NIC) | 400–800 Mbps | 940 Mbps | 0.4–0.85× |
| Packets/sec (1514B) | 200K–400K | 800K–1.4M | 0.2–0.5× |
| Recovery time (driver crash) | 20–80 ms | N/A (kernel panic or oops) | N/A |

**The dominant cost is context switching between sidecars.** Every packet on the zero-copy NIC path requires at least one context switch (driver → stack). This costs 3–8 µs per switch, which at 1 Gb/s line rate (~81K pps for 1514B packets) adds 0.24–0.65 ms of CPU time per second — roughly 24–65% of one core.

**With IRQ coalescing (16×), the effective per-packet interrupt cost drops below 1 µs**, bringing total overhead to manageable levels for most workloads. The system is viable for 1 Gb/s networking and suitable for moderate-throughput device drivers (storage, USB, serial).

---

## 2. System Model and Assumptions

### 2.1 Hardware Platform

| Component | Specification |
|-----------|--------------|
| CPU | RISC-V 64-bit, 4 cores, 1.5 GHz (SiFive FU740-class) |
| L1I/L1D | 32 KB each, 4-way |
| L2 | 2 MB shared |
| RAM | 4 GiB DDR4, 64-bit wide |
| Cache line | 64 bytes |
| TLB | 64-entry DTLB (RISC-V Sv39) |
| IOMMU | RISC-V IOMMU, 4KB page granularity |
| NIC | e1000 (1 Gb/s) or virtio-net |
| Interrupt Controller | PLIC (1024 sources, 8 priority levels) |

### 2.2 Software Configuration

| Parameter | Value |
|-----------|-------|
| DMA pool size | 32 MiB (8192 frames) |
| Max DMA buffers | 1024 |
| Channel queue depth | 64 messages |
| IRQ coalescing default | 50 µs (or 8× whichever first) |
| Cap table size | 256 entries per sidecar |
| Sidecar address space | Sv39, per-sidecar page tables |
| Scheduler | Round-robin, 10 ms quantum |

### 2.3 Measurement Methodology

All estimates are derived from:

1. **Analytical modeling** of the code paths (instruction counts × cycle costs).
2. **Host-side simulation** using the benchmark harness (`benchmarks/phase4/`).
3. **Comparison** with published Linux kernel performance data.
4. **Bounding** via worst-case analysis of each subsystem.

Confidence intervals are provided as [optimistic, pessimistic] ranges. The optimistic case assumes hot caches, no contention, and favorable scheduling. The pessimistic case assumes cold caches, contention, and unfavorable scheduling.

---

## 3. Interrupt Latency Analysis

### 3.1 Interrupt Delivery Path

The full path from hardware interrupt to driver handler execution:

```
┌─────────────────────────────────────────────────────────────────┐
│  T0: Hardware interrupt asserted (NIC asserts INTx/MSI line)    │
│      Cost: 0 (hardware)                                         │
├─────────────────────────────────────────────────────────────────┤
│  T1: PLIC claims interrupt, raises to supervisor mode            │
│      Cost: ~200 ns (PLIC internal + mode switch)                │
├─────────────────────────────────────────────────────────────────┤
│  T2: Kernel trap entry (trap_riscv.S)                           │
│      Cost: ~300 ns (save 4 regs, load kernel sp, jump)         │
├─────────────────────────────────────────────────────────────────┤
│  T3: Trap dispatch → identify PLIC source ID                    │
│      Cost: ~100 ns (PLIC source read)                          │
├─────────────────────────────────────────────────────────────────┤
│  T4: IRQ cap lookup (irq_deliver)                               │
│      Cost: ~200 ns (128-slot array scan + validation)          │
├─────────────────────────────────────────────────────────────────┤
│  T5: Coalescing check                                           │
│      Cost: ~50 ns (timestamp comparison)                       │
├─────────────────────────────────────────────────────────────────┤
│  T6: Build IRQMessage (24 bytes)                                │
│      Cost: ~100 ns (struct fill + rdcycle)                     │
├─────────────────────────────────────────────────────────────────┤
│  T7: cap_send_msg → channel enqueue                             │
│      Cost: ~500 ns (lock, copy 24B, wake driver)              │
├─────────────────────────────────────────────────────────────────┤
│  T8: PLIC claim complete (write PLIC_CLAIM)                     │
│      Cost: ~100 ns (MMIO write)                                │
├─────────────────────────────────────────────────────────────────┤
│  T9: Context switch to driver sidecar                           │
│      Cost: 3–8 µs (save kernel regs, load driver regs,         │
│            switch satp, TLB refill via ASID)                    │
├─────────────────────────────────────────────────────────────────┤
│  T10: Driver message loop receives IRQ message                  │
│       Cost: ~200 ns (channel dequeue + memcpy)                 │
├─────────────────────────────────────────────────────────────────┤
│  T11: Driver handler executes (reads device, processes event)   │
│       Cost: 0.5–5 µs (device-dependent)                       │
├─────────────────────────────────────────────────────────────────┤
│  T12: Driver calls irq_ack (SYS_SLS_IRQ_ACK)                   │
│       Cost: ~2 µs (syscall entry + PLIC complete + exit)       │
└─────────────────────────────────────────────────────────────────┘
```

### 3.2 Latency Breakdown

| Segment | Path | Optimistic | Typical | Pessimistic |
|---------|------|-----------|---------|-------------|
| T0→T1 | HW → PLIC claim | 200 ns | 300 ns | 500 ns |
| T1→T2 | PLIC → trap entry | 200 ns | 300 ns | 500 ns |
| T2→T3 | Trap dispatch | 50 ns | 100 ns | 200 ns |
| T3→T5 | IRQ lookup + coalesce | 200 ns | 250 ns | 400 ns |
| T5→T7 | Build + enqueue | 400 ns | 600 ns | 1 µs |
| T7→T8 | PLIC complete | 50 ns | 100 ns | 200 ns |
| **T0→T8** | **Kernel overhead** | **1.1 µs** | **1.65 µs** | **2.8 µs** |
| T8→T9 | Context switch | 3 µs | 5 µs | 8 µs |
| T9→T10 | Driver recv | 100 ns | 200 ns | 400 ns |
| **T0→T10** | **Handler entry** | **4.2 µs** | **6.85 µs** | **11.2 µs** |
| T10→T11 | Handler execution | 0.5 µs | 2 µs | 5 µs |
| T11→T12 | IRQ ack | 1.5 µs | 2 µs | 3 µs |
| **T0→T12** | **Full round-trip** | **5.7 µs** | **10.85 µs** | **19.2 µs** |

### 3.3 Comparison with Linux

| Metric | AeroSLS | Linux (e1000) | Notes |
|--------|---------|--------------|-------|
| T0→handler entry (p50) | 6.85 µs | 3–5 µs | Linux: NAPI softirq scheduling |
| T0→handler entry (p99) | 11.2 µs | 8–12 µs | Similar at tail latency |
| Full round-trip (p50) | 10.85 µs | 5–8 µs | AeroSLS includes ack syscall |
| With coalescing (8×) | 0.86 µs/amortized | 0.6–1 µs | NAPI poll batch |

**Analysis:** AeroSLS adds approximately 2–5 µs of overhead per interrupt compared to Linux, primarily from the context switch (driver is a separate process) and the channel message enqueue. At low interrupt rates (<10K/s), this is negligible. At high rates (>100K/s), coalescing is essential.

### 3.4 Coalescing Impact

| Coalesce Factor | Effective IRQ Cost/pkt | Interrupts/sec at 1 Gb/s | CPU Overhead |
|-----------------|----------------------|-------------------------|-------------|
| 1× (none) | 6.85 µs | 81,250 | 556 µs/s (5.6%) |
| 4× | 1.71 µs | 20,313 | 34.8 µs/s (0.3%) |
| 8× | 0.86 µs | 10,156 | 8.7 µs/s (0.09%) |
| 16× | 0.43 µs | 5,078 | 2.2 µs/s (0.02%) |

At 16× coalescing, interrupt overhead drops to 0.02% of one core — effectively free.

---

## 4. DMA Setup and Allocation Costs

### 4.1 DMA Allocation Path

```
┌─────────────────────────────────────────────────────────────────┐
│  SYS_SLS_DMA_ALLOC syscall entry                                │
│  Cost: ~1.5 µs (syscall marshaling)                            │
├─────────────────────────────────────────────────────────────────┤
│  Validate arguments (npages, align, flags)                      │
│  Cost: ~50 ns                                                  │
├─────────────────────────────────────────────────────────────────┤
│  Acquire DMA pool lock                                          │
│  Cost: ~100 ns (spinlock or mutex)                             │
├─────────────────────────────────────────────────────────────────┤
│  Bitmap first-fit search                                        │
│  Cost: ~200 ns (8192 frames / 8 = 1 KB bitmap scan)           │
│  Worst case: ~800 ns (fragmented pool)                         │
├─────────────────────────────────────────────────────────────────┤
│  Mark frames in bitmap                                          │
│  Cost: ~50 ns per page (negligible for small allocs)           │
├─────────────────────────────────────────────────────────────────┤
│  Allocate buffer descriptor                                     │
│  Cost: ~100 ns (freelist pop)                                  │
├─────────────────────────────────────────────────────────────────┤
│  Pin pages in frame pool                                        │
│  Cost: ~300 ns (reference count + bitmap update)               │
├─────────────────────────────────────────────────────────────────┤
│  Program IOMMU page table                                       │
│  Cost: ~500 ns (4-level page table walk + write)               │
│  With IOTLB miss: ~1–2 µs (additional page walks)             │
├─────────────────────────────────────────────────────────────────┤
│  Create MEM cap in caller's table                               │
│  Cost: ~300 ns (cap table slot allocation + validation)        │
├─────────────────────────────────────────────────────────────────┤
│  Return (cap_idx + dma_buf_id)                                  │
│  Cost: ~200 ns (syscall exit)                                  │
└─────────────────────────────────────────────────────────────────┘
```

### 4.2 Allocation Cost Summary

| Component | 1 Page | 64 Pages | 1024 Pages |
|-----------|--------|----------|------------|
| Syscall overhead | 1.7 µs | 1.7 µs | 1.7 µs |
| Bitmap search | 200 ns | 200 ns | 400 ns |
| Frame marking | 50 ns | 3.2 µs | 51.2 µs |
| Pin pages | 300 ns | 19.2 µs | 307 µs |
| IOMMU programming | 500 ns | 500 ns | 500 ns |
| Cap creation | 300 ns | 300 ns | 300 ns |
| **Total** | **3.05 µs** | **25.1 µs** | **361 µs** |

**Key insight:** For small allocations (1–16 pages, typical for NIC ring buffers), DMA allocation is comparable to `malloc()` in userspace. Large allocations (>256 pages) are dominated by pinning overhead.

### 4.3 DMA Free Cost

| Component | Cost |
|-----------|------|
| IOMMU unmap | 500 ns |
| IOTLB flush | 1–5 µs (posted) |
| Unpin pages | 300 ns |
| Bitmap release | 50 ns |
| Cap revoke (if shared) | 500 ns–2 µs |
| **Total (unshared)** | **2.3–6.8 µs** |
| **Total (shared, 3 holders)** | **4.3–12.8 µs** |

### 4.4 Allocation Throughput

| Scenario | Throughput |
|----------|-----------|
| Sequential (1 page, no contention) | 300K–500K allocs/sec |
| Sequential (4 pages, typical NIC) | 250K–400K allocs/sec |
| Under fragmentation (50% allocated) | 150K–250K allocs/sec |
| Pool at 90% capacity | 50K–100K allocs/sec |
| Concurrent (4 threads) | 200K–350K allocs/sec |

---

## 5. Channel IPC Throughput

### 5.1 Channel Send Path

```
cap_send_msg(chan_wr, payload, caps):
  1. Validate chan_wr capability (type, permissions)    ~200 ns
  2. Acquire channel lock                               ~100 ns
  3. Copy payload to channel buffer                     ~100 ns (64B)
                                                       ~400 ns (4KB)
  4. Validate and transfer caps (if any)                ~300 ns per cap
  5. Enqueue to receiver's queue                        ~50 ns
  6. Wake receiver (if blocked)                         ~200 ns
  7. Release lock                                       ~50 ns
                                                       ─────────────
  Total (64B, no caps):                                ~1.0 µs
  Total (64B, 1 MEM cap):                              ~1.3 µs
  Total (4KB, no caps):                                ~1.3 µs
```

### 5.2 Channel Receive Path

```
cap_recv_msg(chan_rd, buf, cap_slots):
  1. Validate chan_rd capability                        ~200 ns
  2. Acquire channel lock                               ~100 ns
  3. Dequeue from queue                                 ~50 ns
  4. Copy payload to receiver buffer                    ~100 ns (64B)
  5. Copy caps to receiver's cap table                  ~300 ns per cap
  6. Release lock                                       ~50 ns
                                                       ─────────────
  Total (64B, no caps):                                ~0.5 µs
  Total (64B, 1 MEM cap):                              ~0.8 µs
```

### 5.3 Round-Trip Latency

| Payload Size | Send | Recv | Context Switch | Round-Trip |
|-------------|------|------|---------------|------------|
| 8B | 0.8 µs | 0.4 µs | 3–8 µs | 4.2–9.2 µs |
| 64B | 1.0 µs | 0.5 µs | 3–8 µs | 4.5–9.5 µs |
| 256B | 1.1 µs | 0.6 µs | 3–8 µs | 4.7–9.7 µs |
| 1KB | 1.2 µs | 0.7 µs | 3–8 µs | 4.9–9.9 µs |
| 4KB | 1.3 µs | 0.8 µs | 3–8 µs | 5.1–10.1 µs |
| 64B + 1 MEM cap | 1.3 µs | 0.8 µs | 3–8 µs | 5.1–10.1 µs |

**The context switch dominates** for all payload sizes. Channel message overhead is sub-microsecond.

### 5.4 Throughput

| Configuration | Throughput |
|--------------|-----------|
| Unidirectional, 64B | 500K–1M msgs/sec |
| Unidirectional, 4KB | 300K–600K msgs/sec |
| Ping-pong, 64B | 100K–200K round-trips/sec |
| With 1 MEM cap transfer | 400K–800K msgs/sec |
| 4 concurrent channels | 1.5M–3M msgs/sec (aggregate) |

### 5.5 Channel vs. Linux IPC

| Mechanism | Latency | Throughput | Notes |
|-----------|---------|-----------|-------|
| AeroSLS channel (64B) | 4.5–9.5 µs | 500K–1M/s | Capability-validated |
| Linux futex | 1–3 µs | 1–5M/s | No cap validation |
| Linux UNIX socket | 5–15 µs | 200K–500K/s | Full kernel path |
| Linux shared memory | 0.1–1 µs | 10M+/s | No kernel involvement |
| AeroSLS shared memory (MEM cap) | 0.1–0.5 µs | 5M+/s | Direct mapping |

**AeroSLS channels are comparable to Linux UNIX sockets** and faster than network-based IPC. For highest throughput, sidecars should use MEM-cap shared memory directly (bypassing channel messages for data, using channels only for control).

---

## 6. Zero-Copy NIC Throughput

### 6.1 RX Path Analysis

The zero-copy receive path for a single packet:

```
Step 1: NIC DMA-writes packet to driver's RX DMA buffer
        Cost: 0 (hardware, concurrent with CPU)

Step 2: NIC raises interrupt → kernel delivers IRQ message
        Cost: 6.85 µs (T0→T10 from §3.2)

Step 3: Driver handler reads RX descriptor
        Cost: ~200 ns (MMIO read via IO_PORT cap)

Step 4: Driver creates MEM cap for the DMA buffer
        Cost: ~300 ns (cap_create_mem in driver table)

Step 5: Driver sends RX_PACKET message with MEM cap to stack
        Cost: ~1.3 µs (channel send with 1 cap)

Step 6: Context switch to network stack
        Cost: 3–8 µs

Step 7: Stack receives message, maps MEM cap
        Cost: ~500 ns (cap_map)

Step 8: Stack processes packet (TCP/IP parsing)
        Cost: 2–5 µs (packet-size dependent)

Step 9: Stack calls cap_map to read packet data
        Cost: ~100 ns (page already mapped)

Total RX latency: 14–23 µs (p50: ~18 µs)
```

### 6.2 TX Path Analysis

```
Step 1: Stack fills TX DMA buffer (via MEM cap)
        Cost: ~500 ns (memcpy or direct write)

Step 2: Stack sends TX_PACKET message to driver
        Cost: ~1.3 µs (channel send with 1 cap)

Step 3: Context switch to driver
        Cost: 3–8 µs

Step 4: Driver reads TX descriptor, signals device (TDT write)
        Cost: ~300 ns (MMIO write via IO_PORT cap)

Step 5: NIC DMA-reads from TX buffer and transmits
        Cost: 0 (hardware, concurrent with CPU)

Total TX latency: 5.1–10.1 µs (p50: ~7 µs)
```

### 6.3 Throughput Calculation

**RX throughput (1514B packets, no coalescing):**

```
Per-packet RX cost: ~18 µs
Max pps: 1,000,000 / 18 = 55,555 pps
Throughput: 55,555 × 1514 × 8 = 673 Mbps
```

**TX throughput (1514B packets):**

```
Per-packet TX cost: ~7 µs
Max pps: 1,000,000 / 7 = 142,857 pps
Throughput: 142,857 × 1514 × 8 = 1.73 Gbps (NIC-limited to 1 Gbps)
```

**Bidirectional (1514B packets, alternating RX/TX):**

```
Per-packet cost: ~18 µs (RX-dominated)
Max pps: 55,555 pps each direction
Total throughput: ~1.35 Gbps (NIC-limited to 2 Gbps aggregate)
```

### 6.4 With IRQ Coalescing

| Coalesce | RX pps | RX Mbps | TX pps | TX Mbps | Bidir Mbps |
|----------|--------|---------|--------|---------|-----------|
| 1× | 55K | 673 | 143K | 1730 | 1350 |
| 4× | 130K | 1579 | 200K | 2428 | 2000 |
| 8× | 180K | 2185 | 230K | 2793 | 2400 |
| 16× | 220K | 2670 | 250K | 3035 | 2700 |

With 8× coalescing, AeroSLS exceeds 1 Gb/s line rate for both RX and TX. The bottleneck shifts from CPU overhead to NIC hardware limits.

### 6.5 Copy vs. Zero-Copy Comparison

| Path | Per-Packet Cost | Throughput (1514B) | CPU Util at 1 Gb/s |
|------|----------------|-------------------|-------------------|
| Zero-copy (AeroSLS) | 18 µs RX | 673 Mbps | 35–45% |
| Copy (AeroSLS) | 22 µs RX | 550 Mbps | 45–55% |
| Zero-copy (Linux) | 5 µs RX | 2.4 Gbps | 8–12% |
| Copy (Linux) | 8 µs RX | 1.5 Gbps | 12–18% |

**Zero-copy saves ~4 µs per packet** (the memcpy cost for 1514 bytes). This is a 22% improvement over the copy path in AeroSLS.

---

## 7. Capability System Overhead

### 7.1 Capability Validation Cost

Every capability access (cap_send, cap_recv, cap_map, cap_revoke) requires validation:

```
cap_word_valid(w):
  1. Check state == VALID                    ~10 ns (branch)
  2. Extract type, check against expected    ~10 ns (shift + mask)
  3. Extract permissions, check required     ~10 ns (AND + branch)
  4. Look up CapObject by ID                 ~50 ns (array index)
  5. Check holder list contains caller       ~100 ns (linked list scan)
                                           ────────
  Total:                                    ~180 ns
```

### 7.2 Capability Creation Cost

```
cap_create_mem / cap_create_irq / etc.:
  1. Syscall entry + argument validation     ~500 ns
  2. Anti-aliasing check (IO_PORT overlap)   ~200 ns (256-object scan)
  3. CapObject allocation                     ~100 ns
  4. Cap word construction                    ~50 ns
  5. Cap table slot allocation                ~100 ns
  6. Holder list linkage                      ~50 ns
  7. Syscall exit                             ~200 ns
                                           ────────
  Total:                                    ~1.2 µs
```

### 7.3 Capability Revocation Cost

```
cap_revoke(cap_idx):
  1. Validate cap exists and is VALID        ~200 ns
  2. Set object state to REVOKED             ~10 ns
  3. IRQ: mask at PLIC                       ~200 ns
  4. DMA_MEM: IOMMU unmap                    ~500 ns–2 µs
  5. DMA_MEM: IOTLB flush                    ~1–5 µs
  6. Walk holder list, stamp each slot       ~100 ns × N holders
  7. Clear holder list                       ~50 ns
  8. Free object resources                   ~200 ns–1 µs
                                           ────────
  Total (no holders):                       ~1.3–3.5 µs
  Total (3 holders):                        ~1.6–3.8 µs
  Total (DMA_MEM, 3 holders):               ~3.3–10.8 µs
```

### 7.4 Cap Table Lookup Performance

| Table Size | Lookup Time | Notes |
|-----------|------------|-------|
| 32 entries | ~50 ns | Small sidecar |
| 64 entries | ~80 ns | Typical driver |
| 128 entries | ~120 ns | Complex driver |
| 256 entries | ~180 ns | Maximum configured |

Cap table lookup is O(1) with a direct-indexed array, but the cache footprint grows with table size. At 256 entries × 16 bytes/entry = 4 KB, the table fits in L1D cache.

---

## 8. Context Switch Cost

### 8.1 RISC-V Context Switch Breakdown

```
Context switch (sidecar A → sidecar B):
  1. Save callee-saved registers (s0-s11)    ~12 × 8 ns = 96 ns
  2. Save FP registers (if used)             ~0 ns (lazy, Phase 4)
  3. Save trap frame (if from interrupt)     ~200 ns
  4. Store stack pointer                     ~8 ns
  5. Load new stack pointer                  ~8 ns
  6. Restore callee-saved registers          ~96 ns
  7. Switch satp register (ASID-based)       ~100 ns
  8. TLB refill (ASID, no full flush)        ~200 ns–1 µs
  9. Update per-CPU scheduler state          ~50 ns
                                           ────────
  Total (best case, warm TLB):              ~550 ns
  Total (typical):                          3–5 µs
  Total (worst case, cold TLB):             8–15 µs
```

### 8.2 ASID-Based TLB Optimization

With ASID (Address Space Identifier), the TLB does not need to be flushed on every context switch. Each sidecar gets a unique ASID, and TLB entries are tagged with the ASID. This reduces the context switch cost from ~10–15 µs (full TLB flush) to ~3–5 µs (ASID switch only).

**TLB pressure analysis:**

| Sidecar | Working Set | TLB Entries Needed | TLB Capacity (64-entry) |
|---------|------------|-------------------|------------------------|
| NIC driver | 128 KB (rings + code) | 32 pages | 50% |
| Network stack | 512 KB (buffers + code) | 128 pages | Overflow → 2/page |
| POSIX sidecar | 2 MB (heap + code) | 512 pages | Overflow → 8/page |
| Device Manager | 64 KB (code + data) | 16 pages | 25% |

The NIC driver fits entirely in the DTLB. The network stack causes moderate TLB pressure. The POSIX sidecar causes significant TLB pressure with 8× overflow.

---

## 9. IOMMU Performance

### 9.1 IOMMU TLB Miss Cost

| Operation | Cost | Notes |
|-----------|------|-------|
| IOMMU TLB hit | 50–100 ns | No page table walk |
| IOMMU TLB miss (256 entry) | 500 ns–1 µs | 4-level walk |
| IOMMU TLB miss (4K entry) | 1–2 µs | With deeper tables |
| IOTLB flush (single domain) | 1–3 µs | Posted, non-blocking |
| IOTLB flush (all domains) | 3–10 µs | Global flush |

### 9.2 IOMMU Mapping Cost

| Pages | Mapping Cost | Notes |
|-------|-------------|-------|
| 1 | 500 ns | Single page table entry |
| 64 | 600 ns | Same page table level |
| 256 | 700 ns | Still within one PMD |
| 1024 | 1 µs | Crosses PMD boundary |
| 4096 (16 MB) | 2 µs | Multiple PMDs |

### 9.3 IOMMU vs. No IOMMU

| Metric | With IOMMU | Without IOMMU | Overhead |
|--------|-----------|--------------|---------|
| DMA latency (first access) | +500 ns | 0 | +500 ns |
| DMA latency (steady state) | +50 ns | 0 | +50 ns |
| DMA setup (per buffer) | +500 ns | 0 | +500 ns |
| Security | Confined | Unconfined | N/A |

The IOMMU adds approximately 50 ns per DMA access in steady state (TLB hit) and 500 ns for the first access after allocation (TLB miss). This is a 1–3% overhead on NIC throughput at 1 Gb/s.

---

## 10. Micro-Architectural Effects

### 10.1 Cache Effects

**L1D Cache Pressure:**

| Data Structure | Size | L1D Impact |
|---------------|------|-----------|
| Capability table (256 entries) | 4 KB | 12.5% of 32 KB L1D |
| Channel queue (64 messages) | 4 KB | 12.5% of L1D |
| DMA bitmap (8192 bits) | 1 KB | 3.1% of L1D |
| IRQ registry (128 entries) | 8 KB | 25% of L1D |
| **Total kernel data** | **~17 KB** | **53% of L1D** |

The kernel's data structures consume over half of L1D cache. This leaves limited space for sidecar code and data, increasing cache miss rates.

**Mitigation:** Pin frequently-accessed kernel structures (cap table, channel queues) to specific cache lines. Use cache-line-aligned allocations for hot paths.

**L2 Cache Contention:**

With 4 cores sharing 2 MB L2, each core gets ~512 KB effective L2. The NIC driver's working set (128 KB rings + code) fits in L2. The network stack's working set (512 KB) exceeds per-core L2 allocation, causing L2 thrashing.

**Mitigation:** Use cache partitioning (if available) to reserve L2 for the NIC driver core.

### 10.2 Branch Prediction

The capability validation path has several conditional branches:

```
if (cap_word_valid(w))          // predictable after first access
if (type == expected)           // predictable for same cap type
if (permissions & required)     // predictable for same operations
if (holder_list_contains(...))  // branchy, ~30% mispredict
```

Estimated branch misprediction rate: 5–10% on the capability validation path. Each misprediction costs ~10–15 cycles (15 ns at 1.5 GHz).

### 10.3 Memory Ordering

RISC-V has a weak memory model. The capability system uses atomic operations for:

- Cap table slot updates (`atomic_store` with release semantics)
- Channel queue operations (lock-based, not lock-free)
- Holder list modifications (linked list with CAS)

The lock-based channel implementation serializes access, which is correct but limits scalability. A lock-free SPSC (single-producer, single-consumer) channel would reduce latency by ~200 ns per message (eliminating lock acquire/release).

### 10.4 TLB Pressure

| Sidecar | Pages in Working Set | DTLB Entries | TLB Miss Rate |
|---------|---------------------|-------------|---------------|
| NIC driver | 32 | 32 | <1% |
| Network stack | 128 | 64 | ~50% overflow |
| POSIX sidecar | 512 | 64 | ~88% overflow |
| Device Manager | 16 | 16 | <1% |

The network stack experiences significant TLB pressure, with ~50% of accesses requiring a TLB refill. Each refill costs ~100–200 ns, adding ~1–2 µs per packet to the stack's processing time.

**Mitigation:** Use 2 MB superpages for the network stack's buffer pool to reduce TLB pressure by 512×.

### 10.5 Prefetching

The DMA allocator's bitmap scan is sequential (first-fit), which benefits from hardware prefetching. The capability table lookup is random (by cap index), which does not benefit from prefetching. The channel queue dequeue is sequential within a message, which benefits from prefetching.

Estimated prefetch effectiveness:
- DMA bitmap scan: 80% of accesses prefetched
- Cap table lookup: 20% of accesses prefetched
- Channel queue: 70% of accesses prefetched

---

## 11. Comparison with Linux In-Kernel Drivers

### 11.1 Architecture Comparison

| Aspect | AeroSLS | Linux |
|--------|---------|-------|
| Driver execution | User-space sidecar | Kernel module |
| Isolation | Capability tables + page tables | Kernel trust |
| IPC | Channel messages + MEM caps | Function calls + sk_buff |
| DMA | DMA_MEM cap + IOMMU | `dma_map_single` + IOMMU |
| Interrupt | IRQ channel message | IRQ handler + softirq |
| Memory management | Capability arena | Kernel allocator |
| Context switch | Sidecar ↔ kernel | Process ↔ kernel |

### 11.2 Overhead Breakdown

| Overhead Source | AeroSLS | Linux | Delta |
|----------------|---------|-------|-------|
| Context switch | 3–8 µs/pkt | 0 (in-kernel) | +3–8 µs |
| Capability validation | 180 ns/access | 0 (trusted) | +180 ns |
| Channel message | 1 µs/send | 0 (function call) | +1 µs |
| IOMMU mapping | 500 ns/setup | 500 ns/setup | 0 |
| DMA allocation | 3 µs | 1 µs (CMA) | +2 µs |
| IRQ delivery | 6.85 µs | 3–5 µs | +2–4 µs |
| **Total per packet** | **~18 µs** | **~5 µs** | **+13 µs** |

### 11.3 When AeroSLS Wins

AeroSLS does not aim to outperform Linux on raw throughput. It provides:

1. **Strong isolation:** A compromised driver cannot corrupt the kernel or other drivers.
2. **Crash recovery:** Driver crashes are recoverable without kernel panic.
3. **Language flexibility:** Drivers can be written in Rust, C, or any language.
4. **Capability-based security:** Fine-grained access control without discretionary DAC.

These properties are worth 2–5× overhead for safety-critical or untrusted driver scenarios.

---

## 12. Scalability Analysis

### 12.1 Number of Drivers

| Drivers | Cap Table Total | IRQ Registry | DMA Pool Usage | Impact |
|---------|----------------|-------------|---------------|--------|
| 1 | 256 | 128 entries | 32 MiB | Baseline |
| 4 | 1024 | 128 entries | 32 MiB | Minimal |
| 16 | 4096 | 128 entries | 32 MiB | Cap table L2 pressure |
| 64 | 16384 | 128 entries | 32 MiB | Cap table spills to L3 |

With 16+ drivers, the aggregate cap table size exceeds L2 cache, increasing lookup latency from ~50 ns to ~200 ns.

### 12.2 Number of Channels

| Channels | Queue Memory | Lock Contention | Throughput Impact |
|----------|-------------|----------------|------------------|
| 1 | 4 KB | None | Baseline |
| 4 | 16 KB | Low | -5% |
| 16 | 64 KB | Medium | -15% |
| 64 | 256 KB | High | -30% |

With 64+ concurrent channels, the channel lock becomes a serialization bottleneck. Lock-free channels would eliminate this.

### 12.3 Interrupt Rate

| Rate | Coalesce Needed? | CPU Overhead | Notes |
|------|-----------------|-------------|-------|
| <10K/s | No | <1% | Low-rate devices (serial, USB) |
| 10K–100K/s | Recommended | 1–10% | Moderate NIC traffic |
| 100K–1M/s | Required | 10–50% | High-rate NIC traffic |
| >1M/s | Aggressive | >50% | 10 Gb/s+ NICs |

---

## 13. Optimization Opportunities

### 13.1 High-Impact Optimizations

| Optimization | Expected Gain | Effort | Risk |
|-------------|--------------|--------|------|
| Lock-free SPSC channels | -200 ns/msg | Medium | Low |
| Batch channel sends (NAPI-style) | -50% ctx switches | Low | Low |
| 2 MB superpages for DMA pool | -500 ns IOMMU miss | Low | Low |
| ASID-based TLB (already planned) | -5 µs ctx switch | Medium | Low |
| IRQ coalescing tuning | -90% IRQ overhead | Low | Low |
| Cap table caching (hot caps) | -100 ns/lookup | Medium | Medium |

### 13.2 Medium-Impot optimizations

| Optimization | Expected Gain | Effort | Risk |
|-------------|--------------|--------|------|
| Dedicated IRQ core (isolcpus) | -30% tail latency | Low | Low |
| DMA pool per-device partitioning | -50% lock contention | Medium | Low |
| Zero-copy channel bypass (MEM-only) | -1 µs/msg | Medium | Medium |
| IOMMU superpage support | -2 µs mapping | High | Medium |

### 13.3 Low-Priority Optimizations

| Optimization | Expected Gain | Effort | Risk |
|-------------|--------------|--------|------|
| Cap table hash lookup | -30 ns (small tables) | Low | Low |
| Channel queue batching | -100 ns amortized | Medium | Medium |
| Pre-allocated cap slots | -50 ns allocation | Low | Low |

---

## 14. Benchmark Plan Summary

### 14.1 Host-Side Benchmarks (benchmarks/phase4/)

| Benchmark | What It Measures | Iterations |
|-----------|-----------------|-----------|
| IRQ delivery | Channel-based interrupt latency | 10,000 |
| IRQ coalescing | Amortized IRQ cost at 2×–16× | 10,000 each |
| DMA single alloc | 1-page allocation latency | 10,000 |
| DMA alloc sizes | 1–1024 page allocation | 1,000 each |
| DMA alloc+free | Round-trip latency | 10,000 |
| DMA fragmented | Allocation under fragmentation | 10,000 |
| DMA exhaustion | Latency at 10–99% pool full | 1,000 each |
| Channel ping-pong | Round-trip IPC latency | 5,000 |
| Channel sizes | 8B–2048B unidirectional | 10,000 each |
| Channel cap transfer | MEM cap overhead | 5,000 |
| Channel backpressure | Producer/consumer mismatch | 5,000 |
| Channel multi | 2–16 concurrent channels | 5,000 each |
| NIC throughput | Zero-copy vs. copy model | Analytical |
| MEM cap creation | Per-packet cap overhead | 10,000 |
| IOMMU TLB miss | Page table walk cost | 10,000 |

### 14.2 On-Target Benchmarks

| Benchmark | What It Measures | Duration |
|-----------|-----------------|---------|
| IRQ latency (instrumented) | Real PLIC → handler entry | 10,000 IRQs |
| DMA alloc (instrumented) | Real IOMMU programming | 10,000 allocs |
| Channel throughput | Real context switches | 30s sustained |
| iperf3 TCP | End-to-end throughput | 30s × 3 runs |
| pktgen UDP | Maximum packet rate | 10s × 3 runs |
| Zero-copy validation | Copy vs. no-copy path | 30s each |
| Crash recovery | Driver restart time | 10 crashes |

### 14.3 Regression Gates

| Metric | Gate | Action |
|--------|------|--------|
| IRQ p99 latency | < 50 µs | Block merge |
| DMA alloc median | < 10 µs | Block merge |
| Channel round-trip | < 15 µs | Block merge |
| TCP throughput | > 300 Mbps | Warning |
| Recovery time | < 100 ms | Block merge |

---

## 15. Risk Factors and Confidence Intervals

### 15.1 Estimation Uncertainty

| Factor | Uncertainty | Impact |
|--------|-----------|--------|
| QEMU vs. real hardware | ±50% | Context switch, IRQ latency |
| Cache behavior (analytical) | ±30% | All latency estimates |
| IOMMU TLB behavior | ±40% | DMA latency |
| Linux baseline (published) | ±20% | Comparison accuracy |
| Coalescing effectiveness | ±25% | Throughput estimates |

### 15.2 Confidence Intervals (95%)

| Metric | Low | Central | High |
|--------|-----|---------|------|
| IRQ-to-handler (p50) | 10 µs | 18 µs | 30 µs |
| DMA alloc (1 page) | 1.5 µs | 3 µs | 6 µs |
| Channel round-trip (64B) | 3 µs | 7 µs | 12 µs |
| TCP throughput (1 Gb/s NIC) | 300 Mbps | 600 Mbps | 850 Mbps |
| Recovery time | 15 ms | 50 ms | 120 ms |

### 15.3 Known Unknowns

1. **Real hardware IRQ latency:** QEMU emulates PLIC with different timing than real hardware. Real RISC-V boards may have 2–5× different interrupt latency.
2. **IOMMU TLB behavior:** RISC-V IOMMU implementations vary. Some have 64-entry TLBs, others have 1024-entry. TLB miss rates are highly implementation-dependent.
3. **Driver complexity:** The e1000 is a simple NIC. More complex drivers (NVMe, GPU) may have higher per-packet overhead due to larger ring buffers and more complex descriptor processing.
4. **Multi-queue NICs:** The analysis assumes single-queue NICs. Multi-queue NICs (RSS, RSSv2) can distribute interrupt load across cores, reducing per-core overhead.

---

## Appendix A: Instruction Count Estimates

| Operation | Instruction Count | Cycles (1.5 GHz) | Time |
|-----------|------------------|-------------------|------|
| Trap entry (RISC-V) | ~20 | ~30 | 200 ns |
| PLIC claim | ~10 | ~15 | 100 ns |
| IRQ cap lookup | ~30 | ~45 | 300 ns |
| Channel enqueue | ~50 | ~75 | 500 ns |
| Context switch | ~100 | ~150 | 1 µs |
| Cap validation | ~25 | ~38 | 250 ns |
| Cap table lookup | ~15 | ~23 | 150 ns |
| IOMMU page table walk | ~60 | ~90 | 600 ns |
| DMA bitmap scan (1 KB) | ~200 | ~300 | 200 ns |

## Appendix B: Comparison with Published Linux Numbers

| Metric | Linux Value | Source |
|--------|-----------|--------|
| e1000 IRQ latency (p50) | 3–5 µs | LKML benchmarks |
| e1000 IRQ latency (p99) | 8–12 µs | LKML benchmarks |
| NAPI poll batch | 64 packets | `net/core/dev.c` |
| `dma_map_single` | 0.5–1 µs | IOMMU-enabled |
| Context switch (same VM) | 1–3 µs | `perf bench sched` |
| futex wake/wait | 1–2 µs | `strace -T` |
| UNIX socket round-trip | 5–10 µs | `netperf` |

---

## Document History

| Version | Date | Changes |
|---------|------|---------|
| 1.0 | 08-23-2026 | Initial Phase 4 performance estimates |

---

*This document provides analytical estimates. Actual performance must be validated with the benchmark harness (`benchmarks/phase4/`) and on-target measurements. Update confidence intervals as real data becomes available.*
