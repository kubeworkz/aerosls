//! Window manager for the AeroSLS GUI.
//!
//! Provides:
//! - Window decorations (title bar, close/minimize/maximize buttons)
//! - Drag-and-drop for window movement
//! - Multi-window support (multiple Slint windows in one server)
//! - Double buffering and vsync simulation

extern crate alloc;

use alloc::string::{String, ToString};
use alloc::vec;
use alloc::vec::Vec;
use core::cell::RefCell;

use crate::Framebuffer;

// ── Constants ───────────────────────────────────────────────────────────────

/// Default title bar height in pixels.
pub const TITLE_BAR_HEIGHT: u32 = 24;

/// Default button size in pixels.
pub const BUTTON_SIZE: u32 = 16;

/// Default button padding from edges.
pub const BUTTON_PADDING: u32 = 4;

/// Title bar colors.
pub const TITLE_BG: u32 = 0xFF_2B5797; // Blue
pub const TITLE_TEXT: u32 = 0xFF_FFFFFF; // White
pub const TITLE_INACTIVE_BG: u32 = 0xFF_808080; // Gray

/// Button colors.
pub const BTN_CLOSE_BG: u32 = 0xFF_E81123; // Red
pub const BTN_MINIMIZE_BG: u32 = 0xFF_FF B900; // Yellow
pub const BTN_MAXIMIZE_BG: u32 = 0xFF_60CD6B; // Green
pub const BTN_HOVER_BG: u32 = 0xFF_FFFFFF; // White on hover

/// Border colors.
pub const BORDER_ACTIVE: u32 = 0xFF_0078D7; // Blue
pub const BORDER_INACTIVE: u32 = 0xFF_A0A0A0; // Gray

/// Resize handle size.
pub const RESIZE_HANDLE: u32 = 8;

// ── Window Button ───────────────────────────────────────────────────────────

/// Window control button type.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum WindowButton {
    Close,
    Minimize,
    Maximize,
}

/// Window button state.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ButtonState {
    Normal,
    Hovered,
    Pressed,
}

/// A window control button (close, minimize, maximize).
pub struct WindowButtonRenderer {
    button: WindowButton,
    state: ButtonState,
    x: u32,
    y: u32,
}

impl WindowButtonRenderer {
    pub fn new(button: WindowButton, x: u32, y: u32) -> Self {
        WindowButtonRenderer {
            button,
            state: ButtonState::Normal,
            x,
            y,
        }
    }

    /// Set button state.
    pub fn set_state(&mut self, state: ButtonState) {
        self.state = state;
    }

    /// Check if a point is inside the button.
    pub fn contains(&self, px: u32, py: u32) -> bool {
        px >= self.x && px < self.x + BUTTON_SIZE && py >= self.y && py < self.y + BUTTON_SIZE
    }

    /// Render the button onto the framebuffer.
    pub fn render(&self, fb: &mut Framebuffer) {
        let bg = match self.state {
            ButtonState::Normal => match self.button {
                WindowButton::Close => BTN_CLOSE_BG,
                WindowButton::Minimize => BTN_MINIMIZE_BG,
                WindowButton::Maximize => BTN_MAXIMIZE_BG,
            },
            ButtonState::Hovered | ButtonState::Pressed => BTN_HOVER_BG,
        };

        // Draw button background.
        fb.fill_rect(self.x, self.y, BUTTON_SIZE, BUTTON_SIZE, bg);

        // Draw button icon.
        let cx = self.x + BUTTON_SIZE / 2;
        let cy = self.y + BUTTON_SIZE / 2;
        let icon_color = 0xFF_000000; // Black icon

        match self.button {
            WindowButton::Close => {
                // X icon.
                for i in 0..4 {
                    fb.pixels[((cy - 2 + i) * fb.width + cx - 2 + i) as usize] = icon_color;
                    fb.pixels[((cy - 2 + i) * fb.width + cx + 2 - i) as usize] = icon_color;
                    fb.pixels[((cy + 2 - i) * fb.width + cx - 2 + i) as usize] = icon_color;
                    fb.pixels[((cy + 2 - i) * fb.width + cx + 2 - i) as usize] = icon_color;
                }
            }
            WindowButton::Minimize => {
                // Horizontal line.
                for i in -3..=3 {
                    fb.pixels[(cy * fb.width + cx + i) as usize] = icon_color;
                }
            }
            WindowButton::Maximize => {
                // Square outline.
                for i in -3..=3 {
                    fb.pixels[((cy - 3) * fb.width + cx + i) as usize] = icon_color;
                    fb.pixels[((cy + 3) * fb.width + cx + i) as usize] = icon_color;
                    fb.pixels[((cy + i) * fb.width + cx - 3) as usize] = icon_color;
                    fb.pixels[((cy + i) * fb.width + cx + 3) as usize] = icon_color;
                }
            }
        }
    }
}

