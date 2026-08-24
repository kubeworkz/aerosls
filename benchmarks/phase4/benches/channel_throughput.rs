//! Criterion benchmark for channel message throughput.
//!
//! Run with: `cargo bench --bench channel_throughput`

use criterion::{criterion_group, criterion_main, Criterion, BenchmarkId};
use aerosls_phase4_bench::channel_throughput;

fn bench_channel_pingpong(c: &mut Criterion) {
    let mut group = c.benchmark_group("channel_pingpong");
    group.sample_size(500);

    group.bench_function("64B_payload", |b| {
        b.iter(|| {
            channel_throughput::bench_channel_pingpong(1000)
        })
    });

    group.finish();
}

fn bench_channel_sizes(c: &mut Criterion) {
    let mut group = c.benchmark_group("channel_sizes");
    group.sample_size(200);

    let sizes = [8, 32, 64, 128, 256, 512, 1024, 2048];
    for &size in &sizes {
        group.bench_with_input(
            BenchmarkId::new("unidirectional", size),
            &size,
            |b, &sz| {
                b.iter(|| {
                    channel_throughput::bench_channel_unidirectional(10_000, sz)
                })
            },
        );
    }

    group.finish();
}

fn bench_channel_cap_transfer(c: &mut Criterion) {
    let mut group = c.benchmark_group("channel_cap_transfer");
    group.sample_size(500);

    group.bench_function("with_mem_cap", |b| {
        b.iter(|| {
            channel_throughput::bench_channel_cap_transfer(1000)
        })
    });

    group.finish();
}

fn bench_channel_backpressure(c: &mut Criterion) {
    let mut group = c.benchmark_group("channel_backpressure");
    group.sample_size(100);

    group.bench_function("fast_producer_slow_consumer", |b| {
        b.iter(|| {
            channel_throughput::bench_channel_backpressure(5000)
        })
    });

    group.finish();
}

fn bench_channel_multi(c: &mut Criterion) {
    let mut group = c.benchmark_group("channel_multi");
    group.sample_size(200);

    for n_chans in [2, 4, 8, 16] {
        group.bench_with_input(
            BenchmarkId::new("channels", n_chans),
            &n_chans,
            |b, &n| {
                b.iter(|| {
                    channel_throughput::bench_channel_multi(n, 5000)
                })
            },
        );
    }

    group.finish();
}

criterion_group!(
    benches,
    bench_channel_pingpong,
    bench_channel_sizes,
    bench_channel_cap_transfer,
    bench_channel_backpressure,
    bench_channel_multi
);
criterion_main!(benches);
