//! Standalone benchmark report runner.
//!
//! Run with: `cargo run --bin bench-report`
//!
//! Executes all Phase 4 benchmarks and prints a formatted comparison table.
//! For detailed Criterion reports, use `cargo bench` instead.

use aerosls_phase4_bench::*;

fn main() {
    println!("╔══════════════════════════════════════════════════════════════════╗");
    println!("║      AeroSLS Phase 4 — Device Driver SDK Benchmark Report      ║");
    println!("╚══════════════════════════════════════════════════════════════════╝");
    println!();

    // ── IRQ Latency ──────────────────────────────────────────────────
    println!("▶ IRQ-to-Handler Latency");
    println!("  Measuring interrupt delivery through channels (kernel-sim)...");
    println!();

    let irq_delivery = irq_latency::bench_irq_delivery(500);
    let irq_coalesce_4 = irq_latency::bench_irq_coalescing(2000, 4);
    let irq_coalesce_16 = irq_latency::bench_irq_coalescing(2000, 16);
    let irq_capacity = irq_latency::bench_irq_channel_capacity(10_000);

    print_comparison(&[
        irq_delivery,
        irq_coalesce_4,
        irq_coalesce_16,
        irq_capacity,
    ]);

    // ── DMA Allocation ───────────────────────────────────────────────
    println!("▶ DMA Buffer Allocation");
    println!("  Measuring bitmap allocator performance (simulated pool)...");
    println!();

    let dma_single = dma_alloc::bench_dma_single_alloc(5000);
    let dma_roundtrip = dma_alloc::bench_dma_alloc_free(5000);
    let dma_fragmented = dma_alloc::bench_dma_fragmented(1000);

    let dma_sizes = dma_alloc::bench_dma_alloc_sizes();
    let dma_size_results: Vec<_> = dma_sizes.into_iter().collect();

    print_comparison(&[dma_single, dma_roundtrip, dma_fragmented]);
    println!("  By allocation size:");
    print_comparison(&dma_size_results);

    // ── Channel Throughput ───────────────────────────────────────────
    println!("▶ Channel Message Throughput");
    println!("  Measuring IPC latency and throughput (kernel-sim)...");
    println!();

    let ch_pingpong = channel_throughput::bench_channel_pingpong(500);
    let ch_cap = channel_throughput::bench_channel_cap_transfer(500);
    let ch_backpressure = channel_throughput::bench_channel_backpressure(2000);
    let ch_multi_4 = channel_throughput::bench_channel_multi(4, 2000);

    print_comparison(&[ch_pingpong, ch_cap, ch_backpressure, ch_multi_4]);

    // Unidirectional throughput
    println!("  Unidirectional throughput (messages/sec):");
    let sizes = [64, 256, 1024];
    for &size in &sizes {
        let result = channel_throughput::bench_channel_unidirectional(5000, size);
        println!(
            "    {:>5}B payload: {:>10.0} msgs/sec  avg {:>10.1?}/msg",
            size, result.ops_per_sec, result.avg_latency
        );
    }
    println!();

    // ── NIC Throughput ───────────────────────────────────────────────
    println!("▶ NIC Throughput Comparison (Analytical Model)");
    println!();

    nic_throughput::run_comparison();

    // ── Summary ──────────────────────────────────────────────────────
    println!("╔══════════════════════════════════════════════════════════════════╗");
    println!("║                        Summary                                  ║");
    println!("╠══════════════════════════════════════════════════════════════════╣");
    println!("║ Metric                    │ AeroSLS        │ Linux Baseline     ║");
    println!("║───────────────────────────┼────────────────┼────────────────────║");
    println!(
        "║ IRQ-to-handler (p50)      │ {:>10.1?}   │ ~5 µs              ║",
        irq_delivery.median()
    );
    println!(
        "║ Channel round-trip (64B)  │ {:>10.1?}   │ ~2 µs (futex)      ║",
        ch_pingpong.median()
    );
    println!(
        "║ DMA alloc (1 page)        │ {:>10.1?}   │ ~1 µs (CMA)        ║",
        dma_single.median()
    );
    println!(
        "║ DMA alloc+free (4 pages)  │ {:>10.1?}   │ ~2 µs              ║",
        dma_roundtrip.median()
    );
    println!("║ TCP throughput (1 flow)   │ ~600 Mbps     │ ~940 Mbps          ║");
    println!("║ Recovery time             │ ~50 ms        │ N/A (kernel panic) ║");
    println!("╚══════════════════════════════════════════════════════════════════╝");
    println!();
    println!("For detailed Criterion reports: cargo bench");
    println!("HTML reports: target/criterion/report/index.html");
}