// ── Title Bar ───────────────────────────────────────────────────────────────

/// Title bar renderer with buttons.
pub struct TitleBar {
    title: String,
    focused: bool,
    close_btn: WindowButtonRenderer,
    minimize_btn: WindowButtonRenderer,
    maximize_btn: WindowButtonRenderer,
    drag_start: Option<(i32, i32)>,
    window_pos: (i32, i32),
}

impl TitleBar {
    pub fn new(title: &str, window_x: i32, window_y: i32) -> Self {
        let btn_y = (TITLE_BAR_HEIGHT - BUTTON_SIZE) / 2;
        let btn_x_start = 100; // Buttons on the right side.

        TitleBar {
            title: title.to_string(),
            focused: true,
            close_btn: WindowButtonRenderer::new(
                WindowButton::Close,
                btn_x_start + BUTTON_SIZE * 2 + BUTTON_PADDING * 2,
                btn_y,
            ),
            minimize_btn: WindowButtonRenderer::new(
                WindowButton::Minimize,
                btn_x_start + BUTTON_SIZE + BUTTON_PADDING,
                btn_y,
            ),
            maximize_btn: WindowButtonRenderer::new(
                WindowButton::Maximize,
                btn_x_start,
                btn_y,
            ),
            drag_start: None,
            window_pos: (window_x, window_y),
        }
    }

    /// Set window title.
    pub fn set_title(&mut self, title: &str) {
        self.title = title.to_string();
    }

    /// Set focus state.
    pub fn set_focused(&mut self, focused: bool) {
        self.focused = focused;
    }

    /// Get window position.
    pub fn window_position(&self) -> (i32, i32) {
        self.window_pos
    }

    /// Handle mouse press on title bar.
    pub fn handle_press(&mut self, x: u32, y: u32) -> Option<TitleBarAction> {
        if y >= TITLE_BAR_HEIGHT {
            return None;
        }

        // Check buttons.
        if self.close_btn.contains(x, y) {
            self.close_btn.set_state(ButtonState::Pressed);
            return Some(TitleBarAction::Close);
        }
        if self.minimize_btn.contains(x, y) {
            self.minimize_btn.set_state(ButtonState::Pressed);
            return Some(TitleBarAction::Minimize);
        }
        if self.maximize_btn.contains(x, y) {
            self.maximize_btn.set_state(ButtonState::Pressed);
            return Some(TitleBarAction::Maximize);
        }

        // Start drag.
        self.drag_start = Some((x as i32 + self.window_pos.0, y as i32 + self.window_pos.1));
        Some(TitleBarAction::DragStart)
    }

    /// Handle mouse motion on title bar.
    pub fn handle_motion(&mut self, x: u32, y: u32) -> Option<TitleBarAction> {
        // Update button hover states.
        self.close_btn.set_state(if self.close_btn.contains(x, y) {
            ButtonState::Hovered
        } else {
            ButtonState::Normal
        });
        self.minimize_btn.set_state(if self.minimize_btn.contains(x, y) {
            ButtonState::Hovered
        } else {
            ButtonState::Normal
        });
        self.maximize_btn.set_state(if self.maximize_btn.contains(x, y) {
            ButtonState::Hovered
        } else {
            ButtonState::Normal
        });

        // Handle drag.
        if let Some(start) = self.drag_start {
            let new_x = x as i32 + self.window_pos.0 - start.0;
            let new_y = y as i32 + self.window_pos.1 - start.1;
            self.window_pos = (new_x, new_y);
            return Some(TitleBarAction::DragMove(new_x, new_y));
        }

        None
    }

