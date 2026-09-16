#ifndef ENV_SERVICE_H
#define ENV_SERVICE_H

#include <stdint.h>

/* env_service.h — the kernel side of the environment-manager control channel
 * (POSIX-Environments E4).
 *
 * cap_create_sidecar() wires init a manifest CAP_CHAN whose peer is
 * "kernel.env.control": init holds one end, the kernel context (pid 0) holds
 * the other. The kernel end is registered here. The HTTP control plane calls
 * env_service_create() to ask init's environment manager to create an
 * environment IN a partition — a synchronous round trip: send ENV_CREATE, let
 * init run under timer preemption, drain the reply.
 *
 * Unlike the console service (which drains EVERY kernel-context CHAN_R to
 * serial), the env reply must reach the HTTP handler, so console_service_tick()
 * asks env_service_reply_slot() and skips the env channel's CHAN_R. */

/* cap.c calls this when it wires the "kernel.env.control" channel: the kernel
 * (pid 0) CHAN_R (init's replies arrive here) and CHAN_W (requests go to init). */
void env_service_register(uint16_t kernel_chan_r, uint16_t kernel_chan_w);

/* 1 if `slot` is the env-control reply CHAN_R in the pid-0 cap table, so
 * console_service_tick() leaves init's ENV replies for env_service_create(). */
int env_service_reply_slot(uint16_t slot);

/* Create an environment with `index` in `partition` by round-tripping
 * ENV_CREATE to init. On a reply, returns 0 and sets *out_status (ENV_OK or an
 * ENV_ERR_* from env_proto.h) and, on ENV_OK, *out_env_id. Returns -1 if the
 * channel is not wired or init did not reply before the deadline. */
int env_service_create(uint32_t partition, uint32_t index,
                       uint16_t* out_status, uint32_t* out_env_id);

#endif /* ENV_SERVICE_H */
