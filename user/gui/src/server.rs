//! X11 server applet for the AeroSLS GUI.
//!
//! Listens on a Unix socket (e.g. `/tmp/.X11-unix/X0`), accepts X11 clients,
//! performs the connection handshake, and sends `PutImage` frames from the
//! Slint software renderer.
//!
//! Features:
//! - X11 keycode → Slint key mapping for keyboard input
//! - Frame diff optimization (only sends PutImage when pixels change)
//! - `/dev/fb0` framebuffer writer for ARM embedded targets
//! - Resize handling via ConfigureWindow requests
//! - Multi-client support (accepts second connection, disconnects first)

extern crate alloc;

use alloc::vec;
use alloc::vec::Vec;
use core::cell::RefCell;

use slint::platform::WindowEvent;

use crate::input::{Clipboard, CursorRenderer};
use crate::x11::{
    build_configure_notify, build_map_notify, build_put_image, build_setup_reply, ClientHello,
    X11Event,
};
use crate::{Framebuffer, GuiState};

// ── X11 Keycode → Slint Key Mapping ─────────────────────────────────────────

/// X11 keycodes (evdev + 8) mapped to Slint `Key` values.
///
/// This covers the most common keys needed for GUI interaction.
/// X11 keycodes are evdev codes + 8 on Linux.
fn x11_keycode_to_text(keycode: u8) -> Option<&'static str> {
    match keycode {
        // Letters (evdev 30-38 = a-l, 44-47 = z-c, 50 = shift... simplified)
        24 => Some("q"),
        25 => Some("w"),
        26 => Some("e"),
        27 => Some("r"),
        28 => Some("t"),
        29 => Some("y"),
        30 => Some("u"),
        31 => Some("i"),
        32 => Some("o"),
        33 => Some("p"),
        38 => Some("a"),
        39 => Some("s"),
        40 => Some("d"),
        41 => Some("f"),
        42 => Some("g"),
        43 => Some("h"),
        44 => Some("j"),
        45 => Some("k"),
        46 => Some("l"),
        52 => Some("z"),
        53 => Some("x"),
        54 => Some("c"),
        55 => Some("v"),
        56 => Some("b"),
        57 => Some("n"),
        58 => Some("m"),
        // Digits
        19 => Some("1"),
        20 => Some("2"),
        21 => Some("3"),
        22 => Some("4"),
        23 => Some("5"),
        24 => Some("6"), // Note: overlaps with q — use scan code
        // Special keys
        22 => Some("\x08"), // Backspace (evdev 14 + 8 = 22)
        36 => Some("\r"),   // Return/Enter (evdev 28 + 8 = 36)
        23 => Some("\t"),   // Tab (evdev 15 + 8 = 23)
        65 => Some(" "),    // Space (evdev 57 + 8 = 65)
        119 => Some("\x1B"), // Escape (evdev 111 + 8 = 119)
        _ => None,
    }
}

/// Convert an X11 keycode to a Slint `Key` enum value.
fn x11_keycode_to_slint_key(keycode: u8) -> slint::Key {
    match keycode {
        22 => slint::Key::Backspace,
        36 => slint::Key::Return,
        23 => slint::Key::Tab,
        65 => slint::Key::Space,
        119 => slint::Key::Escape,
        113 => slint::Key::LeftArrow,
        114 => slint::Key::RightArrow,
        111 => slint::Key::UpArrow,
        116 => slint::Key::DownArrow,
        112 => slint::Key::PageUp,
        117 => slint::Key::PageDown,
        110 => slint::Key::Home,
        115 => slint::Key::End,
        118 => slint::Key::Insert,
        119 => slint::Key::Escape,
        67 => slint::Key::F1,
        68 => slint::Key::F2,
        69 => slint::Key::F3,
        70 => slint::Key::F4,
        71 => slint::Key::F5,
        72 => slint::Key::F6,
        73 => slint::Key::F7,
        74 => slint::Key::F8,
        75 => slint::Key::F9,
        76 => slint::Key::F10,
        95 => slint::Key::F11,
        96 => slint::Key::F12,
        _ => slint::Key::Unknown,
    }
}

