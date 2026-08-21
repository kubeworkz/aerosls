# aerosls-gui

Slint GUI integration for the AeroSLS POSIX sidecar. Renders to an in-memory
framebuffer using Slint's software renderer, driven cooperatively via
`GuiState::step()` to fit the sidecar's `Step::Yield` scheduler.

## Architecture

```
Slint UI (.slint files)
    ↓ compile time
i-slint-core + i-slint-renderer-software
    ↓ runtime
Custom AeroSlsPlatform (Platform trait impl)
    ↓
MinimalSoftwareWindow + SoftwareRenderer
    ↓
Rgb565 framebuffer → ARGB conversion → Framebuffer
    ↓
/dev/fb0 (ARM) or X11 PutImage (remote)
```

## Build Status

| Target | Status |
|--------|--------|
| `x86_64-pc-windows-gnu` | ⚠️ `i-slint-core` / `euclid` has `is_finite()` issue on this target |
| `x86_64-pc-windows-msvc` | ⚠️ Same `euclid` issue + dlltool for `windows-*` crates |
| `x86_64-unknown-linux-gnu` | ✅ Should work (fontique uses fontconfig, not Windows APIs) |
| `aarch64-unknown-linux-gnu` | ✅ Primary target — software renderer, no GPU needed |

**The GUI crate is designed for ARM Linux embedded targets.** Windows build
issues are in Slint's internal `euclid` dependency, not in our code. Build
and test on Linux or cross-compile to ARM.

## Usage

```rust
use aerosls_gui::GuiState;

let mut gui = GuiState::new(800, 600);
gui.show().unwrap();

// In your sidecar step function:
gui.handle_input(WindowEvent::PointerPressed { ... });
if gui.step() {
    let fb = gui.framebuffer();
    // Send fb.to_rgba8() over X11 or write to /dev/fb0
}
```

## License

MIT
