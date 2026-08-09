/*
 * stress_atomics.c — the real-hardware concurrency stress harness for
 * OP_CAS / OP_ATOMIC_ADD (Gap Remediation SIMI Phase 15, plan doc Part II).
 *
 * The four-way parity harness is single-threaded: it proves the FUNCTIONAL
 * semantics (CAS success/failure, returned-old values, atomic-add results)
 * bit-identical across the interp/x86/RV64/ARM engines. It cannot prove
 * CONCURRENCY semantics — mutual exclusion, no lost updates under
 * contention — because nothing in the sandbox runs two CPUs at once. This
 * harness closes that documented gap for x86: the A1 codegen's `lock
 * cmpxchg` / `lock xadd` are REAL CPU instructions, so translating the
 * fixture once and calling the same entry function from M pthreads makes
 * the atomics contend on real hardware, on a real shared scratch region.
 *
 * The fixture (tests/stress_atomics.simi) has two entries sharing one
 * scratch region (translated twice — entry "main" for the producers,
 * entry "consumer" for the single drainer):
 *
 *   - Shared counter leg (OP_ATOMIC_ADD): every producer bumps the enqueue
 *     counter R times. No lost updates  <=>  final [enq] == M*R exactly.
 *   - Lock-free queue leg (OP_CAS): every producer CAS-claims the next
 *     free slot of a bounded slot array (the classic cmpxchg retry loop —
 *     no locks, each claim succeeds exactly once). Mutual exclusion  <=>
 *     final [claim] == M*R AND each of the M*R slots holds exactly one
 *     producer's tid AND the per-tid histogram shows exactly R writes per
 *     thread (no slot double-claimed, no slot skipped, no write lost).
 *   - Concurrent drainer: the single consumer thread spins on the enqueue
 *     counter and drains every slot (proving a real concurrent reader
 *     observes every item exactly once). x86-TSO-sound: the producer's
 *     value store precedes its lock xadd (a full barrier), so once
 *     enq > pos is observed the slot is visible. ARM/RV64 weak memory
 *     would need the A4 acquire/release extension (deferred by the plan).
 *   - Ticket leg: each producer claims a unique tid via ATOMIC_ADD's
 *     returned-old (0..M-1); the M return values must be a permutation.
 *
 * Usage: stress_atomics [threads] [iters]   (defaults 8 / 20000;
 *         threads*iters must fit the CAP = 262144-slot array).
 *
 * The scratch cells at +0..+32 are zeroed up front; M/R/CAP are written at
 * +40/+48/+56 BEFORE the threads are spawned (the fixture reads them, so
 * the happens-before edge is pthread_create — no fixture-side races).
 * Exit status 0 = every check passed; 1 = any check failed; a crash in any
 * thread (bad codegen would SIGSEGV/SIGILL/SIGFPE) is caught by the signal
 * handler and reported as a clean failure instead of a raw segfault.
 */
#define _GNU_SOURCE
#include "simi_x86.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <sys/mman.h>
#include <signal.h>
#include <pthread.h>
#include <unistd.h>
#include <time.h>

/* Scratch layout — MUST match tests/stress_atomics.simi. */
enum {
    OFF_TID   = 0,   /* producer tid claim counter (ATOMIC_ADD)  */
    OFF_ENQ   = 8,   /* producer enqueue/hit counter (ATOMIC_ADD) */
    OFF_CLAIM = 16,  /* CAS claim index (next free slot)         */
    OFF_DEQ   = 24,  /* consumer dequeue counter (ATOMIC_ADD)    */
    OFF_BAD   = 32,  /* consumer out-of-range-value counter      */
    OFF_M     = 40,  /* M (threads) — harness-written pre-start  */
    OFF_R     = 48,  /* R (iters per producer) — harness-written */
    OFF_CAP   = 56,  /* CAP (slot capacity) — harness-written    */
    OFF_VALS  = 64,  /* values[CAP], 8 bytes each                */
};

#define DEFAULT_THREADS 8
#define DEFAULT_ITERS   20000
#define CAP             (1 << 20)   /* 1048576 slots = 8 MiB array; M*R must fit */

/* A crash anywhere (bad codegen, an out-of-array CAS on a slot index past
 * CAP, etc.) must be reported, not silently swallowed. Async-signal-safe. */
static void crash_handler(int sig) {
    static const char msg[] = "stress_atomics: CRASHED (SIGSEGV/SIGILL/SIGFPE) in a translated thread — bad codegen or out-of-bounds access\n";
    (void)sig;
    ssize_t ignored = write(STDERR_FILENO, msg, sizeof(msg) - 1);
    (void)ignored;
    _exit(1);
}

struct thread_arg {
    long long (*fn)(void);
    long long ret;
};

static void *thread_main(void *p) {
    struct thread_arg *a = (struct thread_arg *)p;
    a->ret = a->fn();          /* the translated producer/consumer body */
    return NULL;
}

