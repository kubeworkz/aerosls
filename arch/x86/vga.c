// arch/x86/vga.c — 80×25 VGA text-mode HMI driver
//
// The VGA text buffer lives at physical 0xB8000.  The boot.asm identity-maps
// the first 4 GiB (p2_table_0 … p2_table_3) so this address is always valid
// by the time kernel_main() is entered.
//
// Each cell is a 16-bit word:
//   bits 7:0   — ASCII character
//   bits 11:8  — foreground colour (CGA palette)
//   bits 14:12 — background colour
//   bit  15    — blink (disabled here)

#include "vga.h"
#include <stdint.h>

// ─── Constants ───────────────────────────────────────────────────────────────
#define VGA_WIDTH       80
#define VGA_HEIGHT      25
#define VGA_BUF_PHYS    0xB8000UL

// Default attribute: bright green on black — classic terminal look
#define VGA_DEFAULT_ATTR  VGA_COLOR(VGA_LIGHT_GREEN, VGA_BLACK)

// CRTC index/data ports (colour mode)
#define VGA_CRTC_INDEX  0x3D4
#define VGA_CRTC_DATA   0x3D5

// ─── State ───────────────────────────────────────────────────────────────────
static volatile uint16_t* const vga_buf =
    (volatile uint16_t*)VGA_BUF_PHYS;

static int     vga_row   = 0;
static int     vga_col   = 0;
static uint8_t vga_attr  = VGA_DEFAULT_ATTR;
static int     vga_ready = 0;

// ─── Port I/O (local copies — vga.c is freestanding) ────────────────────────
static inline void vga_outb(uint16_t port, uint8_t val) {
    __asm__ volatile("outb %0, %1" : : "a"(val), "Nd"(port));
}

// ─── Hardware cursor ─────────────────────────────────────────────────────────
static void vga_update_cursor(void) {
    uint16_t pos = (uint16_t)(vga_row * VGA_WIDTH + vga_col);
    vga_outb(VGA_CRTC_INDEX, 0x0F);
    vga_outb(VGA_CRTC_DATA,  (uint8_t)(pos & 0xFF));
    vga_outb(VGA_CRTC_INDEX, 0x0E);
    vga_outb(VGA_CRTC_DATA,  (uint8_t)((pos >> 8) & 0xFF));
}

// Enable cursor: scanline 14–15 (thin underline)
static void vga_enable_cursor(void) {
    vga_outb(VGA_CRTC_INDEX, 0x0A);
    vga_outb(VGA_CRTC_DATA,  (uint8_t)((vga_outb(VGA_CRTC_INDEX, 0x0A), 0x0E)));
    vga_outb(VGA_CRTC_INDEX, 0x0B);
    vga_outb(VGA_CRTC_DATA,  0x0F);
}

// ─── Scroll up one line ───────────────────────────────────────────────────────
static void vga_scroll(void) {
    uint16_t blank = (uint16_t)(' ' | ((uint16_t)vga_attr << 8));
    for (int r = 0; r < VGA_HEIGHT - 1; r++)
        for (int c = 0; c < VGA_WIDTH; c++)
            vga_buf[r * VGA_WIDTH + c] = vga_buf[(r + 1) * VGA_WIDTH + c];
    for (int c = 0; c < VGA_WIDTH; c++)
        vga_buf[(VGA_HEIGHT - 1) * VGA_WIDTH + c] = blank;
    vga_row = VGA_HEIGHT - 1;
}

// ─── Public API ──────────────────────────────────────────────────────────────
void vga_init(void) {
    vga_attr = VGA_DEFAULT_ATTR;
    uint16_t blank = (uint16_t)(' ' | ((uint16_t)vga_attr << 8));
    for (int i = 0; i < VGA_WIDTH * VGA_HEIGHT; i++)
        vga_buf[i] = blank;
    vga_row = 0;
    vga_col = 0;
    vga_enable_cursor();
    vga_update_cursor();
    vga_ready = 1;
}

void vga_putchar(char c) {
    if (!vga_ready) return;

    if (c == '\n') {
        vga_col = 0;
        if (++vga_row >= VGA_HEIGHT) vga_scroll();
    } else if (c == '\r') {
        vga_col = 0;
    } else if (c == '\t') {
        vga_col = (vga_col + 8) & ~7;
        if (vga_col >= VGA_WIDTH) {
            vga_col = 0;
            if (++vga_row >= VGA_HEIGHT) vga_scroll();
        }
    } else {
        vga_buf[vga_row * VGA_WIDTH + vga_col] =
            (uint16_t)((uint8_t)c | ((uint16_t)vga_attr << 8));
        if (++vga_col >= VGA_WIDTH) {
            vga_col = 0;
            if (++vga_row >= VGA_HEIGHT) vga_scroll();
        }
    }
    vga_update_cursor();
}

void vga_print(const char* s) {
    while (*s) vga_putchar(*s++);
}

int vga_is_ready(void) {
    return vga_ready;
}
