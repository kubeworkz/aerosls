#ifndef SHELL_H
#define SHELL_H

#include <stdint.h>
#include <stddef.h>

// ─── Per-caller shell session state ────────────────────────────────────────
//
// Architectural Phase 2 (docs/AeroSLS-Architectural-MVP-Roadmap-v0.1.md).
//
// Used to be three file-scope globals in shell.c (current_session_uid,
// current_session_gid, current_tx_id) shared by every caller -- fine when
// there was only ever one execution path (the serial console), unsafe once
// Phase 1 let multiple HTTP requests be in flight at once, since two
// different users' identities/open transactions could stomp on each other.
//
// Now owned by the caller: sls_shell_loop() (the physical serial console)
// keeps one persistent instance for its single machine-wide session, exactly
// matching today's behavior. The HTTP shell-fallback route (net/http.c,
// api_shell_exec_post()) keeps one instance per authenticated uid, so a
// client's `tx begin` on one request is found by `tx commit` on a later
// request -- necessary since every HTTP request is its own short-lived TCP
// connection with nothing to hang session state off of -- without two
// different users ever being able to see each other's session, since uid is
// both the table's lookup key and reseeded from the bearer token on every
// call rather than trusted from anything stored.
//
// This gets its own header (breaking from this codebase's usual convention
// of a bare `extern` at the call site for one function -- see
// SHELL_EXEC_OUT_CAP's comment in shell.c) because a struct passed by
// pointer across a translation-unit boundary needs an identical layout on
// both sides; that's a correctness requirement a header enforces and a
// duplicated `extern` declaration does not.
struct ShellSession {
    uint32_t uid;
    uint32_t gid;
    uint64_t tx_id;   // 0 = no open transaction
};

// Runs one command line to completion against `sess`, capturing everything
// the command would normally print to the serial console into out_buf
// instead. Returns 1 if the command was recognized, 0 otherwise (see
// shell.c's own comment on sls_shell_execute() for the full contract).
int sls_shell_execute(const char* input_buffer, struct ShellSession* sess,
                      char* out_buf, size_t out_cap);

void sls_shell_loop(void);

#endif /* SHELL_H */
