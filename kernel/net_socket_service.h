#ifndef NET_SOCKET_SERVICE_H
#define NET_SOCKET_SERVICE_H

#include <stdint.h>

/* net_socket_service.h — POSIX-Environments P2, increments 1, 2 and 3.
 *
 * INCREMENT 1 (the wire) is what this file was born with: the tenant's
 * manifest gains a Chan cap whose peer is the KERNEL-OWNED name
 * "kernel.net.socket" — never `drv.network.0`, the system partition's
 * network sidecar, because pointing a tenant at that would cross the LPAR
 * Phase 11 IPC boundary (E2's scoped registry refuses to wire a peer that
 * is not registered in the caller's own partition; the P2_TOOTH=system-peer
 * tooth is exactly that refusal). cap_create_sidecar() calls
 * net_socket_service_register() the moment it mints the kernel end of the
 * channel, exactly as it calls env_console_register() for
 * "kernel.env.console" — one shape, copied deliberately (v0.2 §6).
 *
 * INCREMENT 2 (the outbound path, attributed and admitted at the connect)
 * is what replaces increment 1's blanket refusal. The service now implements
 * the client verbs §6 scopes — socket, connect, send/sendto, recv/recvfrom,
 * shutdown, close — against the KERNEL's own stack (net/tcp.c), never
 * drv.network.0. The load-bearing change is the ADMIT PATH: before a
 * connection exists, the service runs tcp_conn_attribute_partition(conn_id,
 * partition) and the caller's partition quota (net/tcp_quota.h). An outbound
 * connection with no partition counts against nobody, so attribution is not
 * optional decoration — it is the admission. The default stays 0 = unlimited
 * (the module's BSS-zero-safe rule), so behaviour is byte-identical for any
 * operator who has not opted a partition in.
 *
 * ─── What admits and what is still refused, by name ───────────────────────
 * Admits:  NET_SOCKET, NET_BIND, NET_LISTEN, NET_ACCEPT (increment 3's
 *          listener — non-blocking, the accepted conn attributed at the
 *          instant it is handed out), NET_CONNECT (attributed),
 *          NET_SEND/NET_SENDTO, NET_RECV/NET_RECVFROM, NET_SHUTDOWN,
 *          NET_CLOSE_SOCK.
 * Refuses by name: every verb outside the eleven-name table, and the
 *          malformed-argument arms of an admitted verb (a listen() on an
 *          unbound socket, an accept() on a non-listener). An accept() with
 *          nothing pending answers NET_EAGAIN (11), a STATUS not a refusal —
 *          the client's bounded retry loop polls again rather than erroring.
 *          A refusal is still a NET_FLAG_ERROR reply whose payload carries
 *          the rendered reason (net_socket_refusal_text), the same text
 *          logged to serial naming the caller. "Refused by name" was
 *          increment 1's whole story; in increment 3 it is what remains for
 *          the types no increment defines, so a tenant is never left
 *          wondering whether a verb is absent or broken.
 *
 * The isolation property (§6) is structural, not a check: tcp_conns[] is a
 * single kernel-global pool, so a listener on port P only ever matches an
 * inbound SYN the stack already delivered to port P. A cross-partition
 * socket is not representable at this wire.
 *
 * The NET_INFO handshake stays honest and changes meaning with the
 * increment: max_sockets was 0 (nothing admits) and is now the real cap on
 * sockets a single channel may open — the handshake is how a client learns
 * the ceiling before it spends a socket. */

/* Channels with a live socket-service registration. M1 targets four
 * concurrent environments; this is headroom, not a design limit — the same
 * arithmetic env_console.h makes for ENV_CONSOLE_MAX. */
#define NET_SOCKET_SERVICE_MAX 8

/* Sockets a single channel may hold open at once. M1 targets four
 * concurrent environments; this bounds sockets PER environment, not
 * across them — headroom, not a design limit, the same arithmetic
 * env_console.h makes for ENV_CONSOLE_MAX. */
#define NSS_MAX_SOCKS 8u

/* cap.c calls this the moment it mints the kernel end of a
 * "kernel.net.socket" peer for sidecar `pid` (named `name`) in `partition`.
 * A re-registration from the same (partition, pid) REPLACES the entry
 * rather than leaking it (E5's recycle), the same rule
 * env_console_register() applies to a re-created environment's index.
 * Returns 1 on registration, 0 when the registry is full — the caller
 * must log that, because an unregistered socket channel is one nobody
 * will ever answer. The registration is also what carries the caller's
 * PARTITION into the service — the identity every later connect is
 * attributed to, so a channel registered without it is refused, not
 * silently un-attributed. */
int net_socket_service_register(uint16_t k_rd, uint16_t k_wr,
                                uint32_t partition, uint32_t pid,
                                const char* name);

/* console_service_tick() asks this before draining a kernel-held CHAN_R
 * slot: this channel carries NET_* frames, not console text, and printing
 * it to the serial transcript would both leak a tenant's socket traffic
 * into the shared log and steal the messages the service must answer. The
 * same exclusion shape as env_console_kernel_slot(). */
int net_socket_service_kernel_slot(uint16_t slot);

/* Drain every registered channel: answer NET_INFO, admit the client verbs
 * (socket/bind/listen/accept/connect/send/recv/shutdown/close) against the
 * kernel stack with the connect attributed and quota-checked, accept non-
 * blocking with the accepted conn attributed at hand-out, refuse the not-
 * yet-defined verbs by name, retire a channel whose peer is gone (closing
 * its sockets and listeners and releasing their attributions first).
 * Called from microkernel_service_poll() beside env_console_tick() — the
 * same non-IRQ kernel context that tick runs in, so, as there, no lock
 * guards the registry and the drain buffer is a single static. */
void net_socket_service_tick(void);

/* The wire's verb names — the refusal's "by name". Returns a static
 * string ("NET_SOCKET", "NET_CONNECT", …) matching user/proto's NET_*
 * constants, or 0 for a type outside the table. */
const char* net_socket_verb_name(uint16_t ty);

/* Renders the refusal for a verb this increment does not admit:
 * "<VERB> refused by kernel.net.socket — <reason>", or
 * "verb <n> refused by …" for a type this table does not know. Writes at
 * most `cap` bytes INCLUDING the NUL and returns the length written
 * (excluding the NUL) — always terminated, truncated if it must be, the
 * contract env_ckpt_refusal_text() keeps for the same reason (an
 * unterminated renderer corrupts the serial transcript). This ONE renderer
 * feeds the reply payload, the tick's serial line, and the guard's live
 * clause: one rendering, three callers, no drift. */
uint32_t net_socket_refusal_text(uint16_t ty, char* out, uint32_t cap);

/* Introspection for the host test. Beyond the registry counts, increment 2
 * adds the admission counters a quota clause and the guard's attribution
 * clause read: how many connects were ADMITTED (each one attributed to its
 * partition before the connection existed) and how many were REFUSED for
 * being over their partition's quota. A connect refused for a non-quota
 * reason (bad address, stack failure) is neither — it is an error, not an
 * admission decision, and counting it here would blur the two. */
uint32_t net_socket_service_count(void);
uint32_t net_socket_refusals(void);
uint32_t net_socket_dropped(void);   /* frames too short or not NET_* at all */
uint32_t net_socket_admits(void);    /* connects admitted, each attributed */
uint32_t net_socket_quota_refusals(void); /* connects refused: over quota */

#endif /* NET_SOCKET_SERVICE_H */
