//! The POSIX sidecar's built-in applet registry — BusyBox-style argv
//! dispatch, miniature (Phase 2 design §6.2 step 9).
//!
//! Programs on the rootfs are script files whose first line names an
//! applet (the crate-level exec model); the applets read their arguments
//! from `Ctx::argv`. `register_default_applets` installs the system
//! applets every POSIX sidecar needs:
//!
//! - **`init`** — the boot script runner. Executes `/etc/init.rc` line by
//!   line, forking one child per command and waiting for it (the design's
//!   "rc scripts" step). Commands parse through the same word parser as
//!   `sh` — quotes, backslash escapes, `<`/`>` redirects (`$?` expands to
//!   0, since init tracks no status). A line containing `|` fails 127:
//!   init runs one command per line, and a piped line is an error, not a
//!   silently dropped stage. `#` comments and blank lines are skipped.
//!   When the script is consumed, init exits 0 (a real init would park
//!   forever and reap orphans — the `WaitingChild` park + orphan-reparent
//!   machinery is already there, and the entry's event loop owns the
//!   "never exits" half).
//! - **`cat`** — copies its first argument (or stdin) to fd 1, using
//!   `read_blocking` so it parks on an empty stdin instead of spinning.
//! - **`echo`** — writes its arguments (space-joined, newline-terminated)
//!   to fd 1.
//! - **`sh`** — the minimal interactive shell: prompts on fd 1, reads
//!   commands from fd 0 with a blocking read (parking on an empty
//!   console), forks one child per pipeline stage, waits, and tracks the
//!   last exit status as `$?` (expandable in the next command). `<` /
//!   `>` redirect stdin/stdout per stage (the redirect overrides the
//!   pipeline connection, like POSIX), single / double quotes group
//!   whitespace into one argument (`'…'` fully literal, `"…"` still
//!   expanding `$?`), and backslash escapes the next character outside
//!   quotes (so `\ `, `\$`, `\|`, … are literal).
//! - **`true`** / **`false`** — exit 0 / 1.
//!
//! The script runner's data layout is the applet's contract with its fork
//! children: `data[0]` = phase, `data[1..5]` = u32 LE line cursor (phase 1
//! parent) or child id (phase 2), then the immutable script text. A fork
//! child's snapshot carries `[1, cursor, script, FORK_MARKER]`; the child
//! parses the line at the cursor, sets up stdio, and execs — one command
//! per child, exactly like the `cat | grep` pipeline test's phase machine.
//! `sh`'s pipeline children use the same pattern with their own layout
//! (see `sh_child`).

use alloc::string::{String, ToString};
use alloc::vec::Vec;

use aerosls_procmgr::{is_child, BlockReason, Ctx, ProcManager, ReadBlock, Step, WaitOutcome};
use aerosls_proto::kabi::Kernel;
use aerosls_vfs::{BufferAlloc, Errno, O_CREAT, O_RDONLY, O_TRUNC, O_WRONLY};

/// The boot script every POSIX sidecar init runs.
pub const INIT_RC: &str = "/etc/init.rc";

/// `init`: the boot script runner (see module docs for the data layout).
pub fn init<K: Kernel, A: BufferAlloc>(ctx: &mut Ctx<'_, K, A>) -> Step {
    let task = ctx.task;
    if ctx.data.is_empty() {
        ctx.data.push(0); // phase
    }
    if is_child(&ctx.data) {
        return init_child(ctx);
    }
    match ctx.data[0] {
        // Load the script: [0, cursor=0, script...]. A missing script is
        // an empty one — the sidecar boots to a quiet system.
        0 => {
            let fd = match ctx.vfs().open(task, INIT_RC, O_RDONLY, 0) {
                Ok(fd) => fd,
                Err(Errno::ENoent) => return Step::Exit(0),
                Err(_) => return Step::Exit(2),
            };
            ctx.data.extend_from_slice(&0u32.to_le_bytes());
            let mut buf = [0u8; 64];
            loop {
                match ctx.vfs().read(task, fd, &mut buf) {
                    Ok(0) => break,
                    Ok(n) => ctx.data.extend_from_slice(&buf[..n]),
                    Err(_) => return Step::Exit(2),
                }
            }
            ctx.vfs().close(task, fd).ok();
            ctx.data[0] = 1;
            Step::Yield
        }
        // Run the next command: point the cursor at it (the fork child's
        // snapshot inherits that cursor), fork, then wait. The parent's
        // own cursor advances past the line and the child id is appended
        // after the script — so a wake resumes the loop exactly where the
        // completed command left it.
        1 => {
            let (ls, le) = match next_command(&ctx.data) {
                Some(x) => x,
                None => return Step::Exit(0), // script consumed
            };
            ctx.data[1..5].copy_from_slice(&ls.to_le_bytes());
            let child = match ctx.fork() {
                Ok(c) => c,
                Err(_) => return Step::Exit(2),
            };
            ctx.data[1..5].copy_from_slice(&(le + 1).to_le_bytes());
            ctx.data[0] = 2;
            ctx.data.push(child as u8);
            match ctx.wait(child) {
                WaitOutcome::Blocked => Step::Blocked(BlockReason::WaitingChild(child)),
                WaitOutcome::Reaped(_) => {
                    ctx.data.pop();
                    ctx.data[0] = 1;
                    Step::Yield
                }
                WaitOutcome::NoSuchChild => Step::Exit(2),
            }
        }
        // A child finished (we woke); reap it and resume the loop.
        2 => {
            let child = *ctx.data.last().unwrap() as u32;
            match ctx.wait(child) {
                WaitOutcome::Reaped(_) => {
                    ctx.data.pop();
                    ctx.data[0] = 1;
                    Step::Yield
                }
                WaitOutcome::Blocked => Step::Blocked(BlockReason::WaitingChild(child)),
                WaitOutcome::NoSuchChild => Step::Exit(2),
            }
        }
        _ => Step::Exit(2),
    }
}

