// user/network/build.rs — links the network bin with network.ld
fn main() {
    let ld = concat!(env!("CARGO_MANIFEST_DIR"), "/network.ld");
    println!("cargo:rustc-link-arg-bins=-T{ld}");
    println!("cargo:rerun-if-changed=network.ld");
    println!("cargo:rerun-if-changed=src/crt0.S");
}
