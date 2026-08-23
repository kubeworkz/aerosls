//! `polyglot` — the two-process Polyglot Nexus e2e.
//!
//! The lib exposes the wire protocol shared by the three processes:
//! `sls-kerneld` (arena + cap refcounts + channel queues over TCP or the
//! shared-memory rings in `ring`), `wasm-sidecar` (a wasmi embedding driving
//! the generated client), and the `lisp_sidecar.lisp` (a real SBCL process
//! running the generated dispatch). The request bodies are the exact pinned
//! 302-304 wire layouts.

pub mod transport;

/// Shared-memory channel ring (unix-only: mmap). The bins are already gated
/// behind the `linux` feature; this keeps the Windows lib build clean.
#[cfg(unix)]
pub mod ring;
