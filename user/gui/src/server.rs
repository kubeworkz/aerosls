//! X11 server applet for the AeroSLS GUI.
//!
//! Listens on a Unix socket (e.g. `/tmp/.X11-unix/X0`), accepts X11 clients,
//! performs the connection handshake, and sends `PutImage` frames from the
//! Slint software renderer.
//!
//! This is designed to be used as a sidecar applet step function, driven by
//! the cooperative `Step::Yield` scheduler.

extern crate alloc;

use alloc::vec;
use alloc::vec::Vec;
use core::cell::RefCell;

use slint::platform::WindowEvent;

use crate::x11::{
    build_map_notify, build_put_image, build_setup_reply, ClientHello, X11Event,
};
use crate::{Framebuffer, GuiState};

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
        }
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

    /// Run one step of the X11 server state machine.
    ///
    /// Returns the number of bytes to write to the client (if any),
    /// and whether the server produced a frame.
    pub fn step(&mut self) -> X11StepResult {
        match self.phase {
            Phase::Listening => {
                // Nothing to do — the caller handles accept.
                X11StepResult::default()
            }
            Phase::RecvHello => {
                // Try to parse a ClientHello from the recv buffer.
                if let Some(hello) = ClientHello::parse(&self.recv_buf) {
                    self.recv_buf.clear();
                    let major = hello.proto_major;
                    let minor = hello.proto_minor;
                    let width = self.gui.framebuffer().width as u16;
                    let height = self.gui.framebuffer().height as u16;

                    // Send the setup reply.
                    let reply = build_setup_reply(major, minor, width, height);
                    self.send_buf.extend_from_slice(&reply);
                    self.phase = Phase::SendHello;
                }
                X11StepResult::default()
            }
            Phase::SendHello => {
                // The caller drains send_buf. Move to WaitMap.
                if self.send_buf.is_empty() {
                    self.phase = Phase::WaitMap;
                }
                X11StepResult::default()
            }
            Phase::WaitMap => {
                // Parse 32-byte events from the recv buffer.
                while self.recv_buf.len() >= 32 {
                    let evt_buf: Vec<u8> = self.recv_buf.drain(..32).collect();
                    if let Some(evt) = X11Event::parse(&evt_buf) {
                        match evt {
                            X11Event::Unknown(42) => {
                                // CreateWindow request (type 42).
                                // Extract window ID from bytes 4-7.
                                if evt_buf.len() >= 8 {
                                    let win_id =
                                        u32::from_le_bytes(evt_buf[4..8].try_into().unwrap());
                                    self.client_window = win_id;
                                    // Allocate a GC.
                                    self.gc_id = self.alloc_resource();
                                }
                            }
                            X11Event::Unknown(8) => {
                                // MapWindow request (type 8).
                                self.window_mapped = true;
                                // Send MapNotify event.
                                let map_notify =
                                    build_map_notify(self.client_window, self.client_window);
                                self.send_buf.extend_from_slice(&map_notify);
                                // Transition to rendering.
                                self.phase = Phase::Rendering;
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
                // Parse any pending input events.
                while self.recv_buf.len() >= 32 {
                    let evt_buf: Vec<u8> = self.recv_buf.drain(..32).collect();
                    if let Some(evt) = X11Event::parse(&evt_buf) {
                        self.pending_events.push(evt);
                    }
                }

                // Dispatch input events to Slint.
                for evt in &self.pending_events {
                    match evt {
                        X11Event::KeyPress { keycode, x, y } => {
                            self.gui.handle_input(WindowEvent::KeyPressed {
                                text: alloc::string::String::new().into(),
                                // Map X11 keycode to Slint key code.
                                ..Default::default()
                            });
                        }
                        X11Event::ButtonPress { button, x, y } => {
                            self.gui.handle_input(WindowEvent::PointerPressed {
                                position: slint::LogicalPosition::new(*x as f32, *y as f32),
                                button: if *button == 1 {
                                    slint::platform::PointerEventButton::Left
                                } else if *button == 3 {
                                    slint::platform::PointerEventButton::Right
                                } else {
                                    slint::platform::PointerEventButton::Middle
                                },
                            });
                        }
                        X11Event::ButtonRelease { button, x, y } => {
                            self.gui.handle_input(WindowEvent::PointerReleased {
                                position: slint::LogicalPosition::new(*x as f32, *y as f32),
                                button: if *button == 1 {
                                    slint::platform::PointerEventButton::Left
                                } else {
                                    slint::platform::PointerEventButton::Middle
                                },
                            });
                        }
                        X11Event::MotionNotify { x, y } => {
                            self.gui.handle_input(WindowEvent::PointerMoved {
                                position: slint::LogicalPosition::new(*x as f32, *y as f32),
                            });
                        }
                        _ => {}
                    }
                }
                self.pending_events.clear();

                // Step the GUI (update timers, render if needed).
                let rendered = self.gui.step();

                if rendered {
                    // Send a PutImage with the new frame.
                    let fb = self.gui.framebuffer().clone();
                    let put = build_put_image(self.client_window, self.gc_id, &fb, 0, 0);
                    self.send_buf.extend_from_slice(&put);
                    self.frame_count += 1;
                }

                X11StepResult {
                    rendered,
                    frame_count: self.frame_count,
                }
            }
            Phase::Disconnected => X11StepResult::default(),
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

    #[test]
    fn server_starts_listening() {
        let server = X11Server::new(320, 240);
        assert_eq!(server.phase(), Phase::Listening);
    }

    #[test]
    fn server_handles_handshake() {
        let mut server = X11Server::new(320, 240);

        // Simulate accept.
        server.set_client_fd(10);
        assert_eq!(server.phase(), Phase::RecvHello);

        // Send a ClientHello.
        let mut hello = vec![0u8; 12 + 8];
        hello[0] = 0x6C; // 'l' — LSB first
        hello[2] = 11; // proto major
        hello[3] = 0;
        hello[4] = 0; // proto minor
        hello[5] = 0;
        hello[6] = 4; // auth name len
        hello[7] = 0;

        server.feed_recv(&hello);
        server.step();
        assert_eq!(server.phase(), Phase::SendHello);

        // Drain the send buffer (setup reply).
        let reply = server.take_send_buf();
        assert!(!reply.is_empty());
        assert_eq!(reply[0], 1); // success

        // After send buffer is drained, move to WaitMap.
        server.step();
        assert_eq!(server.phase(), Phase::WaitMap);
    }

    #[test]
    fn server_renders_after_map() {
        let mut server = X11Server::new(64, 64);

        // Fast-forward through handshake.
        server.set_client_fd(10);
        let mut hello = vec![0u8; 12 + 8];
        hello[0] = 0x6C;
        hello[2] = 11;
        hello[6] = 4;
        server.feed_recv(&hello);
        server.step();
        server.take_send_buf();
        server.step();

        // Send a MapWindow request (type 8).
        let mut map_req = vec![0u8; 32];
        map_req[0] = 8; // MapWindow
        server.feed_recv(&map_req);
        server.step();

        assert_eq!(server.phase(), Phase::Rendering);

        // Step should produce a PutImage frame.
        let result = server.step();
        assert!(result.rendered);
        assert!(result.frame_count >= 1);

        let send = server.take_send_buf();
        // Should contain PutImage (type 72).
        assert!(!send.is_empty());
        assert_eq!(send[0], 72);
    }

    #[test]
    fn frame_buffer_size_matches() {
        let server = X11Server::new(100, 50);
        let fb = server.framebuffer();
        assert_eq!(fb.width, 100);
        assert_eq!(fb.height, 50);
        assert_eq!(fb.pixels.len(), 5000);
    }
}
