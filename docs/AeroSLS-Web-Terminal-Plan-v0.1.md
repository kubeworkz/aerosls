# AeroSLS Web Terminal — Implementation Plan v0.1

## 1. What this is

A new "Terminal" tab in the slsos-sim web app where a user types shell-style commands (`valloc foo DB_TABLE 2`, `partition list`, `sql SELECT * FROM users`, ...) and sees output, styled and behaving like a real command line, without needing the QEMU serial console.

## 2. The real constraint this plan is built around

`user/shell.c` (~1,765 lines, ~90 commands) is a single blocking loop, `sls_shell_loop()`, invoked once at kernel boot (`kernel/kernel.c:321`). It reads lines from the serial UART and writes output straight to the serial console via `kernel_serial_print`/`kernel_serial_printf` — there is no reusable "take a command string, return an output string" function anywhere in the codebase. The one thing that looks like an HTTP-facing shell entry point, `route_sls_shell_command()` (referenced in `arch/riscv/sbi.c:19,42`), has **no definition anywhere** — it's dead code left over from an earlier phase, confirmed by a codebase-wide grep.

So "add a terminal" cannot mean "pipe keystrokes to the existing shell" today. Two real paths exist:

- **A. Client-side command router** — the terminal UI parses what's typed and maps it onto the ~70-80 existing REST routes in `net/http.c` (valloc, schema, sql, vec/*, partition/*, agent/*, ...). Zero kernel changes. Ships fast. Covers most of the command surface, since most of shell.c's commands already have a syscall-backed HTTP route.
- **B. Kernel-side shell refactor** — split `sls_shell_loop()`'s command dispatch out into a real callable function (string in, string out), keep the serial loop as a thin wrapper around it, add a new HTTP route that calls it. One true implementation instead of two. Bigger change (touches ~90 command branches in core kernel C) and — like every RISC-V/kernel-C change in this project's history — can't be fully compile-verified end-to-end in this sandbox; verification would lean on the same careful-review-plus-partial-compile-check discipline used for the RISC-V trap work.

**Decision: start with A.** B is real, valuable follow-on work, not attempted in this pass — flagged explicitly in §7 rather than silently dropped.

## 3. Command coverage (A) — what's real today

Checked directly against `net/http.c`'s route table (not assumed from the shell's command list). "Have" means a route exists and can be called with roughly the same arguments the shell command takes.

