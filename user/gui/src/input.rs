//! Input and output utilities for the AeroSLS GUI.
//!
//! Provides:
//! - Clipboard support with X11 selection protocol
//! - Mouse cursor rendering and hardware cursor support
//! - Touch screen input handling for embedded ARM
//! - Screenshot/capture functionality

extern crate alloc;

use alloc::string::{String, ToString};
use alloc::vec;
use alloc::vec::Vec;
use core::cell::RefCell;

use crate::Framebuffer;

// ── Clipboard ───────────────────────────────────────────────────────────────

/// X11 selection atoms for clipboard.
pub const CLIPBOARD: &str = "CLIPBOARD";
pub const TARGETS: &str = "TARGETS";
pub const TEXT_PLAIN: &str = "TEXT_PLAIN";
pub const UTF8_STRING: &str = "UTF8_STRING";

/// Clipboard state shared between X11 server and Slint.
pub struct Clipboard {
    /// Current clipboard content (UTF-8 text).
    content: RefCell<String>,
    /// Owner window ID (for X11 selection ownership).
    owner: RefCell<u32>,
    /// Timestamp of last copy.
    timestamp: RefCell<u64>,
}

impl Clipboard {
    pub fn new() -> Self {
        Clipboard {
            content: RefCell::new(String::new()),
            owner: RefCell::new(0),
            timestamp: RefCell::new(0),
        }
    }

    /// Copy text to clipboard.
    pub fn copy(&self, text: &str, window_id: u32, timestamp: u64) {
        *self.content.borrow_mut() = text.to_string();
        *self.owner.borrow_mut() = window_id;
        *self.timestamp.borrow_mut() = timestamp;
    }

    /// Paste text from clipboard.
    pub fn paste(&self) -> String {
        self.content.borrow().clone()
    }

    /// Get clipboard content as bytes.
    pub fn paste_bytes(&self) -> Vec<u8> {
        self.content.borrow().as_bytes().to_vec()
    }

    /// Clear clipboard.
    pub fn clear(&self) {
        self.content.borrow_mut().clear();
        *self.owner.borrow_mut() = 0;
    }

    /// Check if clipboard has content.
    pub fn has_content(&self) -> bool {
        !self.content.borrow().is_empty()
    }

    /// Get the owner window ID.
    pub fn owner(&self) -> u32 {
        *self.owner.borrow()
    }

    /// Get the timestamp.
    pub fn timestamp(&self) -> u64 {
        *self.timestamp.borrow()
    }

    /// Handle X11 SelectionRequest (type 30).
    /// Returns response bytes to send back.
    pub fn handle_selection_request(&self, request: &[u8]) -> Option<Vec<u8>> {
        if request.len() < 32 {
            return None;
        }

        let requestor = u32::from_le_bytes([request[4], request[5], request[6], request[7]]);
        let selection = u32::from_le_bytes([request[8], request[9], request[10], request[11]]);
        let target = u32::from_le_bytes([request[12], request[13], request[14], request[15]]);
        let property = u32::from_le_bytes([request[16], request[17], request[18], request[19]]);
        let _time = u32::from_le_bytes([request[20], request[21], request[22], request[23]]);

        // For now, respond with the clipboard content for any target.
        // A full implementation would check the target atom.
        let content = self.paste();
        if content.is_empty() {
            return None;
        }

        // Build SelectionNotify (type 31).
        let mut reply = vec![0u8; 32];
        reply[0] = 31; // SelectionNotify
        reply[1] = 0; // unused
        // sequence number at bytes 2-3
        reply[4..8].copy_from_slice(&requestor.to_le_bytes()); // requestor
        reply[8..12].copy_from_slice(&selection.to_le_bytes()); // selection
        reply[12..16].copy_from_slice(&target.to_le_bytes()); // target
        reply[16..20].copy_from_slice(&property.to_le_bytes()); // property
        reply[20..24].copy_from_slice(&0u32.to_le_bytes()); // time

        Some(reply)
    }

    /// Handle X11 SelectionClear (type 32).
    pub fn handle_selection_clear(&self, event: &[u8]) {
        if event.len() < 32 {
            return;
        }
        let _owner = u32::from_le_bytes([event[4], event[5], event[6], event[7]]);
        let _selection = u32::from_le_bytes([event[8], event[9], event[10], event[11]]);
        // Clear clipboard if we lost ownership.
        // For now, keep the content (v1 is lenient).
    }
}

