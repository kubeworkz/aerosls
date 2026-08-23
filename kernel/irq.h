/*
 * irq.h — Phase 4: IRQ delivery via channels.
 *
 * Translates hardware interrupts into fixed-format channel messages
 * (IRQMessage) delivered to driver sidecars. The driver's interrupt
 * handler is just its message-processing loop — no special handler
 * registration required.
 *
 * Design: docs/AeroSLS-Device-Driver-SDK-Phase4-Design-v0.1.md §3.
 *
 * Integration points:
 *   - Trap handlers (arch/{riscv,x86}) call irq_deliver() when a
 *     hardware interrupt fires.
 *   - cap_create_irq() calls irq_register() to bind an IRQ number
 *     to a CapObject for message delivery.
 *   - cap_revoke() calls irq_unregister() to unbind.
 *   - The PLIC driver provides strong overrides for mask/eoi/valid.
 */
#ifndef IRQ_H
#define IRQ_H

#include <stdint.h>

/* ─── Limits ────────────────────────────────────────────────────────────── */
#define IRQ_REGISTRY_MAX    128   /* max registered IRQ sources */
#define IRQ_PRIORITY Levels  8    /* priority levels (PLIC 0-7) */

/* ─── IRQ registry entry ──────────────────────────────────────────────────
 * One per registered IRQ cap. Maps a hardware interrupt number to the
 * CapObject that delivers messages to the driver's channel. */
struct IRQRegistryEntry {
    uint32_t irq_number;        /* PLIC source ID or MSI vector */
    uint32_t cap_obj_id;        /* CapObject id for this IRQ cap */
    uint32_t chan_id;            /* channel id for message delivery */
    uint8_t  masked;            /* 1 = masked (no messages delivered) */
    uint8_t  active;            /* 1 = entry is live */
    uint8_t  trigger;           /* edge/level, high/low */
    uint8_t  _pad;
    uint64_t coalesce_us;       /* minimum interval between messages */
    uint64_t last_deliver_us;   /* timestamp of last delivered message */
    uint32_t coalesce_count;    /* pending coalesced interrupt count */
    uint32_t sequence;          /* monotonic message sequence number */
    uint32_t dropped;           /* messages dropped (channel full) */
    uint32_t total_delivered;   /* lifetime delivered count */
    uint32_t total_coalesced;   /* lifetime coalesced count */
};

/* ─── Public API ────────────────────────────────────────────────────────── */

/* Boot-time init: zero the registry. */
void irq_init(void);

/* Register an IRQ source: bind irq_number to a CapObject and channel.
 * Called by cap_create_irq(). Returns 0 on success, -1 if registry full
 * or irq_number already registered. */
int irq_register(uint32_t irq_number, uint32_t cap_obj_id, uint32_t chan_id,
                 uint8_t trigger, uint64_t coalesce_us);

/* Unregister an IRQ source: unbind irq_number.
 * Called by cap_revoke() on IRQ caps. No-op if not registered. */
void irq_unregister(uint32_t irq_number);

/* Deliver a hardware interrupt as an IRQMessage to the registered channel.
 * Called by the trap handler when a hardware interrupt fires.
 * Handles coalescing, masking, sequence numbering, and channel delivery.
 * Returns 0 on success (message delivered or coalesced), -1 on error
 * (no registered cap, masked, etc). */
int irq_deliver(uint32_t irq_number);

/* Batch-delivery: deliver ALL pending (unmasked) IRQs for a given source.
 * Used by the PLIC claim loop that drains multiple interrupts. */
int irq_deliver_pending(void);

/* Mask/unmask at the registry level. Also programs the PLIC/APLIC. */
int irq_mask(uint32_t irq_number);
int irq_unmask(uint32_t irq_number);

/* Acknowledge (EOI) at the PLIC. */
void irq_eoi(uint32_t irq_number);

/* Validate an IRQ source number. */
int irq_source_valid(uint32_t irq_number);

/* Get the registry entry for an IRQ number (or NULL). */
const struct IRQRegistryEntry* irq_get_entry(uint32_t irq_number);

/* Get cumulative stats for an IRQ. */
void irq_stats(uint32_t irq_number, uint32_t* out_delivered,
               uint32_t* out_coalesced, uint32_t* out_dropped);

/* Debug: dump all registered IRQs. */
void irq_list(void);

/* Get current timestamp in microseconds (for coalescing).
 * Weak default reads a cycle counter; strong override in timer.c. */
uint64_t irq_timestamp_us(void);

#endif /* IRQ_H */
