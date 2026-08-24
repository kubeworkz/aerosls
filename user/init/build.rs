// user/init/build.rs — links the init bin with init.ld (the flat-binary
// linker script: ENTRY(_start), .text._start at address 0).
//
// `cargo:rustc-link-arg-bins` is used instead of .cargo/config.toml
// rustflags because cargo does not expand ${CARGO_MANIFEST_DIR} in
// rustflags, and rustc is invoked from the workspace root — a relative
// `-Tinit.ld` cannot resolve. The build script has the manifest dir as a
// compile-time constant, so the flag is absolute and package-scoped to the
// bin target (the lib is never linked with it).
fn main() {
    let ld = concat!(env!("CARGO_MANIFEST_DIR"), "/init.ld");
    println!("cargo:rustc-link-arg-bins=-T{ld}");
    println!("cargo:rerun-if-changed=init.ld");
    println!("cargo:rerun-if-changed=crt0.S");
}
