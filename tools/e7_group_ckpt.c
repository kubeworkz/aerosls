/*
 * e7_group_ckpt.c — the TG-2d two-thread checkpoint workload.
 *
 * The binary is freestanding: no libc, no dynamic loader, no PIE. It is built
 * static ET_EXEC and entered at _start with the kernel's §5.2 stack image. The
 * only syscalls it uses are the ones the shim's measured core already carries:
 * clone, gettid, futex, openat/read/write/fsync/close, exit and exit_group.
 *
 * What it does:
 *   1. main clones ONE raw sibling with the same flags Go uses (no
 *      CLONE_CHILD_CLEARTID / PARENT_SETTID, which v1 refuses).
 *   2. both threads wait on one shared futex word. The worker has a 60 s
 *      relative timeout; main waits forever. A gate-property group prints
 *      `[E7-GATE] group barrier: ... all futex waiters` from the kernel when
 *      the second thread parks, which is the checkpoint trigger.
 *   3. after restore the worker's re-armed timeout expires, it wakes main, and
 *      both verify the signature main wrote BEFORE the barrier. A restarted
 *      binary would print G2-START a second time; an unrestored wait would
 *      never print the wake pair.
 *   4. main copies a staged 64 KiB input and prints its FNV-1a checksum, then
 *      exit_group tears the group down once.
 *
 * The worker exits with SYS_exit (thread-local), not exit_group: the leader
 * must remain the sidecar's named reap target until the copy is done.
 *
 * Build (from the repo root):
 *   cc -O2 -static -nostdlib -no-pie -fno-pie -ffreestanding \
 *      -fno-stack-protector -fno-builtin -fno-asynchronous-unwind-tables \
 *      -Wl,-e,_start -Wl,--build-id=none \
 *      -o user/sidecar/fixtures/e7_group_ckpt.elf tools/e7_group_ckpt.c
 */

typedef unsigned long u64;
typedef unsigned int u32;
typedef long i64;

#define SYS_READ        0
#define SYS_WRITE       1
#define SYS_CLOSE       3
#define SYS_FSYNC       74
#define SYS_OPENAT      257
#define SYS_CLONE       56
#define SYS_EXIT        60
#define SYS_EXIT_GROUP  231
#define SYS_GETTID      186
#define SYS_FUTEX       202

#define FUTEX_WAIT      128
#define FUTEX_WAKE      129
#define AT_FDCWD        (-100)
#define O_RDONLY        0
#define O_WRONLY        1
#define O_CREAT         0100
#define O_TRUNC         01000

#define CLONE_VM        0x00000100ul
#define CLONE_FS        0x00000200ul
#define CLONE_FILES     0x00000400ul
#define CLONE_SIGHAND   0x00000800ul
#define CLONE_THREAD    0x00010000ul
#define CLONE_SYSVSEM   0x00040000ul
#define CLONE_FLAGS     (CLONE_VM | CLONE_FS | CLONE_FILES | CLONE_SIGHAND | \
                         CLONE_THREAD | CLONE_SYSVSEM)

#define SIGNATURE       0x5a17c0deu
#define WORKER_STACK    (64u * 1024u)
#define COPY_CHUNK      4096u

static long sys6(long n, long a1, long a2, long a3, long a4, long a5,
                 long a6) {
    register long r10 __asm__("r10") = a4;
    register long r8  __asm__("r8")  = a5;
    register long r9  __asm__("r9")  = a6;
    __asm__ volatile("syscall"
                     : "+a"(n)
                     : "D"(a1), "S"(a2), "d"(a3), "r"(r10), "r"(r8), "r"(r9)
                     : "rcx", "r11", "memory");
    return n;
}

static long sys3(long n, long a1, long a2, long a3) {
    return sys6(n, a1, a2, a3, 0, 0, 0);
}

static u64 slen(const char* s) {
    u64 n = 0;
    while (s[n]) n++;
    return n;
}

static void write_all(long fd, const char* p, u64 n) {
    while (n) {
        long w = sys3(SYS_WRITE, fd, (long)p, (long)n);
        if (w <= 0) return;
        p += (u64)w;
        n -= (u64)w;
    }
}