/// The fork child of a script line: its snapshot is `[1, cursor, script,
/// FORK_MARKER]`. Parse the command at the cursor through the shared word
/// parser (`parse_line` — quotes, backslash escapes, `<`/`>` redirects),
/// apply the stage's redirects at fd 0 / fd 1, and exec it with the
/// parsed argv (argv[0] stays a full path, as rc scripts specify). A line
/// that parses to more than one stage (a `|`) or to nothing fails 127 —
/// init runs one command per line. On any failure the child exits 127 —
/// the parent reaps the status and moves on to the next line.
fn init_child<K: Kernel, A: BufferAlloc>(ctx: &mut Ctx<'_, K, A>) -> Step {
    let cursor = u32::from_le_bytes([ctx.data[1], ctx.data[2], ctx.data[3], ctx.data[4]]) as usize;
    // The fork marker is the last byte; everything after the cursor slot is
    // the (inherited) script text.
    let script = &ctx.data[5..ctx.data.len() - 1];
    let end = match script[cursor..].iter().position(|&b| b == b'\n') {
        Some(i) => cursor + i,
        None => script.len(),
    };
    let line = match core::str::from_utf8(&script[cursor..end]) {
        Ok(s) => s,
        Err(_) => return Step::Exit(127),
    };
    // `$?` is always 0: init tracks no exit status.
    let stages = parse_line(line, 0);
    if stages.len() != 1 {
        return Step::Exit(127);
    }
    let stage = &stages[0];
    if stage.argv.is_empty() {
        return Step::Exit(127);
    }
    let task = ctx.task;
    if let Some(path) = &stage.out_redir {
        let fd = match ctx.vfs().open(task, path, O_CREAT | O_WRONLY | O_TRUNC, 0o644) {
            Ok(fd) => fd,
            Err(_) => return Step::Exit(127),
        };
        if ctx.vfs().dup2(task, fd, 1).is_err() {
            return Step::Exit(127);
        }
        ctx.vfs().close(task, fd).ok();
    }
    if let Some(path) = &stage.in_redir {
        let fd = match ctx.vfs().open(task, path, O_RDONLY, 0) {
            Ok(fd) => fd,
            Err(_) => return Step::Exit(127),
        };
        if ctx.vfs().dup2(task, fd, 0).is_err() {
            return Step::Exit(127);
        }
        ctx.vfs().close(task, fd).ok();
    }
    let argv: Vec<&str> = stage.argv.iter().map(|a| a.as_str()).collect();
    match ctx.exec(stage.argv[0].as_str(), &argv) {
        Ok(()) => Step::Yield,
        Err(_) => Step::Exit(127),
    }
}

/// The next real command in the script, scanning from the cursor: skips
/// blank and `#`-comment lines (and non-UTF8 garbage), returns the line's
/// byte range within the script (`data[5..]`), or None at end of script.
fn next_command(data: &[u8]) -> Option<(u32, u32)> {
    let script = &data[5..];
    let mut pos = u32::from_le_bytes([data[1], data[2], data[3], data[4]]) as usize;
    loop {
        if pos >= script.len() {
            return None;
        }
        let start = pos;
        let end = match script[start..].iter().position(|&b| b == b'\n') {
            Some(i) => start + i,
            None => script.len(),
        };
        let t = match core::str::from_utf8(&script[start..end]) {
            Ok(s) => s.trim(),
            Err(_) => "", // treat garbage like a comment
        };
        if !t.is_empty() && !t.starts_with('#') {
            return Some((start as u32, end as u32));
        }
        pos = if end < script.len() { end + 1 } else { end };
    }
}