    /// Handle mouse release on title bar.
    pub fn handle_release(&mut self) -> Option<TitleBarAction> {
        self.drag_start = None;
        self.close_btn.set_state(ButtonState::Normal);
        self.minimize_btn.set_state(ButtonState::Normal);
        self.maximize_btn.set_state(ButtonState::Normal);
        None
    }

    /// Render the title bar onto the framebuffer.
    pub fn render(&self, fb: &mut Framebuffer, offset_x: u32, offset_y: u32) {
        // Title bar background.
        let bg = if self.focused { TITLE_BG } else { TITLE_INACTIVE_BG };
        fb.fill_rect(offset_x, offset_y, fb.width, TITLE_BAR_HEIGHT, bg);

        // Title text (simple bitmap font).
        let text_x = offset_x + 10;
        let text_y = offset_y + 6;
        render_text(fb, text_x, text_y, &self.title, TITLE_TEXT);

        // Buttons.
        self.close_btn.render(fb);
        self.minimize_btn.render(fb);
        self.maximize_btn.render(fb);
    }
}

/// Actions from title bar interactions.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum TitleBarAction {
    Close,
    Minimize,
    Maximize,
    DragStart,
    DragMove(i32, i32),
}

// ── Window ──────────────────────────────────────────────────────────────────

/// Window state.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum WindowState {
    Normal,
    Minimized,
    Maximized,
}

/// A managed window with decorations.
pub struct ManagedWindow {
    /// Window ID.
    pub id: u32,
    /// Window title.
    pub title: String,
    /// Window position.
    pub x: i32,
    pub y: i32,
    /// Window dimensions (excluding title bar).
    pub width: u32,
    pub height: u32,
    /// Window state.
    pub state: WindowState,
    /// Whether window has focus.
    pub focused: bool,
    /// Title bar renderer.
    title_bar: TitleBar,
    /// Content framebuffer (below title bar).
    content: Framebuffer,
    /// Z-order (higher = on top).
    z_order: u32,
}

impl ManagedWindow {
    pub fn new(id: u32, title: &str, x: i32, y: i32, width: u32, height: u32) -> Self {
        let title_bar = TitleBar::new(title, x, y);

        ManagedWindow {
            id,
            title: title.to_string(),
            x,
            y,
            width,
            height,
            state: WindowState::Normal,
            focused: true,
            title_bar,
            content: Framebuffer::new(width, height),
            z_order: 0,
        }
    }

    /// Get total height including title bar.
    pub fn total_height(&self) -> u32 {
        match self.state {
            WindowState::Minimized => TITLE_BAR_HEIGHT,
            _ => TITLE_BAR_HEIGHT + self.height,
        }
    }

    /// Get total width.
    pub fn total_width(&self) -> u32 {
        self.width
    }

    /// Handle mouse press.
    pub fn handle_press(&mut self, x: u32, y: u32) -> Option<WindowAction> {
        let abs_x = x as i32 - self.x;
        let abs_y = y as i32 - self.y;

        if abs_x < 0 || abs_y < 0 {
            return None;
        }

        let abs_x = abs_x as u32;
        let abs_y = abs_y as u32;

        // Title bar interaction.
        if abs_y < TITLE_BAR_HEIGHT {
            if let Some(action) = self.title_bar.handle_press(abs_x, abs_y) {
                return Some(match action {
                    TitleBarAction::Close => WindowAction::Close(self.id),
                    TitleBarAction::Minimize => {
                        self.state = WindowState::Minimized;
                        WindowAction::Minimize(self.id)
                    }
                    TitleBarAction::Maximize => {
                        self.state = match self.state {
                            WindowState::Maximized => WindowState::Normal,
                            _ => WindowState::Maximized,
                        };
                        WindowAction::Maximize(self.id)
                    }
                    TitleBarAction::DragStart => WindowAction::DragStart(self.id),
                    TitleBarAction::DragMove(x, y) => WindowAction::DragMove(self.id, x, y),
                });
            }
        }

        // Content area interaction.
        Some(WindowAction::ContentPress(self.id, abs_x, abs_y - TITLE_BAR_HEIGHT))
    }

