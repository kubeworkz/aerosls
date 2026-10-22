/*
 * linux_syscall_table_host_test.c — Phase E7 increment 1, §5.6/§10.1: the
 * census artifact against the REAL kernel/linux_abi.c table.
 *
 * ─── What this exists to prove ────────────────────────────────────────────
 * §5.5 made the census the specification ("measured with strace, not
 * guessed") and §10.1 made this test the thing that makes it load-bearing
 * rather than decorative: every number in tools/linux_syscall_census.txt
 * — traced rows AND the declared vDSO backstop — must have a dispatch
 * class, and the class must be the destination §5.6 names. Delete a table
 * entry and this test reddens naming the missing number.
 *
 * The second property is the number-space one (§2.2): the SLS numbers the
 * Linux ABI overlaps — 105 (setuid / SYS_SLS_ALLOCATE), 160, 320
 * (kexec_file_load / SYS_SLS_ALLOC_REGION) — must be LSC_UNKNOWN in this
 * table. No entry, no semantics, no path to a SYS_ arm: that is what
 * makes D1's routing structural instead of a filter.
 *
 * Build and run:
 *   gcc -Wall -Wextra -std=c11 -I . -I kernel \
 *       -o /tmp/linux_syscall_table_host_test \
 *       tests/linux_syscall_table_host_test.c kernel/linux_abi.c
 *   /tmp/linux_syscall_table_host_test
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>

#include "linux_abi.h"

static int checks_passed = 0;
static int checks_failed = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); checks_failed++; } \
    else         { printf("ok:   %s\n", msg); checks_passed++; } \
} while (0)

#define CENSUS "tools/linux_syscall_census.txt"

int main(void)
{
    /* ─── Parse the artifact: data rows and the vdso-backstopped rows ───── */
    FILE *f = fopen(CENSUS, "r");
    uint64_t nums[64];
    char names[64][64];
    int is_vdso[64];
    int n = 0;
    char line[512];

    CHECK(f != NULL, "the census artifact opens (run from the repo root)");
    if (!f) {
        printf("linux_syscall_table_host_test: cannot continue without %s\n", CENSUS);
        return 1;
    }
    while (fgets(line, sizeof(line), f)) {
        unsigned long num;
        char name[64];
        long count;
        if (line[0] == '#') {
            if (sscanf(line, "# vdso-backstopped: %lu %63s", &num, name) == 2) {
                if (n < 64) {
                    nums[n] = num;
                    snprintf(names[n], sizeof(names[n]), "%s", name);
                    is_vdso[n] = 1;
                    n++;
                }
            }
            continue;
        }
        if (sscanf(line, "%lu %63s %ld", &num, name, &count) == 3) {
            if (n < 64) {
                nums[n] = num;
                snprintf(names[n], sizeof(names[n]), "%s", name);
                is_vdso[n] = 0;
                n++;
            }
        }
    }
    fclose(f);

    CHECK(n == 27, "the artifact parses to exactly 27 syscall numbers (26 traced + 1 declared vDSO backstop)");

    /* ─── Every artifact number has a dispatch class ─────────────────────── */
    {
        int missing = 0;
        int i;
        for (i = 0; i < n; i++) {
            if (linux_syscall_class(nums[i]) == LSC_UNKNOWN) {
                printf("FAIL: census number %lu (%s%s) has no dispatch-table entry\n",
                       nums[i], names[i], is_vdso[i] ? ", declared vDSO backstop" : "");
                missing++;
            }
        }
        CHECK(missing == 0,
              "every census number — traced and declared — has a non-UNKNOWN class");
    }

    /* ─── The class of each number is the destination §5.6 names ────────── */
    struct { uint64_t num; enum LinuxSyscallClass cls; const char *name; } want[] = {
        {   0, LSC_FD,    "read" },
        {   1, LSC_FD,    "write" },
        {   2, LSC_FD,    "open" },
        {   3, LSC_FD,    "close" },
        {   4, LSC_FD,    "stat" },
        {   6, LSC_FD,    "lstat" },
        {   7, LSC_FD,    "poll" },
        {   8, LSC_FD,    "lseek" },
        {   9, LSC_MEM,   "mmap" },
        {  10, LSC_MEM,   "mprotect" },
        {  11, LSC_MEM,   "munmap" },
        {  12, LSC_MEM,   "brk" },
        {  13, LSC_SIG,   "rt_sigaction" },
        {  14, LSC_SIG,   "rt_sigprocmask" },
        {  59, LSC_SPAWN, "execve" },
        {  72, LSC_FD,    "fcntl" },
        {  74, LSC_FD,    "fsync" },
        {  77, LSC_FD,    "ftruncate" },
        {  82, LSC_FD,    "rename" },
        {  83, LSC_FD,    "mkdir" },
        {  87, LSC_FD,    "unlink" },
        { 131, LSC_SIG,   "sigaltstack" },
        { 158, LSC_MSR,   "arch_prctl" },
        { 217, LSC_FD,    "getdents64" },
        { 218, LSC_LOCAL, "set_tid_address" },
        { 228, LSC_LOCAL, "clock_gettime" },
        { 231, LSC_EXIT,  "exit_group" },
    };
    size_t i;
    for (i = 0; i < sizeof(want) / sizeof(want[0]); i++) {
        enum LinuxSyscallClass got = linux_syscall_class(want[i].num);
        char msg[128];
        snprintf(msg, sizeof(msg), "%s (%lu) classifies as \"%s\"",
                 want[i].name, want[i].num, linux_syscall_class_name(want[i].cls));
        CHECK(got == want[i].cls, msg);
    }

    /* ─── §5.4's W^X refusal is pinned against the census's own row ─────── */
    CHECK((linux_syscall_flags(10) & LSC_F_REFUSES_WX) != 0,
          "mprotect carries LSC_F_REFUSES_WX — H2's Node exclusion as a table property (§5.4)");
    CHECK(linux_syscall_flags(9) == 0,
          "mmap does not carry the W^X flag (the refusal belongs to mprotect alone)");
    CHECK(linux_syscall_flags(999) == 0,
          "an unknown number carries no flags");

    /* ─── The number-space property (§2.2): overlaps are UNKNOWN ────────── */
    CHECK(linux_syscall_class(105) == LSC_UNKNOWN,
          "105 (SLS SYS_SLS_ALLOCATE / Linux setuid) is UNKNOWN: the Linux table cannot reach an SLS arm");
    CHECK(linux_syscall_class(160) == LSC_UNKNOWN,
          "160 (SLS SYS_SLS_PROC_CREATE) is UNKNOWN");
    CHECK(linux_syscall_class(320) == LSC_UNKNOWN,
          "320 (SLS SYS_SLS_ALLOC_REGION / Linux kexec_file_load) is UNKNOWN");
    CHECK(linux_syscall_class(200) == LSC_UNKNOWN,
          "any number outside the census is UNKNOWN — refusal by name, never dispatch");

    /* ─── Table hygiene: sorted, self-consistent, total names ───────────── */
    {
        int sorted = 1, dup = 0, named = 1;
        size_t j;
        for (j = 1; j < linux_syscall_table_len; j++) {
            if (linux_syscall_table[j].num <= linux_syscall_table[j - 1].num)
                sorted = 0;
        }
        for (j = 0; j < linux_syscall_table_len; j++) {
            if (strcmp(linux_syscall_class_name(linux_syscall_table[j].cls), "unknown") == 0)
                named = 0;
            if (linux_syscall_table[j].cls == LSC_UNKNOWN)
                dup = 1;  /* an UNKNOWN row would make the table lie about itself */
        }
        CHECK(sorted, "the table is strictly sorted by number (binary-search-ready, no duplicates)");
        CHECK(!dup, "no row classifies itself as UNKNOWN (a table entry means dispatchable)");
        CHECK(named, "every row's class has a real name");
        CHECK(strcmp(linux_syscall_class_name(LSC_UNKNOWN), "unknown") == 0,
              "LSC_UNKNOWN's name is \"unknown\" (the refusal line's wording)");
        CHECK(linux_syscall_table_len == sizeof(want) / sizeof(want[0]),
              "the table's length equals the census's row count");
    }

    printf("linux_syscall_table_host_test: %d passed, %d failed\n",
           checks_passed, checks_failed);
    return checks_failed == 0 ? 0 : 1;
}