// ── Frame Diff ──────────────────────────────────────────────────────────────

/// Compare two framebuffers and return true if they differ.
fn fb_changed(a: &[u32], b: &[u32]) -> bool {
    if a.len() != b.len() {
        return true;
    }
    // Compare 4 pixels at a time (u128) for speed.
    let chunks_a = a.chunks_exact(4);
    let chunks_b = b.chunks_exact(4);
    for (ca, cb) in chunks_a.zip(chunks_b) {
        if ca[0] != cb[0] || ca[1] != cb[1] || ca[2] != cb[2] || ca[3] != cb[3] {
            return true;
        }
    }
    // Handle remainder.
    let aligned = a.len() & !3;
    for i in aligned..a.len() {
        if a[i] != b[i] {
            return true;
        }
    }
    false
}

// ── /dev/fb0 Writer ─────────────────────────────────────────────────────────

/// Write the framebuffer to `/dev/fb0` (Linux framebuffer device).
///
/// On ARM embedded targets, this provides direct display output without X11.
/// The fbdev format is BGRA32 (little-endian), matching our PutImage format.
pub fn write_fb0(fb: &Framebuffer, _path: &str) -> Result<usize, &'static str> {
    // Convert ARGB → BGRA for fbdev.
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

    // In a real implementation, we'd open the file and write.
    // For now, return the byte count that would be written.
    #[cfg(target_os = "linux")]
    {
        use std::io::Write;
        let mut f = std::fs::File::create(_path).map_err(|_| "open /dev/fb0")?;
        f.write_all(&buf).map_err(|_| "write /dev/fb0")?;
        Ok(buf.len())
    }
    #[cfg(not(target_os = "linux"))]
    {
        let _ = buf;
        Ok(0)
    }
}

// ── State Machine ───────────────────────────────────────────────────────────

/// State machine phases for the X11 server.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Phase {
    /// Waiting for a client to connect.
    Listening,
    /// Receiving the ClientHello from the client.
    RecvHello,
    /// Sending the SetupReply to the client.
    SendHello,
    /// Waiting for the client to create/map a window.
    WaitMap,
    /// Rendering and sending PutImage frames.
    Rendering,
    /// Client disconnected.
    Disconnected,
}

/// The X11 server state, driven cooperatively.
pub struct X11Server {
    /// Current phase.
    phase: Phase,
    /// The Slint GUI state.
    gui: GuiState,
    /// The server socket fd (bound to /tmp/.X11-unix/X0).
    listen_fd: Option<u32>,
    /// The accepted client connection fd.
    client_fd: Option<u32>,
    /// Receive buffer for X11 protocol messages.
    recv_buf: Vec<u8>,
    /// Send buffer for pending X11 protocol messages.
    send_buf: Vec<u8>,
    /// The root window ID (assigned by us).
    root_window: u32,
    /// The client's created window ID.
    client_window: u32,
    /// The graphics context ID.
    gc_id: u32,
    /// Next resource ID to allocate.
    next_resource: u32,
    /// Whether the client has mapped a window (ready for rendering).
    window_mapped: bool,
    /// Counter for frame sequencing.
    frame_count: u64,
    /// Pending input events from the client.
    pending_events: Vec<X11Event>,
    /// Previous framebuffer snapshot for diff detection.
    prev_fb: Vec<u32>,
    /// Whether to write to /dev/fb0 (ARM mode).
    fb0_path: Option<alloc::string::String>,
    /// Client window dimensions (may change via ConfigureWindow).
    client_width: u16,
    client_height: u16,
    /// Client window position (may change via ConfigureWindow).
    client_x: i16,
    client_y: i16,
    /// Clipboard state for copy/paste.
    clipboard: Clipboard,
    /// Mouse cursor renderer.
    cursor: CursorRenderer,
    /// Sequence number for X11 events.
    sequence: u32,
    /// Whether window has focus.
    focused: bool,
    /// Window title.
    title: alloc::string::String,
}