    /// Handle mouse motion.
    pub fn handle_motion(&mut self, x: u32, y: u32) -> Option<WindowAction> {
        let abs_x = x as i32 - self.x;
        let abs_y = y as i32 - self.y;

        if abs_x < 0 || abs_y < 0 {
            return None;
        }

        let abs_x = abs_x as u32;
        let abs_y = abs_y as u32;

        if abs_y < TITLE_BAR_HEIGHT {
            if let Some(action) = self.title_bar.handle_motion(abs_x, abs_y) {
                return Some(match action {
                    TitleBarAction::DragMove(new_x, new_y) => {
                        self.x = new_x;
                        self.y = new_y;
                        self.title_bar.window_pos = (new_x, new_y);
                        WindowAction::DragMove(self.id, new_x, new_y)
                    }
                    _ => WindowAction::None,
                });
            }
        }

        None
    }

    /// Handle mouse release.
    pub fn handle_release(&mut self) -> Option<WindowAction> {
        self.title_bar.handle_release();
        None
    }

    /// Get content framebuffer for writing.
    pub fn content_mut(&mut self) -> &mut Framebuffer {
        &mut self.content
    }

    /// Get content framebuffer for reading.
    pub fn content(&self) -> &Framebuffer {
        &self.content
    }

    /// Render window to a composite framebuffer.
    pub fn render(&self, fb: &mut Framebuffer) {
        let offset_x = self.x.max(0) as u32;
        let offset_y = self.y.max(0) as u32;

        // Render title bar.
        self.title_bar.render(fb, offset_x, offset_y);

        // Render border.
        let border_color = if self.focused { BORDER_ACTIVE } else { BORDER_INACTIVE };
        let total_w = self.total_width();
        let total_h = self.total_height();

        // Top border.
        fb.fill_rect(offset_x, offset_y, total_w, 1, border_color);
        // Bottom border.
        fb.fill_rect(offset_x, offset_y + total_h - 1, total_w, 1, border_color);
        // Left border.
        fb.fill_rect(offset_x, offset_y, 1, total_h, border_color);
        // Right border.
        fb.fill_rect(offset_x + total_w - 1, offset_y, 1, total_h, border_color);

        // Render content.
        if self.state != WindowState::Minimized {
            let content_y = offset_y + TITLE_BAR_HEIGHT;
            for row in 0..self.height {
                let src_start = (row * self.width) as usize;
                let src_end = ((row + 1) * self.width) as usize;
                let dst_y = content_y + row;
                if dst_y >= fb.height {
                    break;
                }
                let dst_start = (dst_y * fb.width + offset_x) as usize;
                let dst_end = dst_start + self.width as usize;
                if dst_end <= fb.pixels.len() && src_end <= self.content.pixels.len() {
                    fb.pixels[dst_start..dst_end]
                        .copy_from_slice(&self.content.pixels[src_start..src_end]);
                }
            }
        }
    }
}

/// Actions from window interactions.
#[derive(Debug, Clone, PartialEq)]
pub enum WindowAction {
    None,
    Close(u32),
    Minimize(u32),
    Maximize(u32),
    DragStart(u32),
    DragMove(u32, i32, i32),
    ContentPress(u32, u32, u32),
}

// ── Multi-Window Manager ────────────────────────────────────────────────────

/// Manages multiple windows with z-ordering and focus.
pub struct WindowManager {
    windows: Vec<ManagedWindow>,
    next_id: u32,
    focused_id: Option<u32>,
    drag_window: Option<u32>,
    /// Composite framebuffer for the entire desktop.
    desktop: Framebuffer,
}

impl WindowManager {
    pub fn new(width: u32, height: u32) -> Self {
        WindowManager {
            windows: Vec::new(),
            next_id: 1,
            focused_id: None,
            drag_window: None,
            desktop: Framebuffer::new(width, height),
        }
    }

