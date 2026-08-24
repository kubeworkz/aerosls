//! Criterion benchmark for IRQ-to-handler latency.
//!
//! Run with: `cargo bench --bench irq_latency`

use criterion::{criterion_group, criterion_main, Criterion, BenchmarkId, black_box};
use aerosls_phase4_bench::irq_latency;

fn bench_irq_delivery(c: &mut Criterion) {
    let mut group = c.benchmark_group("irq_delivery");
    // These benchmarks create threads internally; use smaller sample sizes
    // since the per-iteration work is non-trivial.
    group.sample_size(50);

    group.bench_function("single_irq_100iter", |b| {
        b.iter(|| {
            let result = irq_latency::bench_irq_delivery(black_box(100));
            black_box(result.median())
        })
    });

    // Coalescing variants — measure amortized per-IRQ cost
    for coalesce in [2, 4, 8, 16] {
        group.bench_with_input(
            BenchmarkId::new("coalesced", coalesce),
            &coalesce,
            |b, &cc| {
                b.iter(|| {
                    let iters = (cc as u64) * 250; // total interrupts simulated
                    let result = irq_latency::bench_irq_coalescing(black_box(iters), black_box(cc));
                    black_box(result.median())
                })
            },
        );
    }

    group.finish();
}

fn bench_irq_channel(c: &mut Criterion) {
    let mut group = c.benchmark_group("irq_channel");

    group.bench_function("channel_fill_no_consumer", |b| {
        // Measure how fast we can enqueue messages without a consumer
        b.iter(|| {
            let result = irq_latency::bench_irq_channel_capacity(black_box(10_000));
            black_box(result.samples.len()) // number of messages before full
        })
    });

    group.bench_function("mask_unmask_simulated", |b| {
        b.iter(|| {
            let result = irq_latency::bench_irq_mask_unmask(black_box(10_000));
            black_box(result.median())
        })
    });

    group.finish();
}

criterion_group!(benches, bench_irq_delivery, bench_irq_channel);
criterion_main!(benches);