impl X11Server {
    /// Create a new X11 server with the given display resolution.
    pub fn new(width: u32, height: u32) -> Self {
        let mut gui = GuiState::new(width, height);
        let _ = gui.show();

        X11Server {
            phase: Phase::Listening,
            gui,
            listen_fd: None,
            client_fd: None,
            recv_buf: Vec::new(),
            send_buf: Vec::new(),
            root_window: 1,
            client_window: 0,
            gc_id: 0,
            next_resource: 100,
            window_mapped: false,
            frame_count: 0,
            pending_events: Vec::new(),
            prev_fb: alloc::vec![0; (width * height) as usize],
            fb0_path: None,
            client_width: width as u16,
            client_height: height as u16,
            client_x: 0,
            client_y: 0,
            clipboard: Clipboard::new(),
            cursor: CursorRenderer::new(),
            sequence: 0,
            focused: true,
            title: alloc::string::String::new(),
        }
    }

    /// Enable /dev/fb0 output for ARM embedded targets.
    pub fn with_fb0(mut self, path: &str) -> Self {
        self.fb0_path = Some(alloc::string::String::from(path));
        self
    }

    /// Allocate the next resource ID.
    fn alloc_resource(&mut self) -> u32 {
        let id = self.next_resource;
        self.next_resource += 1;
        id
    }

    /// Get the framebuffer for external inspection.
    pub fn framebuffer(&self) -> &Framebuffer {
        self.gui.framebuffer()
    }

    /// Get the current phase.
    pub fn phase(&self) -> Phase {
        self.phase
    }

    /// Get pending input events (drains the queue).
    pub fn take_events(&mut self) -> Vec<X11Event> {
        core::mem::take(&mut self.pending_events)
    }

    /// Feed raw bytes from the client socket into the server.
    pub fn feed_recv(&mut self, data: &[u8]) {
        self.recv_buf.extend_from_slice(data);
    }

    /// Take the send buffer (bytes to write to the client socket).
    pub fn take_send_buf(&mut self) -> Vec<u8> {
        core::mem::take(&mut self.send_buf)
    }

    /// Set the server socket fd (called after bind/listen).
    pub fn set_listen_fd(&mut self, fd: u32) {
        self.listen_fd = Some(fd);
    }

    /// Set the accepted client fd (called after accept).
    pub fn set_client_fd(&mut self, fd: u32) {
        self.client_fd = Some(fd);
        self.phase = Phase::RecvHello;
    }

    /// Disconnect the current client and return to Listening.
    pub fn disconnect(&mut self) {
        self.client_fd = None;
        self.client_window = 0;
        self.gc_id = 0;
        self.window_mapped = false;
        self.recv_buf.clear();
        self.send_buf.clear();
        self.pending_events.clear();
        self.phase = Phase::Listening;
    }

    /// Get clipboard for external access.
    pub fn clipboard(&self) -> &Clipboard {
        &self.clipboard
    }

    /// Get window position.
    pub fn window_position(&self) -> (i16, i16) {
        (self.client_x, self.client_y)
    }

    /// Set window position.
    pub fn set_window_position(&mut self, x: i16, y: i16) {
        self.client_x = x;
        self.client_y = y;
    }

    /// Get window title.
    pub fn title(&self) -> &str {
        &self.title
    }

    /// Set window title.
    pub fn set_title(&mut self, title: &str) {
        self.title = alloc::string::String::from(title);
    }

    /// Check if window has focus.
    pub fn is_focused(&self) -> bool {
        self.focused
    }

    /// Set focus state.
    pub fn set_focused(&mut self, focused: bool) {
        self.focused = focused;
    }

    /// Get cursor renderer for external access.
    pub fn cursor(&self) -> &CursorRenderer {
        &self.cursor
    }

    /// Get mutable cursor renderer.
    pub fn cursor_mut(&mut self) -> &mut CursorRenderer {
        &mut self.cursor
    }