/// `cat`: copies argv[1] (or stdin) to fd 1. Reads block — a console stdin
/// parks the task until input arrives instead of spinning.
pub fn cat<K: Kernel, A: BufferAlloc>(ctx: &mut Ctx<'_, K, A>) -> Step {
    let task = ctx.task;
    // Copy the path out: argv is borrowed from the manager, and open needs
    // it mutably.
    let arg = ctx.argv().get(1).map(|s| s.clone());
    let src = match arg {
        Some(p) => match ctx.vfs().open(task, &p, O_RDONLY, 0) {
            Ok(fd) => fd,
            Err(_) => return Step::Exit(2),
        },
        None => 0, // stdin
    };
    let mut buf = [0u8; 16];
    loop {
        match ctx.read_blocking(src, &mut buf) {
            ReadBlock::Data(0) => {
                if src != 0 {
                    ctx.vfs().close(task, src).ok();
                }
                return Step::Exit(0);
            }
            ReadBlock::Data(n) => {
                if ctx.vfs().write(task, 1, &buf[..n]).is_err() {
                    return Step::Exit(2);
                }
            }
            ReadBlock::WouldBlock => return Step::Blocked(BlockReason::Readable(src)),
            ReadBlock::Err(_) => return Step::Exit(2),
        }
    }
}

/// `echo`: writes its arguments (space-joined, newline-terminated) to fd 1.
pub fn echo<K: Kernel, A: BufferAlloc>(ctx: &mut Ctx<'_, K, A>) -> Step {
    let mut text = String::new();
    for (i, a) in ctx.argv()[1..].iter().enumerate() {
        if i > 0 {
            text.push(' ');
        }
        text.push_str(a);
    }
    text.push('\n');
    let task = ctx.task;
    if ctx.vfs().write(task, 1, text.as_bytes()).is_err() {
        return Step::Exit(2);
    }
    Step::Exit(0)
}

/// `true`: exits 0.
pub fn do_true<K: Kernel, A: BufferAlloc>(_ctx: &mut Ctx<'_, K, A>) -> Step {
    Step::Exit(0)
}

/// `false`: exits 1 (for `sh`'s `$?` and the like).
pub fn do_false<K: Kernel, A: BufferAlloc>(_ctx: &mut Ctx<'_, K, A>) -> Step {
    Step::Exit(1)
}

/// `sh`: the minimal interactive shell. Data layout: `data[0]` = phase,
/// `data[1]` = last exit status (`$?`), `data[2..]` = the input batch
/// queue (unconsumed console bytes, possibly several lines). Phase 0
/// prints the prompt, phase 1 consumes a complete buffered line or reads
/// more console input (parking on an empty console via `read_blocking`),
/// phase 2 runs the first buffered line, phase 3 reaps the pipeline's
/// children in order — the queued remainder survives all of it, so a
/// multi-line read runs line by line with a single prompt around the
/// batch. A pipeline child's fork snapshot carries `[2, status, stage,
/// n_stages, nfd, pipes..., in_len, in..., out_len, out..., n_args,
/// (arg_len, arg...)..., queue, FORK_MARKER]` — see `sh_child`.
pub fn sh<K: Kernel, A: BufferAlloc>(ctx: &mut Ctx<'_, K, A>) -> Step {
    let task = ctx.task;
    if ctx.data.is_empty() {
        ctx.data.extend_from_slice(&[0, 0]); // phase, last status
        return Step::Yield;
    }
    if is_child(&ctx.data) {
        return sh_child(ctx);
    }
    match ctx.data[0] {
        0 => {
            if ctx.vfs().write(task, 1, b"$ ").is_err() {
                return Step::Exit(2);
            }
            ctx.data[0] = 1;
            Step::Yield
        }
        1 => {
            // A buffered complete line runs without touching the console.
            if ctx.data[2..].contains(&b'\n') {
                ctx.data[0] = 2;
                return Step::Yield;
            }
            let mut buf = [0u8; 16];
            match ctx.read_blocking(0, &mut buf) {
                // EOF: run any partial buffered line, then exit.
                ReadBlock::Data(0) => {
                    if ctx.data.len() > 2 {
                        ctx.data[0] = 2;
                        Step::Yield
                    } else {
                        Step::Exit(0)
                    }
                }
                ReadBlock::Data(n) => {
                    ctx.data.extend_from_slice(&buf[..n]);
                    if ctx.data[2..].contains(&b'\n') {
                        ctx.data[0] = 2;
                    }
                    Step::Yield
                }
                ReadBlock::WouldBlock => Step::Blocked(BlockReason::Readable(0)),
                ReadBlock::Err(_) => Step::Exit(2),
            }
        }
        2 => run_line(ctx),
        3 => reap_next(ctx),
        _ => Step::Exit(2),
    }
}