impl Default for Clipboard {
    fn default() -> Self {
        Self::new()
    }
}

// ── Mouse Cursor ────────────────────────────────────────────────────────────

/// Cursor shapes for the GUI.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum CursorShape {
    /// Default arrow cursor.
    Arrow,
    /// Text input cursor (I-beam).
    Text,
    /// Hand/pointer cursor.
    Hand,
    /// Resize horizontal.
    ResizeH,
    /// Resize vertical.
    ResizeV,
    /// Crosshair.
    Crosshair,
    /// Wait/spinner.
    Wait,
    /// Hidden cursor.
    Hidden,
}

/// Simple cursor renderer for the framebuffer.
pub struct CursorRenderer {
    shape: CursorShape,
    x: i32,
    y: i32,
    visible: bool,
    hotspot: (i32, i32),
}

impl CursorRenderer {
    pub fn new() -> Self {
        CursorRenderer {
            shape: CursorShape::Arrow,
            x: 0,
            y: 0,
            visible: true,
            hotspot: (0, 0),
        }
    }

    /// Set cursor position.
    pub fn set_position(&mut self, x: i32, y: i32) {
        self.x = x;
        self.y = y;
    }

    /// Set cursor shape.
    pub fn set_shape(&mut self, shape: CursorShape) {
        self.shape = shape;
        self.hotspot = match shape {
            CursorShape::Arrow => (0, 0),
            CursorShape::Text => (4, 8),
            CursorShape::Hand => (2, 0),
            CursorShape::ResizeH => (8, 8),
            CursorShape::ResizeV => (8, 8),
            CursorShape::Crosshair => (8, 8),
            CursorShape::Wait => (8, 8),
            CursorShape::Hidden => (0, 0),
        };
    }

    /// Show/hide cursor.
    pub fn set_visible(&mut self, visible: bool) {
        self.visible = visible;
    }

    /// Get current cursor position.
    pub fn position(&self) -> (i32, i32) {
        (self.x, self.y)
    }

    /// Get current shape.
    pub fn shape(&self) -> CursorShape {
        self.shape
    }

    /// Render cursor onto framebuffer.
    pub fn render(&self, fb: &mut Framebuffer) {
        if !self.visible || self.shape == CursorShape::Hidden {
            return;
        }

        let cursor_color = 0xFF_FFFFFF; // White cursor
        let outline_color = 0xFF_000000; // Black outline

        match self.shape {
            CursorShape::Arrow => {
                // Simple 12x16 arrow cursor.
                let arrow = [
                    "XX          ",
                    "XXX         ",
                    "XXXX        ",
                    "XXXXX       ",
                    "XXXXXX      ",
                    "XXXXXXX     ",
                    "XXXXXXXX    ",
                    "XXXXXXXXX   ",
                    "XXXXXXXXXX  ",
                    "XX  XXXXXX  ",
                    "X   XXXXX   ",
                    "    XXXX    ",
                    "    XXX     ",
                    "    XX      ",
                    "            ",
                    "            ",
                ];
                self.render_pattern(fb, &arrow, cursor_color, outline_color);
            }
            CursorShape::Text => {
                // I-beam cursor.
                let text = [
                    "    X    ",
                    "    X    ",
                    "    X    ",
                    "    X    ",
                    "    X    ",
                    "    X    ",
                    "    X    ",
                    "    X    ",
                    "    X    ",
                    "    X    ",
                    "    X    ",
                    "XXXXXXXXX",
                    "XXXXXXXXX",
                    "    X    ",
                    "    X    ",
                    "    X    ",
                ];
                self.render_pattern(fb, &text, cursor_color, outline_color);
            }
            CursorShape::Hand => {
                // Pointing hand cursor.
                let hand = [
                    "    XX   ",
                    "   XXX   ",
                    "  XXXX   ",
                    "  XXXX   ",
                    " XXXXX   ",
                    " XXXXX   ",
                    " XXXXXX  ",
                    "XXXXXXXX ",
                    "XXXXXXXX ",
                    "XXXXXXXX ",
                    " XXXXXX  ",
                    " XXXXX   ",
                    "  XXX    ",
                    "  XXX    ",
                    "   X     ",
                    "   X     ",
                ];
                self.render_pattern(fb, &hand, cursor_color, outline_color);
            }
            CursorShape::Crosshair => {
                // Crosshair cursor.
                let cross = [
                    "    X    ",
                    "    X    ",
                    "    X    ",
                    "    X    ",
                    "    X    ",
                    "    X    ",
                    "XXXXXXXXX",
                    "XXXXXXXXX",
                    "    X    ",
                    "    X    ",
                    "    X    ",
                    "    X    ",
                    "    X    ",
                    "    X    ",
                ];
                self.render_pattern(fb, &cross, cursor_color, outline_color);
            }
            _ => {
                // For other shapes, render a simple rectangle.
                fb.fill_rect(
                    (self.x - self.hotspot.0) as u32,
                    (self.y - self.hotspot.1) as u32,
                    16,
                    16,
                    cursor_color,
                );
            }
        }
    }