    /// Create a new window.
    pub fn create_window(&mut self, title: &str, x: i32, y: i32, width: u32, height: u32) -> u32 {
        let id = self.next_id;
        self.next_id += 1;

        let mut win = ManagedWindow::new(id, title, x, y, width, height);
        win.z_order = self.windows.len() as u32;
        win.focused = true;

        // Unfocus other windows.
        for w in &mut self.windows {
            w.focused = false;
        }

        self.focused_id = Some(id);
        self.windows.push(win);
        id
    }

    /// Close a window.
    pub fn close_window(&mut self, id: u32) {
        self.windows.retain(|w| w.id != id);
        if self.focused_id == Some(id) {
            self.focused_id = self.windows.last().map(|w| w.id);
            if let Some(fid) = self.focused_id {
                if let Some(w) = self.windows.iter_mut().find(|w| w.id == fid) {
                    w.focused = true;
                }
            }
        }
    }

    /// Get a window by ID.
    pub fn get_window(&self, id: u32) -> Option<&ManagedWindow> {
        self.windows.iter().find(|w| w.id == id)
    }

    /// Get a mutable window by ID.
    pub fn get_window_mut(&mut self, id: u32) -> Option<&mut ManagedWindow> {
        self.windows.iter_mut().find(|w| w.id == id)
    }

    /// Get all windows.
    pub fn windows(&self) -> &[ManagedWindow] {
        &self.windows
    }

    /// Get focused window ID.
    pub fn focused_id(&self) -> Option<u32> {
        self.focused_id
    }

    /// Handle mouse press at desktop coordinates.
    pub fn handle_press(&mut self, x: u32, y: u32) -> Option<WindowAction> {
        // Find topmost window under cursor (highest z-order).
        let mut hit_id = None;
        for win in self.windows.iter().rev() {
            let wx = win.x as u32;
            let wy = win.y as u32;
            let ww = win.total_width();
            let wh = win.total_height();

            if x >= wx && x < wx + ww && y >= wy && y < wy + wh {
                hit_id = Some(win.id);
                break;
            }
        }

        if let Some(id) = hit_id {
            // Focus this window.
            self.focus_window(id);

            if let Some(win) = self.windows.iter_mut().find(|w| w.id == id) {
                return win.handle_press(x, y);
            }
        }

        None
    }

    /// Handle mouse motion.
    pub fn handle_motion(&mut self, x: u32, y: u32) -> Option<WindowAction> {
        // If dragging, update the drag window.
        if let Some(drag_id) = self.drag_window {
            if let Some(win) = self.windows.iter_mut().find(|w| w.id == drag_id) {
                return win.handle_motion(x, y);
            }
        }

        // Update hover states on all windows.
        for win in &mut self.windows {
            if let Some(action) = win.handle_motion(x, y) {
                return Some(action);
            }
        }

        None
    }

    /// Handle mouse release.
    pub fn handle_release(&mut self) -> Option<WindowAction> {
        self.drag_window = None;

        for win in &mut self.windows {
            if let Some(action) = win.handle_release() {
                return Some(action);
            }
        }

        None
    }

    /// Focus a window.
    pub fn focus_window(&mut self, id: u32) {
        for w in &mut self.windows {
            w.focused = w.id == id;
        }
        self.focused_id = Some(id);

        // Bring to front.
        if let Some(pos) = self.windows.iter().position(|w| w.id == id) {
            let max_z = self.windows.iter().map(|w| w.z_order).max().unwrap_or(0);
            self.windows[pos].z_order = max_z + 1;
        }
    }

    /// Start dragging a window.
    pub fn start_drag(&mut self, id: u32) {
        self.drag_window = Some(id);
    }

    /// Render the desktop (composite all windows).
    pub fn render(&mut self) -> &Framebuffer {
        // Clear desktop.
        self.desktop.fill(0xFF_1A1A2E); // Dark blue background

        // Sort windows by z-order.
        let mut indices: Vec<usize> = (0..self.windows.len()).collect();
        indices.sort_by_key(|&i| self.windows[i].z_order);

        // Render windows in z-order.
        for &i in &indices {
            self.windows[i].render(&mut self.desktop);
        }

        &self.desktop
    }

