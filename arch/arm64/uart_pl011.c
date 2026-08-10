/* arch/arm64/uart_pl011.c — M4a/M5: the qemu -M virt PL011 console.
 *
 * UART0 lives at physical 0x09000000 in the virt MMIO region. Since M5
 * the kernel is TTBR1-pure (plan doc §6 M5 / §10.191): the low half is
 * unmapped in the kernel, so the device is reached at its HIGH VA
 * (physical + KERNEL_VIRT_OFF), mapped by the boot tree's L3 device
 * page. The driver is TX-only, polled — no interrupts, no RX, no FIFO
 * management; QEMU's PL011 resets already enabled at 8n1, so
 * uart_init() only guarantees TXE|UARTEN on silicon that resets
 * differently (a documented M4a scope cut, not a driver gap). */
#include <stdint.h>

#define PL011_BASE      0xFFFF000009000000UL
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
