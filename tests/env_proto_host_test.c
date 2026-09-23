/*
 * tests/env_proto_host_test.c — the ENV_* protocol's C mirror (E4).
 *
 * kernel/env_proto.h is the C side of the ENV_* control-plane protocol; its
 * Rust twin is user/proto/src/env_proto.rs. The two speak the same bytes over
 * init's kernel-held request channel, so they MUST agree on the magic, the
 * opcodes, the status codes, and the little-endian frame/body layouts. This
 * guard pins the C side against the documented wire contract (the same values
 * env_proto.rs's own tests pin the Rust side against) and round-trips the
 * frame + bodies so a layout drift on either side fails the build here rather
 * than as a silent create that init never answers.
 *
 * Build and run:
 *   gcc -std=c11 -Wall -Wextra -I. -Ikernel \
 *       -o /tmp/env_proto_host_test tests/env_proto_host_test.c
 *   /tmp/env_proto_host_test
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "../kernel/env_proto.h"

#define CHECK(cond, msg) do {           \
    if (!(cond)) {                      \
        printf("FAIL: %s\n", msg);      \
        failures++;                     \
    }                                   \
} while (0)

int main(void) {
    int failures = 0;

    /* ── the cross-language constant contract (== env_proto.rs) ───────────── */
    CHECK(ENV_VERSION == 1, "ENV_VERSION");
    CHECK(ENV_CREATE == 1 && ENV_DESTROY == 2, "opcodes");
    CHECK(ENV_FLAG_ERROR == 0x0001, "ENV_FLAG_ERROR");
    CHECK(ENV_OK == 0 && ENV_ERR_INVAL == 1 && ENV_ERR_NOMEM == 2 &&
          ENV_ERR_PART == 3 && ENV_ERR_FULL == 4 && ENV_ERR_UNSUPP == 5 &&
          ENV_ERR_NOENT == 6,
          "status codes");
    CHECK(ENV_FRAME_SIZE == 16 && ENV_CREATE_BODY_SIZE == 8 &&
          ENV_DESTROY_BODY_SIZE == 8 && ENV_REPLY_BODY_SIZE == 12,
          "sizes");

    /* ── frame encode: magic "AEROSEN\x01" + LE fields ────────────────────── */
    uint8_t f[ENV_FRAME_SIZE];
    env_frame_encode(f, ENV_CREATE, 0);
    CHECK(memcmp(f, "AEROSEN\x01", 8) == 0, "frame magic bytes");
    CHECK(f[8] == 1 && f[9] == 0, "frame version LE");
    CHECK(f[10] == ENV_CREATE && f[11] == 0, "frame type LE");
    CHECK(f[12] == 0 && f[13] == 0, "frame flags clear on a request");

    /* an error reply sets the flag */
    uint8_t ferr[ENV_FRAME_SIZE];
    env_frame_encode(ferr, ENV_CREATE, 1);
    CHECK(env_read_u16(ferr, 12) == ENV_FLAG_ERROR, "error frame sets ENV_FLAG_ERROR");

    /* ── frame parse: type out, magic + version validated ─────────────────── */
    uint16_t ty = 0xFFFF;
    CHECK(env_frame_parse(f, sizeof f, &ty) == 1 && ty == ENV_CREATE,
          "frame parses and yields its type");
    CHECK(env_frame_parse(f, 8, &ty) == 0, "a short frame is rejected");
    uint8_t bad[ENV_FRAME_SIZE];
    memcpy(bad, f, sizeof bad);
    bad[0] = 'X';
    CHECK(env_frame_parse(bad, sizeof bad, &ty) == 0, "a foreign magic is rejected");
    memcpy(bad, f, sizeof bad);
    env_put_u16(bad, 8, 99);
    CHECK(env_frame_parse(bad, sizeof bad, &ty) == 0, "a wrong version is rejected");

    /* ── ENV_CREATE body: { partition u32, index u32 } LE ─────────────────── */
    uint8_t cb[ENV_CREATE_BODY_SIZE];
    env_create_body_encode(cb, 0x11223344u, 0x55667788u);
    CHECK(env_read_u32(cb, 0) == 0x11223344u && env_read_u32(cb, 4) == 0x55667788u,
          "create body round-trips partition + index");
    CHECK(cb[0] == 0x44 && cb[3] == 0x11, "create body is little-endian");

    /* ── ENV_DESTROY body: { env_id u32, partition u32 } LE ───────────────── */
    /* The partition is carried, not assumed: the control plane's destroy route
     * is nested under the partition it names, so without it a caller could end
     * partition B's environment through a path naming partition A. */
    uint8_t db[ENV_DESTROY_BODY_SIZE];
    env_destroy_body_encode(db, 0x01020304u, 0x0A0B0C0Du);
    CHECK(env_read_u32(db, 0) == 0x01020304u && env_read_u32(db, 4) == 0x0A0B0C0Du,
          "destroy body round-trips env_id + partition");
    CHECK(db[0] == 0x04 && db[3] == 0x01, "destroy body is little-endian");

    /* ── reply body parse: { status u16, pad u16, env_id u32, partition u32 } */
    uint8_t rb[ENV_REPLY_BODY_SIZE];
    env_put_u16(rb, 0, ENV_OK);
    env_put_u16(rb, 2, 0);
    env_put_u32(rb, 4, 7);   /* env_id */
    env_put_u32(rb, 8, 5);   /* partition */
    uint16_t st = 0xFFFF; uint32_t eid = 0, part = 0;
    env_reply_body_parse(rb, &st, &eid, &part);
    CHECK(st == ENV_OK && eid == 7 && part == 5, "reply body parses status/env_id/partition");

    if (failures == 0) {
        printf("ALL PASS: kernel/env_proto.h matches the ENV_* wire contract\n");
        return 0;
    }
    printf("%d FAILURE(S): env_proto.h drifted from the ENV_* wire contract\n", failures);
    return 1;
}
