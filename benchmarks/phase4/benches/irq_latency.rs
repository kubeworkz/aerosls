//! Criterion benchmark for IRQ-to-handler latency.
//!
//! Run with: `cargo bench --bench irq_latency`

use criterion::{criterion_group, criterion_main, Criterion, BenchmarkId};
use aerosls_phase4_bench::irq_latency;

fn bench_irq_delivery(c: &mut Criterion) {
    let mut group = c.benchmark_group("irq_delivery");
    group.sample_size(1000);

    group.bench_function("single_irq", |b| {
        b.iter(|| {
            irq_latency::bench_irq_delivery(100)
        })
    });

    // Coalescing variants
    for coalesce in [2, 4, 8, 16] {
        group.bench_with_input(
            BenchmarkId::new("coalesced", coalesce),
            &coalesce,
            |b, &cc| {
                b.iter(|| {
                    irq_latency::bench_irq_coalescing(1000, cc)
                })
            },
        );
    }

    group.finish();
}

fn bench_irq_channel(c: &mut Criterion) {
    let mut group = c.benchmark_group("irq_channel");
    group.sample_size(100);

    group.bench_function("channel_capacity", |b| {
        b.iter(|| {
            irq_latency::bench_irq_channel_capacity(10_000)
        })
    });

    group.bench_function("mask_unmask", |b| {
        b.iter(|| {
            irq_latency::bench_irq_mask_unmask(10_000)
        })
    });

    group.finish();
}

criterion_group!(benches, bench_irq_delivery, bench_irq_channel);
criterion_main!(benches);