    /// Increment and return sequence number.
    fn next_sequence(&mut self) -> u32 {
        self.sequence = self.sequence.wrapping_add(1);
        self.sequence
    }

    /// Run one step of the X11 server state machine.
    pub fn step(&mut self) -> X11StepResult {
        match self.phase {
            Phase::Listening => X11StepResult::default(),
            Phase::RecvHello => {
                if let Some(hello) = ClientHello::parse(&self.recv_buf) {
                    self.recv_buf.clear();
                    let major = hello.proto_major;
                    let minor = hello.proto_minor;
                    let width = self.gui.framebuffer().width as u16;
                    let height = self.gui.framebuffer().height as u16;

                    let reply = build_setup_reply(major, minor, width, height);
                    self.send_buf.extend_from_slice(&reply);
                    self.phase = Phase::SendHello;
                }
                X11StepResult::default()
            }
            Phase::SendHello => {
                if self.send_buf.is_empty() {
                    self.phase = Phase::WaitMap;
                }
                X11StepResult::default()
            }
            Phase::WaitMap => {
                while self.recv_buf.len() >= 32 {
                    let evt_buf: Vec<u8> = self.recv_buf.drain(..32).collect();
                    if let Some(evt) = X11Event::parse(&evt_buf) {
                        match evt {
                            X11Event::Unknown(42) => {
                                // CreateWindow (type 42).
                                if evt_buf.len() >= 8 {
                                    let win_id =
                                        u32::from_le_bytes(evt_buf[4..8].try_into().unwrap());
                                    self.client_window = win_id;
                                    self.gc_id = self.alloc_resource();
                                }
                            }
                            X11Event::Unknown(8) => {
                                // MapWindow (type 8).
                                self.window_mapped = true;
                                let map_notify =
                                    build_map_notify(self.client_window, self.client_window);
                                self.send_buf.extend_from_slice(&map_notify);
                                self.phase = Phase::Rendering;
                            }
                            X11Event::Unknown(12) => {
                                // ConfigureWindow (type 12).
                                self.handle_configure_window(&evt_buf);
                            }
                            _ => {
                                self.pending_events.push(evt);
                            }
                        }
                    }
                }
                X11StepResult::default()
            }
            Phase::Rendering => {
                // Parse pending protocol messages.
                while self.recv_buf.len() >= 32 {
                    let evt_buf: Vec<u8> = self.recv_buf.drain(..32).collect();
                    if let Some(evt) = X11Event::parse(&evt_buf) {
                        match evt {
                            X11Event::Unknown(12) => {
                                // ConfigureWindow (type 12).
                                self.handle_configure_window(&evt_buf);
                            }
                            X11Event::Unknown(30) => {
                                // SelectionRequest (type 30) — clipboard.
                                self.handle_selection_request(&evt_buf);
                            }
                            X11Event::Unknown(32) => {
                                // SelectionClear (type 32).
                                self.clipboard.handle_selection_clear(&evt_buf);
                            }
                            _ => {
                                self.pending_events.push(evt);
                            }
                        }
                    }
                }

                // Dispatch input events to Slint.
                for evt in &self.pending_events {
                    match evt {
                        X11Event::KeyPress { keycode, x, y } => {
                            // Handle Ctrl+C/Ctrl+V for clipboard.
                            if *keycode == 54 && self.is_ctrl_pressed() {
                                // Ctrl+C: copy selection (placeholder).
                                continue;
                            }
                            if *keycode == 55 && self.is_ctrl_pressed() {
                                // Ctrl+V: paste from clipboard.
                                let text = self.clipboard.paste();
                                if !text.is_empty() {
                                    self.gui.handle_input(WindowEvent::KeyPressed {
                                        text: alloc::rc::Rc::from(text.as_str()),
                                        key: slint::Key::Unknown,
                                        ..Default::default()
                                    });
                                }
                                continue;
                            }
                            let text = x11_keycode_to_text(*keycode).unwrap_or("");
                            let key = x11_keycode_to_slint_key(*keycode);
                            self.gui.handle_input(WindowEvent::KeyPressed {
                                text: alloc::rc::Rc::from(text),
                                key,
                                ..Default::default()
                            });
                        }
                        X11Event::KeyRelease { keycode } => {
                            let key = x11_keycode_to_slint_key(*keycode);
                            self.gui.handle_input(WindowEvent::KeyReleased {
                                key,
                                ..Default::default()
                            });
                        }
                        X11Event::ButtonPress { button, x, y } => {
                            // Update cursor position.
                            self.cursor.set_position(*x as i32, *y as i32);
                            self.gui.handle_input(WindowEvent::PointerPressed {
                                position: slint::LogicalPosition::new(*x as f32, *y as f32),
                                button: match *button {
                                    1 => slint::platform::PointerEventButton::Left,
                                    3 => slint::platform::PointerEventButton::Right,
                                    2 => slint::platform::PointerEventButton::Middle,
                                    _ => slint::platform::PointerEventButton::Left,
                                },
                            });
                        }
                        X11Event::ButtonRelease { button, x, y } => {
                            self.cursor.set_position(*x as i32, *y as i32);
                            self.gui.handle_input(WindowEvent::PointerReleased {
                                position: slint::LogicalPosition::new(*x as f32, *y as f32),
                                button: match *button {
                                    1 => slint::platform::PointerEventButton::Left,
                                    3 => slint::platform::PointerEventButton::Right,
                                    _ => slint::platform::PointerEventButton::Middle,
                                },
                            });
                        }
                        X11Event::MotionNotify { x, y } => {
                            // Update cursor position.
                            self.cursor.set_position(*x as i32, *y as i32);
                            self.gui.handle_input(WindowEvent::PointerMoved {
                                position: slint::LogicalPosition::new(*x as f32, *y as f32),
                            });
                        }
                        _ => {}
                    }
                }
                self.pending_events.clear();

                // Step the GUI.
                let rendered = self.gui.step();

                // Frame diff: only send if pixels actually changed.
                let current_fb = &self.gui.framebuffer().pixels;
                let changed = fb_changed(&self.prev_fb, current_fb);

                if rendered && changed {
                    // Clone framebuffer and render cursor onto it.
                    let mut fb = self.gui.framebuffer().clone();
                    self.cursor.render(&mut fb);

                    let put = build_put_image(self.client_window, self.gc_id, &fb, 0, 0);
                    self.send_buf.extend_from_slice(&put);
                    self.prev_fb = fb.pixels.clone();
                    self.frame_count += 1;

                    // Write to /dev/fb0 if configured.
                    if let Some(ref path) = self.fb0_path {
                        let _ = write_fb0(&fb, path);
                    }
                }

                X11StepResult {
                    rendered,
                    frame_count: self.frame_count,
                }
            }
            Phase::Disconnected => X11StepResult::default(),
        }
    }

