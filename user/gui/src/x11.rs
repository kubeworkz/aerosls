//! Minimal X11 server protocol implementation for the AeroSLS GUI.
//!
//! Handles the X11 connection handshake (ClientHello/ServerHello) and sends
//! `PutImage` protocol messages to display framebuffer contents. This is a
//! *stub* — enough to make `x11rb`-style clients connect and receive frames.
//!
//! # Protocol Overview
//!
//! ```text
//! Client → Server: ClientHello (byte order, protocol version)
//! Server → Client: ServerHello (connection setup reply)
//! Client → Server: CreateWindow, MapWindow
//! Server → Client: MapNotify
//! Server → Client: PutImage (framebuffer data)
//! Client → Server: Input events (KeyPress, ButtonPress, MotionNotify)
//! ```

extern crate alloc;

use alloc::vec;
use alloc::vec::Vec;

use crate::Framebuffer;

// ── X11 Constants ───────────────────────────────────────────────────────────

/// X11 connection byte order.
const LSB_FIRST: u8 = 0x6C; // 'l' — little-endian (our ARM target)
const MSB_FIRST: u8 = 0x42; // 'B' — big-endian

// ── X11 ClientHello ─────────────────────────────────────────────────────────

/// Parse the X11 ClientHello message.
///
/// The ClientHello is: byte_order (1), unused (1), proto_major (2), proto_minor (2),
/// auth_name_len (2), unused (2), auth_data_len (2), unused (2)
#[derive(Debug)]
pub struct ClientHello {
    pub byte_order: u8,
    pub proto_major: u16,
    pub proto_minor: u16,
    pub auth_name: Vec<u8>,
    pub auth_data: Vec<u8>,
}

impl ClientHello {
    pub fn parse(buf: &[u8]) -> Option<Self> {
        if buf.len() < 12 {
            return None;
        }
        let byte_order = buf[0];
        let proto_major = u16::from_le_bytes([buf[2], buf[3]]);
        let proto_minor = u16::from_le_bytes([buf[4], buf[5]]);
        let auth_name_len = u16::from_le_bytes([buf[6], buf[7]]) as usize;
        let auth_data_len = u16::from_le_bytes([buf[10], buf[11]]) as usize;

        // Pad to 4-byte boundary for auth name.
        let auth_name_padded = (auth_name_len + 3) & !3;
        let total = 12 + auth_name_padded + auth_data_len;
        if buf.len() < total {
            return None;
        }
        let auth_name = buf[12..12 + auth_name_len].to_vec();
        let auth_data = buf[12 + auth_name_padded..12 + auth_name_padded + auth_data_len].to_vec();

        Some(ClientHello {
            byte_order,
            proto_major,
            proto_minor,
            auth_name,
            auth_data,
        })
    }

    pub fn is_le(&self) -> bool {
        self.byte_order == LSB_FIRST
    }
}

// ── X11 ServerHello (Connection Setup Reply) ────────────────────────────────

