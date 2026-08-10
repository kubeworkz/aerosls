/* arch/arm64/uart_pl011.h — M4a PL011 console driver prototypes. */
#ifndef ARCH_ARM64_UART_PL011_H
#define ARCH_ARM64_UART_PL011_H

void uart_init(void);
void uart_putc(char c);
void uart_puts(const char *s);

#endif /* ARCH_ARM64_UART_PL011_H */