/* One complete serial line per write syscall: a per-character write races
 * the sidecar's single forward lease and interleaves the console with the
 * kernel's own log. The buffer is flushed at every newline. */
static char linebuf[160];
static u64  linelen;

static void flush_line(void) {
    if (linelen) write_all(1, linebuf, linelen);
    linelen = 0;
}

static void out_char(char c) {
    if (c == '\n') { flush_line(); return; }
    if (linelen == sizeof(linebuf)) flush_line();
    linebuf[linelen++] = c;
}

static void puts_raw(const char* s) {
    while (*s) out_char(*s++);
}

static void put_dec(u64 v) {
    char b[32];
    int i = 0;
    if (v == 0) {
        out_char('0');
        return;
    }
    while (v && i < (int)sizeof(b)) {
        b[i++] = (char)('0' + (v % 10u));
        v /= 10u;
    }
    while (i > 0) out_char(b[--i]);
}

static void put_signed(long v) {
    if (v < 0) {
        out_char('-');
        put_dec((u64)(-(v + 1)) + 1u);
    } else {
        put_dec((u64)v);
    }
}

static void put_hex(u64 v) {
    static const char d[] = "0123456789abcdef";
    char b[16];
    int i = 0;
    out_char('0'); out_char('x');
    if (v == 0) {
        out_char('0');
        return;
    }
    while (v && i < 16) {
        b[i++] = d[v & 0xfu];
        v >>= 4;
    }
    while (i > 0) out_char(b[--i]);
}

static volatile u32 ready;
static volatile u32 worker_go;
static volatile u32 worker_done;
static volatile u32 worker_saw;
static volatile u32 barrier;
static volatile u32 signature;
static u32 main_tid;
static u32 worker_tid;
static long worker_wait_ret;
static long worker_wake_ret;

/* The ELF loader maps a PT_LOAD only when filesz is nonzero; a pure-BSS
 * segment is skipped. One initialized byte in the RW segment keeps filesz
 * nonzero, and the loader zero-fills the rest of memsz (the stacks and the
 * flags). */
static volatile unsigned char rw_anchor __attribute__((used)) = 0x5a;

static unsigned char worker_stack[WORKER_STACK] __attribute__((aligned(64)));

static void worker_entry(void);

__attribute__((naked)) static long raw_clone(unsigned long flags, void* stack) {
    (void)flags;
    (void)stack;
    __asm__ volatile(
        "xor %rdx,%rdx\n"
        "xor %r10,%r10\n"
        "xor %r8,%r8\n"
        "mov $56,%rax\n"
        "syscall\n"
        "test %rax,%rax\n"
        "jne 1f\n"
        "mov %rsi,%rsp\n"
        "xor %rbp,%rbp\n"
        "call worker_entry\n"
        "mov %rax,%rdi\n"
        "mov $60,%rax\n"
        "syscall\n"
        "1: ret\n");
}

__attribute__((used)) static void worker_entry(void) {
    worker_tid = (u32)sys3(SYS_GETTID, 0, 0, 0);
    ready = 1;
    /* Main prints the TIDS/SHARED-WRITE lines without this thread writing in
     * the middle of them; the barrier still forms after both are done. */
    while (!worker_go) { }
    puts_raw("G2-WORKER-READY\n");

    /* 60 s relative wait. Capture happens while this is parked; restore
     * re-arms the remaining duration, and this timeout is what wakes main. */
    struct { long sec; long nsec; } ts;
    ts.sec = 60;
    ts.nsec = 0;
    worker_wait_ret = sys6(SYS_FUTEX, (long)&barrier, FUTEX_WAIT, 0,
                           (long)&ts, 0, 0);
    puts_raw("G2-WORKER-TIMEOUT r=");
    put_signed(worker_wait_ret);
    puts_raw("\n");

    worker_wake_ret = sys6(SYS_FUTEX, (long)&barrier, FUTEX_WAKE, 1, 0, 0, 0);
    puts_raw("G2-WORKER-WAKE w=");
    put_dec((u64)worker_wake_ret);
    puts_raw("\n");

    if (signature == SIGNATURE) {
        worker_saw = 1;
        puts_raw("G2-SHARED=1\n");
    } else {
        puts_raw("G2-SHARED=0\n");
    }
    worker_done = 1;
    sys3(SYS_EXIT, 1, 0, 0);
    for (;;) { }
}

