#ifndef ENV_CONSOLE_H
#define ENV_CONSOLE_H

#include <stdint.h>

/* env_console.h — POSIX-Environments E6: per-environment consoles.
 *
 * Before E6 every sidecar's console was one channel to the kernel context:
 * the manifest cap named "kernel.debug.console", cap_create_sidecar() minted
 * the far end into pid 0, and console_service_tick() drained every kernel-held
 * console channel to the serial port. An environment's output was therefore
 * indistinguishable from the kernel's, and there was no way to send a byte to
 * one environment and not the others — G7, "an environment you cannot reach is
 * not an environment".
 *
 * E6 gives each tenant POSIX sidecar its OWN kernel-brokered channel pair. The
 * tenant manifest's console cap names "kernel.env.console"; cap_create_sidecar()
 * mints the far end into pid 0 exactly as before, and registers it HERE against
 * the environment the sidecar belongs to. The kernel end is then that
 * environment's console endpoint:
 *
 *   - output is buffered PER ENVIRONMENT (a drained message never reaches the
 *     serial port — that is the whole point), and
 *   - input written through it reaches that environment and no other.
 *
 * The kernel finds which environment a console belongs to without a second
 * syscall or a create-window flag: every tenant POSIX sidecar is named
 * `aerosls.posix.<index>` (init builds that name in one place for both the
 * environment manager and the E3 boot spawn), and its partition is the
 * partition it was created in — (partition, index) is exactly the environment's
 * identity (E4). So the registry is keyed by the pair, and a console whose name
 * does not parse is reported and left unwired rather than guessed at. */

/* Environments with a live console. M1 targets four concurrent environments;
 * the per-partition PROC_MAX ceiling allows fewer than eight even before
 * ring-3 programs, so this is headroom, not a design limit. */
#define ENV_CONSOLE_MAX 8

/* Buffered output per environment before the oldest bytes are dropped. An
 * attach drains this; 4 KiB matches the kernel console drain buffer and is far
 * more than a line-oriented command's output. Drops are counted and reported. */
#define ENV_CONSOLE_BUF 4096

/* cap.c calls this the moment it mints the kernel end of a "kernel.env.console"
 * peer for sidecar `pid` (named `name`) in `partition` — the same place it
 * calls env_service_register() for "kernel.env.control".
 *
 * Returns 1 if the console was attributed to an environment and is now its
 * endpoint (the caller logs success), or 0 if `name` is not of the form
 * `aerosls.posix.<index>` or the registry is full — in which case the far end
 * is NOT registered here and the caller must say so, because a silently
 * unregistered console is an environment nobody can reach. */
int env_console_register(uint16_t k_rd, uint16_t k_wr, uint32_t partition,
                         uint32_t pid, const char* name);

/* The environment index in a sidecar name: 1 and *out_index = <n> when `name`
 * is exactly `aerosls.posix.<n>`, 0 otherwise (`out_index` untouched). Exported
 * because it is the kernel's ONE answer to "which environment is this name",
 * and E6's BIB v3 has a second asker: cap.c writes the same index into the
 * sidecar's own boot info block so the sidecar can announce it on its own
 * console (boot.rs), and the index it announces has to be the index this
 * registry files that console under — otherwise an operator's attached stream
 * and the environment's own claim about itself disagree. */
int env_console_name_index(const char* name, uint32_t* out_index);

/* Bind the environment's manager-assigned `env_id` to the console registered
 * for (partition, index). The two are DIFFERENT identifiers: `index` is the
 * environment's identity within its partition (it is in the sidecar names, so
 * it is what attribution above can see while the sidecars are being created),
 * while `env_id` is the monotonic id init's manager assigns and that the
 * control plane's create/destroy routes speak. The kernel learns the id only
 * when the ENV_CREATE reply arrives, at the end of the same round trip in
 * which the sidecars — and so the console — were created, so env_service_create
 * calls this to complete the mapping.
 *
 * Returns 1 if a console for (partition, index) was found and bound (the
 * normal case), or 0 if there is none — an environment whose POSIX sidecar
 * failed to come up, or an E3 boot spawn that no manager created, which is
 * therefore not addressable by id. */
int env_console_bind_env(uint32_t partition, uint32_t index, uint32_t env_id);

/* 1 if `slot` is the kernel-held CHAN_R of a registered environment console.
 *
 * console_service_tick() asks this and SKIPS the slot: an environment's output
 * must not be printed to the kernel's serial port, which is the property E6
 * exists to establish. (It is the same skip shape the env-control reply
 * already uses.) */
int env_console_kernel_slot(uint16_t slot);

/* Drain every registered environment console once. Call from
 * microkernel_service_poll(), next to console_service_tick().
 *
 * Like the console service this takes cap spinlocks, so it must run from a
 * non-IRQ kernel context — never from timer_irq_handler(). A console whose
 * peer has closed is unregistered and its kernel end revoked, so a destroyed
 * environment's console cannot linger. */
void env_console_tick(void);

/* Send `len` bytes to environment (partition, env_id)'s console — the attach
 * surface's input half. The pair is the address the control plane speaks (the
 * same one create/destroy take), not the internal index. Returns the number of
 * bytes queued, 0 if the peer has not drained the previous line, or -1 if that
 * environment has no console (an unknown or unbound env_id, or one that
 * ended). A send that fails because the peer is gone also unregisters the
 * console. */
int env_console_write(uint32_t partition, uint32_t env_id,
                      const uint8_t* bytes, uint32_t len);

/* Destructive read: move up to `cap` buffered output bytes for environment
 * (partition, env_id) into `out` and set *out_len. Returns 1 if the environment
 * has a console (including when nothing was buffered — *out_len 0), or 0 if it
 * has none. Output is returned once; the next read sees only what arrived
 * since (an attach drains, it does not re-read). */
int env_console_read(uint32_t partition, uint32_t env_id,
                     uint8_t* out, uint32_t cap, uint32_t* out_len);

/* Bytes dropped for (partition, env_id) because its buffer was full, or 0. Lets
 * a reader tell "the environment is quiet" from "the environment was louder
 * than the buffer". */
uint32_t env_console_dropped(uint32_t partition, uint32_t env_id);

/* Registry iteration, for the control plane's env listing: enumerate the LIVE
 * consoles (0 <= i < env_console_count()). Returns 1 and fills the out
 * parameters, or 0 if `i` is past the end. `out_pid` is the POSIX sidecar's
 * pid; `out_env_id` is 0 for a console the environment manager never bound
 * (see env_console_bind_env) — which is what an E3 boot spawn is. */
uint32_t env_console_count(void);
int env_console_entry(uint32_t i, uint32_t* out_partition, uint32_t* out_env_id,
                      uint32_t* out_index, uint32_t* out_pid);

/* Total messages drained so far (diagnostics / host tests). */
uint32_t env_console_drained(void);

#endif /* ENV_CONSOLE_H */
