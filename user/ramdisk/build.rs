// user/ramdisk/build.rs — links the ramdisk bin with ramdisk.ld
fn main() {
    let ld = concat!(env!("CARGO_MANIFEST_DIR"), "/ramdisk.ld");
    println!("cargo:rustc-link-arg-bins=-T{ld}");
    println!("cargo:rerun-if-changed=ramdisk.ld");
    println!("cargo:rerun-if-changed=src/crt0.S");
}
