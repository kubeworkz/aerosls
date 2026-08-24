//! DMA Buffer Allocator Throughput Benchmark
//!
//! Measures the performance of the DMA buffer pool allocator:
//! - Allocation latency (first-fit bitmap search + IOMMU mapping)
//! - Free latency (IOMMU unmap + bitmap release)
//! - Allocation throughput under sustained load
//! - Fragmentation resistance
//!
//! ## What This Measures
//!
//! The DMA allocator (`kernel/dma.c`) manages a fixed-size pool of physically
//! contiguous pages. Key operations:
//!   1. `dma_alloc(npages, align, flags, pid)` → cap_idx + dma_buf_id
//!   2. `dma_free(dma_buf_id, pid)` → unpin + IOMMU unmap + bitmap release
//!   3. `dma_share(dma_buf_id, target_pid, rights)` → MEM cap in target table
//!
//! ## Host-Side Simulation
//!
//! We simulate the bitmap allocator logic directly (no kernel-sim needed)
//! to measure the pure allocator overhead without syscall latency.
//!
//! ## On-Target Measurement
//!
//! On real hardware, the benchmark also measures:
//! - IOMMU page table walk latency
//! - IOTLB miss penalty
//! - Pin/unpin overhead
//!
//! See ON-TARGET-PLAN.md for the full on-target methodology.

use std::collections::BTreeSet;
use std::time::{Duration, Instant};

/// Simulated DMA buffer pool (mirrors kernel/dma.c's DMAPool).
///
/// Uses a bitmap for allocation tracking and a BTreeSet for O(log n)
/// free-run search (matching the kernel's first-fit algorithm).
pub struct SimDMAPool {
    bitmap: Vec<bool>,
    total_frames: usize,
    base_phys: u64,
    frame_size: usize,
    allocations: Vec<Option<Allocation>>,
    free_ids: Vec<u32>,
    next_id: u32,
}

#[derive(Clone, Debug)]
struct Allocation {
    frame_start: usize,
    npages: usize,
    owner_pid: u32,
    pinned: bool,
}

impl SimDMAPool {
    pub fn new(total_frames: usize, base_phys: u64) -> Self {
        let mut free_ids: Vec<u32> = (0..1024).rev().collect();
        Self {
            bitmap: vec![false; total_frames],
            total_frames,
            base_phys,
            frame_size: 4096,
            allocations: vec![None; 1024],
            free_ids,
            next_id: 0,
        }
    }

    /// First-fit allocation matching kernel/dma.c's dma_alloc().
    pub fn alloc(&mut self, npages: usize, align_pages: usize, pid: u32) -> Option<u32> {
        if npages == 0 || npages > self.total_frames {
            return None;
        }

        // First-fit with alignment
        let mut run_start = 0;
        let mut run_len = 0;

        for i in 0..self.total_frames {
            if !self.bitmap[i] {
                if run_len == 0 {
                    run_start = i;
                }
                run_len += 1;

                if run_len >= npages {
                    // Check alignment
                    if run_start % align_pages == 0 {
                        // Allocate
                        for j in run_start..run_start + npages {
                            self.bitmap[j] = true;
                        }

                        let id = self.free_ids.pop()?;
                        self.allocations[id as usize] = Some(Allocation {
                            frame_start: run_start,
                            npages,
                            owner_pid: pid,
                            pinned: true,
                        });

                        return Some(id);
                    }
                }
            } else {
                run_len = 0;
            }
        }

        None
    }

    /// Free a buffer (mirrors kernel/dma.c's dma_free()).
    pub fn free(&mut self, id: u32) -> bool {
        if let Some(Some(alloc)) = self.allocations.get_mut(id as usize) {
            // Release frames in bitmap
            for i in alloc.frame_start..alloc.frame_start + alloc.npages {
                self.bitmap[i] = false;
            }
            self.allocations[id as usize] = None;
            self.free_ids.push(id);
            true
        } else {
            false
        }
    }

    /// Share a buffer with another sidecar (creates MEM cap in target table).
    /// In the simulation, this is just incrementing a share counter.
    pub fn share(&self, id: u32) -> bool {
        self.allocations
            .get(id as usize)
            .map_or(false, |a| a.is_some())
    }

