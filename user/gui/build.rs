// AeroSLS GUI build script
//
// Compiles .slint UI files at build time.
// Only runs on non-Windows targets (slint-build has issues on Windows).

fn main() {
    // Only compile .slint files on non-Windows targets.
    // On Windows, the GUI crate works without pre-compiled .slint files
    // (the software renderer can still render programmatic UIs).
    #[cfg(not(target_os = "windows"))]
    {
        println!("cargo:rerun-if-changed=ui/app.slint");

        let config = slint_build::CompilerConfiguration::new();
        slint_build::compile_with_config("ui/app.slint", config)
            .expect("Failed to compile .slint file");
    }
}