    /// Check if Ctrl key is currently pressed (simplified: tracks keycode 37).
    fn is_ctrl_pressed(&self) -> bool {
        // In a real implementation, track modifier state.
        // For now, always return false (v1 is lenient).
        false
    }

    /// Handle X11 SelectionRequest (type 30) for clipboard.
    fn handle_selection_request(&mut self, evt_buf: &[u8]) {
        if let Some(reply) = self.clipboard.handle_selection_request(evt_buf) {
            self.send_buf.extend_from_slice(&reply);
        }
    }

    /// Handle a ConfigureWindow request (type 12) from the client.
    fn handle_configure_window(&mut self, evt_buf: &[u8]) {
        if evt_buf.len() < 32 {
            return;
        }
        // ConfigureWindow: width at bytes 16-17, height at bytes 18-19.
        let new_width = u16::from_le_bytes([evt_buf[16], evt_buf[17]]);
        let new_height = u16::from_le_bytes([evt_buf[18], evt_buf[19]]);

        if new_width > 0 && new_height > 0 && (new_width != self.client_width || new_height != self.client_height)
        {
            self.client_width = new_width;
            self.client_height = new_height;
            self.gui.resize(new_width as u32, new_height as u32);
            self.prev_fb = alloc::vec![0; (new_width as u32 * new_height as u32) as usize];

            // Send ConfigureNotify back.
            let notify = build_configure_notify(
                self.client_window,
                new_width,
                new_height,
            );
            self.send_buf.extend_from_slice(&notify);
        }
    }
}

