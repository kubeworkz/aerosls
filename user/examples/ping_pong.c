/* user/examples/ping_pong.c — IPC Phase 5 demo
 *
 * Single Ring-3 process that:
 *   1. Binds to user port 0x2000
 *   2. Sends a "ping" message to itself (src and dst = 0x2000)
 *   3. Spins receiving until the message arrives
 *   4. Verifies the payload and logs the round-trip
 *
 * With multiple processes (future Phase 5 extension): process A binds to
 * 0x2000, process B binds to 0x2001; A sends to 0x2001, B responds to 0x2000.
 *
 * Build:
 *   make user-programs
 *   python3 utils/program_upload.py --file user/examples/ping_pong.bin \
 *                                   --name ping_pong
 *   curl -X POST .../api/program/spawn -d '{"name":"ping_pong"}'
 */

#include <sls.h>

#define MY_PORT    0x2000
#define PING_OPCODE  0xAA01
#define MAGIC_ARG    0xDEADBEEFULL

int main(void) {
    sls_puts("[ping] starting\n");

    /* 1. Bind to user port 0x2000 */
    int rc = sls_ipc_bind(MY_PORT);
    if (rc != 0) {
        sls_puts("[ping] bind failed\n");
        return 1;
    }
    sls_puts("[ping] bound to port 0x2000\n");

    /* 2. Send a ping message to ourselves */
    rc = sls_ipc_send(MY_PORT, PING_OPCODE, MAGIC_ARG, 0, 0, 0);
    if (rc != 0) {
        sls_puts("[ping] send failed\n");
        return 2;
    }
    sls_puts("[ping] sent ping\n");

    /* 3. Spin-receive until message arrives (preemptive timer lets kernel run) */
    struct sls_ipc_msg msg;
    int tries = 0;
    while (1) {
        if (sls_ipc_recv(MY_PORT, &msg)) break;
        tries++;
        if (tries > 100000) {
            sls_puts("[ping] receive timeout\n");
            return 3;
        }
    }

    /* 4. Verify and report */
    if (msg.opcode == PING_OPCODE && msg.payload[0] == MAGIC_ARG)
        sls_puts("[ping] pong received — payload matches!\n");
    else
        sls_puts("[ping] pong received — payload MISMATCH\n");

    sls_puts("[ping] done\n");
    return 0;
}
