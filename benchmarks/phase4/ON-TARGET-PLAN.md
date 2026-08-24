# AeroSLS Phase 4 — On-Target Benchmark Plan

**Version:** 1.0
**Date:** August 23, 2026
**Scope:** Methodology for measuring Phase 4 performance on real hardware (RISC-V 64-bit with IOMMU).

---

## 1. Overview

The host-side benchmarks (`cargo bench`) measure subsystem performance using simulated kernel semantics. This document defines the **on-target** methodology for measuring real hardware performance with actual device drivers, IOMMU, and PLIC interrupts.

### 1.1 Target Hardware

| Component | Specification |
|-----------|--------------|
| CPU | RISC-V 64-bit (e.g., SiFive FU740 or QEMU virt with PLIC) |
| RAM | ≥ 4 GiB (for DMA pool) |
| NIC | e1000 (emulated in QEMU) or virtio-net |
| IOMMU | RISC-V IOMMU or PMP/ePMP |
| Interrupt Controller | PLIC (Platform-Level Interrupt Controller) |
| Firmware | OpenSBI |

### 1.2 QEMU Test Configuration

```bash
qemu-system-riscv64 \
  -machine virt \
  -smp 4 \
  -m 4G \
  -cpu rv64 \
  -device e1000,netdev=net0 \
  -netdev user,id=net0,hostfwd=tcp::8080-:80 \
  -device virtio-iommu-pci \
  -bios default \
  -kernel aerosls.bin
```

---

## 2. Micro-Benchmarks

### 2.1 IRQ-to-Handler Latency

**Objective:** Measure the time from hardware interrupt assertion to driver handler execution.

**Measurement Points:**

```
T0: Hardware interrupt asserted (PLIC interrupt pending bit set)
T1: Trap entry (kernel trap_riscv.S: rdcycle after saving registers)
T2: IRQ cap identified (kernel/irq.c: irq_deliver() entry)
T3: IRQMessage enqueued (kernel/irq.c: cap_send_msg() return)
T4: Driver sidecar wakes (kernel scheduler: context switch to driver)
T5: Driver handler entry (driver lib.rs: message loop recv returns)
T6: Driver reads device register (IO_PORT MMIO read)
T7: Driver calls irq_ack() (SYS_SLS_IRQ_ACK syscall entry)
T8: PLIC claim/complete (kernel: PLIC_CLAIM register write)
```

**Primary Metric:** T0 → T5 (interrupt-to-handler)
**Secondary Metrics:** T0 → T8 (full round-trip), T1 → T3 (kernel overhead)

**Methodology:**

1. Instrument `trap_riscv.S` to write `rdcycle` to a dedicated memory-mapped timestamp buffer at T0 and T1.
2. Instrument `irq_deliver()` to write timestamp at T2 and T3.
3. Driver sidecar reads timestamp buffer via MEM cap at T5.
4. Driver writes T6, T7 via IO_PORT (memory-mapped log buffer).
5. Kernel writes T8 in `irq_eoi()`.

**Expected Results:**

| Metric | Target | Linux Baseline |
|--------|--------|---------------|
| T0→T5 (handler entry) | 10–28 µs | 3–10 µs |
| T0→T8 (full round-trip) | 15–35 µs | 5–15 µs |
| T1→T3 (kernel overhead) | 3–8 µs | N/A (in-kernel) |

**Test Cases:**

| Test | Description |
|------|-------------|
| IRQ-1 | Single IRQ, no coalescing, 64B payload |
| IRQ-2 | Single IRQ, coalesce=4 |
| IRQ-3 | Single IRQ, coalesce=16 |
| IRQ-4 | Burst: 256 IRQs in rapid succession |
| IRQ-5 | Shared IRQ line (multiple devices) |
| IRQ-6 | IRQ under load (CPU at 80% utilization) |

---

### 2.2 DMA Allocation Throughput

**Objective:** Measure the performance of the DMA buffer pool allocator.

**Measurement Points:**

