//! The AeroSLS POSIX sidecar (`aerosls.posix.v1`) — bootstrap and entry.
//!
//! This crate is the top of the phase-2 stack: it takes the kernel's Boot
//! Info Block (built from the sidecar manifest), stands the device layer
//! up, and hands control to init. Everything below is already in the
//! chain — `aerosls-blockcache` (the ramdisk protocol client), `aerosls-vfs`
//! (mounts, fds, the syscall surface), `aerosls-procmgr` (cooperative
//! tasks) — and the manifest/BIB formats live in `aerosls-proto`.
//!
//! - `boot`       — `BootCaps` (initial caps by manifest name) and `boot()`,
//!   the dependency-ordered bootstrap: connect the block cache, mount `/`,
//!   `/dev`, `/tmp`, spawn init (the boot script runner) with console stdio
//!   (Phase 2 §6.2).
//! - `applets`    — the built-in registry: the `init` boot-script runner
//!   (executes `/etc/init.rc`, one forked child per command), `cat`, `echo`,
//!   `sh` (the minimal interactive shell — console commands, pipelines,
//!   `<`/`>` redirects, `'...'`/`"..."` quoting, backslash escapes,
//!   `$?` and `$VAR`/`${VAR}` expansion, `export`/`setenv`/`unset`/
//!   `unsetenv` builtins and `NAME=value` scoped assignments that
//!   mutate the env), `grep` (glob patterns: `*` any run, `.` any
//!   char, `\` escape; exits 0/1 on match/no-match), `wc` (line /
//!   word / byte counts with pipe EOF propagation), `head`/`tail`
//!   (line windows: head exits at N lines without draining the input
//!   — partial-pipe semantics — tail buffers a sliding window to
//!   EOF), `sort` (buffers the whole input across reads, then emits
//!   in lexicographic byte order), `seq` (the pure producer:
//!   integers `FIRST..LAST` by `STEP`, one per line), `tee` (fans
//!   each chunk to stdout and named files, parking on a full pipe
//!   without rewriting the files), `true`/`false` — plus
//!   `register_default_applets`.
//! - `allocator`  — `BudgetAlloc`: request buffers carved from the sidecar's
//!   own budget MEM cap.
//! - `heap`       — bump allocator over the budget region (v1 reserves it).
//! - `entry`      — the real `extern "C"` entry point (feature `target`).
//!
//! Like every crate in the chain, `boot` is generic over
//! `aerosls_proto::kabi::Kernel` + `BufferAlloc`, so the identical code
//! runs against the host fake kernel (`tests/boot_tests.rs`) and the real
//! kernel ABI.

#![cfg_attr(not(test), no_std)]

extern crate alloc;

pub mod allocator;
pub mod applets;
pub mod boot;
pub mod heap;

#[cfg(feature = "target")]
mod entry;

pub use boot::{boot, BootCaps, BootErr, Booted};