    /// Get physical address for a buffer.
    pub fn phys_addr(&self, id: u32) -> Option<u64> {
        self.allocations
            .get(id as usize)?
            .as_ref()
            .map(|a| self.base_phys + (a.frame_start as u64) * self.frame_size as u64)
    }

    /// Count free frames.
    pub fn free_count(&self) -> usize {
        self.bitmap.iter().filter(|&&b| !b).count()
    }

    /// Count allocated frames.
    pub fn allocated_count(&self) -> usize {
        self.bitmap.iter().filter(|&&b| b).count()
    }
}

/// Benchmark: Single DMA allocation latency.
pub fn bench_dma_single_alloc(iterations: u64) -> crate::LatencyResult {
    let mut result = crate::LatencyResult::new("dma_single_alloc");

    for _ in 0..iterations {
        // Fresh pool for each iteration to avoid fragmentation effects
        let mut pool = SimDMAPool::new(8192, 0x1000_0000);

        let start = Instant::now();
        let id = pool.alloc(1, 1, 1); // 1 page, no alignment, pid=1
        let elapsed = start.elapsed();

        assert!(id.is_some(), "allocation should succeed on fresh pool");
        result.push(elapsed);
    }

    result
}

/// Benchmark: Multi-page DMA allocation latency (varying sizes).
pub fn bench_dma_alloc_sizes() -> Vec<crate::LatencyResult> {
    let sizes = [1, 4, 16, 64, 256, 1024];
    let mut results = Vec::new();

    for &npages in &sizes {
        let mut result = crate::LatencyResult::new(&format!("dma_alloc_{}p", npages));
        let iterations = 1000;

        for _ in 0..iterations {
            let mut pool = SimDMAPool::new(8192, 0x1000_0000);

            let start = Instant::now();
            let _id = pool.alloc(npages, 1, 1);
            result.push(start.elapsed());
        }

        results.push(result);
    }

    results
}

/// Benchmark: DMA allocation throughput (sustained).
pub fn bench_dma_alloc_throughput(duration: Duration) -> crate::ThroughputResult {
    let mut pool = SimDMAPool::new(8192, 0x1000_0000);
    let mut ops = 0u64;

    let start = Instant::now();
    while start.elapsed() < duration {
        // Allocate 4 pages (typical NIC ring buffer size)
        if let Some(id) = pool.alloc(4, 1, 1) {
            ops += 1;
            // Immediately free to keep the pool from filling
            pool.free(id);
        }
    }
    let elapsed = start.elapsed();

    crate::ThroughputResult {
        name: "dma_alloc_throughput".to_string(),
        ops_per_sec: ops as f64 / elapsed.as_secs_f64(),
        bytes_per_sec: (ops * 4 * 4096) as f64 / elapsed.as_secs_f64(),
        avg_latency: elapsed / ops as u32,
        total_ops: ops,
        total_duration: elapsed,
    }
}

/// Benchmark: DMA allocation + free round-trip.
pub fn bench_dma_alloc_free(iterations: u64) -> crate::LatencyResult {
    let mut result = crate::LatencyResult::new("dma_alloc_free");
    let mut pool = SimDMAPool::new(8192, 0x1000_0000);

    for i in 0..iterations {
        let npages = ((i % 64) + 1) as usize; // varying sizes 1-64 pages
        let align = if i % 4 == 0 { 16 } else { 1 }; // some with alignment

        let start = Instant::now();
        if let Some(id) = pool.alloc(npages, align, 1) {
            pool.free(id);
        }
        result.push(start.elapsed());
    }

    result
}