/// Phase 2: run the first complete line of the buffered batch. The queued
/// remainder is appended after every fork snapshot and the wait state, so
/// multi-line input runs line by line and nothing is dropped. See
/// `sh_child` for the fork-snapshot layout.
fn run_line<K: Kernel, A: BufferAlloc>(ctx: &mut Ctx<'_, K, A>) -> Step {
    let task = ctx.task;
    let status = ctx.data[1];
    // The first complete line (or the whole buffer at EOF) and the rest.
    let nl = ctx.data[2..].iter().position(|&b| b == b'\n');
    let (line, remainder) = match nl {
        Some(i) => (ctx.data[2..2 + i].to_vec(), ctx.data[2 + i + 1..].to_vec()),
        None => (ctx.data[2..].to_vec(), Vec::new()),
    };
    let line = match core::str::from_utf8(&line) {
        Ok(s) => s.trim(),
        Err(_) => "", // garbage: drop the line like a comment
    };
    let stages = parse_line(line, status);
    if stages.is_empty() {
        // Blank or comment line: drop it; re-prompt only if the batch is
        // drained, otherwise continue straight to the next buffered line.
        ctx.data.clear();
        ctx.data.push(if remainder.is_empty() { 0 } else { 1 });
        ctx.data.push(status);
        ctx.data.extend_from_slice(&remainder);
        return Step::Yield;
    }
    // Inter-stage pipes; the fds land above stdio (0,1,2 = console).
    let mut pipes: Vec<u32> = Vec::new();
    for _ in 0..stages.len() - 1 {
        let (r, w) = match ctx.vfs().pipe(task) {
            Ok(p) => p,
            Err(_) => return Step::Exit(2),
        };
        pipes.push(r);
        pipes.push(w);
    }
    let nfd = (stages.len() - 1) * 2;
    // Fork one child per stage; each snapshot carries its stage index, the
    // pipe fds, the `<` / `>` redirect targets (length-prefixed), the argv
    // (one length-prefixed field per argument, so words containing spaces
    // — quoted or backslash-escaped — arrive at the child exactly as
    // parsed; the child stops before the queue), and the queued
    // remainder — see `sh_child`.
    let mut children: Vec<u32> = Vec::new();
    for (s, stage) in stages.iter().enumerate() {
        ctx.data.clear();
        ctx.data.push(2); // phase
        ctx.data.push(status);
        ctx.data.push(s as u8);
        ctx.data.push(stages.len() as u8);
        ctx.data.push(nfd as u8);
        for fd in &pipes {
            ctx.data.push(*fd as u8);
        }
        match &stage.in_redir {
            Some(p) => {
                ctx.data.push(p.len() as u8);
                ctx.data.extend_from_slice(p.as_bytes());
            }
            None => ctx.data.push(0),
        }
        match &stage.out_redir {
            Some(p) => {
                ctx.data.push(p.len() as u8);
                ctx.data.extend_from_slice(p.as_bytes());
            }
            None => ctx.data.push(0),
        }
        ctx.data.push(stage.argv.len() as u8); // n_args
        for a in &stage.argv {
            ctx.data.push(a.len() as u8);
            ctx.data.extend_from_slice(a.as_bytes());
        }
        ctx.data.extend_from_slice(&remainder); // the queue survives forks
        match ctx.fork() {
            Ok(c) => children.push(c),
            Err(_) => return Step::Exit(2),
        }
    }
    // The shell's own pipe copies must go, or the readers never see EOF
    // (same rule as the cat | grep test's shell).
    for fd in &pipes {
        ctx.vfs().close(task, *fd).ok();
    }
    // Wait state: [3, status, n, reaped, children..., remainder].
    ctx.data.clear();
    ctx.data.push(3);
    ctx.data.push(status);
    ctx.data.push(children.len() as u8);
    ctx.data.push(0); // reaped count
    for c in &children {
        ctx.data.push(*c as u8);
    }
    ctx.data.extend_from_slice(&remainder);
    let c = children[0];
    match ctx.wait(c) {
        WaitOutcome::Reaped(code) => {
            ctx.data[3] = 1;
            if children.len() == 1 {
                ctx.data[1] = code as u8;
            }
            Step::Yield
        }
        WaitOutcome::Blocked => Step::Blocked(BlockReason::WaitingChild(c)),
        WaitOutcome::NoSuchChild => Step::Exit(2),
    }
}

