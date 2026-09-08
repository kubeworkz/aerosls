#include <stddef.h>
#include "mbedtls/build_info.h"
/* ─── Plumbing stubs ───────────────────────────────────────────────────────
 * Console, panic, process and catalog symbols that kernel/stubs.c references.
 * Every one is unreachable from certificate generation. stubs.c is linked in
 * full rather than excerpted specifically so that sls_tls_snprintf is the REAL
 * one: mbedTLS routes OID and DN formatting through it, and the project has
 * already had one near-miss where a snprintf that ignores its format and
 * writes "<nofmt>" would have landed that literal in a certificate subject
 * name. A stub here could reproduce exactly that, invisibly. */
#include "mbedtls/platform_time.h"
void kernel_serial_printf(const char *f, ...);
void kernel_serial_printf(const char *f, ...) { (void)f; }
void kernel_serial_capture_stop(void);
void kernel_serial_capture_stop(void) {}
void kernel_panic_puts(const char *s);   void kernel_panic_puts(const char *s) { (void)s; }
void kernel_panic_hex64(unsigned long long v); void kernel_panic_hex64(unsigned long long v) { (void)v; }
void kernel_panic_dec(long long v);      void kernel_panic_dec(long long v) { (void)v; }
void process_exit(int c);                void process_exit(int c) { (void)c; }
int  kernel_get_current_thread_id(void); int kernel_get_current_thread_id(void) { return 0; }
void qemu_sls_mmu_shadow_fault(void);    void qemu_sls_mmu_shadow_fault(void) {}
/* M7's guest-#PF hook: kernel/stubs.c's handle_page_fault references the
 * guest-paging flags and the launcher hook. Certificate generation never
 * enables guest paging, so the flags stay 0 and sls_i386_guest_pf is never
 * reached; they must exist for the link, not for the semantics. */
int qemu_sls_guest_active = 0;
int qemu_sls_guest_paging_on = 0;
void sls_i386_guest_pf(uint64_t cr2, uint32_t error_code, uint64_t rip)
{ (void)cr2; (void)error_code; (void)rip; }
void *object_catalog = 0;
unsigned long object_catalog_count = 0;
/* kernel/stubs.c's handle_page_fault calls process_find_current(); the
 * oracle has no process table, so a NULL current-process is the right
 * semantics (the fault path bails out). */
struct ProcessDescriptor;
struct ProcessDescriptor *process_find_current(void) { return 0; }
char stack_bottom[1], stack_top[1];
/* mbedtls_ms_time comes from the real tls_platform.c now. */
void kernel_serial_print(const char *s);
void kernel_serial_print(const char *s) { (void)s; }
volatile unsigned long long kernel_tick_counter = 0;

/* TCP. tls_platform.c's BIO callbacks reference these; certificate generation
 * never reaches them. Deliberately NOT no-ops that pretend success -- if the
 * oracle ever does reach the transport, it should fail loudly. */
#include "tcp.h"
struct TCPConn tcp_conns[TCP_MAX_CONNS];
/* Signatures taken from net/tcp.h rather than guessed. */
#include "tcp.h"
struct TCPConn tcp_conns[TCP_MAX_CONNS];
int tcp_send(int conn_id, const void* buf, uint32_t len) { (void)conn_id;(void)buf;(void)len; return -1; }
int tcp_recv(int conn_id, void* buf, uint16_t max_len) { (void)conn_id;(void)buf;(void)max_len; return -1; }
