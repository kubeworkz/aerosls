/*
 * sql_parser_host_test.c — Phase 19 (relational layer) verification: a
 * standalone host-buildable test for kernel/sql_parser.c's lexer +
 * recursive-descent parser, linked against the REAL, unmodified
 * kernel/sql_parser.c AND kernel/predicate.c (for its Predicate builder
 * functions the parser calls directly) — not a reimplementation.
 *
 * This is the "parser correctness is real-execution-testable in total
 * isolation" half the roadmap's own verification plan calls for:
 * sql_parser.c never looks up a table, evaluates a predicate, or touches
 * rowstore/row_index state at all, so the only stub needed is a one-line
 * rowstore_table_scan() the parser itself never calls (predicate.c links
 * against it, but predicate_table_scan() -- the only predicate.c function
 * that calls it -- is never exercised by this test; see sql_exec_host_
 * test.c for that). Zero fake NVMe, zero catalog, zero rowstore state.
 *
 * Build and run:
 *   gcc -Wall -Wextra -std=c11 -I . -I kernel -I drivers \
 *       -o /tmp/sql_parser_host_test \
 *       tests/sql_parser_host_test.c kernel/sql_parser.c kernel/predicate.c
 *   /tmp/sql_parser_host_test
 */
#include "kernel/object_catalog.h"
#include "kernel/rowstore.h"
#include "kernel/sql_parser.h"
#include <stdio.h>
#include <string.h>
#include <stdint.h>

// ─── The only stub this test needs: predicate.c links against
// rowstore_table_scan(), but nothing this test exercises ever calls it. ───
struct SLSObjectEntry  object_catalog[CATALOG_MAX_OBJECTS];
uint32_t               object_catalog_count = 0;
struct RowTableHeader  table_headers[ROWSTORE_MAX_TABLES];
uint32_t               rowstore_next_free_page_id = 0;
uint32_t rowstore_table_scan(uint32_t caller_uid, const char* table_name,
                             RowScanCb cb, void* ctx) {
    (void)caller_uid; (void)table_name; (void)cb; (void)ctx;
    return 0;
}

static int g_fail = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); g_fail++; } \
    else          { printf("ok:   %s\n", msg); } \
} while (0)