/// Phase 3: reap the pipeline's children in order; the last stage's status
/// becomes `$?`. When all are reaped, back to the prompt if the batch is
/// drained, else straight on to the next buffered line (no new prompt).
fn reap_next<K: Kernel, A: BufferAlloc>(ctx: &mut Ctx<'_, K, A>) -> Step {
    let n = ctx.data[2] as usize;
    let reaped = ctx.data[3] as usize;
    if reaped >= n {
        // The queued remainder survives at data[4+n..].
        let queue = ctx.data[4 + n..].to_vec();
        let status = ctx.data[1];
        ctx.data.clear();
        ctx.data.push(if queue.is_empty() { 0 } else { 1 });
        ctx.data.push(status);
        ctx.data.extend_from_slice(&queue);
        return Step::Yield;
    }
    let c = ctx.data[4 + reaped] as u32;
    match ctx.wait(c) {
        WaitOutcome::Reaped(code) => {
            ctx.data[3] = (reaped + 1) as u8;
            if reaped + 1 == n {
                ctx.data[1] = code as u8;
            }
            Step::Yield
        }
        WaitOutcome::Blocked => Step::Blocked(BlockReason::WaitingChild(c)),
        WaitOutcome::NoSuchChild => {
            ctx.data[3] = (reaped + 1) as u8;
            Step::Yield
        }
    }
}

/// A pipeline stage child: its snapshot is `[2, status, stage, n_stages,
/// nfd, pipes..., in_len, in_path..., out_len, out_path..., n_args,
/// (arg_len, arg...)..., queue, FORK_MARKER]` — one length-prefixed field
/// per argument, so each argv word (even one containing spaces, from
/// quoting or backslash escapes) arrives exactly as parsed and the child
/// stops before the shell's queued input batch. Wire stdin/stdout to the
/// pipe ends (or the console for the first/last stage), apply the stage's
/// `<` / `>` redirects (which override the pipeline connection at fd 0 /
/// fd 1, like POSIX), drop every pipe fd, and exec — on any failure the
/// child exits 127, which the shell reaps.
fn sh_child<K: Kernel, A: BufferAlloc>(ctx: &mut Ctx<'_, K, A>) -> Step {
    let task = ctx.task;
    // The fork marker is the last byte.
    let d = &ctx.data[..ctx.data.len() - 1];
    let stage = d[2] as usize;
    let n = d[3] as usize;
    let nfd = d[4] as usize;
    let pipes: Vec<u32> = d[5..5 + nfd].iter().map(|&b| b as u32).collect();
    let mut p = 5 + nfd;
    let in_len = d[p] as usize;
    let in_redir = if in_len > 0 {
        match core::str::from_utf8(&d[p + 1..p + 1 + in_len]) {
            Ok(s) => Some(s.to_string()),
            Err(_) => return Step::Exit(127),
        }
    } else {
        None
    };
    p += 1 + in_len;
    let out_len = d[p] as usize;
    let out_redir = if out_len > 0 {
        match core::str::from_utf8(&d[p + 1..p + 1 + out_len]) {
            Ok(s) => Some(s.to_string()),
            Err(_) => return Step::Exit(127),
        }
    } else {
        None
    };
    p += 1 + out_len;
    let n_args = d[p] as usize;
    p += 1;
    let mut tokens: Vec<String> = Vec::new();
    for _ in 0..n_args {
        let alen = d[p] as usize;
        match core::str::from_utf8(&d[p + 1..p + 1 + alen]) {
            Ok(s) => tokens.push(s.to_string()),
            Err(_) => return Step::Exit(127),
        }
        p += 1 + alen;
    }
    if tokens.is_empty() {
        return Step::Exit(127);
    }
    let in_fd = if stage == 0 { 0 } else { pipes[(stage - 1) * 2] };
    let out_fd = if stage == n - 1 { 1 } else { pipes[stage * 2 + 1] };
    if ctx.vfs().dup2(task, in_fd, 0).is_err() || ctx.vfs().dup2(task, out_fd, 1).is_err() {
        return Step::Exit(127);
    }
    // Redirects override the pipeline connection at fd 0 / fd 1.
    if let Some(path) = &in_redir {
        let fd = match ctx.vfs().open(task, path, O_RDONLY, 0) {
            Ok(fd) => fd,
            Err(_) => return Step::Exit(127),
        };
        if ctx.vfs().dup2(task, fd, 0).is_err() {
            return Step::Exit(127);
        }
        ctx.vfs().close(task, fd).ok();
    }
    if let Some(path) = &out_redir {
        let fd = match ctx.vfs().open(task, path, O_CREAT | O_WRONLY | O_TRUNC, 0o644) {
            Ok(fd) => fd,
            Err(_) => return Step::Exit(127),
        };
        if ctx.vfs().dup2(task, fd, 1).is_err() {
            return Step::Exit(127);
        }
        ctx.vfs().close(task, fd).ok();
    }
    for fd in &pipes {
        ctx.vfs().close(task, *fd).ok();
    }
    let argv: Vec<&str> = tokens.iter().map(|t| t.as_str()).collect();
    // argv[0] stays as typed; the file to exec is resolved through PATH.
    let prog = resolve_path(&tokens[0]);
    match ctx.exec(&prog, &argv) {
        Ok(()) => Step::Yield,
        Err(_) => Step::Exit(127),
    }
}