**Have (map directly to an existing route):**
`ls` / `ls objects` → `GET /api/objects`; `valloc` → `POST /api/valloc`; `schema set`/`schema show` → `POST /api/schema` / part of `GET /api/tables/:name/schema`; `table create` → `POST /api/tables`; `sql` → `POST /api/sql`; `stat` → `GET /api/objects/:name` (or similar); `query`/`query scan` → `GET /api/query`; `tier list` → `GET /api/tiers`; `tx begin/commit/rollback` → `POST /api/tx/begin|commit|rollback`; `cursor open/fetch/close/list` → `/api/cursor/*` + `GET /api/cursors`; `index create/drop/rebuild/list` → `/api/index/*` + `GET /api/indexes`; `constraint add/remove/list` → `/api/constraint/*` + `GET /api/constraints`; `journal attach/detach/list` → `/api/journal/attach|detach` + `GET /api/journals`; `mqt create/drop/refresh/list` → `/api/mqt/*` + `GET /api/mqts`; `aggregate` → `POST /api/aggregate`; `svc list` → `GET /api/services`; `lock list` → `GET /api/locks`; `wal dump` → `GET /api/wal`; `vec create/insert/search/join/list`, `vec index create/search/list` → `/api/vec/*`; `partition create/list/assign/destroy/pause/resume/quota/quotas` → `POST/GET /api/partitions`, `/api/partition/*`; `proc list` → `GET /api/processes`; `program create/upload/spawn` (shell's rough equivalent) → `/api/program/*`; `stream create/upload` (new, not a shell command but same shape) → `/api/stream/*`; `agent create/list/run/schedule/unschedule` → `/api/agent/*` + `GET /api/agents`; `workflow create/list/run` → `/api/workflow/*` + `GET /api/workflows`; `simi info` → `GET /api/simi/:name`.

**Partial (a route exists but the mapping isn't 1:1 — needs a small adapter, not a new kernel route):**
`insert`/`update`/`delete`/`select` (legacy KV record ops) → all fold through `POST /api/record` / `GET /api/objects/:name` with different body shapes; `agent kill` → shell has this, the HTTP route is named `/api/agent/drop` (naming mismatch, not a missing feature — the router just needs to know `kill` → `drop`).

**Corrections found during Task 1 implementation (2026-07-20):** this section originally listed `journal dump`, `index scan`, `mqt scan`, and `agent status` as missing/partial. Reading `net/http.c` more closely while writing the actual handlers turned up real routes for all four (`GET /api/journal/:name`, `GET /api/index/:name?q=`, `GET /api/mqt/:name`, `GET /api/agent/:name`) — moved to "Have" below. The opposite correction also surfaced: `vfree` was listed below as destructive-but-implementable in §6, but a direct grep for `vfree|valloc_free|object_free|DELETE` across `net/http.c` returned no matches — there is no object-deallocation HTTP route at all. `vfree` moves to "Missing."

**Have (map directly to an existing route):**
`ls` / `ls objects` → `GET /api/objects`; `valloc` → `POST /api/valloc`; `stat` → `GET /api/objects/:name`; `schema set`/`schema show` → `POST /api/schema` / `GET /api/tables/:name/schema`; `table create` → `POST /api/tables`; `sql` → `POST /api/sql`; `query`/`query scan` → `GET /api/query` / `GET /api/scan`; `tier list` → `GET /api/tiers`; `tx begin/commit/rollback` → `POST /api/tx/begin|commit|rollback`; `cursor open/fetch/close/list` → `POST /api/cursor/open`, `GET /api/cursor/fetch|close`, `GET /api/cursors`; `index create/drop/rebuild/list/scan` → `/api/index/*` + `GET /api/indexes` + `GET /api/index/:name?q=`; `constraint add/remove/list` → `/api/constraint/*` + `GET /api/constraints`; `journal attach/detach/list/dump` → `/api/journal/attach|detach` + `GET /api/journals` + `GET /api/journal/:name`; `mqt create/drop/refresh/list/scan` → `/api/mqt/*` + `GET /api/mqts` + `GET /api/mqt/:name`; `aggregate` → `POST /api/aggregate`; `svc list` → `GET /api/services`; `lock list` → `GET /api/locks`; `wal dump` → `GET /api/wal`; `vec create/insert/embed-insert/search/join/list`, `vec index create/search/list` → `/api/vec/*`; `partition create/list/assign/destroy/pause/resume/quota/quotas` → `POST/GET /api/partitions`, `/api/partition/*`; `proc list` → `GET /api/processes`; `program create/upload/spawn` → `/api/program/*`; `stream create/upload` → `/api/stream/*`; `agent create/list/status/run/schedule/unschedule/kill` → `/api/agent/*` + `GET /api/agents` + `GET /api/agent/:name` (`kill` → `/api/agent/drop`); `workflow create/list/run/status` → `/api/workflow/*` + `GET /api/workflows` + `GET /api/workflow/:name`; `simi info` → `GET /api/simi/:name`.

**Missing (no HTTP route exists at all — these commands will show "not available over the web yet" in the terminal, not silently fail):**
`login` (session concept doesn't map to this app's fixed-bearer-token model anyway), `role set`, `grant`, `revoke`, `chmod` (permission mutation), `auth create/list/revoke` (token management), `seal`, `write` (legacy raw heap write), `demo`/`upload`/`load`/`loader list` (legacy loader path, superseded by `/api/program/*` and `/api/stream/*` for anything that matters today), `svc crash`/`svc restart` (service fault injection/restart), `proc kill` (no kill route exists, unlike `program spawn`), `ipc post`/`ipc stat`, `journal create`/`journal purge`, `tier demote`/`tier promote` (no promote/demote route; only the `GET /api/tiers` listing exists), `vfree` (no object-deallocation route — corrected, see above), `workflow addstep`, `webapp set/list/append`.

That's roughly 24 of ~90 commands with no backend to call. The terminal will list these in its own `help` output as "not yet supported over HTTP" rather than pretending they work.

## 4. UI design

New top-level tab, "Terminal," alongside the existing `memory`/`security`/`transactions`/`microkernel`/`coprocessor`/`dbengine`/`portal`/`agents`/`workflows` tabs in `App.tsx` — a system-wide shell belongs at that level, not nested inside DB Engine, since it spans partitions/agents/vectors/programs too, not just the database.

New component, `src/components/SlsTerminal.tsx`, following the established dark-panel/monospace styling (`#0B0E14` background, cyan-400 accents, `font-mono`) already used by the SQL Console and WAL viewer:

- Scrollback output area (array of `{ type: "input" | "output" | "error" | "confirm"; text: string }` lines, auto-scrolled to bottom).
- Single-line prompt input, styled like a real shell prompt (`aerosls$ `).
- Command history via Up/Down arrows (reuse the pattern already in `SqlConsole`'s 10-entry history, extended to the terminal's full session).
- `Tab` does nothing fancy in v1 (no autocomplete) — out of scope, noted as a natural follow-on.
- `help` (client-side, always available) lists every recognized command, grouped by category, and marks the ~20 unsupported ones.
- `clear` clears scrollback.

## 5. Command router design

A single module, `src/lib/shellCommands.ts`, exporting a registry:

```ts
type CommandHandler = (args: string[]) => Promise<string>;
const COMMANDS: Record<string, CommandHandler> = { ... };
```

Each entry parses its own `args` (mirroring shell.c's own per-command parsing, just in TS instead of hand-rolled C pointer-walking) and calls `authFetch`/`kFetch`-equivalent against the matching route, formatting the JSON response back into shell-style text output (e.g. `ls` renders the objects array as a column table, matching what `ls` prints on the real serial console today, not just a JSON dump).

Unknown commands: `command not found: <word> (try 'help')`. Commands in the "Missing" list from §3: a specific, honest message — `'<cmd>' has no web equivalent yet — see docs/AeroSLS-Web-Terminal-Plan-v0.1.md §3` — rather than a generic error, so it's clear this is a known, documented gap and not a bug.

## 6. Destructive-command confirmation

Per the chosen design: destructive commands execute normally but require inline confirmation first, the same shape a real shell's `rm -i` uses. Flagged as destructive: `proc kill`\*, `svc crash`\*, `svc restart`\*, `partition destroy`, `vfree`\*, `delete`, `index drop`, `constraint remove`, `journal detach`, `mqt drop`, `agent kill`/`agent drop`, `role set`\*, `grant`\*, `revoke`\*, `chmod`\*, `auth revoke`\* (\* = currently in the "Missing" list from §3 — flagged now so the confirmation gate is already in place the moment B or a targeted kernel route closes that gap, not bolted on later; `vfree` was originally assumed implementable and has moved into this starred group after Task 1 confirmed it has no route).

Flow: typing a destructive command doesn't execute it — it prints `Confirm: <full command>? [y/N]` and the *next* line typed is interpreted as the confirmation (`y`/`yes` executes the pending command, anything else cancels), then the prompt returns to normal. This needs a small piece of terminal state (`pendingConfirm: string | null`) rather than a `window.confirm()` popup, to stay in-terminal and scriptable-feeling.

## 7. Explicitly out of scope for this pass

- **The kernel-side shell refactor (§2, option B)** — the ~20 commands in the "Missing" list stay missing until that separate, larger piece of work happens. Not silently dropped: tracked here as the named follow-on.
- **Tab-completion / command history persistence across page reloads** — nice-to-haves, not needed for a working v1.
- **Piping/scripting (`;`, `&&`, output redirection)** — shell.c itself doesn't support this either (each line is one command), so the terminal matches that, not exceeds it.
- **A real pty/xterm.js integration** — no terminal-emulator library exists in `package.json` today (confirmed) and none is needed: this is a line-based request/response console, not an interactive TTY program (no `vim`-in-the-browser scenario exists on the kernel side to justify one).

## 8. Verification plan

- Every command handler in `shellCommands.ts` gets a small host-side test (Vitest or a plain Node script, matching how this project has always preferred an actual-execution check over review-only) asserting: given a command string, the right route + method + body shape is produced, using mocked `fetch`.
- Manual pass once deployed: run through every command in the "Have" category from a real browser session against the live kernel, confirm output formatting is readable (not raw JSON dumped into the terminal).
- Confirm the "Missing" list's error message actually fires for each of the ~20 unsupported commands, rather than silently no-op'ing.
- Confirm the destructive-confirmation flow can't be bypassed (typing the destructive command followed immediately by unrelated text cancels rather than executes).

## 9. Implementation order

1. `src/lib/shellCommands.ts` — registry + parser + router, covering the full "Have" list from §3.
2. `src/components/SlsTerminal.tsx` — UI shell: scrollback, prompt, history, `help`/`clear`.
3. Wire destructive-command confirmation state machine.
4. Wire the "Missing" list's honest not-supported messaging.
5. Add the "Terminal" tab to `App.tsx`'s top-level nav.
6. Host-side tests for the command router.
7. Manual verification pass (§8) once built + deployed (`npm run build` → `make bundle` → kernel restart, same deploy step as every other frontend change this session).

Status: 1-6 done and verified this pass (real `tsc --noEmit` typecheck plus a 43-assertion mocked-fetch host test, both clean). Item 7 is blocked on the user's own machine — this sandbox's `node_modules` has Windows-only native binaries for both esbuild and rollup, so `npm run build` cannot run here.

## 10. Kernel-Side Shell Refactor (Option B) — Design

Full read of `user/shell.c` (all 1,765 lines, not just the command-name grep §2 was based on) plus `kernel/kernel_io.c`/`.h` and a representative `net/http.c` route handler (`api_sql_post`). Findings below are what the design in this section is built on.

### 10.1 The dispatch body extracts cleanly — with one exception

Every one of the ~90 commands lives in a single `if/else-if` chain keyed off a local 256-byte `char input_buffer[256]` in `sls_shell_loop()`. Across the whole file, exactly one command breaks a pure "take a string, return a string" contract: `seal <name>` prints "Enter encryption password: " and then calls `read_line(pw)` a *second* time to interactively read a password on its own line — the only nested `read_line()` call anywhere outside the loop's own top-of-loop read. No other command reads more than the one line it's given, and nothing ever mutates `input_buffer` itself (every command only reads through pointer offsets), so passing a `const char*` into the new function is safe everywhere else.

**Decision:** change `seal`'s calling convention from an interactive two-line prompt to a single-line `seal <name> <password>`, identically for both the serial console and the new HTTP path (one shared function, so the two entry points can't diverge anyway). This is a deliberate, minimal syntax change — not a silent drop — and it costs nothing security-wise: `seal`'s own existing doc comment already states it "does NOT encrypt its data," so this was never a real secret-handling path to begin with.

### 10.2 Output capture: one choke point, not 602 call sites

`kernel_serial_print`/`kernel_serial_printf` are called 602 times across 38 kernel files — output is not localized to `shell.c`, it's scattered through `object_catalog.c`, `journal.c`, `cursor.c`, `mqt.c`, `agent.c`, and everywhere else a command's real logic lives. Redirecting all 602 call sites was the naive approach and not what this design does. Instead: every one of those 602 calls, and `kernel_serial_printf`'s own internal `emit_uint`/`emit_str` helpers, bottom out in exactly one function — `kernel_serial_putchar()` in `kernel/kernel_io.c` — which is the only place that actually touches the UART port and mirrors to VGA. That's the single choke point this design redirects.

New capture API in `kernel_io.h`/`.c`:

```c
void   kernel_serial_capture_start(char* buf, size_t cap);  // begin redirecting
size_t kernel_serial_capture_stop(void);                    // stop, NUL-terminate, return length written
```

`kernel_serial_putchar()` gains a branch: when a capture buffer is active, it appends the byte there (bounds-checked, silently truncating past `cap`) and returns immediately — no UART wait-loop, no VGA mirror. Serial-console behavior when capture is *not* active is unchanged (this is an additive branch, not a rewrite of the existing hardware path). Capture is suppressed-to-buffer rather than tee'd to both buffer and hardware: an HTTP-triggered command shouldn't block on real UART timing, and mixing web-terminal output into the physical serial transcript would be confusing, not helpful.

This means `sls_shell_execute()` doesn't need to touch a single one of the 602 existing print call sites, in any of the 38 files. That was the main risk this section's investigation was for, and it turned out not to exist.

### 10.3 Function signature and session state

```c
// user/shell.c
int sls_shell_execute(const char* input, char* out_buf, size_t out_cap);
// returns 1 if input matched a known command (output text was produced by
// that command, whatever it is — this does NOT mean the underlying
// operation succeeded, only that it was recognized and ran; most commands
// only communicate real success/failure through the printed text itself,
// same as a human reading the serial console today), 0 for an unrecognized
// non-empty command (output text is the "Unknown command: '...'" message),
// or 0 with empty output for an empty/whitespace input (matches the loop's
// existing silent no-op behavior).
```

`sls_shell_loop()` becomes a thin wrapper:

```c
void sls_shell_loop(void) {
    char input_buffer[256];
    static char output_buffer[SHELL_EXEC_OUT_CAP];   // see 10.5 for sizing
    kernel_serial_print("\n--- Multi-User SLS Secure Shell Active ---\n");
    kernel_serial_print("Type 'help' for available commands.\n\n");
    while (1) {
        if (current_tx_id) kernel_serial_printf("uid:%u[tx:%lu]> ", current_session_uid, current_tx_id);
        else                kernel_serial_printf("uid:%u> ", current_session_uid);
        read_line(input_buffer);
        sls_shell_execute(input_buffer, output_buffer, sizeof(output_buffer));
        kernel_serial_print(output_buffer);
    }
}
```

`current_session_uid`, `current_session_gid`, and `current_tx_id` stay exactly where they are — file-scope statics in `shell.c` — and are **shared** between the serial console and every `/api/shell/exec` call. This isn't a new risk the refactor introduces: it's the same single-simulated-machine model the rest of this app already has (one `DEMO_TOKEN`, no per-request session concept anywhere else in this codebase). A `login` or `tx begin` typed in the web terminal affects the physical serial console's session too, and vice versa. Documented here as an accepted characteristic, not a bug to fix later.

One behavioral difference from today, called out explicitly rather than left implicit: output is now buffered and flushed once per command instead of streamed character-by-character as each `do_syscall()`/helper runs. Content is identical; only the timing of when bytes reach the wire changes. No existing command in `shell.c` is long-running or progress-bar-style, so this has no visible effect on the interactive serial experience.

### 10.4 HTTP route

Follows `net/http.c`'s existing POST-route convention exactly (`api_sql_post` as the template — `JSONBuf`/`jb_*` helpers, `json_str()` body parsing, registered as an `if (!strcmp(path, ...))` block after the existing bearer-token gate that already 401s on `ROLE_GUEST`, same as every other POST route):

```c
// declared extern at the call site, matching how kernel.c declares
// sls_shell_loop() today -- no new header needed for one function.
extern int sls_shell_execute(const char* input, char* out_buf, size_t out_cap);

#define SHELL_EXEC_OUT_CAP 8192   // see 10.5

static int api_shell_exec_post(const char* body, char* buf, int max, uint32_t req_uid) {
    JSONBuf j = { buf, 0, max };
    if (!body) { jb_obj_open(&j,0); jb_str(&j,"error","missing body"); jb_obj_close(&j); j.buf[j.pos]='\0'; return j.pos; }
    (void)req_uid;  // shell session state is global/shared -- see §10.3
    char command[256];
    json_str(body, "command", command, sizeof(command));
    static char shell_out[SHELL_EXEC_OUT_CAP];
    int recognized = sls_shell_execute(command, shell_out, sizeof(shell_out));
    jb_obj_open(&j, 0);
    jb_str(&j, "ok", recognized ? "true" : "false"); jb_putc(&j, ',');
    jb_str_multiline(&j, "output", shell_out);   // see below
    jb_obj_close(&j); j.buf[j.pos]='\0'; return j.pos;
}
// registered in the POST block: if (!strcmp(path, "/api/shell/exec")) { ... }
```

`POST /api/shell/exec`, body `{"command": "ls objects"}`, response `{"ok": true, "output": "..."}` — bearer-token gated exactly like every other `/api/*` route, no new auth mechanism.

**Escaping bug caught before writing any code, not after:** the existing `jb_esc_str()` helper (used by every `jb_str()` call in `http.c` today) escapes `"` and `\` but not control characters — it was never exercised with embedded newlines before, because no existing JSON field in this codebase routinely carries multi-line text. Shell command output routinely will (`ls`, `journal dump`, `mqt list`, ...), and a raw, unescaped `\n` inside a JSON string literal is invalid JSON that would break `JSON.parse()` on every multi-line response. Fix: a new `jb_str_multiline()` helper, local to this one field, escaping `\n`→`\n`, `\r`→`\r`, `\t`→`\t`, plus `"`/`\` same as `jb_esc_str()`. Deliberately *not* changing the shared `jb_esc_str()` itself — that function has ~40 existing call sites across this file and none of them need this, so fixing it in place would be unscoped risk for zero benefit; a new, narrowly-used helper is the smaller change.

`json_str()`'s own parser doesn't unescape `\"`/`\\` sequences either (copies raw bytes until the next literal `"`) — a pre-existing limitation shared by every other `json_str()` call site in this file, not something new here. A `command` value containing a literal `"` would truncate early. Noted, not fixed — out of scope for this pass, and no existing shell command syntax requires literal double quotes (SQL string literals use single quotes, per `kernel/sql_parser.c`).

### 10.5 Buffer sizing

`SHELL_EXEC_OUT_CAP = 8192`, matching the largest existing per-command static buffer already in `shell.c` (`aggregate_exec`'s `agg_result[8192]`; `cursor_fetch_buf` is smaller at 4096). Commands that can produce more than one buffer's worth of output already have their own truncation conventions before this refactor (e.g. `vec search`/`vec index search` cap at `VEC_SEARCH_MAX_K`/similar) — `kernel_serial_capture_start()`'s bounds-checked truncation is a second, outer safety net, not the primary one, and is silent (matches how the existing UART path has no overflow signal either).

Noted, not fixed: `net/http.c`'s shared `resp_body[16384]` is comfortably larger than 8192 raw bytes for any realistic command output, but `jb_str_multiline()`'s escaping can up to double a string's length in the pathological case of output that's mostly newlines (`\n` → `\` `n`, 2 bytes each) — a command whose entire 8192-byte output were newlines could, in theory, have its closing `"}` silently truncated by `jb_putc()`'s own bounds check (memory-safe, just malformed JSON in that one edge case). No real command's output is anywhere close to majority-newline, so this is accepted rather than sized around.

### 10.6 What this closes — every "Missing" command from §3, for free

`sls_shell_execute()` runs the *entire* dispatch chain, so once `/api/shell/exec` exists, every one of the ~90 commands in `shell.c` — not just a hand-picked subset — becomes reachable over HTTP, including all 24 commands on §3's "Missing" list (`login`, `role set`, `grant`, `revoke`, `chmod`, `auth create/list/revoke`, `seal` (new syntax, §10.1), `write`, `demo`, `upload`, `load`, `loader list`, `svc crash`, `svc restart`, `proc kill`, `ipc post`, `ipc stat`, `journal create`, `journal purge`, `tier demote`, `tier promote`, `vfree`, `workflow addstep`, `webapp set/list/append`). No individual per-command HTTP routes needed — this was the whole point of Option B over hand-writing 24 more one-off routes.

**Done, as a follow-on pass:** `shellCommands.ts`'s missing-command table (renamed `SHELL_FALLBACK_COMMANDS`, from `MISSING_COMMANDS`, since "missing" stopped being accurate) now calls `/api/shell/exec` and shows the kernel's real plain-text output, instead of a static "not available over the web yet" message. `runCommand()`'s fallback branch, `helpText()`'s wording, and the destructive-confirmation set (unchanged — `isDestructive()` still reads the same per-command flags) all route through a new `execViaShellFallback()` helper. `ok:false` from that route means "the kernel's parser didn't recognize this exact command line" (a real, honest outcome — shown as an error, not silently swallowed), not a network failure.

Fixing the fallback table's usage strings mattered more once this shipped: they'd been written as inert documentation for commands nobody could actually run, and several were wrong against real `shell.c` syntax (`auth create` takes `<email> <uid> <role>`, not just `<name>`; `login` is a session `<uid> <gid>` switch, not username/password; `demo`/`upload` need a leading `<name>` the old strings omitted; `ipc post` takes a hex opcode, not free text; `webapp set`/`append` need a leading `<obj>`; `seal` picked up its new post-§10.1 `<name> <password>` single-line form). `"delete object"` was removed outright — it was never a real `shell.c` command, just an invented alias for `vfree` that would always have come back `Unknown command` once actually sent to the kernel. Host test (`shellCommands.test.ts`) updated to assert the new routing (path/method/body to `/api/shell/exec`, trailing-newline trimming, `ok:false` → `isError`) instead of the old static-message assertions — 48 assertions passing (was 43).
