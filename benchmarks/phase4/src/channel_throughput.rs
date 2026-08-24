//! Channel Message Throughput Benchmark
//!
//! Measures the performance of the channel messaging system:
//! - Send/recv round-trip latency
//! - Unidirectional throughput (producer → consumer)
//! - Bidirectional throughput (ping-pong)
//! - Various payload sizes
//! - Capability transfer overhead
//!
//! ## What This Measures
//!
//! The channel system (`kernel/ipc.c`) provides typed message-passing between
//! sidecars. Key operations:
//!   1. `cap_send_msg(chan_wr, payload, caps)` → enqueue + wake
//!   2. `cap_recv_msg(chan_rd, buf, cap_slots)` → dequeue + copy
//!   3. Channel queue depth and backpressure
//!
//! ## Host-Side Simulation
//!
//! Uses `aerosls-kernel-sim` to simulate the full channel path including
//! the FakeKernel's mutex-based queue and condvar wakeups.
//!
//! ## On-Target Measurement
//!
//! On real hardware, additional measurements:
//! - Context switch overhead (sidecar → kernel → sidecar)
//! - Capability validation cost
//! - TLB pressure from channel buffers
//!
//! See ON-TARGET-PLAN.md for the full on-target methodology.

use aerosls_kernel_sim::{FakeKernel, FakeClient};
use aerosls_proto::kabi;
use std::sync::{Arc, Barrier};
use std::thread;
use std::time::{Duration, Instant};

/// Benchmark: Send/recv round-trip latency (ping-pong).
///
/// Measures the time for a message to travel from client → kernel → driver
/// and back (driver → kernel → client). This is the fundamental latency
/// unit for all sidecar-to-sidecar communication.
pub fn bench_channel_pingpong(iterations: u64) -> crate::LatencyResult {
    let mut result = crate::LatencyResult::new("channel_pingpong");

    let storage = vec![0u8; 64 * 1024];
    let (kernel, client) = FakeKernel::new(storage, 1);
    let chans = kernel.initial_chan_caps();
    let chan = chans[0];

    let barrier = Arc::new(Barrier::new(2));
    let barrier_clone = barrier.clone();

    // Driver side (echo server)
    let kernel_clone = kernel.clone();
    let driver = thread::spawn(move || {
        barrier_clone.wait();
        let mut buf = [0u8; 4096];
        let mut grants = [kabi::GrantedCap::default(); 16];

        for _ in 0..iterations {
            // Wait for request
            let _ = kernel_clone.wait(&[chan], u64::MAX);
            let recv_result = kernel_clone.recv(chan, &mut buf, &mut grants).unwrap();

            // Echo back (reply)
            kernel_clone
                .send(chan, recv_result.tag, kabi::F_REPLY, &buf[..recv_result.len], &[])
                .unwrap();
        }
    });

    barrier.wait();

    let mut payload = vec![0xABu8; 64]; // 64-byte payload
    let start = Instant::now();

    for i in 0..iterations {
        // Tag must match for reply validation
        let tag = (i as u32) | 0x8000_0000;
        client.send(chan, tag, 0, &payload, &[]).unwrap();

        // Wait for reply
        let mut reply_buf = [0u8; 4096];
        let mut reply_grants = [kabi::GrantedCap::default(); 16];
        client.recv(chan, &mut reply_buf, &mut reply_grants).unwrap();
    }

    let total = start.elapsed();
    driver.join().unwrap();

    let avg = total / iterations as u32;
    for _ in 0..iterations {
        result.push(avg);
    }

    result
}

