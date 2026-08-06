/*
 * node_reset.c — reset this node's machine.
 *
 * ─── Why this exists ───────────────────────────────────────────────────────
 * Several kernel states are unrecoverable without a machine reset, and until
 * now the only way out was SSH plus `pm2 restart`. The one that forced the
 * issue is the guest-paging defect
 * (docs/AeroSLS-QEMU-SLS-Guest-Paging-Reset-Defect-v0.1.md): once a guest
 * enables paging, the guest window's identity mappings are gone, nothing
 * restores them, and every later guest launch is refused. The node is up,
 * serving HTTP, healthy by every other measure, and permanently unable to run
 * a guest.
 *
 * ─── What it does, and what it deliberately does not ───────────────────────
 * NO CHECKPOINT IS TAKEN. Everything not already persisted is lost.
 *
 * That is the deliberate choice, not an oversight. The reason to reset is
 * usually that the node is in a state nobody trusts, and checkpointing a state
 * nobody trusts writes that state to NVMe where the next boot restores it. An
 * operator who wants a checkpoint can run `checkpoint` first and see it
 * succeed before resetting -- two explicit steps, each of which can be
 * observed, rather than one that quietly does something consequential.
 *
 * ─── Mechanism ─────────────────────────────────────────────────────────────
 * Two methods, tried in order, because neither is universal:
 *
 *   0xCF9 -- the PCI/ACPI reset control register. QEMU implements it for both
 *            i440fx and q35, which is what this project's nodes run under.
 *            Writing 0x02 (system reset select) then 0x06 (reset) is the
 *            conventional sequence.
 *
 *   0x64  -- the 8042 keyboard controller's pulse-reset line, command 0xFE.
 *            Older and more widely emulated. Kept as the fallback because
 *            0xCF9 is a no-op on some chipsets rather than an error, so a
 *            single method can fail silently.
 *
 * If both fail the CPU is halted rather than returned to. Returning would drop
 * back into an HTTP handler on a node the operator has already been told is
 * restarting, which is a worse outcome than a node that is visibly down.
 */
#include <stdint.h>
#include "kernel_io.h"

/* This file's own outb, matching the convention in kernel_io.c and console.c
 * -- each keeps a local copy rather than exporting one, so port access is
 * always visible in the file performing it. */
static inline void nr_outb(uint16_t port, uint8_t val) {
    __asm__ volatile("outb %0, %1" : : "a"(val), "Nd"(port));
}

static inline void nr_io_wait(void) {
    /* Port 0x80 is the POST diagnostic port: writing it costs roughly one
     * bus cycle and is the standard way to let a slow controller settle. */
    __asm__ volatile("outb %%al, $0x80" : : "a"((uint8_t)0));
}

void sls_node_reset(void) {
    kernel_serial_print(
        "\n[NODE-RESET] Resetting this node NOW. Nothing has been "
        "checkpointed -- anything\n"
        "[NODE-RESET] not already on NVMe is gone. If that is not what you "
        "wanted, the order\n"
        "[NODE-RESET] is `checkpoint` first, confirm it succeeded, then "
        "reset.\n");

    /* Give the serial line a moment to drain. Without this the message above
     * is frequently lost -- the reset lands mid-transmission and the operator
     * sees a node that vanished without explanation, which is precisely the
     * failure this project keeps finding in other forms. */
    for (volatile int i = 0; i < 4000000; i++) { }

    /* Method 1: PCI/ACPI reset control. */
    nr_outb(0xCF9, 0x02);
    nr_io_wait();
    nr_outb(0xCF9, 0x06);
    nr_io_wait();

    /* Method 2: 8042 pulse reset. Reached only if 0xCF9 was a no-op. */
    for (volatile int i = 0; i < 100000; i++) { }
    nr_outb(0x64, 0xFE);
    nr_io_wait();

    /* Neither took. Halt rather than return: the caller has already responded
     * "restarting" to a client, and a node that quietly carries on after that
     * is lying about its own state. */
    kernel_serial_print(
        "[NODE-RESET] Both reset methods returned without resetting. Halting "
        "-- a node that\n"
        "[NODE-RESET] carries on here would be contradicting what it already "
        "told its caller.\n");
    for (;;) __asm__ volatile("cli; hlt");
}