    /// Get desktop framebuffer.
    pub fn desktop(&self) -> &Framebuffer {
        &self.desktop
    }
}

// ── Simple Bitmap Font ──────────────────────────────────────────────────────

/// Render text using a simple 5x7 bitmap font.
pub fn render_text(fb: &mut Framebuffer, x: u32, y: u32, text: &str, color: u32) {
    // Simple ASCII bitmap font (5x7 per character).
    const FONT: &[[u8; 5]] = &[
        // Space (32)
        [0x00, 0x00, 0x00, 0x00, 0x00],
        // ! (33)
        [0x04, 0x04, 0x04, 0x04, 0x04],
        // " (34)
        [0x0A, 0x0A, 0x00, 0x00, 0x00],
        // # (35)
        [0x0A, 0x1F, 0x0A, 0x1F, 0x0A],
        // $ (36)
        [0x0E, 0x15, 0x0E, 0x14, 0x0F],
        // % (37)
        [0x03, 0x02, 0x04, 0x08, 0x13],
        // & (38)
        [0x06, 0x09, 0x06, 0x12, 0x0D],
        // ' (39)
        [0x04, 0x04, 0x00, 0x00, 0x00],
        // ( (40)
        [0x02, 0x04, 0x08, 0x08, 0x04],
        // ) (41)
        [0x08, 0x04, 0x02, 0x02, 0x04],
        // * (42)
        [0x00, 0x0A, 0x04, 0x0A, 0x00],
        // + (43)
        [0x00, 0x04, 0x0E, 0x04, 0x00],
        // , (44)
        [0x00, 0x00, 0x00, 0x04, 0x08],
        // - (45)
        [0x00, 0x00, 0x0E, 0x00, 0x00],
        // . (46)
        [0x00, 0x00, 0x00, 0x00, 0x04],
        // / (47)
        [0x02, 0x02, 0x04, 0x08, 0x08],
        // 0-9 (48-57)
        [0x0E, 0x11, 0x13, 0x15, 0x0E], // 0
        [0x04, 0x0C, 0x04, 0x04, 0x0E], // 1
        [0x0E, 0x11, 0x02, 0x04, 0x1F], // 2
        [0x1F, 0x02, 0x04, 0x02, 0x1F], // 3
        [0x02, 0x06, 0x0A, 0x1F, 0x02], // 4
        [0x1F, 0x10, 0x1E, 0x01, 0x1E], // 5
        [0x06, 0x08, 0x1E, 0x11, 0x0E], // 6
        [0x1F, 0x02, 0x04, 0x08, 0x10], // 7
        [0x0E, 0x11, 0x0E, 0x11, 0x0E], // 8
        [0x0E, 0x11, 0x0F, 0x01, 0x0E], // 9
        // : (58)
        [0x00, 0x04, 0x00, 0x04, 0x00],
        // ; (59)
        [0x00, 0x04, 0x00, 0x04, 0x08],
        // < (60)
        [0x02, 0x04, 0x08, 0x04, 0x02],
        // = (61)
        [0x00, 0x0E, 0x00, 0x0E, 0x00],
        // > (62)
        [0x08, 0x04, 0x02, 0x04, 0x08],
        // ? (63)
        [0x0E, 0x11, 0x02, 0x00, 0x02],
        // @ (64)
        [0x0E, 0x11, 0x15, 0x16, 0x0C],
        // A-Z (65-90)
        [0x0E, 0x11, 0x1F, 0x11, 0x11], // A
        [0x1E, 0x11, 0x1E, 0x11, 0x1E], // B
        [0x0E, 0x11, 0x10, 0x11, 0x0E], // C
        [0x1E, 0x11, 0x11, 0x11, 0x1E], // D
        [0x1F, 0x10, 0x1E, 0x10, 0x1F], // E
        [0x1F, 0x10, 0x1E, 0x10, 0x10], // F
        [0x0E, 0x11, 0x13, 0x11, 0x0F], // G
        [0x11, 0x11, 0x1F, 0x11, 0x11], // H
        [0x0E, 0x04, 0x04, 0x04, 0x0E], // I
        [0x01, 0x01, 0x01, 0x11, 0x0E], // J
        [0x11, 0x12, 0x1C, 0x12, 0x11], // K
        [0x10, 0x10, 0x10, 0x10, 0x1F], // L
        [0x11, 0x1B, 0x15, 0x11, 0x11], // M
        [0x11, 0x19, 0x15, 0x13, 0x11], // N
        [0x0E, 0x11, 0x11, 0x11, 0x0E], // O
        [0x1E, 0x11, 0x1E, 0x10, 0x10], // P
        [0x0E, 0x11, 0x15, 0x12, 0x0D], // Q
        [0x1E, 0x11, 0x1E, 0x12, 0x11], // R
        [0x0E, 0x10, 0x0E, 0x01, 0x0E], // S
        [0x1F, 0x04, 0x04, 0x04, 0x04], // T
        [0x11, 0x11, 0x11, 0x11, 0x0E], // U
        [0x11, 0x11, 0x11, 0x0A, 0x04], // V
        [0x11, 0x11, 0x15, 0x1B, 0x11], // W
        [0x11, 0x0A, 0x04, 0x0A, 0x11], // X
        [0x11, 0x0A, 0x04, 0x04, 0x04], // Y
        [0x1F, 0x02, 0x04, 0x08, 0x1F], // Z
    ];

    let mut char_x = x;
    for ch in text.chars() {
        let idx = ch as usize;
        if idx >= 32 && idx < 32 + FONT.len() {
            let glyph = &FONT[idx - 32];
            for (row, &bits) in glyph.iter().enumerate() {
                for bit in 0..5 {
                    if bits & (1 << (4 - bit)) != 0 {
                        let px = char_x + bit;
                        let py = y + row as u32;
                        if px < fb.width && py < fb.height {
                            fb.pixels[(py * fb.width + px) as usize] = color;
                        }
                    }
                }
            }
        }
        char_x += 6; // 5 pixels + 1 spacing
    }
}

