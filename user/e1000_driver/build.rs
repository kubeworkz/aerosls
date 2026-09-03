// user/e1000_driver/build.rs — links the e1000_driver bin with e1000.ld
// (the flat-binary linker script: ENTRY(_start), .text._start at address
// 0). Same mechanism as user/network/build.rs.
fn main() {
    let ld = concat!(env!("CARGO_MANIFEST_DIR"), "/e1000.ld");
    println!("cargo:rustc-link-arg-bins=-T{ld}");
    println!("cargo:rerun-if-changed=e1000.ld");
    println!("cargo:rerun-if-changed=src/crt0.S");
}