/* Stubs — the fixture never executes RESOLVE/OBJSIZE/OBJTYPE. */
uint64_t simi_rt_resolve(const char *name) { (void)name; return 0; }
uint64_t simi_rt_objsize(uint64_t v) { (void)v; return 0; }
uint64_t simi_rt_objtype(uint64_t v) { (void)v; return 0xFFFFFFFFu; }

static double now_sec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static int translate(const char *path, const char *entry,
                     uint8_t *scratch, uint8_t **code_out,
                     uint32_t *entry_off_out) {
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); return -1; }
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *obj = malloc((size_t)sz);
    if (fread(obj, 1, (size_t)sz, f) != (size_t)sz) { perror("fread"); fclose(f); return -1; }
    fclose(f);

    uint32_t cap = 65536;
    uint8_t *code = mmap(NULL, cap, PROT_READ | PROT_WRITE,
                         MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (code == MAP_FAILED) { perror("mmap code"); free(obj); return -1; }

    uint32_t out_len = 0, entry_off = 0;
    int rc = simi_x86_translate(obj, (uint32_t)sz, code, cap, entry,
                                (uint64_t)(uintptr_t)scratch,
                                (uint64_t)(uintptr_t)simi_rt_resolve,
                                (uint64_t)(uintptr_t)simi_rt_objsize,
                                (uint64_t)(uintptr_t)simi_rt_objtype,
                                &out_len, &entry_off);
    free(obj);
    if (rc != TX_OK) {
        fprintf(stderr, "stress_atomics: translate(%s) failed: %s\n",
                entry, simi_x86_strerror(rc));
        return -1;
    }
    if (mprotect(code, cap, PROT_READ | PROT_EXEC) != 0) { perror("mprotect"); return -1; }
    *code_out = code;
    *entry_off_out = entry_off;
    return 0;
}

