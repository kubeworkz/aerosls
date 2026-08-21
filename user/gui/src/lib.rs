//! AeroSLS GUI — Slint integration for the POSIX sidecar.
//!
//! Provides a custom [`slint::platform::Platform`] backend that renders to
//! an in-memory framebuffer using Slint's software renderer. The framebuffer
//! can be:
//!
//! - Written to `/dev/fb0` (direct framebuffer on ARM embedded)
//! - Sent as X11 `PutImage` through the sshd X11 relay
//! - Read by a test harness for snapshot assertions
//!
//! The GUI is driven cooperatively via [`GuiState::step()`], which fits the
//! sidecar's `Step::Yield` scheduler model — no blocking event loop needed.
//!
//! On ARM embedded targets, the monotonic clock should be replaced with a
//! hardware timer. The prototype uses a simple counter.

extern crate alloc;

pub mod x11;
pub mod server;
pub mod input;

use alloc::boxed::Box;
use alloc::rc::Rc;
use alloc::vec::Vec;
use core::cell::Cell;
use core::time::Duration;

use slint::platform::software_renderer::{
    MinimalSoftwareWindow, RepaintBufferType, Rgb565Pixel,
};
use slint::platform::{Platform, WindowAdapter, WindowEvent};
use slint::PhysicalSize;

// ── Framebuffer ─────────────────────────────────────────────────────────────

/// An in-memory ARGB framebuffer (0xAARRGGBB).
#[derive(Clone)]
pub struct Framebuffer {
    pub width: u32,
    pub height: u32,
    pub pixels: Vec<u32>,
}

impl Framebuffer {
    pub fn new(width: u32, height: u32) -> Self {
        Framebuffer {
            width,
            height,
            pixels: alloc::vec![0xFF000000; (width * height) as usize],
        }
    }

    /// Convert to RGBA8 bytes (for X11 PutImage or PNG export).
    pub fn to_rgba8(&self) -> Vec<u8> {
        let mut buf = Vec::with_capacity((self.width * self.height * 4) as usize);
        for &pixel in &self.pixels {
            buf.push(((pixel >> 16) & 0xFF) as u8);
            buf.push(((pixel >> 8) & 0xFF) as u8);
            buf.push((pixel & 0xFF) as u8);
            buf.push(((pixel >> 24) & 0xFF) as u8);
        }
        buf
    }

    /// Fill the entire buffer with a solid color.
    pub fn fill(&mut self, color: u32) {
        self.pixels.fill(color);
    }

    /// Draw a filled rectangle (for test patterns / cursor overlay).
    pub fn fill_rect(&mut self, x: u32, y: u32, w: u32, h: u32, color: u32) {
        let x2 = (x + w).min(self.width);
        let y2 = (y + h).min(self.height);
        for row in y..y2 {
            let start = (row * self.width + x) as usize;
            let end = (row * self.width + x2) as usize;
            self.pixels[start..end].fill(color);
        }
    }

    /// Count non-black pixels (for render verification in tests).
    pub fn non_black_count(&self) -> usize {
        self.pixels.iter().filter(|&&p| p != 0xFF000000).count()
    }
}

// ── Custom Platform ─────────────────────────────────────────────────────────

/// Global tick counter used as a monotonic clock.
/// In production, replace with a hardware timer read.
static TICK: Cell<u64> = Cell::new(0);

/// Advance the global tick counter. Call this from `GuiState::step()`.
fn advance_tick() {
    TICK.set(TICK.get().wrapping_add(1));
}

/// Custom Slint platform backend for the AeroSLS sidecar.
struct AeroSlsPlatform;

impl Platform for AeroSlsPlatform {
    fn create_window_adapter(
        &self,
    ) -> Result<Rc<dyn WindowAdapter>, slint::platform::PlatformError> {
        Ok(MinimalSoftwareWindow::new(RepaintBufferType::ReusedBuffer)
            as Rc<dyn WindowAdapter>)
    }

    fn duration_since_start(&self) -> Duration {
        // 1 tick = ~16ms (60 fps). Replace with hardware timer on ARM.
        Duration::from_millis(TICK.get() * 16)
    }

    fn run_event_loop(&self) -> Result<(), slint::platform::PlatformError> {
        Err(slint::platform::PlatformError::NoPlatform)
    }

    fn debug_log(&self, _arguments: core::fmt::Arguments<'_>) {}
}

// ── RGB565 → ARGB conversion ────────────────────────────────────────────────

#[inline]
fn rgb565_to_argb(p: Rgb565Pixel) -> u32 {
    let raw = p.0;
    let r5 = (raw >> 11) & 0x1F;
    let g6 = (raw >> 5) & 0x3F;
    let b5 = raw & 0x1F;
    let r8 = ((r5 << 3) | (r5 >> 2)) as u32;
    let g8 = ((g6 << 2) | (g6 >> 4)) as u32;
    let b8 = ((b5 << 3) | (b5 >> 2)) as u32;
    0xFF000000 | (r8 << 16) | (g8 << 8) | b8
}

// ── GUI State (step-function integration) ───────────────────────────────────

/// The GUI state, driven cooperatively by the sidecar's scheduler.
pub struct GuiState {
    window: Rc<MinimalSoftwareWindow>,
    framebuffer: Framebuffer,
}

impl GuiState {
    /// Create a new GUI state with the given resolution.
    pub fn new(width: u32, height: u32) -> Self {
        let _ = slint::platform::set_platform(Box::new(AeroSlsPlatform));

        let window = MinimalSoftwareWindow::new(RepaintBufferType::ReusedBuffer);
        window.set_size(PhysicalSize::new(width, height));

        let framebuffer = Framebuffer::new(width, height);

        GuiState {
            window,
            framebuffer,
        }
    }