int main(void) {
    struct SqlStatement st;
    char err[SQL_ERR_MSG_LEN];

    /* ── SELECT: full grammar in one statement ────────────────────────────── */
    int rc = sql_parse("SELECT * FROM employees WHERE id = 25 AND active = TRUE "
                       "ORDER BY name DESC LIMIT 10", &st, err, sizeof(err));
    CHECK(rc == 0, "SELECT (full grammar) parses successfully");
    CHECK(st.kind == SQL_STMT_SELECT, "parsed as SQL_STMT_SELECT");
    CHECK(strcmp(st.u.select.table_name, "employees") == 0, "table_name = employees");
    CHECK(st.u.select.select_all == 1, "select_all = 1 for '*'");
    CHECK(st.u.select.has_where == 1, "has_where = 1");
    CHECK(st.u.select.has_order_by == 1 && strcmp(st.u.select.order_by, "name") == 0, "ORDER BY name captured");
    CHECK(st.u.select.order_desc == 1, "DESC captured");
    CHECK(st.u.select.has_limit == 1 && st.u.select.limit == 10, "LIMIT 10 captured");
    {
        /* WHERE tree shape: AND(id=25, active=true) */
        CHECK(st.u.select.where.node_count == 3, "WHERE tree has 3 nodes (two comparisons + one AND)");
        struct PredicateNode* root = &st.u.select.where.nodes[st.u.select.where.root];
        CHECK(root->kind == PRED_NODE_AND, "WHERE root is an AND node");
        struct PredicateNode* left = &st.u.select.where.nodes[root->left];
        struct PredicateNode* right = &st.u.select.where.nodes[root->right];
        CHECK(strcmp(left->column_name, "id") == 0 && left->op == PRED_OP_EQ && strcmp(left->literal, "25") == 0,
              "left child: id = 25");
        CHECK(strcmp(right->column_name, "active") == 0 && right->op == PRED_OP_EQ && strcmp(right->literal, "true") == 0,
              "right child: active = true (TRUE keyword lowercased to predicate.c's own 'true' convention)");
    }

    /* ── SELECT: explicit column list, lowercase keywords, float literal ──── */
    rc = sql_parse("select id, name from employees where salary >= 50000.5", &st, err, sizeof(err));
    CHECK(rc == 0, "lowercase SELECT with explicit columns parses");
    CHECK(st.u.select.select_all == 0 && st.u.select.column_count == 2, "column_count = 2, select_all = 0");
    CHECK(strcmp(st.u.select.columns[0], "id") == 0 && strcmp(st.u.select.columns[1], "name") == 0, "columns captured in order");
    {
        struct PredicateNode* root = &st.u.select.where.nodes[st.u.select.where.root];
        CHECK(root->kind == PRED_NODE_COMPARISON && root->op == PRED_OP_GE && strcmp(root->literal, "50000.5") == 0,
              "float literal '50000.5' parsed intact");
    }

    /* ── OR / AND precedence: OR(id=1, AND(active=true, salary<0)) ────────── */
    rc = sql_parse("SELECT * FROM t WHERE id = 1 OR active = TRUE AND salary < 0", &st, err, sizeof(err));
    CHECK(rc == 0, "mixed AND/OR parses");
    {
        struct PredicateNode* root = &st.u.select.where.nodes[st.u.select.where.root];
        CHECK(root->kind == PRED_NODE_OR, "root is OR (AND binds tighter, so OR is outermost)");
        struct PredicateNode* right = &st.u.select.where.nodes[root->right];
        CHECK(right->kind == PRED_NODE_AND, "OR's right child is the AND(active=true, salary<0) subtree");
    }

    /* ── Every comparison operator lexes correctly, including <> as a
     * synonym for != ──────────────────────────────────────────────────────── */
    {
        struct { const char* q; PredicateCompareOp expect; } ops[] = {
            {"SELECT * FROM t WHERE a = 1", PRED_OP_EQ},
            {"SELECT * FROM t WHERE a != 1", PRED_OP_NE},
            {"SELECT * FROM t WHERE a <> 1", PRED_OP_NE},
            {"SELECT * FROM t WHERE a < 1", PRED_OP_LT},
            {"SELECT * FROM t WHERE a > 1", PRED_OP_GT},
            {"SELECT * FROM t WHERE a <= 1", PRED_OP_LE},
            {"SELECT * FROM t WHERE a >= 1", PRED_OP_GE},
        };
        int all_ok = 1;
        for (size_t i = 0; i < sizeof(ops)/sizeof(ops[0]); i++) {
            struct SqlStatement s2;
            if (sql_parse(ops[i].q, &s2, err, sizeof(err)) != 0) { all_ok = 0; break; }
            struct PredicateNode* n = &s2.u.select.where.nodes[s2.u.select.where.root];
            if (n->op != ops[i].expect) { all_ok = 0; break; }
        }
        CHECK(all_ok, "all seven comparison-operator spellings (incl. <> as != synonym) lex correctly");
    }

    /* ── String literal with an escaped quote ('' -> ') ────────────────────── */
    rc = sql_parse("SELECT * FROM t WHERE name = 'O''Brien'", &st, err, sizeof(err));
    CHECK(rc == 0, "string literal with an escaped quote parses");
    {
        struct PredicateNode* n = &st.u.select.where.nodes[st.u.select.where.root];
        CHECK(strcmp(n->literal, "O'Brien") == 0, "'' inside a string literal correctly becomes a single literal quote");
    }

    /* ── INSERT ─────────────────────────────────────────────────────────────── */
    rc = sql_parse("INSERT INTO employees (id, name, active) VALUES (25, 'carol', TRUE)", &st, err, sizeof(err));
    CHECK(rc == 0, "INSERT parses");
    CHECK(st.kind == SQL_STMT_INSERT, "parsed as SQL_STMT_INSERT");
    CHECK(st.u.insert.count == 3, "3 column/value pairs captured");
    CHECK(strcmp(st.u.insert.columns[0], "id") == 0 && strcmp(st.u.insert.values[0], "25") == 0, "pair 0: id = 25");
    CHECK(strcmp(st.u.insert.columns[1], "name") == 0 && strcmp(st.u.insert.values[1], "carol") == 0, "pair 1: name = carol");
    CHECK(strcmp(st.u.insert.columns[2], "active") == 0 && strcmp(st.u.insert.values[2], "true") == 0, "pair 2: active = true");

    rc = sql_parse("INSERT INTO t (a, b) VALUES (1, 2, 3)", &st, err, sizeof(err));
    CHECK(rc == 1, "INSERT with mismatched column/value counts is rejected");

    /* ── UPDATE ─────────────────────────────────────────────────────────────── */
    rc = sql_parse("UPDATE employees SET name = 'bob', active = FALSE WHERE id = 25 OR id = 30", &st, err, sizeof(err));
    CHECK(rc == 0, "UPDATE parses");
    CHECK(st.kind == SQL_STMT_UPDATE, "parsed as SQL_STMT_UPDATE");
    CHECK(st.u.update.set_count == 2, "2 SET assignments captured");
    CHECK(strcmp(st.u.update.set_columns[0], "name") == 0 && strcmp(st.u.update.set_values[0], "bob") == 0, "SET name = bob");
    CHECK(strcmp(st.u.update.set_columns[1], "active") == 0 && strcmp(st.u.update.set_values[1], "false") == 0, "SET active = false");
    CHECK(st.u.update.has_where == 1, "WHERE captured");

    rc = sql_parse("UPDATE employees SET name = 'bob'", &st, err, sizeof(err));
    CHECK(rc == 0 && st.u.update.has_where == 0, "UPDATE without WHERE parses, has_where = 0");

    /* ── DELETE ─────────────────────────────────────────────────────────────── */
    rc = sql_parse("DELETE FROM employees WHERE id != 25", &st, err, sizeof(err));
    CHECK(rc == 0 && st.kind == SQL_STMT_DELETE, "DELETE parses");
    CHECK(st.u.del.has_where == 1, "DELETE's WHERE captured");

    rc = sql_parse("DELETE FROM employees", &st, err, sizeof(err));
    CHECK(rc == 0 && st.u.del.has_where == 0, "DELETE without WHERE (delete-everything) parses cleanly");

    /* ── Trailing semicolon is optional and ignored ───────────────────────── */
    rc = sql_parse("SELECT * FROM t;", &st, err, sizeof(err));
    CHECK(rc == 0, "trailing ';' is accepted");

    /* ── Syntax errors are caught, not silently misparsed ─────────────────── */
    CHECK(sql_parse("SELECT * FROM t WHERE", &st, err, sizeof(err)) == 1, "incomplete WHERE clause rejected");
    CHECK(sql_parse("SELECT FROM t", &st, err, sizeof(err)) == 1, "missing column list / '*' rejected");
    CHECK(sql_parse("SELECT * employees", &st, err, sizeof(err)) == 1, "missing FROM keyword rejected");
    CHECK(sql_parse("GARBAGE STATEMENT", &st, err, sizeof(err)) == 1, "unrecognized statement keyword rejected");
    CHECK(sql_parse("SELECT * FROM t WHERE id = 1 extra tokens here", &st, err, sizeof(err)) == 1, "trailing garbage after a complete statement rejected");
    CHECK(sql_parse("", &st, err, sizeof(err)) == 1, "empty input rejected");
    CHECK(err[0] != '\0', "a rejected parse leaves a non-empty error message");

    printf("\n%s\n", g_fail == 0 ? "ALL CHECKS PASSED" : "SOME CHECKS FAILED");
    return g_fail == 0 ? 0 : 1;
}
