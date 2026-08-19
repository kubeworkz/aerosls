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
//!   "rc scripts" step). v1 commands are whitespace-tokenized: `path
//!   arg1 arg2`, with one `>` stdout redirect; `#` comments and blank
//!   lines are skipped. When the script is consumed, init exits 0 (a real
//!   init would park forever and reap orphans — the `WaitingChild` park +
//!   orphan-reparent machinery is already there, and the entry's event
//!   loop owns the "never exits" half).
//! - **`cat`** — copies its first argument (or stdin) to fd 1, using
//!   `read_blocking` so it parks on an empty stdin instead of spinning.
//! - **`echo`** — writes its arguments (space-joined, newline-terminated)
//!   to fd 1.
//! - **`sh`** — the minimal interactive shell: prompts on fd 1, reads
//!   commands from fd 0 with a blocking read (parking on an empty
//!   console), forks one child per pipeline stage, waits, and tracks the
//!   last exit status as `$?` (expandable in the next command). `<` /
//!   `>` redirect stdin/stdout per stage (the redirect overrides the
//!   pipeline connection, like POSIX).
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
/// FORK_MARKER]`. Parse the command at the cursor, set up stdio (v1: one
/// `>` stdout redirect), and exec it. On any failure the child exits 127 —
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
    let tokens: Vec<String> = match core::str::from_utf8(&script[cursor..end]) {
        Ok(s) => s.split_whitespace().map(|t| t.to_string()).collect(),
        Err(_) => return Step::Exit(127),
    };
    // Split at the redirect: everything before `>` is argv; the token after
    // names the target. (One redirect, v1.)
    let cut = tokens.iter().position(|t| t == ">");
    let argv: Vec<String> = match cut {
        Some(i) => match tokens.get(i + 1) {
            Some(target) => {
                let task = ctx.task;
                let fd = match ctx.vfs().open(task, target, O_CREAT | O_WRONLY | O_TRUNC, 0o644) {
                    Ok(fd) => fd,
                    Err(_) => return Step::Exit(127),
                };
                if ctx.vfs().dup2(task, fd, 1).is_err() {
                    return Step::Exit(127);
                }
                ctx.vfs().close(task, fd).ok();
                tokens[..i].to_vec()
            }
            None => return Step::Exit(127),
        },
        None => tokens.to_vec(),
    };
    if argv.is_empty() {
        return Step::Exit(127);
    }
    let argv_refs: Vec<&str> = argv.iter().map(|a| a.as_str()).collect();
    match ctx.exec(argv[0].as_str(), &argv_refs) {
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
/// `data[1]` = last exit status (`$?`). Phase 0 prints the prompt, phase 1
/// accumulates console input until a newline (parking on an empty console
/// via `read_blocking`), phase 2 runs the parsed command, phase 3 reaps
/// the pipeline's children in order. A pipeline child's fork snapshot
/// carries `[2, status, stage, n_stages, pipes..., stage_text,
/// FORK_MARKER]` — see `sh_child`.
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
            let mut buf = [0u8; 16];
            match ctx.read_blocking(0, &mut buf) {
                ReadBlock::Data(0) => Step::Exit(0), // console closed: EOF
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

/// Phase 2: parse the accumulated line, fork one child per pipeline stage,
/// close the shell's own pipe copies (or the readers never see EOF), and
/// wait for the first child.
fn run_line<K: Kernel, A: BufferAlloc>(ctx: &mut Ctx<'_, K, A>) -> Step {
    let task = ctx.task;
    let status = ctx.data[1];
    let nl = match ctx.data[2..].iter().position(|&b| b == b'\n') {
        Some(i) => i,
        None => return Step::Exit(2), // phase 2 without a newline: impossible
    };
    let line = match core::str::from_utf8(&ctx.data[2..2 + nl]) {
        Ok(s) => s.trim(),
        Err(_) => return Step::Exit(2),
    };
    let stages = parse_line(line, status);
    if stages.is_empty() {
        ctx.data.truncate(2);
        ctx.data[0] = 0; // blank line: straight back to the prompt
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
    // pipe fds, the `<` / `>` redirect targets (length-prefixed), and its
    // argv text (plus the fork marker) — see `sh_child`.
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
        ctx.data.extend_from_slice(stage.argv.join(" ").as_bytes());
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
    // Wait state: [3, status, n, reaped, children...].
    ctx.data.clear();
    ctx.data.push(3);
    ctx.data.push(status);
    ctx.data.push(children.len() as u8);
    ctx.data.push(0); // reaped count
    for c in &children {
        ctx.data.push(*c as u8);
    }
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
/// becomes `$?`. When all are reaped, back to the prompt.
fn reap_next<K: Kernel, A: BufferAlloc>(ctx: &mut Ctx<'_, K, A>) -> Step {
    let n = ctx.data[2] as usize;
    let reaped = ctx.data[3] as usize;
    if reaped >= n {
        ctx.data.truncate(2);
        ctx.data[0] = 0;
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
/// nfd, pipes..., in_len, in_path..., out_len, out_path..., argv_text,
/// FORK_MARKER]`. Wire stdin/stdout to the pipe ends (or the console for
/// the first/last stage), apply the stage's `<` / `>` redirects (which
/// override the pipeline connection at fd 0 / fd 1, like POSIX), drop
/// every pipe fd, and exec — on any failure the child exits 127, which
/// the shell reaps.
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
    let text = match core::str::from_utf8(&d[p..]) {
        Ok(s) => s,
        Err(_) => return Step::Exit(127),
    };
    let tokens: Vec<String> = text.split_whitespace().map(|t| t.to_string()).collect();
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

/// Parse one command line into pipeline stages. Tokens are
/// whitespace-split; a lone `|` token separates stages; `>` / `<` mark
/// the next token as the stdout / stdin redirect target (the target is
/// not part of argv; a trailing redirect with no target records an empty
/// path, which the stage child fails to open → exit 127). `$?` expands to
/// the last exit status. Empty stages are skipped (a stray `|` cannot
/// produce a stage with no command). Returns [] for a blank line. v1: no
/// quoting, no `>>`, spaces required around `|`.
fn parse_line(line: &str, last_status: u8) -> Vec<Stage> {
    let mut stages: Vec<Stage> = Vec::new();
    let mut argv: Vec<String> = Vec::new();
    let mut in_redir: Option<String> = None;
    let mut out_redir: Option<String> = None;
    let mut tokens = line.split_whitespace();
    while let Some(t) = tokens.next() {
        match t {
            "|" => {
                if !argv.is_empty() || in_redir.is_some() || out_redir.is_some() {
                    stages.push(Stage {
                        argv: core::mem::take(&mut argv),
                        in_redir: in_redir.take(),
                        out_redir: out_redir.take(),
                    });
                }
            }
            ">" => out_redir = Some(tokens.next().unwrap_or("").to_string()),
            "<" => in_redir = Some(tokens.next().unwrap_or("").to_string()),
            "$?" => argv.push(last_status.to_string()),
            _ => argv.push(t.to_string()),
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
}
