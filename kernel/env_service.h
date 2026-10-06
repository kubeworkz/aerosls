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

/* End the environment `env_id` known to live in `partition` by round-tripping
 * ENV_DESTROY to init (E5): init kills the environment's sidecars and hands its
 * frames back. On a reply, returns 0 and sets *out_status — ENV_OK when the
 * environment was ended (whether or not the kernel had finished the deferred
 * teardown by the time init answered), ENV_ERR_NOENT when the manager holds no
 * such environment (which is also the answer after a partition destroy already
 * ended it and init forgot it), or ENV_ERR_INVAL when the environment exists in
 * a DIFFERENT partition than the one named — plus *out_env_id and, on ENV_OK,
 * *out_partition. Same -1 on no channel or no reply as env_service_create. */
int env_service_destroy(uint32_t env_id, uint32_t partition,
                        uint16_t* out_status, uint32_t* out_env_id,
                        uint32_t* out_partition);

/* POSIX-Environments v0.2 P1a: have the environment manager hand over its half
 * of the environment's checkpoint record.
 *
 * A record needs facts that exist in exactly one place each, and this is the
 * direction they travel. init owns the three frame-pool regions it allocated,
 * the four messenger endpoints it holds, and the registry names the sidecars
 * were created under; the kernel owns the loaded sidecars' entry points and the
 * console binding. So this round-trips ENV_REGISTER { env_id, partition } to
 * init, parses the reply into `struct EnvCkptRegister`, resolves each named
 * task to the sidecar the kernel actually released (its `user_rip`), and hands
 * both halves to env_ckpt_register_from() -- which either stores one complete
 * record or stores nothing.
 *
 * `console_id` is the environment's E6 console key (its index) when a console
 * was registered and bound, 0 when it was not -- an environment whose POSIX
 * sidecar never came up, which nobody can reach and therefore nobody can
 * checkpoint the output of either. `index` is passed, not read back from the
 * reply: it is the (partition, index) identity the sidecars' names carry, and
 * it is what the kernel resolves those names with.
 *
 * Returns 0 when a record was stored, -1 when the channel is not wired or init
 * did not answer -- in which case the environment EXISTS and is simply not
 * checkpointed, which is why the caller logs rather than failing the create.
 * A refusal from the record layer (a foreign/duplicated/incomplete
 * registration) is returned as -1 too, with the reason on the serial
 * transcript: refusal-over-partial-application, applied to the create path. */
int env_service_register_env(uint32_t partition, uint32_t index,
                             uint32_t env_id, uint32_t console_id);

/* ─── P1a: restore-through-create ────────────────────────────────────────────
 * Replay every environment record the last boot's snapshot left pending -- each
 * one through env_service_create(), the SAME ENV_CREATE round trip an HTTP
 * `POST /api/partition/{id}/env` makes. A restored environment is a created
 * environment or it is not restored at all: nothing here has a second create
 * path, and the record layer's gate (env_ckpt_restore_admissible) refuses the
 * records that must not be replayed before any of this runs.
 *
 * What the pass does, per record, in this order -- the order is the feature:
 *
 *   1. A partition the snapshot left PAUSED is resumed for the length of the
 *      create (E4's placement gate refuses a paused target) and re-paused by
 *      the pass's own ledger afterwards. From the first resume to that repause
 *      the body is deliberately straight-line with NO `return`: a pass that
 *      bailed out between them would leave a partition an operator had frozen
 *      RUNNING.
 *   2. The create is issued. A failure (no reply, or an ENV_ERR_* status) is
 *      refused with its own reason and recorded -- never reported as a replay.
 *   3. The record the create's own ENV_REGISTER produced is compared with the
 *      one that was adopted (env_ckpt_restore_same_identity()). On a mismatch
 *      the environment the create just made is DESTROYED and the record is
 *      refused: refusal over partial application, so a refused restore leaves
 *      no environment behind -- not a fresh one, and not a half-restored one.
 *      What this layer does NOT have yet is the payload: the environment that
 *      comes back is empty (see env_ckpt.h), which is why a replay is counted
 *      as replayed, never as restored.
 *
 * Fills `out` even when it returns -1: the counts ARE the contract, and a pass
 * that replayed nothing must be distinguishable from one that replayed
 * everything. A NULL `out` is refused (returns -1) rather than silently
 * dropping the report.
 *
 * Returns 0 when the pass ran (including "nothing was pending" -- a boot that
 * adopted nothing has nothing to replay, which is not a failure), or -1 when
 * records ARE pending and the control channel is not wired: nothing is
 * attempted and nothing settles, so the records stay pending and an operator can
 * ask again once the manager is up. */
struct EnvSvcRestoreReport {
    uint32_t n_pending;    /* records the snapshot left to replay */
    uint32_t n_replayed;   /* creates that came back and re-registered */
    uint32_t n_refused;    /* records this pass declined, each with a reason */
    uint32_t n_remaining;  /* still pending when the pass ended */
    uint32_t n_resumed;    /* paused partitions stepped out of for a replay */
    uint32_t n_repaused;   /* ...and put back paused by the same pass */
    uint32_t n_payloads;   /* replays whose CONTENTS were poured back (P1a) */
    uint32_t n_payload_skips; /* replays that came back empty, each named in
                               * the serial log — absent payload, a named
                               * refusal, or a metadata-only request */
};

int env_service_restore_pending(struct EnvSvcRestoreReport* out);

/* P1a payload: the restore route's `"payload": false` knob — a
 * METADATA-ONLY restore brings the environment back without its captured
 * memory (an empty environment, replay counted as replay). This is also
 * the no-restore tooth's boot-side handle: the roadmap's tooth says "the
 * create runs, the payload does not", and this is the seam that makes
 * that state producible on a real boot. Set on every route invocation (no
 * sticky state), and consulted per record inside the pass. */
void env_service_set_restore_payload(int on);

#endif /* ENV_SERVICE_H */
