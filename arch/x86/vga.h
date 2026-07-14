#ifndef VGA_H
#define VGA_H

// ─── VGA Text-Mode HMI Driver ─────────────────────────────────────────────────
// 80×25 colour text mode, buffer at physical 0xB8000.
// Identity-mapped during early boot (covered by p2_table_0, first 1 GiB).

void vga_init(void);          // Clear screen, enable cursor, mark driver ready
void vga_putchar(char c);     // Write one character (handles \n \r \t + scroll)
void vga_print(const char* s);// Write NUL-terminated string
int  vga_is_ready(void);      // Non-zero once vga_init() has completed

// Attribute byte helpers (fg in bits 3:0, bg in bits 6:4)
#define VGA_COLOR(fg, bg)   (((bg) << 4) | (fg))

// Standard CGA colours
#define VGA_BLACK          0
#define VGA_BLUE           1
#define VGA_GREEN          2
#define VGA_CYAN           3
#define VGA_RED            4
#define VGA_MAGENTA        5
#define VGA_BROWN          6
#define VGA_LIGHT_GREY     7
#define VGA_DARK_GREY      8
#define VGA_LIGHT_BLUE     9
#define VGA_LIGHT_GREEN    10
#define VGA_LIGHT_CYAN     11
#define VGA_LIGHT_RED      12
#define VGA_LIGHT_MAGENTA  13
#define VGA_YELLOW         14
#define VGA_WHITE          15

#endif /* VGA_H */