```
T0: SYS_SLS_DMA_ALLOC syscall entry
T1: Bitmap search complete (first-fit found)
T2: Frames marked in bitmap
T3: Buffer descriptor allocated
T4: Pages pinned (frame_pool pin)
T5: IOMMU page table programmed
T6: MEM cap created
T7: Syscall return (cap_idx + dma_buf_id)
```

**Primary Metric:** T0 → T7 (full allocation latency)
**Secondary Metrics:** T1 (search time), T5 (IOMMU programming time)

**Methodology:**

1. Instrument `kernel/dma.c: dma_alloc()` with timestamp reads at each measurement point.
2. Allocate timestamps to a dedicated MEM region readable by the benchmark sidecar.
3. Benchmark sidecar calls `SYS_SLS_DMA_ALLOC` in a loop, recording timestamps.

**Expected Results:**

| Metric | Target |
|--------|--------|
| Single alloc (1 page) | 2–5 µs |
| Single alloc (64 pages) | 3–8 µs |
| Alloc+free round-trip | 5–12 µs |
| Sustained throughput | 100K–500K allocs/sec |

**Test Cases:**

| Test | Description |
|------|-------------|
| DMA-1 | Allocate 1 page, 4K aligned |
| DMA-2 | Allocate 64 pages, 64K aligned |
| DMA-3 | Allocate 1024 pages, 2M aligned |
| DMA-4 | Alloc+free cycle, 100K iterations |
| DMA-5 | Fragmented pool (50% allocated, random holes) |
| DMA-6 | Pool at 90% capacity, measure degradation |
| DMA-7 | Concurrent allocation (4 threads) |

---

### 2.3 Channel Message Throughput

**Objective:** Measure send/recv latency and throughput for the channel IPC system.

**Measurement Points:**

```
T0: cap_send_msg() syscall entry
T1: Message payload copied to channel buffer
T2: Capability validated and transferred (if any)
T3: Receiver woken (scheduler: cap_wake_chan)
T4: cap_recv_msg() syscall entry (receiver)
T5: Message copied to receiver buffer
T6: cap_recv_msg() return
```

**Primary Metric:** T0 → T6 (round-trip for ping-pong)
**Secondary Metrics:** T0 → T3 (send latency), T4 → T6 (recv latency)

**Methodology:**

1. Instrument `kernel/ipc.c: cap_send_msg()` and `cap_recv_msg()` with timestamps.
2. Driver sidecar and client sidecar communicate via a channel pair.
3. Benchmark: client sends request, driver echoes reply (ping-pong).

**Expected Results:**

| Metric | Target |
|--------|--------|
| Ping-pong (64B) | 4–10 µs |
| Unidirectional (64B) | 2–5 µs |
| Unidirectional (4KB) | 3–8 µs |
| With cap transfer (64B + MEM) | 5–12 µs |
| Sustained throughput | 200K–1M msgs/sec |

**Test Cases:**

| Test | Description |
|------|-------------|
| CHAN-1 | Ping-pong, 64B payload |
| CHAN-2 | Ping-pong, 4KB payload |
| CHAN-3 | Unidirectional, 64B–2048B payloads |
| CHAN-4 | With MEM cap transfer |
| CHAN-5 | 4 concurrent channel pairs |
| CHAN-6 | Backpressure (producer 10x faster than consumer) |
| CHAN-7 | Channel with 1024 pending messages |

---

### 2.4 Context Switch Overhead

**Objective:** Measure the cost of switching between sidecar contexts.

**Measurement Points:**

```
T0: Sidecar A calls blocking syscall (cap_recv_msg with no data)
T1: Kernel saves sidecar A context
T2: Kernel loads sidecar B context
T3: Sidecar B entry point
```

**Primary Metric:** T0 → T3 (full context switch)

**Methodology:**

1. Two sidecars ping-pong a message.
2. Measure the time between sidecar A's send and sidecar A's recv (includes the context switch to B and back).

**Expected Results:**