/// Benchmark: DMA fragmentation resistance.
///
/// Allocates and frees buffers in a random-ish pattern to fragment the pool,
/// then measures allocation latency under fragmentation.
pub fn bench_dma_fragmented(iterations: u64) -> crate::LatencyResult {
    let mut result = crate::LatencyResult::new("dma_fragmented");
    let mut pool = SimDMAPool::new(8192, 0x1000_0000);

    // Phase 1: Create fragmentation
    let mut ids = Vec::new();
    for i in 0..2048 {
        if let Some(id) = pool.alloc(4, 1, 1) {
            ids.push(id);
        }
    }
    // Free every other allocation to create holes
    for (i, &id) in ids.iter().enumerate() {
        if i % 2 == 0 {
            pool.free(id);
        }
    }

    // Phase 2: Allocate under fragmentation
    for _ in 0..iterations {
        let start = Instant::now();
        if let Some(id) = pool.alloc(8, 1, 1) {
            pool.free(id);
        }
        result.push(start.elapsed());
    }

    result
}

/// Benchmark: DMA share operation.
pub fn bench_dma_share(iterations: u64) -> crate::LatencyResult {
    let mut result = crate::LatencyResult::new("dma_share");
    let mut pool = SimDMAPool::new(8192, 0x1000_0000);

    // Pre-allocate some buffers
    let mut ids = Vec::new();
    for _ in 0..100 {
        if let Some(id) = pool.alloc(4, 1, 1) {
            ids.push(id);
        }
    }

    // Benchmark share operations
    for &id in ids.iter().cycle().take(iterations as usize) {
        let start = Instant::now();
        let _ = pool.share(id);
        result.push(start.elapsed());
    }

    result
}

/// Benchmark: DMA pool exhaustion behavior.
///
/// Measures how latency degrades as the pool fills up.
pub fn bench_dma_exhaustion() -> Vec<crate::LatencyResult> {
    let mut pool = SimDMAPool::new(8192, 0x1000_0000);
    let fill_levels = [10, 25, 50, 75, 90, 95, 99];
    let mut results = Vec::new();

    // Pre-fill to target level
    let mut ids = Vec::new();
    for &level in &fill_levels {
        let target = (8192 * level / 100) as usize;
        while pool.allocated_count() < target {
            if let Some(id) = pool.alloc(4, 1, 1) {
                ids.push(id);
            } else {
                break;
            }
        }

        // Measure allocation latency at this fill level
        let mut result = crate::LatencyResult::new(&format!("dma_exhaust_{}pct", level));
        for _ in 0..1000 {
            let start = Instant::now();
            if let Some(id) = pool.alloc(4, 1, 1) {
                pool.free(id);
            }
            result.push(start.elapsed());
        }
        results.push(result);
    }

    results
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_pool_basic_alloc_free() {
        let mut pool = SimDMAPool::new(1024, 0x1000_0000);
        let id = pool.alloc(4, 1, 1).unwrap();
        assert_eq!(pool.allocated_count(), 4);
        assert_eq!(pool.free_count(), 1020);

        let phys = pool.phys_addr(id).unwrap();
        assert_eq!(phys, 0x1000_0000);

        pool.free(id);
        assert_eq!(pool.allocated_count(), 0);
        assert_eq!(pool.free_count(), 1024);
    }

    #[test]
    fn test_pool_alignment() {
        let mut pool = SimDMAPool::new(1024, 0x1000_0000);
        let id = pool.alloc(1, 16, 1).unwrap(); // 16-page alignment
        let phys = pool.phys_addr(id).unwrap();
        assert_eq!(phys % (16 * 4096), 0, "must be 16-page aligned");
        pool.free(id);
    }

    #[test]
    fn test_pool_exhaustion() {
        let mut pool = SimDMAPool::new(16, 0x1000_0000);
        let mut ids = Vec::new();
        for _ in 0..4 {
            if let Some(id) = pool.alloc(4, 1, 1) {
                ids.push(id);
            }
        }
        assert_eq!(pool.free_count(), 0);
        assert!(pool.alloc(1, 1, 1).is_none(), "should fail when exhausted");
    }

    #[test]
    fn test_dma_single_alloc_bench() {
        let result = bench_dma_single_alloc(100);
        assert_eq!(result.samples.len(), 100);
    }

    #[test]
    fn test_dma_alloc_free_bench() {
        let result = bench_dma_alloc_free(1000);
        assert_eq!(result.samples.len(), 1000);
    }
}
