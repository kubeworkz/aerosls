//! Criterion benchmark for DMA buffer allocation throughput.
//!
//! Run with: `cargo bench --bench dma_alloc`

use criterion::{criterion_group, criterion_main, Criterion, BenchmarkId, black_box};
use aerosls_phase4_bench::dma_alloc::{self, SimDMAPool};

fn bench_dma_alloc_single(c: &mut Criterion) {
    let mut group = c.benchmark_group("dma_alloc");
    group.sample_size(10_000);

    // Measure a single alloc on a fresh pool — isolates the bitmap search
    group.bench_function("single_page_fresh_pool", |b| {
        b.iter_custom(|iters| {
            let mut total = std::time::Duration::ZERO;
            for _ in 0..iters {
                let mut pool = SimDMAPool::new(8192, 0x1000_0000);
                let start = std::time::Instant::now();
                let _id = pool.alloc(black_box(1), 1, 1);
                total += start.elapsed();
            }
            total
        })
    });

    // Measure alloc+free round-trip on a single pool
    group.bench_function("alloc_free_roundtrip", |b| {
        let mut pool = SimDMAPool::new(8192, 0x1000_0000);
        b.iter(|| {
            if let Some(id) = pool.alloc(black_box(4), 1, 1) {
                pool.free(id);
            }
        })
    });

    // Measure under fragmentation
    group.bench_function("alloc_under_fragmentation", |b| {
        b.iter_custom(|iters| {
            // Set up fragmentation: allocate 2048 buffers, free every other one
            let mut pool = SimDMAPool::new(8192, 0x1000_0000);
            let mut ids = Vec::new();
            for _ in 0..2048 {
                if let Some(id) = pool.alloc(4, 1, 1) {
                    ids.push(id);
                }
            }
            for (i, &id) in ids.iter().enumerate() {
                if i % 2 == 0 {
                    pool.free(id);
                }
            }

            // Measure allocation under fragmentation
            let mut total = std::time::Duration::ZERO;
            for _ in 0..iters {
                let start = std::time::Instant::now();
                if let Some(id) = pool.alloc(black_box(8), 1, 1) {
                    pool.free(id);
                }
                total += start.elapsed();
            }
            total
        })
    });

    group.finish();
}

fn bench_dma_alloc_sizes(c: &mut Criterion) {
    let mut group = c.benchmark_group("dma_alloc_sizes");
    group.sample_size(5_000);

    let sizes = [1, 4, 16, 64, 256, 1024];
    for &npages in &sizes {
        group.bench_with_input(
            BenchmarkId::new("pages", npages),
            &npages,
            |b, &np| {
                b.iter_custom(|iters| {
                    let mut total = std::time::Duration::ZERO;
                    for _ in 0..iters {
                        let mut pool = SimDMAPool::new(8192, 0x1000_0000);
                        let start = std::time::Instant::now();
                        let _ = pool.alloc(black_box(np), 1, 1);
                        total += start.elapsed();
                    }
                    total
                })
            },
        );
    }

    group.finish();
}

fn bench_dma_alloc_alignment(c: &mut Criterion) {
    let mut group = c.benchmark_group("dma_alloc_alignment");
    group.sample_size(5_000);

    let alignments = [1, 16, 64, 256];
    for &align in &alignments {
        group.bench_with_input(
            BenchmarkId::new("align", align),
            &align,
            |b, &al| {
                b.iter_custom(|iters| {
                    let mut total = std::time::Duration::ZERO;
                    for _ in 0..iters {
                        let mut pool = SimDMAPool::new(8192, 0x1000_0000);
                        let start = std::time::Instant::now();
                        let _ = pool.alloc(black_box(4), al, 1);
                        total += start.elapsed();
                    }
                    total
                })
            },
        );
    }

    group.finish();
}

fn bench_dma_exhaustion(c: &mut Criterion) {
    let mut group = c.benchmark_group("dma_exhaustion");
    group.sample_size(50);

    // Pre-fill the pool to various levels and measure alloc latency
    let fill_levels = [10, 25, 50, 75, 90, 95];
    for &level in &fill_levels {
        group.bench_function(
            BenchmarkId::new("fill", format!("{}pct", level)),
            |b| {
                b.iter_custom(|iters| {
                    let mut pool = SimDMAPool::new(8192, 0x1000_0000);
                    let mut ids = Vec::new();
                    let target = (8192 * level / 100) as usize;
                    while pool.allocated_count() < target {
                        if let Some(id) = pool.alloc(4, 1, 1) {
                            ids.push(id);
                        } else {
                            break;
                        }
                    }

                    let mut total = std::time::Duration::ZERO;
                    for _ in 0..iters {
                        let start = std::time::Instant::now();
                        if let Some(id) = pool.alloc(black_box(4), 1, 1) {
                            pool.free(id);
                        }
                        total += start.elapsed();
                    }
                    total
                })
            },
        );
    }

    group.finish();
}

fn bench_dma_throughput(c: &mut Criterion) {
    let mut group = c.benchmark_group("dma_throughput");
    group.measurement_time(std::time::Duration::from_secs(5));
    group.sample_size(10);

    group.bench_function("sustained_alloc_free_4p", |b| {
        b.iter_custom(|_| {
            let mut pool = SimDMAPool::new(8192, 0x1000_0000);
            let mut ops = 0u64;
            let start = std::time::Instant::now();
            while start.elapsed() < std::time::Duration::from_secs(1) {
                if let Some(id) = pool.alloc(4, 1, 1) {
                    pool.free(id);
                    ops += 1;
                }
            }
            std::time::Duration::from_nanos(
                (ops as f64 / start.elapsed().as_secs_f64() * 1000.0) as u64,
            )
        })
    });

    group.finish();
}

criterion_group!(
    benches,
    bench_dma_alloc_single,
    bench_dma_alloc_sizes,
    bench_dma_alloc_alignment,
    bench_dma_exhaustion,
    bench_dma_throughput
);
criterion_main!(benches);
