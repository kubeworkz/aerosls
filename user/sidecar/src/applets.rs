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
//! - **`true`** — exits 0.
//!
//! The script runner's data layout is the applet's contract with its fork
//! children: `data[0]` = phase, `data[1..5]` = u32 LE line cursor (phase 1
//! parent) or child id (phase 2), then the immutable script text. A fork
//! child's snapshot carries `[1, cursor, script, FORK_MARKER]`; the child
//! parses the line at the cursor, sets up stdio, and execs — one command
//! per child, exactly like the `cat | grep` pipeline test's phase machine.

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

/// Install the system applets into a proc manager (called by `boot()`).
pub fn register_default_applets<K: Kernel, A: BufferAlloc>(pm: &mut ProcManager<K, A>) {
    pm.register_applet("init", init);
    pm.register_applet("cat", cat);
    pm.register_applet("echo", echo);
    pm.register_applet("true", do_true);
}
