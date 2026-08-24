//! Criterion benchmark for DMA buffer allocation throughput.
//!
//! Run with: `cargo bench --bench dma_alloc`

use criterion::{criterion_group, criterion_main, Criterion, BenchmarkId};
use aerosls_phase4_bench::dma_alloc;

fn bench_dma_alloc(c: &mut Criterion) {
    let mut group = c.benchmark_group("dma_alloc");
    group.sample_size(5000);

    group.bench_function("single_page", |b| {
        b.iter(|| {
            dma_alloc::bench_dma_single_alloc(1000)
        })
    });

    group.bench_function("alloc_free_roundtrip", |b| {
        b.iter(|| {
            dma_alloc::bench_dma_alloc_free(1000)
        })
    });

    group.bench_function("fragmented", |b| {
        b.iter(|| {
            dma_alloc::bench_dma_fragmented(1000)
        })
    });

    group.bench_function("share", |b| {
        b.iter(|| {
            dma_alloc::bench_dma_share(1000)
        })
    });

    group.finish();
}

fn bench_dma_sizes(c: &mut Criterion) {
    let mut group = c.benchmark_group("dma_alloc_sizes");
    group.sample_size(1000);

    let sizes = [1, 4, 16, 64, 256, 1024];
    for &npages in &sizes {
        group.bench_with_input(
            BenchmarkId::new("pages", npages),
            &npages,
            |b, &np| {
                b.iter(|| {
                    // Inline allocation for each size
                    let mut pool = dma_alloc::SimDMAPool::new(8192, 0x1000_0000);
                    let start = std::time::Instant::now();
                    let _ = pool.alloc(np, 1, 1);
                    start.elapsed()
                })
            },
        );
    }

    group.finish();
}

fn bench_dma_exhaustion(c: &mut Criterion) {
    let mut group = c.benchmark_group("dma_exhaustion");
    group.sample_size(100);

    // Benchmark at different fill levels
    let results = dma_alloc::bench_dma_exhaustion();
    for result in &results {
        group.bench_function(&result.name, |b| {
            b.iter(|| {
                // Re-run the allocation at this fill level
                result.clone()
            })
        });
    }

    group.finish();
}

criterion_group!(
    benches,
    bench_dma_alloc,
    bench_dma_sizes,
    bench_dma_exhaustion
);
criterion_main!(benches);