| Metric | Target | Linux Baseline |
|--------|--------|---------------|
| Context switch (same core) | 3–8 µs | 1–3 µs |
| With TLB flush | 5–15 µs | 2–5 µs |

---

## 3. Macro-Benchmarks

### 3.1 NIC Throughput (iperf3)

**Objective:** Measure end-to-end TCP throughput through the AeroSLS NIC driver sidecar.

**Setup:**

```
┌─────────────┐     ┌─────────────┐     ┌─────────────┐
│   Host      │────►│  AeroSLS    │────►│   QEMU      │
│   (iperf3   │     │  NIC Driver │     │   e1000     │
│    client)  │     │  + Stack    │     │   (server)  │
└─────────────┘     └─────────────┘     └─────────────┘
```

**Methodology:**

1. Boot AeroSLS with e1000 NIC driver sidecar.
2. Run iperf3 server on QEMU host.
3. Run iperf3 client inside AeroSLS (via POSIX sidecar).
4. Measure TCP throughput, CPU utilization, and packet loss.

**Expected Results:**

| Metric | AeroSLS Target | Linux Baseline |
|--------|---------------|---------------|
| TCP throughput (1 flow) | 400–800 Mbps | 940 Mbps (1 Gb/s line rate) |
| TCP throughput (4 flows) | 600–900 Mbps | 940 Mbps |
| UDP throughput | 500–800 Mbps | 940 Mbps |
| Packets/sec (64B) | 200K–400K | 1.488M (theoretical max) |
| CPU utilization | 30–60% | 5–15% |

**Test Cases:**

| Test | Description |
|------|-------------|
| NET-1 | TCP throughput, 10s, 1 flow |
| NET-2 | TCP throughput, 30s, 4 flows |
| NET-3 | UDP throughput, 10s, 1 Gbps target |
| NET-4 | Packet rate (64B packets, pktgen) |
| NET-5 | Latency under load (netperf RR) |
| NET-6 | Throughput with IRQ coalescing (1, 4, 8, 16) |

---

### 3.2 Zero-Copy Path Validation

**Objective:** Verify that the zero-copy path eliminates data copies between driver and stack.

**Setup:**

1. Instrument the NIC driver to log buffer addresses at each stage.
2. Verify that the stack reads directly from the DMA buffer (same physical address).
3. Compare throughput with a "copy" variant (memcpy before sending to stack).

**Methodology:**

1. Add a flag to the NIC driver: `ZERO_COPY=1` vs `ZERO_COPY=0`.
2. Run iperf3 with each variant.
3. Measure throughput difference.

**Expected Results:**

| Variant | Throughput | CPU Util |
|---------|-----------|----------|
| Zero-copy | 600 Mbps | 35% |
| Copy | 400 Mbps | 55% |
| Improvement | +50% | -36% |

---

### 3.3 Crash Recovery Time

**Objective:** Measure how quickly the Device Manager can restart a crashed driver.

**Measurement Points:**

```
T0: Driver crash detected (heartbeat timeout or signal)
T1: Device Manager quarantine (IRQ mask + cap revoke)
T2: Client notified (EVT_DEVICE_REMOVED)
T3: Device Manager spawn new driver
T4: Driver init complete (MMIO mapped, rings initialized)
T5: Driver sends NIC_INFO to stack
T6: Stack resumes packet processing
```

**Primary Metric:** T0 → T6 (full recovery time)

**Expected Results:**

| Metric | Target |
|--------|--------|
| T0→T1 (quarantine) | 1–5 ms |
| T1→T3 (respawn) | 10–50 ms |
| T3→T5 (driver init) | 5–20 ms |
| T5→T6 (service resume) | 1–5 ms |
| **Total recovery** | **20–80 ms** |

---

## 4. Benchmark Execution

### 4.1 Running Host-Side Benchmarks

```bash
cd benchmarks/phase4
cargo bench --bench irq_latency
cargo bench --bench dma_alloc
cargo bench --bench channel_throughput
cargo bench --bench nic_throughput
```