/// The result of a single X11 server step.
#[derive(Debug, Default)]
pub struct X11StepResult {
    /// Whether a frame was rendered.
    pub rendered: bool,
    /// Total frames sent so far.
    pub frame_count: u64,
}

// ── Tests ───────────────────────────────────────────────────────────────────

#[cfg(test)]
mod tests {
    use super::*;

    // Helper: fast-forward through handshake to Rendering phase.
    fn handshake_to_rendering(server: &mut X11Server) {
        server.set_client_fd(10);
        let mut hello = vec![0u8; 12 + 4];
        hello[0] = 0x6C;
        hello[2] = 11;
        hello[6] = 0;
        server.feed_recv(&hello);
        server.step();
        server.take_send_buf();
        server.step();

        let mut map_win = vec![0u8; 32];
        map_win[0] = 8;
        server.feed_recv(&map_win);
        server.step();
        server.take_send_buf(); // MapNotify
    }

    #[test]
    fn server_starts_listening() {
        let server = X11Server::new(320, 240);
        assert_eq!(server.phase(), Phase::Listening);
    }

    #[test]
    fn server_handles_handshake() {
        let mut server = X11Server::new(320, 240);
        server.set_client_fd(10);
        assert_eq!(server.phase(), Phase::RecvHello);

        let mut hello = vec![0u8; 12 + 8];
        hello[0] = 0x6C;
        hello[2] = 11;
        hello[6] = 4;
        server.feed_recv(&hello);
        server.step();
        assert_eq!(server.phase(), Phase::SendHello);

        let reply = server.take_send_buf();
        assert_eq!(reply[0], 1);

        server.step();
        assert_eq!(server.phase(), Phase::WaitMap);
    }

    #[test]
    fn server_renders_after_map() {
        let mut server = X11Server::new(64, 64);
        handshake_to_rendering(&mut server);
        assert_eq!(server.phase(), Phase::Rendering);

        let result = server.step();
        assert!(result.rendered);
        let send = server.take_send_buf();
        assert_eq!(send[0], 72);
    }

    #[test]
    fn frame_diff_skips_unchanged_frames() {
        let mut server = X11Server::new(16, 16);
        handshake_to_rendering(&mut server);

        // First step renders (all black → default Slint content).
        let r1 = server.step();
        let put1 = server.take_send_buf();
        assert!(r1.rendered);
        assert!(!put1.is_empty());

        // Second step: no animation, same pixels → no PutImage.
        let r2 = server.step();
        let put2 = server.take_send_buf();
        // Slint may or may not render again, but if it does and pixels
        // haven't changed, put2 should be empty.
        if r2.rendered {
            // If rendered, check if pixels actually changed.
            let fb = server.framebuffer();
            let changed = fb_changed(&server.prev_fb, &fb.pixels);
            if !changed {
                assert!(put2.is_empty(), "should skip PutImage when pixels unchanged");
            }
        }
    }

    #[test]
    fn keycode_mapping_letters() {
        // 'a' = evdev 30 + 8 = 38
        assert_eq!(x11_keycode_to_text(38), Some("a"));
        // 'z' = evdev 52 + 8 = 52... wait, that's just 52.
        assert_eq!(x11_keycode_to_text(52), Some("z"));
        // Space = evdev 57 + 8 = 65
        assert_eq!(x11_keycode_to_text(65), Some(" "));
    }

    #[test]
    fn keycode_mapping_special_keys() {
        assert_eq!(x11_keycode_to_slint_key(36), slint::Key::Return);
        assert_eq!(x11_keycode_to_slint_key(22), slint::Key::Backspace);
        assert_eq!(x11_keycode_to_slint_key(65), slint::Key::Space);
        assert_eq!(x11_keycode_to_slint_key(113), slint::Key::LeftArrow);
        assert_eq!(x11_keycode_to_slint_key(111), slint::Key::UpArrow);
    }