__attribute__((used)) static int cmain(long argc, char** argv) {
    if (argc != 3 || !argv || !argv[1] || !argv[2]) {
        puts_raw("G2-USAGE\n");
        return 2;
    }

    puts_raw("G2-START\n");
    main_tid = (u32)sys3(SYS_GETTID, 0, 0, 0);

    long child = raw_clone(CLONE_FLAGS,
                           worker_stack + sizeof(worker_stack) - 16u);
    if (child < 0) {
        puts_raw("G2-CLONE-FAILED r=");
        put_signed(child);
        puts_raw("\n");
        return 3;
    }

    while (!ready) { }
    while (!worker_tid) { }
    puts_raw("G2-TIDS main=");
    put_dec(main_tid);
    puts_raw(" worker=");
    put_dec(worker_tid);
    puts_raw("\n");

    /* G2.5: this store is before the barrier, and the worker reads it only
     * after the restored timeout/wake. A stale/restarted image fails it. */
    signature = SIGNATURE;
    puts_raw("G2-SHARED-WRITE=");
    put_hex(signature);
    puts_raw("\n");
    worker_go = 1;
    puts_raw("G2-BARRIER uaddr=");
    put_hex((u64)&barrier);
    puts_raw("\n");

    long r = sys6(SYS_FUTEX, (long)&barrier, FUTEX_WAIT, 0, 0, 0, 0);
    puts_raw("G2-MAIN-WAKE r=");
    put_signed(r);
    puts_raw("\n");

    while (!worker_done) { }
    if (signature != SIGNATURE || !worker_saw) {
        puts_raw("G2-SHARED-FAILED\n");
        return 4;
    }

    long in = sys6(SYS_OPENAT, AT_FDCWD, (long)argv[1], O_RDONLY, 0, 0, 0);
    if (in < 0) {
        puts_raw("G2-INPUT-FAILED r=");
        put_signed(in);
        puts_raw("\n");
        return 5;
    }
    long out = sys6(SYS_OPENAT, AT_FDCWD, (long)argv[2],
                    O_WRONLY | O_CREAT | O_TRUNC, 0644, 0, 0);
    if (out < 0) {
        puts_raw("G2-OUTPUT-FAILED r=");
        put_signed(out);
        puts_raw("\n");
        return 6;
    }

    unsigned char buf[COPY_CHUNK];
    u64 total = 0;
    u64 sum = 14695981039346656037ul;
    for (;;) {
        long got = sys3(SYS_READ, in, (long)buf, (long)sizeof(buf));
        if (got < 0) {
            puts_raw("G2-READ-FAILED r=");
            put_signed(got);
            puts_raw("\n");
            return 7;
        }
        if (got == 0) break;
        u64 n = (u64)got;
        write_all(out, (const char*)buf, n);
        for (u64 i = 0; i < n; i++) {
            sum ^= (u64)buf[i];
            sum *= 1099511628211ul;
        }
        total += n;
    }
    (void)sys3(SYS_FSYNC, out, 0, 0);
    (void)sys3(SYS_CLOSE, out, 0, 0);
    (void)sys3(SYS_CLOSE, in, 0, 0);

    puts_raw("G2-COPY=");
    put_dec(total);
    puts_raw("\nG2-SUM=");
    put_hex(sum);
    puts_raw("\nG2-DONE\n");
    return 0;
}

__attribute__((naked, noreturn)) void _start(void) {
    __asm__ volatile(
        "xor %rbp,%rbp\n"
        "mov (%rsp),%rdi\n"
        "lea 8(%rsp),%rsi\n"
        "and $-16,%rsp\n"
        "call cmain\n"
        "mov %eax,%edi\n"
        "mov $231,%rax\n"
        "syscall\n"
        "hlt\n");
}
