/*
 * tests/payload_host_stubs.h — the payload primitives kernel/persist.c and
 * kernel/env_service.c call (P1a payload), for host tests that link those
 * files but NOT the real kernel/env_payload.c.
 *
 * ─── Why this exists ──────────────────────────────────────────────────────
 * persist_environments() now captures the payload of every frozen environment
 * ((void)env_payload_capture_all(), inside the quiesce) and
 * env_service_restore_pending() now waits for each replayed sidecar to park
 * and pours its payload back (env_payload_wait_parked/env_payload_restore).
 * Those two call sites live in files a dozen SQL/persist host tests link, and
 * linking the real env_payload.c would drag in the NVMe queues, the console
 * registry, and proc_table — the same dependencies partition_host_stubs.h
 * exists to keep out of record-layer tests.
 *
 * ─── Why every stub is weak ───────────────────────────────────────────────
 * The same idiom tests/partition_host_stubs.h and tests/process_host_stubs.h
 * use: a test that DOES link kernel/env_payload.c (env_payload_host_test.c)
 * gets the real definitions and this header's are shadowed, so a TU can
 * include it harmlessly either way.
 *
 * ─── The model ────────────────────────────────────────────────────────────
 * A host test without the payload layer on media is a boot where capture had
 * nothing (or no NVMe queue) and restore has nothing to pour: capture reports
 * 0, restore refuses ABSENT by name with a latched, renderable refusal, and
 * the wait reports "already parked" so the restore pass is not spun on a
 * sidecar this test does not model. Every one of those is the honest answer
 * for a world where no payload was ever written — never a silent success.
 */
#ifndef PAYLOAD_HOST_STUBS_H
#define PAYLOAD_HOST_STUBS_H

#include "kernel/env_payload.h"

__attribute__((weak)) uint32_t env_payload_capture_all(void) { return 0; }

__attribute__((weak)) int env_payload_present(uint32_t partition, uint32_t index) {
    (void)partition; (void)index;
    return 0;
}

__attribute__((weak)) int env_payload_wait_parked(const struct EnvCkptRecord* rec) {
    (void)rec;
    return 1;
}

__attribute__((weak)) int env_payload_restore(uint32_t partition, uint32_t index,
                                              uint32_t new_env_id, uint64_t want_seq,
                                              uint32_t* out_pages, uint32_t* out_console) {
    (void)partition; (void)index; (void)new_env_id; (void)want_seq;
    if (out_pages)  *out_pages  = 0;
    if (out_console) *out_console = 0;
    return EP_REFUSE_ABSENT;
}

static struct EnvPayloadRefusal payload_stub_refusal = {
    EP_REFUSE_ABSENT, 0, 0
};

__attribute__((weak)) struct EnvPayloadRefusal env_payload_last_refusal(void) {
    return payload_stub_refusal;
}

__attribute__((weak)) void env_payload_refusal_text(const struct EnvPayloadRefusal* r,
                                                    char* out, uint32_t cap) {
    if (!out || cap == 0) return;
    const char* msg = (r && r->code != EP_REFUSE_NONE)
        ? "no payload on media (host stub)"
        : "";
    uint32_t i = 0;
    while (msg[i] && i + 1u < cap) { out[i] = msg[i]; i++; }
    out[i] = '\0';
}

__attribute__((weak)) void env_payload_clear_refusal(void) {
    payload_stub_refusal.code = EP_REFUSE_NONE;
    payload_stub_refusal.a = payload_stub_refusal.b = 0;
}

#endif /* PAYLOAD_HOST_STUBS_H */
