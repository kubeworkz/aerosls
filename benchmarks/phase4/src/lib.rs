//! Phase 4 Device Driver SDK — Micro-benchmark Harness
//!
//! This crate provides benchmark utilities for measuring:
//! - IRQ-to-handler latency (IRQ delivery through channels)
//! - DMA allocation throughput (pool allocator performance)
//! - Channel message throughput (send/recv round-trip)
//! - NIC throughput comparison (zero-copy DMA path analysis)
//!
//! Run all benchmarks:
//!   cargo bench
//!
//! Run a specific benchmark group:
//!   cargo bench --bench irq_latency
//!   cargo bench --bench dma_alloc
//!   cargo bench --bench channel_throughput
//!   cargo bench --bench nic_throughput
//!
//! Generate HTML reports in target/criterion/.

pub mod irq_latency;
pub mod dma_alloc;
pub mod channel_throughput;
pub mod nic_throughput;

use std::time::{Duration, Instant};

// Re-export criterion for convenience
pub use criterion;

/// A single latency measurement result.
#[derive(Debug, Clone)]
pub struct LatencyResult {
    pub name: String,
    pub samples: Vec<Duration>,
}

impl LatencyResult {
    pub fn new(name: &str) -> Self {
        Self {
            name: name.to_string(),
            samples: Vec::new(),
        }
    }

    pub fn push(&mut self, d: Duration) {
        self.samples.push(d);
    }

    pub fn median(&self) -> Duration {
        let mut sorted = self.samples.clone();
        sorted.sort();
        sorted[sorted.len() / 2]
    }

    pub fn mean(&self) -> Duration {
        let total: Duration = self.samples.iter().sum();
        total / self.samples.len() as u32
    }

    pub fn p99(&self) -> Duration {
        let mut sorted = self.samples.clone();
        sorted.sort();
        let idx = (sorted.len() as f64 * 0.99) as usize;
        sorted[idx.min(sorted.len() - 1)]
    }

    pub fn min(&self) -> Duration {
        *self.samples.iter().min().unwrap()
    }

    pub fn max(&self) -> Duration {
        *self.samples.iter().max().unwrap()
    }

    pub fn stddev(&self) -> Duration {
        let mean_ns = self.mean().as_nanos() as f64;
        let variance: f64 = self
            .samples
            .iter()
            .map(|d| {
                let diff = d.as_nanos() as f64 - mean_ns;
                diff * diff
            })
            .sum::<f64>()
            / self.samples.len() as f64;
        Duration::from_nanos(variance.sqrt() as u64)
    }
}

impl std::fmt::Display for LatencyResult {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        writeln!(f, "── {} ({}) ──", self.name, self.samples.len())?;
        writeln!(f, "  median:  {:>10.1?}", self.median())?;
        writeln!(f, "  mean:    {:>10.1?}", self.mean())?;
        writeln!(f, "  min:     {:>10.1?}", self.min())?;
        writeln!(f, "  max:     {:>10.1?}", self.max())?;
        writeln!(f, "  p99:     {:>10.1?}", self.p99())?;
        writeln!(f, "  stddev:  {:>10.1?}", self.stddev())
    }
}

/// Throughput measurement result.
#[derive(Debug, Clone)]
pub struct ThroughputResult {
    pub name: String,
    pub ops_per_sec: f64,
    pub bytes_per_sec: f64,
    pub avg_latency: Duration,
    pub total_ops: u64,
    pub total_duration: Duration,
}

impl std::fmt::Display for ThroughputResult {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        writeln!(f, "── {} ──", self.name)?;
        writeln!(f, "  ops/sec:    {:>12.0}", self.ops_per_sec)?;
        writeln!(f, "  bytes/sec:  {:>12.0}", self.bytes_per_sec)?;
        writeln!(f, "  avg lat:    {:>12.1?}", self.avg_latency)?;
        writeln!(f, "  total ops:  {:>12}", self.total_ops)?;
        writeln!(f, "  duration:   {:>12.1?}", self.total_duration)
    }
}

/// Format a comparison table of latency results.
pub fn print_comparison(results: &[LatencyResult]) {
    println!("\n{:=<70}", "");
    println!("  {:<30} {:>10} {:>10} {:>10}", "Benchmark", "Median", "P99", "Stddev");
    println!("{:=<70}", "");
    for r in results {
        println!(
            "  {:<30} {:>10.1?} {:>10.1?} {:>10.1?}",
            r.name,
            r.median(),
            r.p99(),
            r.stddev()
        );
    }
    println!("{:=<70}\n", "");
}