/// Build a minimal X11 Connection Setup Reply.
///
/// This tells the client about the server's capabilities: vendor, byte order,
/// supported extensions, root window details, etc.
pub fn build_setup_reply(
    proto_major: u16,
    proto_minor: u16,
    width: u16,
    height: u16,
) -> Vec<u8> {
    let mut reply = Vec::new();

    // Connection Setup Reply (Success = 1)
    reply.push(1); // success

    // Unused (1 byte).
    reply.push(0);

    // Protocol version (2 bytes).
    reply.extend_from_slice(&proto_major.to_le_bytes());
    reply.extend_from_slice(&proto_minor.to_le_bytes());

    // Release number (4 bytes).
    reply.extend_from_slice(&0u32.to_le_bytes());

    // Resource ID base (4 bytes).
    reply.extend_from_slice(&0u32.to_le_bytes());

    // Resource ID mask (4 bytes).
    reply.extend_from_slice(&0x001F_FFFFu32.to_le_bytes());

    // Motion buffer size (4 bytes).
    reply.extend_from_slice(&0u32.to_le_bytes());

    // Vendor length (2 bytes) + max request length (2 bytes).
    let vendor = b"AeroSLS";
    reply.extend_from_slice(&(vendor.len() as u16).to_le_bytes());
    reply.extend_from_slice(&1u16.to_le_bytes()); // max request length (in 4-byte units)

    // Screen formats (1 byte).
    reply.push(1); // 1 screen format

    // Image byte order (1 byte).
    reply.push(LSB_FIRST);

    // Bitmap bit order (1 byte).
    reply.push(LSB_FIRST);

    // Bitmap scanline unit (1 byte).
    reply.push(32);

    // Bitmap scanline pad (1 byte).
    reply.push(32);

    // Min keycode (1 byte).
    reply.push(8);

    // Max keycode (1 byte).
    reply.push(255);

    // Unused (19 bytes).
    reply.extend_from_slice(&[0u8; 19]);

    // Padding to 4-byte boundary.
    while reply.len() % 4 != 0 {
        reply.push(0);
    }

    // Vendor string (padded to 4 bytes).
    reply.extend_from_slice(vendor);
    while reply.len() % 4 != 0 {
        reply.push(0);
    }

    // ── Formats list ────────────────────────────────────────────────────────

    // Number of pixmap formats (1 byte).
    reply.push(1); // 1 format

    // Unused (3 bytes).
    reply.extend_from_slice(&[0u8; 3]);

    // Pixmap format: depth=24, bits_per_pixel=32, scanline_pad=32
    reply.push(24); // depth
    reply.push(32); // bits per pixel
    reply.push(32); // scanline pad
    reply.extend_from_slice(&[0u8; 5]); // unused

    // ── Screen list ─────────────────────────────────────────────────────────

    // Root window ID (4 bytes).
    reply.extend_from_slice(&1u32.to_le_bytes());

    // Default colormap (4 bytes).
    reply.extend_from_slice(&0u32.to_le_bytes());

    // White pixel (4 bytes).
    reply.extend_from_slice(&0x00FFFFFFu32.to_le_bytes());

    // Black pixel (4 bytes).
    reply.extend_from_slice(&0x00000000u32.to_le_bytes());

    // Current input masks (4 bytes) — key press, button press, pointer motion.
    reply.extend_from_slice(&0x0002_081Bu32.to_le_bytes());

    // Width (2 bytes).
    reply.extend_from_slice(&width.to_le_bytes());

    // Height (2 bytes).
    reply.extend_from_slice(&height.to_le_bytes());

    // Width/height in mm (2 bytes each).
    reply.extend_from_slice(&((width * 100 / 96) as u16).to_le_bytes());
    reply.extend_from_slice(&((height * 100 / 96) as u16).to_le_bytes());

    // Min installed maps (2 bytes) + max installed maps (2 bytes).
    reply.extend_from_slice(&1u16.to_le_bytes());
    reply.extend_from_slice(&1u16.to_le_bytes());

    // Root visual (4 bytes).
    reply.extend_from_slice(&0u32.to_le_bytes());

    // Backing stores (1 byte) — WhenMapped.
    reply.push(1);

    // Save-unders (1 byte) — false.
    reply.push(0);

    // Root depth (1 byte).
    reply.push(24);

    // Number of depths (1 byte).
    reply.push(1);

    // Depth 24:
    reply.push(24); // depth
    reply.push(0); // unused
    // Number of visuals (2 bytes).
    reply.extend_from_slice(&1u16.to_le_bytes());
    // Unused (2 bytes).
    reply.extend_from_slice(&[0u8; 2]);

    // Visual type: visual_id=0, class=DirectColor, bits_per_rgb=8.
    reply.extend_from_slice(&0u32.to_le_bytes()); // visual_id
    reply.push(1); // class: DirectColor
    reply.push(8); // bits per rgb value
    reply.extend_from_slice(&256u16.to_le_bytes()); // colormap entries
    reply.extend_from_slice(&0x00FF0000u32.to_le_bytes()); // red mask
    reply.extend_from_slice(&0x0000FF00u32.to_le_bytes()); // green mask
    reply.extend_from_slice(&0x000000FFu32.to_le_bytes()); // blue mask
    // Unused (4 bytes).
    reply.extend_from_slice(&[0u8; 4]);

    reply
}

// ── X11 PutImage Protocol ───────────────────────────────────────────────────