/// Minimal PATH resolution: a token containing `/` is used as-is (a
/// relative path resolves against the task's cwd); otherwise the command
/// names an applet under `/bin` (v1: a single search directory, no envp).
fn resolve_path(tok: &str) -> String {
    if tok.contains('/') {
        tok.to_string()
    } else {
        alloc::format!("/bin/{}", tok)
    }
}

/// One pipeline stage: the command's argv plus optional `<` stdin / `>`
/// stdout redirect targets (v1: truncating `>`, no `>>`).
#[derive(Debug, PartialEq, Eq)]
pub(crate) struct Stage {
    pub argv: Vec<String>,
    pub in_redir: Option<String>,
    pub out_redir: Option<String>,
}

/// One token of a parsed command line: a word (possibly empty — `""` is
/// still an argument, like POSIX), or a pipeline / redirect metacharacter.
#[derive(Debug, PartialEq, Eq)]
enum Tok {
    Word(String),
    Pipe,
    RedirectIn,
    RedirectOut,
}

/// Tokenize a command line into words and metacharacters, honoring single
/// and double quotes and backslash escapes. A quoted section is a literal
/// part of its word, so quotes group whitespace — and, quoted, the
/// metacharacters `|` `<` `>` — into a single argument. `$?` expands to
/// the last exit status wherever it appears: unquoted, and inside double
/// quotes (POSIX). Single quotes are fully literal, so `'$?'` stays `$?`
/// and `'\'` is a backslash. Outside quotes, backslash removes the
/// special meaning of the next character (POSIX): `\ ` is a literal
/// space, `\$` suppresses `$?` expansion, and `\|` is a word, not a
/// pipe; a trailing backslash is dropped (v1 lenient). Inside double
/// quotes, backslash escapes only `$`, `"` and `\` (POSIX); elsewhere it
/// stays literal. An unterminated quote runs to the end of the line (v1
/// is lenient — no syntax error).
fn tokenize(line: &str, last_status: u8) -> Vec<Tok> {
    let chars: Vec<char> = line.chars().collect();
    let mut toks: Vec<Tok> = Vec::new();
    let mut word = String::new();
    let mut quoted = false; // the current word contains a quoted part
    macro_rules! flush {
        () => {
            if !word.is_empty() || quoted {
                toks.push(Tok::Word(core::mem::take(&mut word)));
                quoted = false;
            }
        };
    }
    let mut i = 0;
    while i < chars.len() {
        let c = chars[i];
        match c {
            ' ' | '\t' => {
                flush!();
                i += 1;
            }
            // Unquoted metacharacters end the current word and stand
            // alone; quoted or escaped versions (handled in the quote and
            // backslash arms) stay literal parts of a word.
            '|' => {
                flush!();
                toks.push(Tok::Pipe);
                i += 1;
            }
            '<' => {
                flush!();
                toks.push(Tok::RedirectIn);
                i += 1;
            }
            '>' => {
                flush!();
                toks.push(Tok::RedirectOut);
                i += 1;
            }
            // Outside quotes, backslash removes the next character's
            // special meaning (POSIX); a trailing backslash is dropped.
            '\\' => {
                if i + 1 < chars.len() {
                    word.push(chars[i + 1]);
                    i += 2;
                } else {
                    i += 1;
                }
            }
            '\'' => {
                quoted = true;
                i += 1;
                while i < chars.len() && chars[i] != '\'' {
                    word.push(chars[i]);
                    i += 1;
                }
                if i < chars.len() {
                    i += 1; // the closing quote
                }
            }
            '"' => {
                quoted = true;
                i += 1;
                loop {
                    if i >= chars.len() {
                        break;
                    }
                    let d = chars[i];
                    if d == '"' {
                        i += 1;
                        break;
                    }
                    // Inside double quotes, backslash escapes only `$`,
                    // `"` and `\` (POSIX); everything else stays literal.
                    if d == '\\' && matches!(chars.get(i + 1), Some('$') | Some('"') | Some('\\'))
                    {
                        word.push(chars[i + 1]);
                        i += 2;
                    } else if d == '$' && chars.get(i + 1) == Some(&'?') {
                        word.push_str(&last_status.to_string());
                        i += 2;
                    } else {
                        word.push(d);
                        i += 1;
                    }
                }
            }
            '$' if chars.get(i + 1) == Some(&'?') => {
                word.push_str(&last_status.to_string());
                i += 2;
            }
            _ => {
                word.push(c);
                i += 1;
            }
        }
    }
    if !word.is_empty() || quoted {
        toks.push(Tok::Word(word));
    }
    toks
}

