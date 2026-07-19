/*
 * sql_parser.h — Phase 19 (relational layer): a hand-written lexer +
 * recursive-descent parser for AeroSLS's minimal SQL subset. See
 * docs/AeroSLS-RDBMS-Roadmap-v0.1.md §6 for the scoped grammar and the
 * "Findings addendum" for what's built here.
 *
 * Hand-rolled, not generated — matching this project's established style
 * (the TIMI assembler in Phase 1 is hand-rolled too). Zero dependency on
 * rowstore.h/row_index.h: this file only builds a `struct SqlStatement`
 * from text; it never looks up a table, evaluates anything, or touches the
 * kernel's actual data. That split is deliberate, not incidental — it's
 * what makes the parser real-execution-testable in total isolation, the
 * same "parser correctness is testable with zero kernel dependencies"
 * verification split the roadmap calls for. See sql_exec.h for the
 * planner/executor that actually runs a parsed statement.
 *
 * ─── Grammar (genuinely minimal, per the roadmap's own scope) ───────────────
 *   select_stmt := SELECT select_list FROM ident [join_clause] [WHERE predicate]
 *                  [ORDER BY column_ref [ASC|DESC]] [LIMIT number] [;]
 *   select_list := '*' | column_ref (',' column_ref)*
 *   join_clause := JOIN ident ON column_ref '=' column_ref
 *   insert_stmt := INSERT INTO ident '(' ident (',' ident)* ')'
 *                  VALUES '(' literal (',' literal)* ')' [;]
 *   update_stmt := UPDATE ident SET ident '=' literal (',' ident '=' literal)*
 *                  [WHERE predicate] [;]
 *   delete_stmt := DELETE FROM ident [WHERE predicate] [;]
 *   predicate   := and_expr (OR and_expr)*
 *   and_expr    := comparison (AND comparison)*
 *   comparison  := column_ref compare_op literal
 *   compare_op  := '=' | '!=' | '<>' | '<' | '>' | '<=' | '>='
 *   column_ref  := ident ['.' ident]      -- Phase 20: "table.column" when
 *                                            qualifying is needed (always
 *                                            required in the ON clause,
 *                                            since bare column names in a
 *                                            joined result are ambiguous by
 *                                            construction -- see sql_exec.h)
 *   literal     := number | 'string' | TRUE | FALSE
 *
 * `predicate` has no explicit parenthesized grouping in this first cut —
 * AND binds tighter than OR (standard SQL operator precedence), which
 * `and_expr` nested inside `predicate` already gives for free without
 * needing user-supplied parens; only EXPLICIT grouping beyond that natural
 * precedence is deferred, matching this roadmap's stated non-goal of a
 * full SQL expression language. The predicate itself is built directly as
 * a `struct Predicate` (predicate.h, Phase 18) via its own add_comparison/
 * add_and/add_or primitives — no second expression-tree type invented.
 *
 * Phase 20's JOIN is deliberately narrow: exactly two tables (`FROM A JOIN
 * B ON ...`, no chaining a second JOIN onto the result), INNER JOIN only
 * (no LEFT/RIGHT/FULL OUTER), and no table aliasing -- ON/SELECT-list/
 * WHERE/ORDER BY column qualifiers must be the tables' own full names
 * (`employees.id`, not `e.id`). A `column_ref` is always allowed to be
 * qualified (`table.column`) even outside a JOIN, for grammar uniformity,
 * though a non-join query has no ambiguity to resolve and a bare name is
 * the normal case there.
 *
 * Explicitly out of scope, per the roadmap: three-or-more-table joins,
 * OUTER JOIN, join reordering, hash/merge join, subqueries, views, GROUP
 * BY, arithmetic expressions, string functions, LIKE, IN lists,
 * parenthesized WHERE grouping (above), table aliasing (above). INSERT
 * requires an explicit column list covering every one of the table's
 * columns (no partial-row/NULL/default support yet — every value must be
 * supplied, matching rowstore_row_insert()'s own existing all-columns-
 * required contract).
 */
