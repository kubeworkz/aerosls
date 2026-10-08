#ifndef NET_SOCKET_SERVICE_H
#define NET_SOCKET_SERVICE_H

#include <stdint.h>

/* net_socket_service.h — POSIX-Environments P2, first increment: the
 * kernel-owned socket service behind a tenant manifest's `network` cap.
 *
 * The tenant's manifest gains a Chan cap whose peer is the KERNEL-OWNED
 * name "kernel.net.socket" — never `drv.network.0`, the system partition's
 * network sidecar, because pointing a tenant at that would cross the LPAR
 * Phase 11 IPC boundary (E2's scoped registry refuses to wire a peer that
 * is not registered in the caller's own partition; the P2_TOOTH=system-peer
 * tooth is exactly that refusal). cap_create_sidecar() calls
 * net_socket_service_register() the moment it mints the kernel end of the
 * channel, exactly as it calls env_console_register() for
 * "kernel.env.console" — one shape, copied deliberately (v0.2 §6).
 *
 * What the service does in THIS increment is the whole increment: it
 * speaks NET_* — parses the 16-byte NetFrame header (magic "AEROSNT\x01",
 * version 1), answers the NET_INFO handshake with an honest body — and
 * answers every other verb with a refusal BY NAME: a NET_FLAG_ERROR reply
 * whose payload carries the rendered refusal (net_socket_refusal_text),
 * the same text logged to serial naming the caller. Nothing admits yet;
 * the channel exists, is typed, and fails honestly, which is the wire the
 * rest of the phase writes into: increments two through four replace the
 * refusal with admission (attribution + quota), listeners, and measured
 * bounds — the wire itself does not move. */

/* Channels with a live socket-service registration. M1 targets four
 * concurrent environments; this is headroom, not a design limit — the same
 * arithmetic env_console.h makes for ENV_CONSOLE_MAX. */
#define NET_SOCKET_SERVICE_MAX 8

/* cap.c calls this the moment it mints the kernel end of a
 * "kernel.net.socket" peer for sidecar `pid` (named `name`) in `partition`.
 * A re-registration from the same (partition, pid) REPLACES the entry
 * rather than leaking it (E5's recycle), the same rule
 * env_console_register() applies to a re-created environment's index.
 * Returns 1 on registration, 0 when the registry is full — the caller
 * must log that, because an unregistered socket channel is one nobody
 * will ever answer. */
int net_socket_service_register(uint16_t k_rd, uint16_t k_wr,
                                uint32_t partition, uint32_t pid,
                                const char* name);

/* console_service_tick() asks this before draining a kernel-held CHAN_R
 * slot: this channel carries NET_* frames, not console text, and printing
 * it to the serial transcript would both leak a tenant's socket traffic
 * into the shared log and steal the messages the service must answer. The
 * same exclusion shape as env_console_kernel_slot(). */
int net_socket_service_kernel_slot(uint16_t slot);

/* Drain every registered channel: answer NET_INFO, refuse every other verb
 * by name, retire a channel whose peer is gone. Called from
 * microkernel_service_poll() beside env_console_tick() — the same non-IRQ
 * kernel context that tick runs in, so, as there, no lock guards the
 * registry and the drain buffer is a single static. */
void net_socket_service_tick(void);

/* The wire's verb names — the refusal's "by name". Returns a static
 * string ("NET_SOCKET", "NET_CONNECT", …) matching user/proto's NET_*
 * constants, or 0 for a type outside the table. */
const char* net_socket_verb_name(uint16_t ty);

/* Renders the refusal for one verb: "<VERB> refused by kernel.net.socket
 * — nothing admits yet (P2 increment 1)", or "verb <n> refused by …" for
 * a type this table does not know. Writes at most `cap` bytes INCLUDING
 * the NUL and returns the length written (excluding the NUL) — always
 * terminated, truncated if it must be, the contract
 * env_ckpt_refusal_text() keeps for the same reason (an unterminated
 * renderer corrupts the serial transcript). This ONE text is what the
 * host test pins, what the tick logs, and what the guard's live clause
 * greps: one rendering, three callers, no drift. */
uint32_t net_socket_refusal_text(uint16_t ty, char* out, uint32_t cap);

/* Introspection for the host test: registered channels, and the refusal
 * counter (the tick increments it once per refused verb — the count a
 * future increment's quota clause will build on). */
uint32_t net_socket_service_count(void);
uint32_t net_socket_refusals(void);
uint32_t net_socket_dropped(void);   /* frames too short or not NET_* at all */

#endif /* NET_SOCKET_SERVICE_H */