### 4.2 Running On-Target Benchmarks

```bash
# 1. Build AeroSLS with instrumentation
make BENCHMARK=1 TARGET=riscv64

# 2. Boot in QEMU with NIC
qemu-system-riscv64 -machine virt -smp 4 -m 4G \
  -device e1000,netdev=net0 -netdev user,id=net0 \
  -bios default -kernel aerosls.bin

# 3. Inside AeroSLS, run benchmark sidecars
aerosls-bench --irq-latency --iterations 10000
aerosls-bench --dma-alloc --iterations 100000
aerosls-bench --channel-throughput --duration 30s

# 4. Run iperf3 (requires network stack sidecar)
iperf3 -c host_ip -t 30 -P 4
```

### 4.3 Comparison Baseline

Run the same benchmarks on Linux with the in-kernel e1000 driver:

```bash
# Linux baseline (same QEMU configuration)
iperf3 -c localhost -t 30 -P 4
pktgen -m 64 -c 4 -t 10  # 64B packet rate
```

---

## 5. Reporting

### 5.1 Benchmark Report Format

Each benchmark run produces a JSON report:

```json
{
  "timestamp": "2026-08-23T12:00:00Z",
  "platform": "riscv64-qemu",
  "kernel_config": {
    "cpu_cores": 4,
    "ram_gb": 4,
    "iommu": true,
    "nic": "e1000"
  },
  "benchmarks": [
    {
      "name": "irq_delivery",
      "iterations": 10000,
      "results": {
        "median_us": 18.5,
        "p99_us": 28.2,
        "min_us": 12.1,
        "max_us": 45.3,
        "stddev_us": 3.2
      }
    }
  ],
  "linux_baseline": {
    "irq_latency_us": 5.2,
    "throughput_mbps": 940,
    "cpu_util_pct": 8
  }
}
```

### 5.2 Performance Regression Gate

CI should enforce:

| Metric | Gate | Action on Failure |
|--------|------|-------------------|
| IRQ latency (p99) | < 50 µs | Block merge |
| DMA alloc (median) | < 10 µs | Block merge |
| Channel round-trip | < 15 µs | Block merge |
| TCP throughput | > 300 Mbps | Warning |
| Recovery time | < 100 ms | Block merge |

---

## 6. Micro-Architectural Effects

### 6.1 Dominant Costs

| Cost | Typical | Notes |
|------|---------|-------|
| Context switch | 3–8 µs | Dominates at low message rates |
| Capability validation | 0.5–2 µs | Cap table lookup + permission check |
| IOMMU TLB miss | 0.1–1 µs | First access after alloc; cached after |
| Channel queue ops | 0.2–0.5 µs | Mutex lock + VecDeque push/pop |
| DMA pin/unpin | 1–3 µs | Frame pool bitmap + reference count |
| PLIC claim/complete | 0.5–1 µs | MMIO register access |

### 6.2 Scaling Effects

- **IRQ coalescing:** Amortizes interrupt overhead. 16x coalescing reduces per-packet IRQ cost from ~1.5 µs to ~94 ns.
- **Channel batching:** Sending multiple messages before receiving reduces context switches.
- **IOMMU TLB:** TLB miss penalty is paid once per buffer allocation; subsequent accesses are cached.
- **CPU cache:** DMA buffers should be cache-line aligned to avoid false sharing.

### 6.3 Optimization Opportunities

1. **Batch channel sends:** Send multiple IRQ messages in one syscall.
2. **IOMMU superpages:** Use 2M pages for large DMA buffers to reduce TLB misses.
3. **Lock-free channels:** Replace mutex-based queues with lock-free SPSC queues.
4. **Interrupt steering:** Pin NIC interrupts to a dedicated core to avoid cache pollution.

---

## Document History

| Version | Date | Changes |
|---------|------|---------|
| 1.0 | 08-23-2026 | Initial on-target benchmark plan |
