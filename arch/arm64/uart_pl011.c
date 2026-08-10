/* arch/arm64/uart_pl011.c — M4a: the qemu -M virt PL011 console.
 *
 * UART0 lives at 0x09000000 in the virt MMIO region. M4a needs TX only
 * (the boot banner + PSCI farewell), polled — no interrupts, no RX, no
 * FIFO management. QEMU's PL011 resets already enabled at 8n1, so
 * uart_init() only guarantees TXE|UARTEN on silicon that resets
 * differently; the divisor/line-control registers are left at the
 * model's working defaults (a documented M4a scope cut, not a driver
 * gap — baud programming is irrelevant to the serial-log assertions). */
#include <stdint.h>

#define PL011_BASE      0x09000000UL
#define PL011_DR        (*(volatile uint32_t *)(PL011_BASE + 0x000))
#define PL011_FR        (*(volatile uint32_t *)(PL011_BASE + 0x018))
#define PL011_CR        (*(volatile uint32_t *)(PL011_BASE + 0x030))

#define PL011_FR_TXFF   (1u << 5)   /* TX FIFO full */
#define PL011_CR_UARTEN (1u << 0)
#define PL011_CR_TXE    (1u << 8)

void uart_init(void)
{
    PL011_CR = PL011_CR_UARTEN | PL011_CR_TXE;
}

void uart_putc(char c)
{
    while (PL011_FR & PL011_FR_TXFF)
        ;
    PL011_DR = (uint32_t)(unsigned char)c;
}

void uart_puts(const char *s)
{
    while (*s)
        uart_putc(*s++);
}
