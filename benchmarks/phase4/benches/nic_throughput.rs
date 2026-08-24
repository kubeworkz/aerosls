//! Criterion benchmark for NIC throughput comparison.
//!
//! Run with: `cargo bench --bench nic_throughput`

use criterion::{criterion_group, criterion_main, Criterion, BenchmarkId};
use aerosls_phase4_bench::nic_throughput;

fn bench_nic_throughput(c: &mut Criterion) {
    let mut group = c.benchmark_group("nic_throughput");
    group.sample_size(100);

    // Analytical model comparison
    group.bench_function("comparison_table", |b| {
        b.iter(|| {
            nic_throughput::run_comparison()
        })
    });

    // Zero-copy model throughput at various packet sizes
    let model = nic_throughput::ZeroCopyModel::default_model();
    let pkt_sizes = [64, 256, 512, 1024, 1514];
    for &size in &pkt_sizes {
        group.bench_with_input(
            BenchmarkId::new("zerocopy_pps", size),
            &size,
            |b, &sz| {
                b.iter(|| model.max_pps(sz, 1))
            },
        );
    }

    // Coalescing impact
    for cc in [1, 4, 8, 16] {
        group.bench_with_input(
            BenchmarkId::new("coalesced_1514B", cc),
            &cc,
            |b, &cc| {
                b.iter(|| model.max_pps(1514, cc))
            },
        );
    }

    group.finish();
}

fn bench_zerocopy_vs_copy(c: &mut Criterion) {
    let mut group = c.benchmark_group("zerocopy_vs_copy");
    group.sample_size(5000);

    let pkt_sizes = [64, 256, 1024, 1514];
    for &size in &pkt_sizes {
        group.bench_with_input(
            BenchmarkId::new("path", size),
            &size,
            |b, &sz| {
                b.iter(|| {
                    nic_throughput::bench_zerocopy_vs_copy(1000, sz)
                })
            },
        );
    }

    group.finish();
}

fn bench_mem_cap(c: &mut Criterion) {
    let mut group = c.benchmark_group("mem_cap_creation");
    group.sample_size(10_000);

    group.bench_function("create_cap", |b| {
        b.iter(|| {
            nic_throughput::bench_mem_cap_creation(1000)
        })
    });

    group.finish();
}

fn bench_iommu(c: &mut Criterion) {
    let mut group = c.benchmark_group("iommu");
    group.sample_size(5000);

    group.bench_function("tlb_miss", |b| {
        b.iter(|| {
            nic_throughput::bench_iommu_tlb_miss(1000)
        })
    });

    group.finish();
}

criterion_group!(
    benches,
    bench_nic_throughput,
    bench_zerocopy_vs_copy,
    bench_mem_cap,
    bench_iommu
);
criterion_main!(benches);
