/* user/examples/hello.c — minimal AeroSLS Ring-3 program
 *
 * Demonstrates: sls_puts (serial debug output), sls_valloc, sls_insert,
 * sls_select, and automatic exit via _start returning from main().
 *
 * Build:
 *   make user-programs
 *   python3 utils/program_upload.py --file user/examples/hello.bin --name hello
 *
 * Spawn (after upload):
 *   curl -s -X POST http://localhost:3001/api/program/spawn \
 *        -H "Authorization: Bearer deadbeef01234567cafebabe76543210" \
 *        -H "Content-Type: application/json" \
 *        -d '{"name":"hello"}'
 */

#include <sls.h>

int main(void) {
    sls_puts("[hello] starting\n");

    /* Create a DB_TABLE for results (may already exist on re-runs) */
    uint64_t oid = sls_valloc("hello_output", SLS_OBJ_DB_TABLE, 4);
    if (!oid) sls_puts("[hello] valloc: object exists, continuing\n");

    /* Insert a record */
    int rc = sls_insert("hello_output", "message", "Hello from Ring-3!");
    if (rc == 0) sls_puts("[hello] insert ok\n");
    else         sls_puts("[hello] insert failed\n");

    /* Read it back */
    char buf[SLS_VAL_LEN];
    rc = sls_select("hello_output", "message", buf, sizeof(buf));
    if (rc == 0) {
        sls_puts("[hello] select: ");
        sls_puts(buf);
        sls_puts("\n");
    } else {
        sls_puts("[hello] select failed\n");
    }

    sls_puts("[hello] done\n");
    return 0;
}