// ── Double Buffering ────────────────────────────────────────────────────────

/// Double-buffered framebuffer for tear-free rendering.
pub struct DoubleBuffer {
    front: Framebuffer,
    back: Framebuffer,
    dirty: bool,
}

impl DoubleBuffer {
    pub fn new(width: u32, height: u32) -> Self {
        DoubleBuffer {
            front: Framebuffer::new(width, height),
            back: Framebuffer::new(width, height),
            dirty: false,
        }
    }

    /// Get the back buffer for writing.
    pub fn back_mut(&mut self) -> &mut Framebuffer {
        self.dirty = true;
        &mut self.back
    }

    /// Get the back buffer for reading.
    pub fn back(&self) -> &Framebuffer {
        &self.back
    }

    /// Get the front buffer for display.
    pub fn front(&self) -> &Framebuffer {
        &self.front
    }

    /// Swap front and back buffers (vsync).
    pub fn swap(&mut self) {
        core::mem::swap(&mut self.front, &mut self.back);
        self.dirty = false;
    }

    /// Check if back buffer has been modified since last swap.
    pub fn is_dirty(&self) -> bool {
        self.dirty
    }

    /// Get width.
    pub fn width(&self) -> u32 {
        self.front.width
    }

    /// Get height.
    pub fn height(&self) -> u32 {
        self.front.height
    }
}

// ── Vsync Simulation ────────────────────────────────────────────────────────

/// Vsync simulator for tear-free rendering.
pub struct VsyncSimulator {
    /// Target frames per second.
    fps: u32,
    /// Frame counter.
    frame_count: u64,
    /// Last frame timestamp.
    last_frame: u64,
    /// Frame interval in ticks.
    frame_interval: u64,
}

impl VsyncSimulator {
    pub fn new(fps: u32) -> Self {
        // Assume 1 tick = 16ms (60fps base).
        let frame_interval = if fps > 0 { 60 / fps as u64 } else { 1 };
        VsyncSimulator {
            fps,
            frame_count: 0,
            last_frame: 0,
            frame_interval,
        }
    }

    /// Check if it's time for the next frame.
    pub fn should_render(&self, current_tick: u64) -> bool {
        current_tick >= self.last_frame + self.frame_interval
    }