#ifndef SQL_PARSER_H
#define SQL_PARSER_H

#include <stdint.h>
#include "object_catalog.h"
#include "rowstore.h"
#include "predicate.h"

// ─── Limits ─────────────────────────────────────────────────────────────────
#define SQL_MAX_TEXT_LEN      512   // max input SQL statement length this parser accepts
#define SQL_ERR_MSG_LEN       128

typedef enum {
    SQL_STMT_SELECT = 0,
    SQL_STMT_INSERT,
    SQL_STMT_UPDATE,
    SQL_STMT_DELETE,
    SQL_STMT_INVALID,
} SqlStmtKind;

struct SqlSelectStmt {
    char     table_name[OBJECT_NAME_LEN];

    // ── Phase 20: JOIN (zero-default -- has_join==0 keeps every Phase 19
    // query's behavior identical to before this phase). join_left_*/
    // join_right_* hold the ON clause's two "qualifier.column" halves
    // exactly as written (in whatever order the user wrote them); the
    // executor resolves which one refers to table_name vs. join_table --
    // see sql_exec.h's header comment for why parsing doesn't try to. ─────
    uint8_t  has_join;
    char     join_table[OBJECT_NAME_LEN];
    char     join_left_qualifier[OBJECT_NAME_LEN];
    char     join_left_col[RECORD_KEY_LEN];
    char     join_right_qualifier[OBJECT_NAME_LEN];
    char     join_right_col[RECORD_KEY_LEN];

    uint8_t  select_all;                                       // '*'
    char     columns[ROWSTORE_MAX_COLUMNS][RECORD_KEY_LEN];     // meaningful iff !select_all; may be
                                                                 // "table.column" qualified when has_join
    uint32_t column_count;
    uint8_t  has_where;
    struct Predicate where;                                     // comparison column_names may be qualified when has_join
    uint8_t  has_order_by;
    char     order_by[RECORD_KEY_LEN];                          // may be qualified when has_join
    uint8_t  order_desc;                                        // 0 = ASC (default), 1 = DESC
    uint8_t  has_limit;
    uint32_t limit;
};

struct SqlInsertStmt {
    char     table_name[OBJECT_NAME_LEN];
    char     columns[ROWSTORE_MAX_COLUMNS][RECORD_KEY_LEN];
    char     values[ROWSTORE_MAX_COLUMNS][RECORD_VAL_LEN];
    uint32_t count;   // columns/values pair count -- must equal the table's full column count at exec time
};

struct SqlUpdateStmt {
    char     table_name[OBJECT_NAME_LEN];
    char     set_columns[ROWSTORE_MAX_COLUMNS][RECORD_KEY_LEN];
    char     set_values[ROWSTORE_MAX_COLUMNS][RECORD_VAL_LEN];
    uint32_t set_count;
    uint8_t  has_where;
    struct Predicate where;
};

struct SqlDeleteStmt {
    char    table_name[OBJECT_NAME_LEN];
    uint8_t has_where;
    struct Predicate where;
};

struct SqlStatement {
    SqlStmtKind kind;
    union {
        struct SqlSelectStmt select;
        struct SqlInsertStmt insert;
        struct SqlUpdateStmt update;
        struct SqlDeleteStmt del;
    } u;
};

// Parses one SQL statement from text (case-insensitive keywords; a single
// trailing ';' is optional and ignored). Returns 0 on success, filling
// *out. Returns 1 on a syntax/scope error, filling err[0..err_max) with a
// short human-readable message (e.g. "expected FROM", "unknown statement
// keyword", "too many columns") and leaving *out in an undefined state.
// Pure function: no globals, no table lookups, no I/O -- text in, a typed
// statement struct out, nothing else.
int sql_parse(const char* text, struct SqlStatement* out, char* err, uint32_t err_max);

#endif /* SQL_PARSER_H */
