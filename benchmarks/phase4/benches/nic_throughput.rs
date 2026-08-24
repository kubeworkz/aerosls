//! Criterion benchmark for NIC throughput comparison.
//!
//! Run with: `cargo bench --bench nic_throughput`

use criterion::{criterion_group, criterion_main, Criterion, BenchmarkId, black_box};
use aerosls_phase4_bench::nic_throughput::{self, ZeroCopyModel, LinuxBaseline};

fn bench_nic_model(c: &mut Criterion) {
    let mut group = c.benchmark_group("nic_model");
    group.sample_size(1000);

    let model = ZeroCopyModel::default_model();

    // Measure zero-copy model evaluation (pure computation, no I/O)
    group.bench_function("zerocopy_rx_latency_1514B", |b| {
        b.iter(|| black_box(model.rx_latency_ns(black_box(1))))
    });

    group.bench_function("zerocopy_tx_latency", |b| {
        b.iter(|| black_box(model.tx_latency_ns()))
    });

    group.bench_function("zerocopy_max_pps_1514B", |b| {
        b.iter(|| black_box(model.max_pps(black_box(1514), black_box(1))))
    });

    // Linux baseline for comparison
    let linux = LinuxBaseline::default_baseline();
    group.bench_function("linux_rx_latency_1514B", |b| {
        b.iter(|| black_box(linux.rx_latency_ns()))
    });

    group.bench_function("linux_max_pps_1514B", |b| {
        b.iter(|| black_box(linux.max_pps(black_box(1514))))
    });

    group.finish();
}

fn bench_coalescing_impact(c: &mut Criterion) {
    let mut group = c.benchmark_group("nic_coalescing");
    group.sample_size(1000);

    let model = ZeroCopyModel::default_model();

    for cc in [1, 2, 4, 8, 16, 32] {
        group.bench_with_input(
            BenchmarkId::new("max_pps", format!("cc={}", cc)),
            &cc,
            |b, &coalesce| {
                b.iter(|| black_box(model.max_pps(black_box(1514), black_box(coalesce))))
            },
        );
    }

    group.finish();
}

fn bench_packet_sizes(c: &mut Criterion) {
    let mut group = c.benchmark_group("nic_packet_sizes");
    group.sample_size(1000);

    let model = ZeroCopyModel::default_model();
    let pkt_sizes = [64, 128, 256, 512, 1024, 1514];

    for &size in &pkt_sizes {
        group.bench_with_input(
            BenchmarkId::new("zerocopy_pps", size),
            &size,
            |b, &sz| {
                b.iter(|| black_box(model.max_pps(black_box(sz), black_box(1))))
            },
        );
    }

    group.finish();
}

fn bench_throughput_mbps(c: &mut Criterion) {
    let mut group = c.benchmark_group("nic_throughput_mbps");
    group.sample_size(1000);

    let model = ZeroCopyModel::default_model();

    group.bench_function("aero_1514B_no_coalesce", |b| {
        b.iter(|| black_box(model.throughput_mbps(black_box(1514), black_box(1))))
    });

    group.bench_function("aero_1514B_cc16", |b| {
        b.iter(|| black_box(model.throughput_mbps(black_box(1514), black_box(16))))
    });

    let linux = LinuxBaseline::default_baseline();
    group.bench_function("linux_1514B", |b| {
        b.iter(|| black_box(linux.throughput_mbps(black_box(1514))))
    });

    group.finish();
}

fn bench_zerocopy_vs_copy(c: &mut Criterion) {
    let mut group = c.benchmark_group("nic_zerocopy_vs_copy");
    group.sample_size(5000);

    let pkt_sizes = [64, 256, 1024, 1514];
    for &size in &pkt_sizes {
        group.bench_with_input(
            BenchmarkId::new("memcpy_1514B", size),
            &size,
            |b, &sz| {
                let src = vec![0xABu8; sz];
                b.iter(|| {
                    let mut dst = vec![0u8; sz];
                    dst.copy_from_slice(&src);
                    black_box(&dst);
                })
            },
        );
    }

    group.finish();
}

fn bench_mem_cap_creation(c: &mut Criterion) {
    let mut group = c.benchmark_group("nic_mem_cap");
    group.sample_size(10_000);

    group.bench_function("cap_object_setup", |b| {
        b.iter(|| {
            let result = nic_throughput::bench_mem_cap_creation(black_box(1000));
            black_box(result.median())
        })
    });

    group.finish();
}

fn bench_iommu(c: &mut Criterion) {
    let mut group = c.benchmark_group("nic_iommu");
    group.sample_size(5000);

    group.bench_function("tlb_miss_simulation", |b| {
        b.iter(|| {
            let result = nic_throughput::bench_iommu_tlb_miss(black_box(1000));
            black_box(result.median())
        })
    });

    // Simulate IOMMU page table walk at different depths
    for levels in [2, 3, 4] {
        group.bench_with_input(
            BenchmarkId::new("page_walk", format!("{}levels", levels)),
            &levels,
            |b, &n| {
                b.iter(|| {
                    let mut pte = [0u64; 4];
                    for level in 0..n {
                        let _addr = black_box(pte[level]);
                    }
                })
            },
        );
    }

    group.finish();
}

criterion_group!(
    benches,
    bench_nic_model,
    bench_coalescing_impact,
    bench_packet_sizes,
    bench_throughput_mbps,
    bench_zerocopy_vs_copy,
    bench_mem_cap_creation,
    bench_iommu
);
criterion_main!(benches);
