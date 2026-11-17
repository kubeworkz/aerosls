/*
 * linux_catalog_exec_host_test.c — M5.2's kernel half of the Linux
 * SYS_SLS_CATALOG_EXEC path.
 *
 * ─── What this exists to prove ────────────────────────────────────────────
 * The syscall is a FIRST-TRAP gate before any catalog call: a null or bad
 * pointer, a zero/over-CAT_SQL_MAX length, a nonzero reserved flags word,
 * a NUL in the statement or a second statement must be refused before the
 * sidecar is posted. The refusal is a Linux errno on the task's own path,
 * so no sidecar CatalogOps::cat_exec can run and no transaction can open.
 *
 * The test is pure: linux_catalog_exec_plan() and
 * linux_catalog_sql_verdict() are the same functions the dispatcher calls.
 * The SQL bytes are the bytes the checked copy would place behind the
 * 72-byte description.
 *
 * Build and run:
 *   gcc -Wall -Wextra -std=c11 -I . -I kernel \
 *       -o /tmp/linux_catalog_exec_host_test \
 *       tests/linux_catalog_exec_host_test.c kernel/linux_abi.c
 *   /tmp/linux_catalog_exec_host_test
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "linux_abi.h"

static int checks_passed = 0;
static int checks_failed = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); checks_failed++; } \
    else         { printf("ok:   %s\n", msg); checks_passed++; } \
} while (0)

int main(void)
{
    /* ─── The number and class are the Linux table's, once ─────────────── */
    CHECK(SYS_SLS_CATALOG_EXEC == 334,
          "SYS_SLS_CATALOG_EXEC is 334 — next after the M4.5/M4.8 private numbers");
    CHECK(linux_syscall_class(SYS_SLS_CATALOG_EXEC) == LSC_CATALOG,
          "334 classifies as LSC_CATALOG, so the Linux branch describes it and\n"
          "      the non-Linux legacy chain refuses it by name");
    CHECK(strcmp(linux_syscall_class_name(LSC_CATALOG), "catalog") == 0,
          "the class has a stable log name");
    CHECK(linux_syscall_class(SYS_LINUX_TRAP_RESUME) == LSC_UNKNOWN,
          "the resume sentinel is still outside the table");

    /* ─── Personality: only the Linux branch may issue it ──────────────── */
    CHECK(linux_personality_intercepts(PROC_PERSONALITY_LINUX) == 1,
          "a Linux-personality task intercepts 334");
    CHECK(linux_personality_intercepts(PROC_PERSONALITY_SLS) == 0,
          "an SLS task does NOT intercept 334 — the legacy chain's named\n"
          "      CAP_ENOSYS arm is the only other answer");

    /* ─── The bounded plan: every first-trap refusal ────────────────────── */
    {
        struct LinuxCatalogExecPlan p;
        uint64_t ok[6] = { 0x1000, 13, 0, 0, 0, 0 };

        p = linux_catalog_exec_plan(SYS_SLS_CATALOG_EXEC, ok);
        CHECK(p.refuse == 0 && p.src == 0x1000 && p.len == 13,
              "a valid pointer/length/flags triplet plans the exact copy");
        CHECK(p.len <= LINUX_CATALOG_SQL_MAX,
              "the plan's length cannot exceed the service's SQL cap");

        p = linux_catalog_exec_plan(SYS_SLS_CATALOG_EXEC, NULL);
        CHECK(p.refuse == -LINUX_EFAULT,
              "a NULL argument capture is EFAULT, not a crash");

        ok[0] = 0;
        p = linux_catalog_exec_plan(SYS_SLS_CATALOG_EXEC, ok);
        CHECK(p.refuse == -LINUX_EFAULT,
              "a NULL SQL pointer is EFAULT before any post");

        ok[0] = 0xFFFF800000001000ULL;
        p = linux_catalog_exec_plan(SYS_SLS_CATALOG_EXEC, ok);
        CHECK(p.refuse == -LINUX_EFAULT,
              "a non-canonical SQL pointer is EFAULT before any post");

        ok[0] = LINUX_USER_ADDR_MAX;
        ok[1] = 2;
        p = linux_catalog_exec_plan(SYS_SLS_CATALOG_EXEC, ok);
        CHECK(p.refuse == -LINUX_EFAULT,
              "a span that runs past the user-canonical ceiling is EFAULT");

        ok[0] = 0x1000;
        ok[1] = 0;
        p = linux_catalog_exec_plan(SYS_SLS_CATALOG_EXEC, ok);
        CHECK(p.refuse == -LINUX_EINVAL,
              "zero length is EINVAL — an empty statement is not a call");

        ok[1] = LINUX_CATALOG_SQL_MAX + 1u;
        p = linux_catalog_exec_plan(SYS_SLS_CATALOG_EXEC, ok);
        CHECK(p.refuse == -LINUX_EINVAL,
              "a length over CAT_SQL_MAX is EINVAL before any post");

        ok[1] = 13;
        ok[2] = 1;
        p = linux_catalog_exec_plan(SYS_SLS_CATALOG_EXEC, ok);
        CHECK(p.refuse == -LINUX_EINVAL,
              "a nonzero reserved flags word is EINVAL — this cut has no flags");

        ok[2] = 0;
        p = linux_catalog_exec_plan(LINUX_WRITE, ok);
        CHECK(p.refuse == 0 && p.len == 0 && p.src == 0,
              "another syscall plans nothing here — the function is total");
    }

    /* ─── The copied bytes' verdict ────────────────────────────────────── */
    {
        const uint8_t insert[] = "INSERT INTO m52_bob (id, note) VALUES (1, 'x')";
        const uint8_t update[] = "UPDATE m52_bob SET note = 'y' WHERE id = 1";
        const uint8_t second[] = "INSERT INTO m52_bob (id) VALUES (1); DELETE FROM m52_bob";
        const uint8_t quoted_semi[] = "INSERT INTO m52_bob (note) VALUES ('a;b')";
        const uint8_t doubled_quote[] = "INSERT INTO m52_bob (note) VALUES ('it''s')";
        const uint8_t trailing_semi[] = "DELETE FROM m52_bob;";
        const uint8_t nul[] = { 'D', 'E', 'L', 'E', 'T', 'E', 0, 'x' };
        uint8_t over[LINUX_CATALOG_SQL_MAX + 1u];

        memset(over, 'x', sizeof(over));
        CHECK(linux_catalog_sql_verdict(insert, sizeof(insert) - 1u) == 0,
              "one INSERT statement passes the copied-bytes verdict");
        CHECK(linux_catalog_sql_verdict(update, sizeof(update) - 1u) == 0,
              "one UPDATE statement passes");
        CHECK(linux_catalog_sql_verdict(second, sizeof(second) - 1u) ==
                  -LINUX_EINVAL,
              "a statement followed by a second one is EINVAL — the first\n"
              "      cut cannot carry two statements into a transaction");
        CHECK(linux_catalog_sql_verdict(quoted_semi, sizeof(quoted_semi) - 1u) == 0,
              "a semicolon inside a single-quoted literal is DATA, not a\n"
              "      second-statement separator");
        CHECK(linux_catalog_sql_verdict(doubled_quote, sizeof(doubled_quote) - 1u) == 0,
              "'' is an escaped quote and does not end the literal early");
        CHECK(linux_catalog_sql_verdict(trailing_semi, sizeof(trailing_semi) - 1u) ==
                  -LINUX_EINVAL,
              "a trailing separator is refused too — one statement means one");
        CHECK(linux_catalog_sql_verdict(nul, sizeof(nul)) == -LINUX_EINVAL,
              "an embedded NUL is EINVAL — the service must never execute a\n"
              "      prefix of the bytes the caller believes it sent");
        CHECK(linux_catalog_sql_verdict(over, sizeof(over)) == -LINUX_EINVAL,
              "the byte verdict repeats the CAT_SQL_MAX bound");
        CHECK(linux_catalog_sql_verdict(NULL, 1) == -LINUX_EINVAL,
              "a NULL byte pointer is EINVAL, total rather than a crash");
        CHECK(linux_catalog_sql_verdict(insert, 0) == -LINUX_EINVAL,
              "zero bytes is EINVAL");
        CHECK(linux_catalog_sql_verdict(over, LINUX_CATALOG_SQL_MAX) == 0,
              "exactly the cap is accepted when every byte is valid");
    }

    printf("\n%d checks passed, %d failed\n", checks_passed, checks_failed);
    return checks_failed == 0 ? 0 : 1;
}