/// Parse one command line into pipeline stages. `tokenize` splits it into
/// words and metacharacters (see `tokenize` for the quote semantics); a
/// lone unquoted `|` token separates stages; `>` / `<` mark the next word
/// as the stdout / stdin redirect target (the target is not part of argv;
/// a trailing or metachar redirect records an empty path, which the stage
/// child fails to open → exit 127). Empty stages are skipped (a stray `|`
/// cannot produce a stage with no command). Returns [] for a blank line.
/// v1: no `>>`, and `|` / redirects need surrounding spaces (a quoted
/// version is a literal word).
fn parse_line(line: &str, last_status: u8) -> Vec<Stage> {
    let toks = tokenize(line, last_status);
    let mut stages: Vec<Stage> = Vec::new();
    let mut argv: Vec<String> = Vec::new();
    let mut in_redir: Option<String> = None;
    let mut out_redir: Option<String> = None;
    let mut i = 0;
    while i < toks.len() {
        match &toks[i] {
            Tok::Pipe => {
                if !argv.is_empty() || in_redir.is_some() || out_redir.is_some() {
                    stages.push(Stage {
                        argv: core::mem::take(&mut argv),
                        in_redir: in_redir.take(),
                        out_redir: out_redir.take(),
                    });
                }
                i += 1;
            }
            Tok::RedirectOut => match toks.get(i + 1) {
                Some(Tok::Word(w)) => {
                    out_redir = Some(w.clone());
                    i += 2;
                }
                _ => {
                    out_redir = Some(String::new());
                    i += 1;
                }
            },
            Tok::RedirectIn => match toks.get(i + 1) {
                Some(Tok::Word(w)) => {
                    in_redir = Some(w.clone());
                    i += 2;
                }
                _ => {
                    in_redir = Some(String::new());
                    i += 1;
                }
            },
            Tok::Word(w) => {
                argv.push(w.clone());
                i += 1;
            }
        }
    }
    if !argv.is_empty() || in_redir.is_some() || out_redir.is_some() {
        stages.push(Stage {
            argv,
            in_redir,
            out_redir,
        });
    }
    stages
}

/// Install the system applets into a proc manager (called by `boot()`).
pub fn register_default_applets<K: Kernel, A: BufferAlloc>(pm: &mut ProcManager<K, A>) {
    pm.register_applet("init", init);
    pm.register_applet("cat", cat);
    pm.register_applet("echo", echo);
    pm.register_applet("sh", sh);
    pm.register_applet("true", do_true);
    pm.register_applet("false", do_false);
}

#[cfg(test)]
mod tests {
    use super::{parse_line, Stage};

    fn st(argv: &[&str]) -> Stage {
        Stage {
            argv: argv.iter().map(|a| a.to_string()).collect(),
            in_redir: None,
            out_redir: None,
        }
    }

    #[test]
    fn parse_line_splits_pipeline_stages() {
        assert_eq!(
            parse_line("echo hello | cat", 0),
            vec![st(&["echo", "hello"]), st(&["cat"])]
        );
    }

    #[test]
    fn parse_line_expands_last_status() {
        assert_eq!(
            parse_line("echo $?", 7),
            vec![Stage {
                argv: vec!["echo".to_string(), "7".to_string()],
                in_redir: None,
                out_redir: None,
            }]
        );
        assert_eq!(parse_line("$? | cat", 3), vec![st(&["3"]), st(&["cat"])]);
    }