    /// Mark a frame as rendered.
    pub fn mark_frame(&mut self, current_tick: u64) {
        self.last_frame = current_tick;
        self.frame_count += 1;
    }

    /// Get frame count.
    pub fn frame_count(&self) -> u64 {
        self.frame_count
    }

    /// Get FPS.
    pub fn fps(&self) -> u32 {
        self.fps
    }
}

// ── Tests ───────────────────────────────────────────────────────────────────

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn title_bar_creation() {
        let tb = TitleBar::new("Test Window", 100, 50);
        assert_eq!(tb.title, "Test Window");
        assert!(tb.focused);
    }

    #[test]
    fn title_bar_button_contains() {
        let close = WindowButtonRenderer::new(WindowButton::Close, 100, 4);
        assert!(close.contains(100, 4));
        assert!(close.contains(115, 19));
        assert!(!close.contains(120, 4));
    }

    #[test]
    fn managed_window_creation() {
        let win = ManagedWindow::new(1, "App", 50, 50, 400, 300);
        assert_eq!(win.id, 1);
        assert_eq!(win.title, "App");
        assert_eq!(win.total_height(), TITLE_BAR_HEIGHT + 300);
        assert_eq!(win.state, WindowState::Normal);
    }

    #[test]
    fn managed_window_minimize() {
        let mut win = ManagedWindow::new(1, "App", 50, 50, 400, 300);
        win.state = WindowState::Minimized;
        assert_eq!(win.total_height(), TITLE_BAR_HEIGHT);
    }

    #[test]
    fn window_manager_create_close() {
        let mut wm = WindowManager::new(1024, 768);
        let id1 = wm.create_window("Window 1", 10, 10, 400, 300);
        let id2 = wm.create_window("Window 2", 50, 50, 400, 300);
        assert_eq!(wm.windows().len(), 2);
        assert_eq!(wm.focused_id(), Some(id2));

        wm.close_window(id1);
        assert_eq!(wm.windows().len(), 1);
        assert_eq!(wm.focused_id(), Some(id2));
    }

    #[test]
    fn window_manager_focus() {
        let mut wm = WindowManager::new(1024, 768);
        let id1 = wm.create_window("Window 1", 10, 10, 400, 300);
        let id2 = wm.create_window("Window 2", 50, 50, 400, 300);

        wm.focus_window(id1);
        assert_eq!(wm.focused_id(), Some(id1));
        assert!(wm.get_window(id1).unwrap().focused);
        assert!(!wm.get_window(id2).unwrap().focused);
    }

    #[test]
    fn double_buffer_swap() {
        let mut db = DoubleBuffer::new(100, 100);
        assert!(!db.is_dirty());

        // Write to back buffer.
        db.back_mut().fill(0xFF_FF0000);
        assert!(db.is_dirty());

        // Swap.
        db.swap();
        assert!(!db.is_dirty());
        assert_eq!(db.front().pixels[0], 0xFF_FF0000);
    }

    #[test]
    fn vsync_simulator() {
        let mut vsync = VsyncSimulator::new(60);
        assert_eq!(vsync.fps(), 60);
        assert_eq!(vsync.frame_count(), 0);

        assert!(vsync.should_render(0));
        vsync.mark_frame(0);
        assert_eq!(vsync.frame_count(), 1);

        // Not enough time for next frame.
        assert!(!vsync.should_render(0));
        assert!(vsync.should_render(1));
    }

    #[test]
    fn render_text_basic() {
        let mut fb = Framebuffer::new(100, 20);
        render_text(&mut fb, 5, 5, "Hi", 0xFF_FFFFFF);

        // Check that some pixels were set.
        let non_black = fb.non_black_count();
        assert!(non_black > 0, "text should render pixels");
    }

    #[test]
    fn desktop_render() {
        let mut wm = WindowManager::new(800, 600);
        wm.create_window("App", 100, 100, 400, 300);

        let fb = wm.render();
        assert_eq!(fb.width, 800);
        assert_eq!(fb.height, 600);

        // Should have non-black pixels (window content).
        assert!(fb.non_black_count() > 0);
    }
}