int main(int argc, char **argv) {
    long long M = DEFAULT_THREADS, R = DEFAULT_ITERS;
    if (argc > 1) M = atoll(argv[1]);
    if (argc > 2) R = atoll(argv[2]);
    if (M < 1 || R < 1) {
        fprintf(stderr, "stress_atomics: threads and iters must be >= 1\n");
        return 1;
    }
    if (M * R > CAP) {
        fprintf(stderr, "stress_atomics: M*R = %lld exceeds CAP = %d slots\n", M * R, CAP);
        return 1;
    }

    const char *tmo = "tests/stress_atomics.tmo";
    size_t scratch_size = OFF_VALS + (size_t)CAP * 8;
    uint8_t *scratch = mmap(NULL, scratch_size, PROT_READ | PROT_WRITE,
                            MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (scratch == MAP_FAILED) { perror("mmap scratch"); return 1; }
    memset(scratch, 0, scratch_size);          /* zero the shared cells + slots */

    /* M/R/CAP are written BEFORE the threads exist — pthread_create is the
     * happens-before edge the fixture relies on (reads them once at entry). */
    *(uint64_t *)(scratch + OFF_M)   = (uint64_t)M;
    *(uint64_t *)(scratch + OFF_R)   = (uint64_t)R;
    *(uint64_t *)(scratch + OFF_CAP) = (uint64_t)CAP;

    uint8_t *prod_code = NULL, *cons_code = NULL;
    uint32_t prod_off = 0, cons_off = 0;
    if (translate(tmo, "main", scratch, &prod_code, &prod_off) != 0) return 1;
    if (translate(tmo, "consumer", scratch, &cons_code, &cons_off) != 0) return 1;
    long long (*producer_fn)(void) = (long long (*)(void))(void *)(prod_code + prod_off);
    long long (*consumer_fn)(void) = (long long (*)(void))(void *)(cons_code + cons_off);

    struct sigaction sa = {0}, old_segv, old_ill, old_fpe;
    sa.sa_handler = crash_handler;
    sigaction(SIGSEGV, &sa, &old_segv);
    sigaction(SIGILL,  &sa, &old_ill);
    sigaction(SIGFPE,  &sa, &old_fpe);

    /* The battery: M producer threads hammer the shared counter and the
     * lock-free slot queue; one consumer thread drains it concurrently. */
    struct thread_arg *prods = calloc((size_t)M, sizeof(struct thread_arg));
    pthread_t *ptids = calloc((size_t)M, sizeof(pthread_t));
    pthread_t ctid;
    double t0 = now_sec();
    for (long long i = 0; i < M; i++) {
        prods[i].fn = producer_fn;
        if (pthread_create(&ptids[i], NULL, thread_main, &prods[i]) != 0) {
            perror("pthread_create producer"); return 1;
        }
    }
    struct thread_arg cons = { consumer_fn, 0 };
    if (pthread_create(&ctid, NULL, thread_main, &cons) != 0) {
        perror("pthread_create consumer"); return 1;
    }
    for (long long i = 0; i < M; i++) pthread_join(ptids[i], NULL);
    pthread_join(ctid, NULL);
    double secs = now_sec() - t0;

    sigaction(SIGSEGV, &old_segv, NULL);
    sigaction(SIGILL,  &old_ill,  NULL);
    sigaction(SIGFPE,  &old_fpe,  NULL);

    int ok = 1;
    uint64_t tid_cell  = *(uint64_t *)(scratch + OFF_TID);
    uint64_t enq_cell  = *(uint64_t *)(scratch + OFF_ENQ);
    uint64_t claim_cell= *(uint64_t *)(scratch + OFF_CLAIM);
    uint64_t deq_cell  = *(uint64_t *)(scratch + OFF_DEQ);
    uint64_t bad_cell  = *(uint64_t *)(scratch + OFF_BAD);
    uint64_t expected  = (uint64_t)M * (uint64_t)R;

    /* 1. The shared counter: every ATOMIC_ADD landed exactly once. */
    if (enq_cell != expected) {
        printf("FAIL  shared counter: enq = %llu, expected %llu (LOST UPDATES)\n",
               (unsigned long long)enq_cell, (unsigned long long)expected);
        ok = 0;
    } else {
        printf("PASS  shared counter: enq == M*R == %llu (no lost updates)\n",
               (unsigned long long)expected);
    }

    /* 2. Mutual exclusion on the CAS claim: exactly M*R claims succeeded. */
    if (claim_cell != expected) {
        printf("FAIL  queue claims: %llu, expected %llu (a claim was lost or double-claimed)\n",
               (unsigned long long)claim_cell, (unsigned long long)expected);
        ok = 0;
    } else {
        printf("PASS  queue claims: CAS next == M*R == %llu (every claim succeeded exactly once)\n",
               (unsigned long long)expected);
    }

    /* 3. The concurrent drainer saw every item exactly once. */
    if (deq_cell != expected) {
        printf("FAIL  consumer drain: deq = %llu, expected %llu\n",
               (unsigned long long)deq_cell, (unsigned long long)expected);
        ok = 0;
    } else {
        printf("PASS  consumer drain: deq == M*R == %llu (concurrent reader saw every slot)\n",
               (unsigned long long)expected);
    }
    if (bad_cell != 0) {
        printf("FAIL  consumer saw %llu out-of-range slot values (torn reads?)\n",
               (unsigned long long)bad_cell);
        ok = 0;
    } else {
        printf("PASS  consumer validation: 0 out-of-range values\n");
    }

    /* 4. The ticket leg: the M producer returns are a permutation of 0..M-1
     *    (each thread got a unique tid — itself an atomicity proof), and the
     *    tid counter agrees. */
    if (tid_cell != (uint64_t)M) {
        printf("FAIL  tid counter = %llu, expected %llu (two threads collided on a tid)\n",
               (unsigned long long)tid_cell, (unsigned long long)M);
        ok = 0;
    }
    {
        int *seen = calloc((size_t)M, sizeof(int));
        int perm_ok = 1;
        for (long long i = 0; i < M; i++) {
            long long t = prods[i].ret;
            if (t < 0 || t >= M || seen[t]) { perm_ok = 0; break; }
            seen[t] = 1;
        }
        if (tid_cell != (uint64_t)M || !perm_ok) {
            printf("FAIL  producer tids: not a unique 0..%lld permutation (atomicity broken)\n",
                   (long long)(M - 1));
            ok = 0;
        } else {
            printf("PASS  ticket leg: %lld producers each claimed a unique tid (0..%lld)\n",
                   (long long)M, (long long)(M - 1));
        }
        free(seen);
    }

    /* 5. The queue contents: every slot holds one producer's tid, and the
     *    per-tid histogram shows exactly R writes per thread — no slot
     *    double-claimed, no slot skipped, no write lost. */
    {
        uint64_t *vals = (uint64_t *)(scratch + OFF_VALS);
        long long *hist = calloc((size_t)M, sizeof(long long));
        int walk_ok = 1;
        for (uint64_t i = 0; i < expected; i++) {
            uint64_t v = vals[i];
            if (v >= (uint64_t)M) { walk_ok = 0; break; }
            hist[v]++;
        }
        int hist_ok = walk_ok;
        if (walk_ok)
            for (long long t = 0; t < M; t++)
                if (hist[t] != R) { hist_ok = 0; break; }
        if (!hist_ok) {
            printf("FAIL  slot walk: a slot was double-claimed, skipped, or a write was lost\n");
            ok = 0;
        } else {
            printf("PASS  slot walk: all %llu slots hold exactly one tid, %lld writes per thread\n",
                   (unsigned long long)expected, (long long)R);
        }
        free(hist);
    }

    /* 6. The consumer returned its full drain count. */
    if (cons.ret != (long long)expected) {
        printf("FAIL  consumer return = %lld, expected %lld\n",
               cons.ret, (long long)expected);
        ok = 0;
    } else {
        printf("PASS  consumer return == M*R == %lld\n", (long long)expected);
    }

    long long atomic_ops = 3 * M * R + M;   /* tid + enq + claim + deq (bad is 0) */
    printf("\nstress: %lld threads x %lld iters, %.3f s, %lld lock ops, %.1fM ops/s — %s\n",
           (long long)M, (long long)R, secs, atomic_ops,
           (double)atomic_ops / secs / 1e6, ok ? "ALL CHECKS PASSED" : "FAILED");
    return ok ? 0 : 1;
}