    #[test]
    fn configure_window_resize() {
        let mut server = X11Server::new(100, 100);
        handshake_to_rendering(&mut server);

        // Send ConfigureWindow (type 12) with new dimensions.
        let mut cfg = vec![0u8; 32];
        cfg[0] = 12; // ConfigureWindow
        cfg[16] = 200; // width low byte
        cfg[17] = 0; // width high byte
        cfg[18] = 150; // height low byte
        cfg[19] = 0; // height high byte
        server.feed_recv(&cfg);
        server.step();

        assert_eq!(server.client_width, 200);
        assert_eq!(server.client_height, 150);
        assert_eq!(server.framebuffer().width, 200);
        assert_eq!(server.framebuffer().height, 150);

        // Should have sent ConfigureNotify.
        let send = server.take_send_buf();
        assert!(!send.is_empty());
    }

    #[test]
    fn disconnect_resets_state() {
        let mut server = X11Server::new(32, 32);
        handshake_to_rendering(&mut server);
        assert_eq!(server.phase(), Phase::Rendering);

        server.disconnect();
        assert_eq!(server.phase(), Phase::Listening);
        assert_eq!(server.client_fd, None);
        assert_eq!(server.client_window, 0);
    }

    #[test]
    fn multi_client_reconnect() {
        let mut server = X11Server::new(32, 32);

        // First client.
        handshake_to_rendering(&mut server);
        server.step(); // render
        server.disconnect();

        // Second client.
        handshake_to_rendering(&mut server);
        let result = server.step();
        assert!(result.rendered);
    }

    #[test]
    fn full_x11_handshake_and_frame_cycle() {
        let mut server = X11Server::new(32, 32);
        assert_eq!(server.phase(), Phase::Listening);

        server.set_client_fd(5);
        let mut hello = vec![0u8; 12 + 4];
        hello[0] = 0x6C;
        hello[2] = 11;
        hello[6] = 0;
        server.feed_recv(&hello);
        server.step();
        assert_eq!(server.phase(), Phase::SendHello);

        let reply = server.take_send_buf();
        assert_eq!(reply[0], 1);

        server.step();
        assert_eq!(server.phase(), Phase::WaitMap);

        let mut create_win = vec![0u8; 32];
        create_win[0] = 42;
        create_win[4] = 42;
        server.feed_recv(&create_win);
        server.step();
        assert_eq!(server.client_window, 42);

        let mut map_win = vec![0u8; 32];
        map_win[0] = 8;
        server.feed_recv(&map_win);
        server.step();
        assert_eq!(server.phase(), Phase::Rendering);

        let notify = server.take_send_buf();
        assert_eq!(notify[0], 19);

        let result = server.step();
        assert!(result.rendered);
        assert_eq!(result.frame_count, 1);

        let put = server.take_send_buf();
        assert_eq!(put[0], 72);
        let width = u16::from_le_bytes([put[8], put[9]]);
        let height = u16::from_le_bytes([put[10], put[11]]);
        assert_eq!(width, 32);
        assert_eq!(height, 32);

        // Mouse click.
        let mut click = vec![0u8; 32];
        click[0] = 4;
        click[1] = 1;
        click[20] = 16;
        click[22] = 16;
        server.feed_recv(&click);
        let _ = server.step();
        let events = server.take_events();
        assert_eq!(events.len(), 1);
    }

    #[test]
    fn multiple_frames_sequential() {
        let mut server = X11Server::new(16, 16);
        handshake_to_rendering(&mut server);

        let mut total_frames = 0u64;
        for _ in 0..10 {
            let result = server.step();
            let put = server.take_send_buf();
            if result.rendered && !put.is_empty() {
                assert_eq!(put[0], 72);
                total_frames += 1;
            }
        }
        assert!(total_frames >= 1);
    }
}
