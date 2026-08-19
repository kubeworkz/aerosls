//! The kernel ABI for sidecars, re-exported from `aerosls_proto::kabi` —
//! the shared home of the `Kernel` trait, cap types, error codes, and the
//! *real* kernel ABI implementation (`RealKernel`, behind the `target`
//! feature). The extern \"C\" declarations and the kernel-side contract live
//! there (`docs/AeroSLS-Kernel-Capability-Layer-Spec-v0.1.md`,
//! `docs/AeroSLS-Sidecar-Channels-Transport-Spec-v0.1.md`); this module is
//! a re-export so the driver can import `crate::kapi::{Kernel, RealKernel, ...}`
//! without naming `proto` at every use site.

pub use aerosls_proto::kabi::*;