    /// Dispatch an input event to the Slint scene.
    pub fn handle_input(&self, event: WindowEvent) {
        self.window.dispatch_event(event);
    }

    /// Run one step: update timers/animations, render if needed.
    /// Returns `true` if a frame was rendered.
    pub fn step(&mut self) -> bool {
        advance_tick();
        slint::platform::update_timers_and_animations();

        let w = self.framebuffer.width as usize;
        let mut rendered = false;

        self.window.draw_if_needed(|renderer| {
            let mut buf = alloc::vec![Rgb565Pixel(0); w * self.framebuffer.height as usize];
            renderer.render(&mut buf, w);
            for (i, &px) in buf.iter().enumerate() {
                self.framebuffer.pixels[i] = rgb565_to_argb(px);
            }
            rendered = true;
        });
        rendered
    }

    /// Whether the window wants a redraw.
    pub fn needs_render(&self) -> bool {
        self.window.has_active_animations()
    }

    pub fn framebuffer(&self) -> &Framebuffer {
        &self.framebuffer
    }

    pub fn framebuffer_mut(&mut self) -> &mut Framebuffer {
        &mut self.framebuffer
    }

    pub fn next_timer(&self) -> Duration {
        slint::platform::duration_until_next_timer_update()
            .unwrap_or(Duration::from_secs(60))
    }

    pub fn resize(&mut self, width: u32, height: u32) {
        self.window.set_size(PhysicalSize::new(width, height));
        self.framebuffer = Framebuffer::new(width, height);
    }

    pub fn show(&self) -> Result<(), slint::platform::PlatformError> {
        self.window.show()
    }

    pub fn hide(&self) -> Result<(), slint::platform::PlatformError> {
        self.window.hide()
    }

    pub fn window(&self) -> &MinimalSoftwareWindow {
        &self.window
    }
}

// ── Tests ───────────────────────────────────────────────────────────────────

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn framebuffer_basics() {
        let fb = Framebuffer::new(100, 50);
        assert_eq!(fb.width, 100);
        assert_eq!(fb.height, 50);
        assert_eq!(fb.pixels.len(), 5000);
        assert_eq!(fb.pixels[0], 0xFF000000);
    }

    #[test]
    fn framebuffer_rgba8_conversion() {
        let mut fb = Framebuffer::new(2, 1);
        fb.pixels[0] = 0xFF_FF0000;
        fb.pixels[1] = 0xFF_00FF00;
        let rgba = fb.to_rgba8();
        assert_eq!(&rgba, &[255, 0, 0, 255, 0, 255, 0, 255]);
    }

    #[test]
    fn framebuffer_fill_rect() {
        let mut fb = Framebuffer::new(10, 10);
        fb.fill_rect(2, 3, 4, 2, 0xFF_FF0000);
        assert_eq!(fb.pixels[0], 0xFF000000);
        assert_eq!(fb.pixels[3 * 10 + 2], 0xFF_FF0000);
        assert_eq!(fb.pixels[3 * 10 + 5], 0xFF_FF0000);
        assert_eq!(fb.pixels[4 * 10 + 2], 0xFF_FF0000);
        assert_eq!(fb.pixels[3 * 10 + 6], 0xFF000000);
    }

    #[test]
    fn rgb565_to_argb_conversion() {
        // Pure red: R=31, G=0, B=0 → 0xF800
        let px = Rgb565Pixel(0xF800);
        let argb = rgb565_to_argb(px);
        assert_eq!(argb & 0xFF0000, 0xFF0000);
        assert_eq!(argb & 0x00FF00, 0x000000);
        assert_eq!(argb & 0x0000FF, 0x000000);
        assert_eq!(argb >> 24, 0xFF);
    }

    #[test]
    fn gui_state_lifecycle() {
        let mut gui = GuiState::new(320, 240);
        assert_eq!(gui.framebuffer().width, 320);
        assert_eq!(gui.framebuffer().height, 240);
        gui.show().unwrap();
        gui.step();
        gui.hide().unwrap();
    }

    #[test]
    fn gui_state_resize() {
        let mut gui = GuiState::new(100, 100);
        gui.resize(200, 150);
        assert_eq!(gui.framebuffer().width, 200);
        assert_eq!(gui.framebuffer().height, 150);
        assert_eq!(gui.framebuffer().pixels.len(), 30000);
    }

    #[test]
    fn gui_state_input_dispatch() {
        let mut gui = GuiState::new(320, 240);
        gui.show().unwrap();
        gui.handle_input(WindowEvent::PointerPressed {
            position: slint::LogicalPosition::new(100.0, 100.0),
            button: slint::platform::PointerEventButton::Left,
        });
        gui.step();
        gui.hide().unwrap();
    }

    #[test]
    fn next_timer_returns_duration() {
        let gui = GuiState::new(100, 100);
        let t = gui.next_timer();
        assert!(t < Duration::from_secs(60));
    }

    #[test]
    fn step_advances_tick() {
        let mut gui = GuiState::new(64, 64);
        gui.show().unwrap();
        let t1 = TICK.get();
        gui.step();
        let t2 = TICK.get();
        assert!(t2 > t1, "tick should advance on each step");
        gui.hide().unwrap();
    }
}
