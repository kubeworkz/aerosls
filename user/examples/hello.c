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
    sls_println("[hello] starting");

    /* Create a DB_TABLE object to store results */
    uint64_t oid = sls_valloc("hello_output", SLS_OBJ_DB_TABLE, 4);
    if (!oid)
        sls_println("[hello] valloc: object already exists, continuing");

    /* Write a record */
    int rc = sls_insert("hello_output", "message", "Hello from Ring-3!");
    if (rc == 0)
        sls_println("[hello] insert ok");
    else
        sls_println("[hello] insert failed (key may exist)");

    /* Read it back */
    char buf[SLS_VAL_LEN];
    rc = sls_select("hello_output", "message", buf, sizeof(buf));
    if (rc == 0) {
        sls_puts("[hello] select: ");
        sls_println(buf);
    } else {
        sls_println("[hello] select failed");
    }

    sls_println("[hello] done");
    return 0;
}
