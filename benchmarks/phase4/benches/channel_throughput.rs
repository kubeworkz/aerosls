//! Criterion benchmark for channel message throughput.
//!
//! Run with: `cargo bench --bench channel_throughput`

use criterion::{criterion_group, criterion_main, Criterion, BenchmarkId, black_box};
use aerosls_phase4_bench::channel_throughput;

fn bench_channel_pingpong(c: &mut Criterion) {
    let mut group = c.benchmark_group("channel_pingpong");
    group.sample_size(30);
    group.measurement_time(std::time::Duration::from_secs(10));

    group.bench_function("64B_request_reply", |b| {
        b.iter(|| {
            let result = channel_throughput::bench_channel_pingpong(black_box(500));
            black_box(result.median())
        })
    });

    group.finish();
}

fn bench_channel_unidirectional(c: &mut Criterion) {
    let mut group = c.benchmark_group("channel_unidirectional");
    group.sample_size(20);

    let sizes = [64, 256, 1024, 2048];
    for &size in &sizes {
        group.bench_with_input(
            BenchmarkId::new("payload", size),
            &size,
            |b, &sz| {
                b.iter(|| {
                    let result = channel_throughput::bench_channel_unidirectional(
                        black_box(2000),
                        black_box(sz),
                    );
                    black_box(result.avg_latency)
                })
            },
        );
    }

    group.finish();
}

fn bench_channel_cap_transfer(c: &mut Criterion) {
    let mut group = c.benchmark_group("channel_cap_transfer");
    group.sample_size(30);

    group.bench_function("send_with_1_mem_cap", |b| {
        b.iter(|| {
            let result = channel_throughput::bench_channel_cap_transfer(black_box(500));
            black_box(result.median())
        })
    });

    group.finish();
}

fn bench_channel_backpressure(c: &mut Criterion) {
    let mut group = c.benchmark_group("channel_backpressure");
    group.sample_size(10);

    group.bench_function("fast_producer_slow_consumer", |b| {
        b.iter(|| {
            let result = channel_throughput::bench_channel_backpressure(black_box(2000));
            black_box(result.samples.len()) // total messages processed
        })
    });

    group.finish();
}

fn bench_channel_multi(c: &mut Criterion) {
    let mut group = c.benchmark_group("channel_multi");
    group.sample_size(20);

    for n_chans in [2, 4, 8] {
        group.bench_with_input(
            BenchmarkId::new("concurrent_channels", n_chans),
            &n_chans,
            |b, &n| {
                b.iter(|| {
                    let result = channel_throughput::bench_channel_multi(n, black_box(2000));
                    black_box(result.median())
                })
            },
        );
    }

    group.finish();
}

fn bench_channel_latency_breakdown(c: &mut Criterion) {
    let mut group = c.benchmark_group("channel_latency_breakdown");
    group.sample_size(50);

    // Measure per-message latency at different queue depths
    // by running the ping-pong with varying iteration counts
    for n in [10, 100, 500] {
        group.bench_with_input(
            BenchmarkId::new("pingpong_depth", n),
            &n,
            |b, &iters| {
                b.iter(|| {
                    let result = channel_throughput::bench_channel_pingpong(black_box(iters));
                    // Report total time amortized per message
                    black_box(result.median())
                })
            },
        );
    }

    group.finish();
}

criterion_group!(
    benches,
    bench_channel_pingpong,
    bench_channel_unidirectional,
    bench_channel_cap_transfer,
    bench_channel_backpressure,
    bench_channel_multi,
    bench_channel_latency_breakdown
);
criterion_main!(benches);