/// Benchmark: Unidirectional throughput (producer → consumer).
///
/// Measures how fast messages can be sent from one sidecar to another
/// without waiting for a reply. This is the throughput for event-style
/// messaging (e.g., IRQ delivery, notifications).
pub fn bench_channel_unidirectional(
    iterations: u64,
    payload_size: usize,
) -> crate::ThroughputResult {
    let storage = vec![0u8; 64 * 1024];
    let (kernel, client) = FakeKernel::new(storage, 1);
    let chans = kernel.initial_chan_caps();
    let chan = chans[0];

    let barrier = Arc::new(Barrier::new(2));
    let barrier_clone = barrier.clone();

    // Consumer (drains messages)
    let kernel_clone = kernel.clone();
    let consumer = thread::spawn(move || {
        barrier_clone.wait();
        let mut buf = [0u8; 4096];
        let mut grants = [kabi::GrantedCap::default(); 16];

        for _ in 0..iterations {
            let _ = kernel_clone.wait(&[chan], u64::MAX);
            let _ = kernel_clone.recv(chan, &mut buf, &mut grants);
        }
    });

    barrier.wait();

    let payload = vec![0xCDu8; payload_size];
    let start = Instant::now();

    for i in 0..iterations {
        client.send(chan, i as u32, 0, &payload, &[]).unwrap();
    }

    let total = start.elapsed();
    consumer.join().unwrap();

    crate::ThroughputResult {
        name: format!("channel_unidirectional_{}B", payload_size),
        ops_per_sec: iterations as f64 / total.as_secs_f64(),
        bytes_per_sec: (iterations * payload_size as u64) as f64 / total.as_secs_f64(),
        avg_latency: total / iterations as u32,
        total_ops: iterations,
        total_duration: total,
    }
}

/// Benchmark: Channel throughput at various payload sizes.
pub fn bench_channel_sizes() -> Vec<crate::ThroughputResult> {
    let sizes = [8, 32, 64, 128, 256, 512, 1024, 2048];
    let iterations = 100_000;

    sizes
        .iter()
        .map(|&size| bench_channel_unidirectional(iterations, size))
        .collect()
}

/// Benchmark: Capability transfer overhead.
///
/// Measures the cost of sending a message with capabilities attached.
/// This is relevant for zero-copy DMA paths where MEM caps are transferred
/// with each packet.
pub fn bench_channel_cap_transfer(iterations: u64) -> crate::LatencyResult {
    let mut result = crate::LatencyResult::new("channel_cap_transfer");

    let storage = vec![0u8; 64 * 1024];
    let (kernel, client) = FakeKernel::new(storage, 1);
    let chans = kernel.initial_chan_caps();
    let chan = chans[0];

    // Create a MEM region to transfer
    let region_data = vec![0u8; 4096];
    let region_cap = client.new_region(region_data, 0x3); // RW

    let barrier = Arc::new(Barrier::new(2));
    let barrier_clone = barrier.clone();

    // Consumer
    let kernel_clone = kernel.clone();
    let consumer = thread::spawn(move || {
        barrier_clone.wait();
        let mut buf = [0u8; 4096];
        let mut grants = [kabi::GrantedCap::default(); 16];

        for _ in 0..iterations {
            let _ = kernel_clone.wait(&[chan], u64::MAX);
            let _ = kernel_clone.recv(chan, &mut buf, &mut grants);
            // Grants are auto-revoked on next send (transient)
        }
    });

    barrier.wait();

    let payload = vec![0xBEu8; 64];
    let start = Instant::now();

    for i in 0..iterations {
        // Send with capability transfer
        let send_caps = vec![kabi::SendCap {
            slot: region_cap,
            rights: 0x3,
            offset: 0,
            len: 4096,
        }];
        client.send(chan, i as u32, 0, &payload, &send_caps).unwrap();
    }

    let total = start.elapsed();
    consumer.join().unwrap();

    let avg = total / iterations as u32;
    for _ in 0..iterations {
        result.push(avg);
    }

    result
}