    /// Render a text pattern onto the framebuffer.
    fn render_pattern(&self, fb: &mut Framebuffer, pattern: &[&str], fg: u32, outline: u32) {
        let start_x = (self.x - self.hotspot.0) as u32;
        let start_y = (self.y - self.hotspot.1) as u32;

        for (row, line) in pattern.iter().enumerate() {
            for (col, ch) in line.chars().enumerate() {
                let px = start_x + col as u32;
                let py = start_y + row as u32;
                if px < fb.width && py < fb.height {
                    match ch {
                        'X' => {
                            fb.pixels[(py * fb.width + px) as usize] = fg;
                        }
                        ' ' => {
                            // Transparent, don't draw.
                        }
                        _ => {}
                    }
                }
            }
        }
    }
}

impl Default for CursorRenderer {
    fn default() -> Self {
        Self::new()
    }
}

// ── Touch Screen ────────────────────────────────────────────────────────────

/// Touch event for embedded ARM touchscreens.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct TouchEvent {
    /// Touch ID (for multi-touch).
    pub id: u32,
    /// X coordinate in screen pixels.
    pub x: i32,
    /// Y coordinate in screen pixels.
    pub y: i32,
    /// Touch pressure (0-255).
    pub pressure: u8,
    /// Touch state.
    pub state: TouchState,
}

/// Touch event state.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum TouchState {
    /// Touch began (finger down).
    Down,
    /// Touch moved (finger drag).
    Move,
    /// Touch ended (finger up).
    Up,
    /// Touch cancelled.
    Cancel,
}

/// Touch input handler for multi-touch screens.
pub struct TouchHandler {
    /// Maximum simultaneous touches.
    max_touches: u32,
    /// Active touch points.
    active: Vec<TouchEvent>,
    /// Screen dimensions for coordinate mapping.
    screen_width: u32,
    screen_height: u32,
}

impl TouchHandler {
    pub fn new(max_touches: u32, screen_width: u32, screen_height: u32) -> Self {
        TouchHandler {
            max_touches,
            active: Vec::with_capacity(max_touches as usize),
            screen_width,
            screen_height,
        }
    }

    /// Process a raw touch event from the input device.
    pub fn process_event(&mut self, event: TouchEvent) -> Option<TouchEvent> {
        // Clamp coordinates to screen bounds.
        let x = event.x.clamp(0, self.screen_width as i32 - 1);
        let y = event.y.clamp(0, self.screen_height as i32 - 1);

        let mapped = TouchEvent {
            id: event.id,
            x,
            y,
            pressure: event.pressure,
            state: event.state,
        };

        match mapped.state {
            TouchState::Down => {
                if self.active.len() < self.max_touches as usize {
                    self.active.push(mapped);
                    Some(mapped)
                } else {
                    None // Too many touches.
                }
            }
            TouchState::Move => {
                if let Some(active) = self.active.iter_mut().find(|t| t.id == mapped.id) {
                    active.x = mapped.x;
                    active.y = mapped.y;
                    active.pressure = mapped.pressure;
                    Some(mapped)
                } else {
                    None // Unknown touch ID.
                }
            }
            TouchState::Up | TouchState::Cancel => {
                if let Some(pos) = self.active.iter().position(|t| t.id == mapped.id) {
                    self.active.remove(pos);
                    Some(mapped)
                } else {
                    None
                }
            }
        }
    }

