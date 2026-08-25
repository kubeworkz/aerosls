// user/sidecar/build.rs — links the posix bin with posix.ld
fn main() {
    let ld = concat!(env!("CARGO_MANIFEST_DIR"), "/posix.ld");
    println!("cargo:rustc-link-arg-bins=-T{ld}");
    println!("cargo:rerun-if-changed=posix.ld");
    println!("cargo:rerun-if-changed=src/crt0.S");
}