/// Benchmark: Channel backpressure (producer faster than consumer).
///
/// Measures how the system behaves when the producer sends faster than
/// the consumer can process. This is relevant for bursty interrupt
/// scenarios (e.g., NIC receiving many packets at once).
pub fn bench_channel_backpressure(iterations: u64) -> crate::LatencyResult {
    let mut result = crate::LatencyResult::new("channel_backpressure");

    let storage = vec![0u8; 64 * 1024];
    let (kernel, client) = FakeKernel::new(storage, 1);
    let chans = kernel.initial_chan_caps();
    let chan = chans[0];

    // Slow consumer (processes 1 message per 10 µs simulated)
    let kernel_clone = kernel.clone();
    let consumer = thread::spawn(move || {
        let mut buf = [0u8; 4096];
        let mut grants = [kabi::GrantedCap::default(); 16];

        let mut count = 0;
        while count < iterations {
            match kernel_clone.wait(&[chan], 1_000_000) {
                Ok(_) => {
                    let _ = kernel_clone.recv(chan, &mut buf, &mut grants);
                    count += 1;
                }
                Err(_) => continue,
            }
        }
    });

    // Fast producer (sends as fast as possible)
    let payload = vec![0xEFu8; 128];
    let start = Instant::now();

    for i in 0..iterations {
        match client.send(chan, i as u32, 0, &payload, &[]) {
            Ok(()) => {}
            Err(_) => {
                // Backpressure: producer blocked, wait and retry
                std::thread::sleep(Duration::from_micros(1));
                client.send(chan, i as u32, 0, &payload, &[]).unwrap();
            }
        }
    }

    let total = start.elapsed();
    consumer.join().unwrap();

    let avg = total / iterations as u32;
    for _ in 0..iterations {
        result.push(avg);
    }

    result
}

/// Benchmark: Multi-channel throughput (multiple independent channels).
///
/// Measures the overhead of having many concurrent channels, which is
/// the typical case in Phase 4 (IRQ channel + control channel + data
/// channels per driver).
pub fn bench_channel_multi(n_channels: usize, iterations: u64) -> crate::LatencyResult {
    let mut result = crate::LatencyResult::new(&format!("channel_multi_{}ch", n_channels));

    let storage = vec![0u8; 64 * 1024];
    let (kernel, client) = FakeKernel::new(storage, n_channels);
    let all_chans = kernel.initial_chan_caps();

    let barrier = Arc::new(Barrier::new(2));
    let barrier_clone = barrier.clone();

    // Consumer: wait on all channels
    let kernel_clone = kernel.clone();
    let chans_clone = all_chans.clone();
    let consumer = thread::spawn(move || {
        barrier_clone.wait();
        let mut buf = [0u8; 4096];
        let mut grants = [kabi::GrantedCap::default(); 16];

        for _ in 0..iterations {
            let _ = kernel_clone.wait(&chans_clone, u64::MAX);
            // Receive from whichever channel is ready
            for &chan in &chans_clone {
                if kernel_clone.recv(chan, &mut buf, &mut grants).is_ok() {
                    break;
                }
            }
        }
    });

    barrier.wait();

    // Producer: round-robin across channels
    let payload = vec![0x55u8; 64];
    let start = Instant::now();

    for i in 0..iterations {
        let chan_idx = (i as usize) % n_channels;
        client
            .send(all_chans[chan_idx], i as u32, 0, &payload, &[])
            .unwrap();
    }

    let total = start.elapsed();
    consumer.join().unwrap();

    let avg = total / iterations as u32;
    for _ in 0..iterations {
        result.push(avg);
    }

    result
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_pingpong_completes() {
        let result = bench_channel_pingpong(100);
        assert_eq!(result.samples.len(), 100);
        assert!(result.median() > Duration::ZERO);
    }

    #[test]
    fn test_unidirectional_completes() {
        let result = bench_channel_unidirectional(1000, 64);
        assert!(result.ops_per_sec > 0.0);
    }

    #[test]
    fn test_cap_transfer_completes() {
        let result = bench_channel_cap_transfer(100);
        assert_eq!(result.samples.len(), 100);
    }

    #[test]
    fn test_multi_channel() {
        let result = bench_channel_multi(4, 100);
        assert_eq!(result.samples.len(), 100);
    }
}