/// Build an X11 `PutImage` request (type 72) to send a framebuffer to the client.
///
/// This sends the entire framebuffer as a single `ZPixmap` image.
pub fn build_put_image(
    window_id: u32,
    gc_id: u32,
    fb: &Framebuffer,
    x: i16,
    y: i16,
) -> Vec<u8> {
    let format: u8 = 2; // ZPixmap
    let depth: u8 = 24;
    let bits_per_pixel: u8 = 32;
    let scanline_pad: u8 = 32;

    let width = fb.width as u16;
    let height = fb.height as u16;

    // Convert ARGB to BGRA for X11 (X11 expects BGRA in LSB_FIRST mode).
    let mut data = Vec::with_capacity(fb.pixels.len() * 4);
    for &pixel in &fb.pixels {
        let a = ((pixel >> 24) & 0xFF) as u8;
        let r = ((pixel >> 16) & 0xFF) as u8;
        let g = ((pixel >> 8) & 0xFF) as u8;
        let b = (pixel & 0xFF) as u8;
        data.push(b);
        data.push(g);
        data.push(r);
        data.push(a);
    }

    // X11 PutImage request: 28 bytes header + data.
    let data_len = data.len() as u32;
    let request_len = 28 + data_len;

    let mut pkt = Vec::with_capacity(32 + data_len as usize);

    // Request type (1 byte).
    pkt.push(72); // PutImage

    // Format (1 byte).
    pkt.push(format);

    // Request length (2 bytes, in 4-byte units).
    pkt.extend_from_slice(&((request_len / 4) as u16).to_le_bytes());

    // Drawable ID (4 bytes).
    pkt.extend_from_slice(&window_id.to_le_bytes());

    // GC ID (4 bytes).
    pkt.extend_from_slice(&gc_id.to_le_bytes());

    // Width (2 bytes).
    pkt.extend_from_slice(&width.to_le_bytes());

    // Height (2 bytes).
    pkt.extend_from_slice(&height.to_le_bytes());

    // dst-x (2 bytes, signed).
    pkt.extend_from_slice(&(x as u16).to_le_bytes());

    // dst-y (2 bytes, signed).
    pkt.extend_from_slice(&(y as u16).to_le_bytes());

    // left_pad (2 bytes).
    pkt.extend_from_slice(&0u16.to_le_bytes());

    // depth (1 byte).
    pkt.push(depth);

    // unused (1 byte).
    pkt.push(0);

    // Pixel data.
    pkt.extend_from_slice(&data);

    pkt
}

// ── X11 Events ──────────────────────────────────────────────────────────────

/// An X11 input event received from the client.
#[derive(Debug, Clone)]
pub enum X11Event {
    KeyPress { keycode: u8, x: i16, y: i16 },
    KeyRelease { keycode: u8 },
    ButtonPress { button: u8, x: i16, y: i16 },
    ButtonRelease { button: u8, x: i16, y: i16 },
    MotionNotify { x: i16, y: i16 },
    Unknown(u8),
}

impl X11Event {
    /// Parse a single X11 event (1 byte type + 31 bytes data = 32 bytes).
    pub fn parse(buf: &[u8]) -> Option<Self> {
        if buf.len() < 32 {
            return None;
        }
        let event_type = buf[0];
        match event_type {
            2 => {
                // KeyPress
                let keycode = buf[1];
                let x = i16::from_le_bytes([buf[20], buf[21]]);
                let y = i16::from_le_bytes([buf[22], buf[23]]);
                Some(X11Event::KeyPress { keycode, x, y })
            }
            3 => {
                // KeyRelease
                let keycode = buf[1];
                Some(X11Event::KeyRelease { keycode })
            }
            4 => {
                // ButtonPress
                let button = buf[1];
                let x = i16::from_le_bytes([buf[20], buf[21]]);
                let y = i16::from_le_bytes([buf[22], buf[23]]);
                Some(X11Event::ButtonPress { button, x, y })
            }
            5 => {
                // ButtonRelease
                let button = buf[1];
                let x = i16::from_le_bytes([buf[20], buf[21]]);
                let y = i16::from_le_bytes([buf[22], buf[23]]);
                Some(X11Event::ButtonRelease { button, x, y })
            }
            6 => {
                // MotionNotify
                let x = i16::from_le_bytes([buf[20], buf[21]]);
                let y = i16::from_le_bytes([buf[22], buf[23]]);
                Some(X11Event::MotionNotify { x, y })
            }
            _ => Some(X11Event::Unknown(event_type)),
        }
    }
}