    /// Get all active touches.
    pub fn active_touches(&self) -> &[TouchEvent] {
        &self.active
    }

    /// Get number of active touches.
    pub fn touch_count(&self) -> usize {
        self.active.len()
    }

    /// Clear all active touches.
    pub fn clear(&mut self) {
        self.active.clear();
    }
}

// ── Screenshot ──────────────────────────────────────────────────────────────

/// Screenshot format.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ScreenshotFormat {
    /// Raw ARGB pixels.
    RawArgb,
    /// Raw BGRA pixels (for fbdev).
    RawBgra,
    /// PPM format (portable pixmap, for easy viewing).
    Ppm,
}

/// Screenshot capture utility.
pub struct Screenshot {
    format: ScreenshotFormat,
}

impl Screenshot {
    pub fn new(format: ScreenshotFormat) -> Self {
        Screenshot { format }
    }

    /// Capture framebuffer as bytes in the configured format.
    pub fn capture(&self, fb: &Framebuffer) -> Vec<u8> {
        match self.format {
            ScreenshotFormat::RawArgb => {
                let mut buf = Vec::with_capacity(fb.pixels.len() * 4);
                for &pixel in &fb.pixels {
                    buf.extend_from_slice(&pixel.to_le_bytes());
                }
                buf
            }
            ScreenshotFormat::RawBgra => {
                let mut buf = Vec::with_capacity(fb.pixels.len() * 4);
                for &pixel in &fb.pixels {
                    let a = ((pixel >> 24) & 0xFF) as u8;
                    let r = ((pixel >> 16) & 0xFF) as u8;
                    let g = ((pixel >> 8) & 0xFF) as u8;
                    let b = (pixel & 0xFF) as u8;
                    buf.push(b);
                    buf.push(g);
                    buf.push(r);
                    buf.push(a);
                }
                buf
            }
            ScreenshotFormat::Ppm => {
                let mut buf = Vec::new();
                // PPM header.
                buf.extend_from_slice(b"P6\n");
                buf.extend_from_slice(format!("{} {}\n", fb.width, fb.height).as_bytes());
                buf.extend_from_slice(b"255\n");
                // Pixel data (RGB).
                for &pixel in &fb.pixels {
                    let r = ((pixel >> 16) & 0xFF) as u8;
                    let g = ((pixel >> 8) & 0xFF) as u8;
                    let b = (pixel & 0xFF) as u8;
                    buf.push(r);
                    buf.push(g);
                    buf.push(b);
                }
                buf
            }
        }
    }

    /// Capture and write to a file (linux only).
    pub fn capture_to_file(&self, fb: &Framebuffer, path: &str) -> Result<usize, &'static str> {
        let data = self.capture(fb);

        #[cfg(target_os = "linux")]
        {
            use std::io::Write;
            let mut f = std::fs::File::create(path).map_err(|_| "create file")?;
            f.write_all(&data).map_err(|_| "write file")?;
            Ok(data.len())
        }
        #[cfg(not(target_os = "linux"))]
        {
            let _ = path;
            Ok(data.len())
        }
    }
}

impl Default for Screenshot {
    fn default() -> Self {
        Self::new(ScreenshotFormat::RawArgb)
    }
}