    #[test]
    fn parse_line_extracts_redirects() {
        // `>` / `<` targets leave argv; they become the stage's redirects.
        assert_eq!(
            parse_line("echo hi > /tmp/x", 0),
            vec![Stage {
                argv: vec!["echo".to_string(), "hi".to_string()],
                in_redir: None,
                out_redir: Some("/tmp/x".to_string()),
            }]
        );
        assert_eq!(
            parse_line("cat < /etc/passwd | grep root > /tmp/out", 0),
            vec![
                Stage {
                    argv: vec!["cat".to_string()],
                    in_redir: Some("/etc/passwd".to_string()),
                    out_redir: None,
                },
                Stage {
                    argv: vec!["grep".to_string(), "root".to_string()],
                    in_redir: None,
                    out_redir: Some("/tmp/out".to_string()),
                },
            ]
        );
        // Last redirect of a kind wins; a trailing redirect records an
        // empty path (the stage child fails to open it → exit 127).
        assert_eq!(
            parse_line("echo a > /tmp/x > /tmp/y", 0),
            vec![Stage {
                argv: vec!["echo".to_string(), "a".to_string()],
                in_redir: None,
                out_redir: Some("/tmp/y".to_string()),
            }]
        );
        assert_eq!(
            parse_line("echo hi >", 0),
            vec![Stage {
                argv: vec!["echo".to_string(), "hi".to_string()],
                in_redir: None,
                out_redir: Some(String::new()),
            }]
        );
    }

    #[test]
    fn parse_line_blank_and_stray_pipes() {
        assert_eq!(parse_line("", 0), Vec::<Stage>::new());
        assert_eq!(parse_line("   \t ", 3), Vec::<Stage>::new());
        assert_eq!(parse_line("| true |", 0), vec![st(&["true"])]);
    }

    #[test]
    fn parse_line_quotes_group_whitespace() {
        // Double quotes keep a space inside one argument.
        assert_eq!(
            parse_line("echo \"hello world\" | cat", 0),
            vec![st(&["echo", "hello world"]), st(&["cat"])]
        );
        // Single quotes behave the same way.
        assert_eq!(parse_line("echo 'a b' c", 0), vec![st(&["echo", "a b", "c"])]);
        // Quoted sections join with the surrounding unquoted word.
        assert_eq!(parse_line("echo a\"b c\"d", 0), vec![st(&["echo", "ab cd"])]);
        // An empty quoted word is still an argument (POSIX: echo "" passes
        // it through — the applet prints an empty line).
        assert_eq!(parse_line("echo \"\"", 0), vec![st(&["echo", ""])]);
    }

    #[test]
    fn parse_line_quotes_are_literal() {
        // Quoted metacharacters are words, not operators.
        assert_eq!(
            parse_line("echo \"|\" | cat", 0),
            vec![st(&["echo", "|"]), st(&["cat"])]
        );
        assert_eq!(parse_line("echo \"x > y\"", 0), vec![st(&["echo", "x > y"])]);
        // `$?` is literal inside single quotes...
        assert_eq!(parse_line("echo '$?'", 7), vec![st(&["echo", "$?"])]);
        // ...but expands inside double quotes, like POSIX.
        assert_eq!(parse_line("echo \"$?\"", 7), vec![st(&["echo", "7"])]);
        // Redirect targets can be quoted paths (spaces survive).
        assert_eq!(
            parse_line("cat < \"my file\"", 0),
            vec![Stage {
                argv: vec!["cat".to_string()],
                in_redir: Some("my file".to_string()),
                out_redir: None,
            }]
        );
        // An unterminated quote runs to the end of the line (v1 lenient).
        assert_eq!(parse_line("echo 'oops", 0), vec![st(&["echo", "oops"])]);
    }

    #[test]
    fn parse_line_backslash_escapes() {
        // `\ ` is a literal space inside one word — no token split.
        assert_eq!(
            parse_line("echo hello\\ world", 0),
            vec![st(&["echo", "hello world"])]
        );
        // `\$` suppresses `$?` expansion; `\\` is a literal backslash.
        assert_eq!(parse_line("echo \\$? \\\\", 7), vec![st(&["echo", "$?", "\\"])]);
        // Escaped metacharacters are words, not operators.
        assert_eq!(
            parse_line("echo \\| \\> \\<", 0),
            vec![st(&["echo", "|", ">", "<"])]
        );
        // Inside double quotes, backslash escapes only `$`, `"`, `\\`
        // (POSIX).
        assert_eq!(parse_line("echo \"a\\\"b\"", 0), vec![st(&["echo", "a\"b"])]);
        assert_eq!(parse_line("echo \"\\$\"", 7), vec![st(&["echo", "$"])]); // no expansion
        assert_eq!(parse_line("echo \"\\\\\"", 0), vec![st(&["echo", "\\"])]);
        // Other backslashes inside double quotes stay literal...
        assert_eq!(parse_line("echo \"a\\ b\"", 0), vec![st(&["echo", "a\\ b"])]);
        // ...and so do backslashes inside single quotes.
        assert_eq!(parse_line("echo 'a\\b'", 0), vec![st(&["echo", "a\\b"])]);
        // A trailing backslash is dropped (v1 lenient).
        assert_eq!(parse_line("echo x\\", 0), vec![st(&["echo", "x"])]);
    }
}