/// Build an X11 `MapNotify` event (type 19) for the given window.
pub fn build_map_notify(window_id: u32, event_window: u32) -> Vec<u8> {
    let mut evt = Vec::with_capacity(32);
    evt.push(19); // MapNotify
    evt.push(0); // unused
    evt.extend_from_slice(&0u32.to_le_bytes()); // sequence number
    evt.extend_from_slice(&window_id.to_le_bytes()); // event window
    evt.extend_from_slice(&event_window.to_le_bytes()); // window
    evt.push(0); // override redirect
    evt.extend_from_slice(&[0u8; 22]); // unused
    evt
}

// ── Tests ───────────────────────────────────────────────────────────────────

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn parse_client_hello() {
        let mut buf = vec![0u8; 12 + 8]; // 12 header + 4-byte auth_name
        buf[0] = LSB_FIRST; // byte order
        buf[2] = 11; // proto major
        buf[3] = 0;
        buf[4] = 0; // proto minor
        buf[5] = 0;
        buf[6] = 4; // auth name len
        buf[7] = 0;

        let hello = ClientHello::parse(&buf).unwrap();
        assert!(hello.is_le());
        assert_eq!(hello.proto_major, 11);
    }

    #[test]
    fn setup_reply_has_required_fields() {
        let reply = build_setup_reply(11, 0, 800, 600);
        assert_eq!(reply[0], 1); // success
        let major = u16::from_le_bytes([reply[2], reply[3]]);
        let minor = u16::from_le_bytes([reply[4], reply[5]]);
        assert_eq!(major, 11);
        assert_eq!(minor, 0);
    }

    #[test]
    fn put_image_header_size() {
        let fb = Framebuffer::new(10, 10);
        let pkt = build_put_image(1, 2, &fb, 0, 0);
        // 28 bytes header + 10*10*4 bytes data = 428.
        assert_eq!(pkt.len(), 428);
        assert_eq!(pkt[0], 72); // PutImage
    }

    #[test]
    fn parse_key_press_event() {
        let mut buf = vec![0u8; 32];
        buf[0] = 2; // KeyPress
        buf[1] = 36; // keycode for Enter
        buf[20] = 100; // x low byte
        buf[21] = 0; // x high byte
        buf[22] = 50; // y low byte
        buf[23] = 0; // y high byte

        let evt = X11Event::parse(&buf).unwrap();
        match evt {
            X11Event::KeyPress { keycode, x, y } => {
                assert_eq!(keycode, 36);
                assert_eq!(x, 100);
                assert_eq!(y, 50);
            }
            _ => panic!("expected KeyPress"),
        }
    }

    #[test]
    fn build_map_notify_size() {
        let evt = build_map_notify(1, 1);
        assert_eq!(evt.len(), 32);
        assert_eq!(evt[0], 19); // MapNotify
    }

    #[test]
    fn put_image_pixel_conversion() {
        // 2x1 framebuffer with known ARGB pixels.
        let mut fb = Framebuffer::new(2, 1);
        fb.pixels[0] = 0xFF_FF0000; // red
        fb.pixels[1] = 0xFF_00FF00; // green

        let pkt = build_put_image(1, 2, &fb, 0, 0);
        // Header is 28 bytes, pixel data starts at byte 28.
        // X11 expects BGRA order in LSB_FIRST mode.
        // Red pixel: ARGB=0xFF_FF0000 → BGRA=[0x00, 0x00, 0xFF, 0xFF]
        assert_eq!(pkt[28], 0x00); // B
        assert_eq!(pkt[29], 0x00); // G
        assert_eq!(pkt[30], 0xFF); // R
        assert_eq!(pkt[31], 0xFF); // A
        // Green pixel: ARGB=0xFF_00FF00 → BGRA=[0x00, 0xFF, 0x00, 0xFF]
        assert_eq!(pkt[32], 0x00); // B
        assert_eq!(pkt[33], 0xFF); // G
        assert_eq!(pkt[34], 0x00); // R
        assert_eq!(pkt[35], 0xFF); // A
    }

    #[test]
    fn parse_motion_notify() {
        let mut buf = vec![0u8; 32];
        buf[0] = 6; // MotionNotify
        buf[20] = 50; // x
        buf[21] = 0;
        buf[22] = 75; // y
        buf[23] = 0;

        let evt = X11Event::parse(&buf).unwrap();
        match evt {
            X11Event::MotionNotify { x, y } => {
                assert_eq!(x, 50);
                assert_eq!(y, 75);
            }
            _ => panic!("expected MotionNotify"),
        }
    }
}