// ── Tests ───────────────────────────────────────────────────────────────────

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn clipboard_copy_paste() {
        let cb = Clipboard::new();
        assert!(!cb.has_content());

        cb.copy("hello world", 1, 100);
        assert!(cb.has_content());
        assert_eq!(cb.paste(), "hello world");
        assert_eq!(cb.paste_bytes(), b"hello world");
        assert_eq!(cb.owner(), 1);
        assert_eq!(cb.timestamp(), 100);
    }

    #[test]
    fn clipboard_clear() {
        let cb = Clipboard::new();
        cb.copy("test", 1, 100);
        assert!(cb.has_content());
        cb.clear();
        assert!(!cb.has_content());
    }

    #[test]
    fn cursor_renderer_position() {
        let mut cursor = CursorRenderer::new();
        assert_eq!(cursor.position(), (0, 0));

        cursor.set_position(100, 200);
        assert_eq!(cursor.position(), (100, 200));
    }

    #[test]
    fn cursor_renderer_shape() {
        let mut cursor = CursorRenderer::new();
        assert_eq!(cursor.shape(), CursorShape::Arrow);

        cursor.set_shape(CursorShape::Text);
        assert_eq!(cursor.shape(), CursorShape::Text);
    }

    #[test]
    fn cursor_renderer_render() {
        let mut fb = Framebuffer::new(100, 100);
        let mut cursor = CursorRenderer::new();
        cursor.set_position(50, 50);

        // Render should not panic.
        cursor.render(&mut fb);

        // Check that cursor pixels were drawn.
        let center = 50 * 100 + 50;
        assert_eq!(fb.pixels[center], 0xFF_FFFFFF); // White cursor
    }

    #[test]
    fn touch_handler_basic() {
        let mut handler = TouchHandler::new(5, 800, 600);

        // Touch down.
        let evt = handler.process_event(TouchEvent {
            id: 0,
            x: 100,
            y: 200,
            pressure: 128,
            state: TouchState::Down,
        });
        assert!(evt.is_some());
        assert_eq!(handler.touch_count(), 1);

        // Touch move.
        let evt = handler.process_event(TouchEvent {
            id: 0,
            x: 150,
            y: 250,
            pressure: 128,
            state: TouchState::Move,
        });
        assert!(evt.is_some());
        assert_eq!(handler.active_touches()[0].x, 150);

        // Touch up.
        let evt = handler.process_event(TouchEvent {
            id: 0,
            x: 150,
            y: 250,
            pressure: 0,
            state: TouchState::Up,
        });
        assert!(evt.is_some());
        assert_eq!(handler.touch_count(), 0);
    }

    #[test]
    fn touch_handler_clamping() {
        let mut handler = TouchHandler::new(5, 100, 100);

        // Out of bounds coordinates should be clamped.
        let evt = handler.process_event(TouchEvent {
            id: 0,
            x: -10,
            y: 200,
            pressure: 128,
            state: TouchState::Down,
        });
        assert!(evt.is_some());
        let evt = evt.unwrap();
        assert_eq!(evt.x, 0);
        assert_eq!(evt.y, 99);
    }

    #[test]
    fn touch_handler_max_touches() {
        let mut handler = TouchHandler::new(2, 800, 600);

        // Add 2 touches.
        handler.process_event(TouchEvent { id: 0, x: 10, y: 10, pressure: 128, state: TouchState::Down });
        handler.process_event(TouchEvent { id: 1, x: 20, y: 20, pressure: 128, state: TouchState::Down });
        assert_eq!(handler.touch_count(), 2);

        // Third touch should be rejected.
        let evt = handler.process_event(TouchEvent { id: 2, x: 30, y: 30, pressure: 128, state: TouchState::Down });
        assert!(evt.is_none());
        assert_eq!(handler.touch_count(), 2);
    }

    #[test]
    fn screenshot_raw_argb() {
        let mut fb = Framebuffer::new(2, 1);
        fb.pixels[0] = 0xFF_FF0000; // red
        fb.pixels[1] = 0xFF_00FF00; // green

        let ss = Screenshot::new(ScreenshotFormat::RawArgb);
        let data = ss.capture(&fb);
        assert_eq!(data.len(), 16); // 2 pixels * 4 bytes
        assert_eq!(data[0..4], [0x00, 0x00, 0xFF, 0xFF]); // red LE
        assert_eq!(data[4..8], [0x00, 0xFF, 0x00, 0xFF]); // green LE
    }

    #[test]
    fn screenshot_ppm() {
        let mut fb = Framebuffer::new(2, 1);
        fb.pixels[0] = 0xFF_FF0000; // red
        fb.pixels[1] = 0xFF_00FF00; // green

        let ss = Screenshot::new(ScreenshotFormat::Ppm);
        let data = ss.capture(&fb);
        let header_end = data.windows(3).position(|w| w == b"255\n").unwrap() + 3;
        let pixels = &data[header_end..];
        assert_eq!(pixels.len(), 6); // 2 pixels * 3 bytes (RGB)
        assert_eq!(pixels[0..3], [0xFF, 0x00, 0x00]); // red
        assert_eq!(pixels[3..6], [0x00, 0xFF, 0x00]); // green
    }
}
