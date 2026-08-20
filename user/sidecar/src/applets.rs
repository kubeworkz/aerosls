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
//!   0 and `$VAR` to nothing, since init tracks no status and has no
//!   environment). A line containing `|` fails 127:
//!   init runs one command per line, and a piped line is an error, not a
//!   silently dropped stage. `#` comments and blank lines are skipped.
//!   When the script is consumed, init exits 0 (a real init would park
//!   forever and reap orphans — the `WaitingChild` park + orphan-reparent
//!   machinery is already there, and the entry's event loop owns the
//!   "never exits" half).
//! - **`cat`** — copies its first argument (or stdin) to fd 1, using
//!   `read_blocking` so it parks on an empty stdin instead of spinning.
//! - **`grep`** — prints the lines of stdin (or the named files) matching
//!   a small glob pattern (`*` any run, `.` any one char, `\x` literal
//!   `x`, substring match); exits 0/1 on match/no-match, like grep.
//! - **`wc`** — counts lines / words / bytes of stdin (or named files),
//!   right-aligned like coreutils; the count ends only at EOF, so it
//!   exercises pipe fd closure (`echo hi | wc`).
//! - **`head` / `tail`** — the line-window pair: `head -n N` prints the
//!   first N lines and exits without draining the input (partial-pipe
//!   semantics — a still-writing child's next write fails `EPIPE` once
//!   head's fd table drops), while `tail -n N` buffers a sliding window
//!   to EOF (parking like `wc`) and prints the last N lines.
//! - **`sort`** — buffers the whole input (stdin or named files
//!   concatenated) across reads, then emits the lines in lexicographic
//!   byte order; the emit phase parks on a full pipe like `cat` and
//!   observes `EPIPE` if the reader leaves early (`cat | sort | head`).
//! - **`seq`** — the pure producer: prints integers `FIRST..LAST` (step
//!   `STEP`), one per line, filling the 4 KiB pipe and parking, so the
//!   reader-side applets get a real fast writer (`seq 10000 | head`).
//! - **`tee`** — the two-writer fan-out: copies stdin to stdout and to
//!   each named file per chunk; a full stdout pipe parks without
//!   rewriting the files, and an early-exiting reader (`tee | head`)
//!   wakes the stdout retry to `EPIPE`.
//! - **`tr`** — the streaming character filter: `tr SET1 SET2` maps
//!   each byte (a 256-byte table built once at init; `SET2`'s last byte
//!   repeats for a longer `SET1`) and `tr -d SET1` deletes, with ranges
//!   (`a-z`) and backslash escapes in the sets; a full pipe parks with
//!   the translated chunk pending.
//! - **`cut`** — the column extractor: `cut -d DELIM -f LIST` emits the
//!   selected fields joined with the delimiter, `cut -c LIST` (or `-b`)
//!   the selected byte positions concatenated — lists of 1-based
//!   positions and ranges (`1`, `3-5`, `2-`, `-3`, `-`) parsed once at
//!   init; lines buffer across reads and each complete line is written
//!   with blocking writes.
//! - **`uniq`** — collapses adjacent duplicate lines into runs: default
//!   prints each run's representative once, `-c` prefixes the `{:>7}`
//!   count, `-d` prints only runs longer than one line, `-u` only runs
//!   of exactly one; only ADJACENT lines dedup (`a b a` keeps both `a`
//!   lines), lines may span reads, and run output writes with blocking
//!   writes.
//! - **`echo`** — writes its arguments (space-joined, newline-terminated)
//!   to fd 1.
//! - **`sh`** — the minimal interactive shell: prompts on fd 1, reads
//!   commands from fd 0 with a blocking read (parking on an empty
//!   console), forks one child per pipeline stage, waits, and tracks the
//!   last exit status as `$?` (expandable in the next command). `<` /
//!   `>` redirect stdin/stdout per stage (the redirect overrides the
//!   pipeline connection, like POSIX), single / double quotes group
//!   whitespace into one argument (`'…'` fully literal, `"…"` still
//!   expanding `$?`), backslash escapes the next character outside
//!   quotes (so `\ `, `\$`, `\|`, … are literal), bare command names
//!   resolve through the exported `PATH` (default `/bin`), and the
//!   builtins `export` (set / list), `setenv NAME value`, `unset NAME...`
//!   and `unsetenv NAME` run in the shell itself, mutating the persistent
//!   env region. Leading `NAME=value` words are scoped assignments: they
//!   persist only for a builtin (POSIX special-builtin rule) or a bare
//!   assignment line, and `PATH=x cmd` scopes the search path to that
//!   command only.
//! - **`true`** / **`false`** — exit 0 / 1.
//!
//! The script runner's data layout is the applet's contract with its fork
//! children: `data[0]` = phase, `data[1..5]` = u32 LE line cursor, then
//! the immutable script text; phase 2 appends the u32 LE child id at the
//! end (it dies with the completed wait). A fork
//! child's snapshot carries `[1, cursor, script, FORK_MARKER]`; the child
//! parses the line at the cursor, sets up stdio, and execs — one command
//! per child, exactly like the `cat | grep` pipeline test's phase machine.
//! `sh`'s pipeline children use the same pattern with their own layout
//! (see `sh_child`).

use alloc::string::{String, ToString};
use alloc::vec::Vec;

use aerosls_procmgr::{is_child, signal_exit_code, BlockReason, Ctx, ProcManager, ReadBlock, SIGINT, Step, WaitOutcome, WriteBlock};
use aerosls_proto::kabi::Kernel;
use aerosls_vfs::{BufferAlloc, Errno, O_CREAT, O_RDONLY, O_TRUNC, O_WRONLY, Vfs};

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
            ctx.data.extend_from_slice(&child.to_le_bytes()); // u32 LE
            match ctx.wait(child) {
                WaitOutcome::Blocked => Step::Blocked(BlockReason::WaitingChild(child)),
                WaitOutcome::Reaped(_) => {
                    ctx.data.truncate(ctx.data.len() - 4);
                    ctx.data[0] = 1;
                    Step::Yield
                }
                WaitOutcome::NoSuchChild => Step::Exit(2),
            }
        }
        // A child finished (we woke); reap it and resume the loop. The
        // child id sits in the last 4 bytes (u32 LE).
        2 => {
            let n = ctx.data.len();
            let child = u32::from_le_bytes(ctx.data[n - 4..n].try_into().unwrap());
            match ctx.wait(child) {
                WaitOutcome::Reaped(_) => {
                    ctx.data.truncate(n - 4);
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
    // `$?` is always 0 (init tracks no status) and `$VAR` expands to
    // nothing (init has no environment).
    let stages = parse_line(line, 0, &[]);
    if stages.len() != 1 {
        return Step::Exit(127);
    }
    let stage = &stages[0];
    if stage.argv.is_empty() {
        return Step::Exit(127);
    }
    if !apply_redirects(ctx, stage) {
        return Step::Exit(127);
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
/// parks the task until input arrives instead of spinning — and writes
/// park too: a full pipe blocks the task (`BlockReason::Writable`) instead
/// of failing, so a long stream survives a slow reader. When the last
/// reader leaves (e.g. `head` exiting early), the retry observes `EPIPE`
/// and cat exits 2. Data layout: `[fd, phase, pending...]` — `pending` is
/// a chunk read from the source but not yet fully written; it survives
/// the parks, so no bytes are lost.
pub fn cat<K: Kernel, A: BufferAlloc>(ctx: &mut Ctx<'_, K, A>) -> Step {
    let task = ctx.task;
    if ctx.data.is_empty() {
        // Copy the path out: argv is borrowed from the manager, and open
        // needs it mutably.
        let arg = ctx.argv().get(1).map(|s| s.clone());
        let src = match arg {
            Some(p) => match ctx.vfs().open(task, &p, O_RDONLY, 0) {
                Ok(fd) => fd,
                Err(_) => return Step::Exit(2),
            },
            None => 0, // stdin
        };
        ctx.data.push(src as u8);
        ctx.data.push(0); // phase: read a chunk
    }
    loop {
        let src = ctx.data[0] as u32;
        if ctx.data[1] == 0 {
            // Read a chunk into the pending buffer (phase 0).
            let mut buf = [0u8; 16];
            match ctx.read_blocking(src, &mut buf) {
                ReadBlock::Data(0) => {
                    if src != 0 {
                        ctx.vfs().close(task, src).ok();
                    }
                    return Step::Exit(0);
                }
                ReadBlock::Data(n) => {
                    ctx.data.extend_from_slice(&buf[..n]);
                    ctx.data[1] = 1; // phase: write the pending chunk
                }
                ReadBlock::WouldBlock => return Step::Blocked(BlockReason::Readable(src)),
                ReadBlock::Err(_) => return Step::Exit(2),
            }
        } else {
            // Write the pending chunk (phase 1), parking on a full pipe.
            // The copy keeps ctx.data unborrowed across the write.
            let pending = ctx.data[2..].to_vec();
            match ctx.write_blocking(1, &pending) {
                WriteBlock::Data(k) => {
                    if k == pending.len() {
                        ctx.data.truncate(2); // all written: back to reading
                        ctx.data[1] = 0;
                    } else {
                        ctx.data.drain(2..2 + k); // retry the remainder
                    }
                }
                WriteBlock::WouldBlock => return Step::Blocked(BlockReason::Writable(1)),
                WriteBlock::Err(_) => return Step::Exit(2),
            }
        }
    }
}

/// The glob-style core matcher: `*` matches any run of characters (incl.
/// none), `.` matches any single character, `\x` a literal `x` (a
/// trailing `\` is a literal backslash); everything else is literal.
/// `p` must match a *prefix* of `t` (any trailing text is ignored) —
/// `is_match` scans the line for the offset where a prefix match holds.
fn glob_match(p: &[u8], t: &[u8]) -> bool {
    if p.is_empty() {
        return true;
    }
    match p[0] {
        b'*' => {
            for i in 0..=t.len() {
                if glob_match(&p[1..], &t[i..]) {
                    return true;
                }
            }
            false
        }
        b'.' => !t.is_empty() && glob_match(&p[1..], &t[1..]),
        b'\\' => {
            if p.len() >= 2 {
                !t.is_empty() && t[0] == p[1] && glob_match(&p[2..], &t[1..])
            } else {
                !t.is_empty() && t[0] == b'\\' && glob_match(&p[1..], &t[1..])
            }
        }
        c => !t.is_empty() && t[0] == c && glob_match(&p[1..], &t[1..]),
    }
}

/// Does `line` contain a match for `pat`? Substring semantics (the match
/// may start anywhere in the line, like grep); `*` = any run, `.` = any
/// one char, `\x` = literal `x`. An empty pattern matches everything.
fn is_match(pat: &str, line: &[u8]) -> bool {
    if pat.is_empty() {
        return true;
    }
    let p = pat.as_bytes();
    for start in 0..=line.len() {
        if glob_match(p, &line[start..]) {
            return true;
        }
    }
    false
}

/// `grep`: prints the lines of its input — stdin, or the named files in
/// order — that match a pattern (a small glob: `*` any run, `.` any one
/// char, `\x` literal `x`, everything else literal; case-sensitive;
/// substring match anywhere in the line). Exit status: 0 if any line
/// matched, 1 if none did, 2 on error (usage, unreadable file, write
/// failure). Reads via `read_blocking`, so an empty pipe/console stdin
/// parks instead of spinning. Data layout: `[phase, file_idx, fd,
/// matched, partial-line...]` — the trailing partial line survives the
/// parks; a file's final unterminated line is flushed at EOF without a
/// newline.
pub fn grep<K: Kernel, A: BufferAlloc>(ctx: &mut Ctx<'_, K, A>) -> Step {
    let task = ctx.task;
    if ctx.data.is_empty() {
        ctx.data.extend_from_slice(&[0, 0, 0, 0]); // phase, file_idx, fd, matched
    }
    // Copy the pattern out (argv borrows the manager).
    let pattern = match ctx.argv().get(1) {
        Some(p) => p.clone(),
        None => return Step::Exit(2),
    };
    loop {
        match ctx.data[0] {
            // Choose the next source: stdin when no files were given,
            // otherwise the file at file_idx.
            0 => {
                let src = if ctx.data[1] == 0 && ctx.argv().len() <= 2 {
                    0
                } else {
                    let path = match ctx.argv().get(2 + ctx.data[1] as usize) {
                        Some(p) => p.clone(),
                        None => return Step::Exit(if ctx.data[3] == 1 { 0 } else { 1 }),
                    };
                    match ctx.vfs().open(task, &path, O_RDONLY, 0) {
                        Ok(fd) => fd,
                        Err(_) => return Step::Exit(2),
                    }
                };
                ctx.data[0] = 1;
                ctx.data[2] = src as u8;
            }
            1 => {
                let fd = ctx.data[2] as u32;
                let mut buf = [0u8; 16];
                match ctx.read_blocking(fd, &mut buf) {
                    ReadBlock::Data(0) => {
                        // EOF: flush the trailing partial line (no
                        // newline), then move to the next source.
                        let tail = ctx.data[4..].to_vec();
                        if ctx.data.len() > 4 && is_match(&pattern, &tail) {
                            if ctx.vfs().write(task, 1, &tail).is_err() {
                                return Step::Exit(2);
                            }
                            ctx.data[3] = 1;
                        }
                        if fd != 0 {
                            ctx.vfs().close(task, fd).ok();
                        }
                        ctx.data.truncate(4);
                        ctx.data[0] = 0; // next source
                        ctx.data[1] += 1;
                        ctx.data[2] = 0;
                    }
                    ReadBlock::Data(n) => {
                        ctx.data.extend_from_slice(&buf[..n]);
                        // Extract the complete lines (newline included)
                        // into owned buffers first — the writes below need
                        // no borrow of ctx.data. Each line runs from just
                        // after the previous newline to this one (a chunk
                        // can hold several); the trailing partial line
                        // stays in the data for the next read.
                        let mut lines: Vec<Vec<u8>> = Vec::new();
                        let mut consumed = 0;
                        let mut line_start = 0;
                        for (i, &b) in ctx.data[4..].iter().enumerate() {
                            if b == b'\n' {
                                lines.push(ctx.data[4 + line_start..=4 + i].to_vec());
                                consumed = i + 1;
                                line_start = i + 1;
                            }
                        }
                        if consumed > 0 {
                            ctx.data.drain(4..4 + consumed);
                        }
                        for line in &lines {
                            if is_match(&pattern, &line[..line.len() - 1]) {
                                if ctx.vfs().write(task, 1, line).is_err() {
                                    return Step::Exit(2);
                                }
                                ctx.data[3] = 1;
                            }
                        }
                    }
                    ReadBlock::WouldBlock => {
                        return Step::Blocked(BlockReason::Readable(fd));
                    }
                    ReadBlock::Err(_) => return Step::Exit(2),
                }
            }
            _ => return Step::Exit(2),
        }
    }
}

/// Accumulate `lines` / `words` for one chunk of bytes. `prev_ws` is the
/// whitespace state carried across chunks — input start counts as
/// whitespace, so the first word is counted when its first character
/// arrives (a word is a maximal run of non-whitespace; space, tab and
/// newline are the separators).
fn count_chunk(chunk: &[u8], lines: &mut u32, words: &mut u32, prev_ws: &mut bool) {
    for &b in chunk {
        if b == b'\n' {
            *lines += 1;
            *prev_ws = true;
        } else if b == b' ' || b == b'\t' {
            *prev_ws = true;
        } else if *prev_ws {
            *words += 1;
            *prev_ws = false;
        }
    }
}

/// Read a u32 (LE) at `off` in the applet data.
fn read_u32(data: &[u8], off: usize) -> u32 {
    u32::from_le_bytes([data[off], data[off + 1], data[off + 2], data[off + 3]])
}

/// Read a u64 (LE) at `off` in the applet data.
fn read_u64(data: &[u8], off: usize) -> u64 {
    let mut b = [0u8; 8];
    b.copy_from_slice(&data[off..off + 8]);
    u64::from_le_bytes(b)
}

/// Parse `seq`'s arguments: `[LAST]`, `[FIRST LAST]`, or `[FIRST STEP
/// LAST]` — non-negative integers; the one-arg form starts at 1 and the
/// two-arg form steps by 1. A missing/wrong arity, an unparsable value,
/// or a zero STEP (which would never terminate) → `None` (usage error,
/// exit 2).
fn parse_seq_args(argv: &[String]) -> Option<(u64, u64, u64)> {
    let (first, last, step) = match argv.len() {
        2 => (1u64, argv[1].parse::<u64>().ok()?, 1u64),
        3 => (argv[1].parse::<u64>().ok()?, argv[2].parse::<u64>().ok()?, 1u64),
        // `FIRST STEP LAST`: the middle argument is the step.
        4 => (
            argv[1].parse::<u64>().ok()?,
            argv[3].parse::<u64>().ok()?,
            argv[2].parse::<u64>().ok()?,
        ),
        _ => return None,
    };
    if step == 0 {
        return None;
    }
    Some((first, last, step))
}

/// Parse `head`/`tail`'s line count: no `-n` → `(10, 1)` (default, first
/// file arg at argv[1]); `-n N` → `(N, 3)`; `-nN` → `(N, 2)`. An unknown
/// option or an unparsable `N` → `None` (usage error, exit 2).
fn parse_n(argv: &[String]) -> Option<(u32, usize)> {
    if argv.len() <= 1 {
        return Some((10, 1));
    }
    let a = &argv[1];
    if let Some(rest) = a.strip_prefix("-n") {
        if rest.is_empty() {
            let n = argv.get(2)?.parse().ok()?;
            Some((n, 3))
        } else {
            let n = rest.parse().ok()?;
            Some((n, 2))
        }
    } else if a.starts_with('-') && a.len() > 1 {
        None // an unknown option is a usage error
    } else {
        Some((10, 1)) // the first arg is a file name
    }
}

/// Count the complete (newline-terminated) lines in `buf`.
fn count_lines(buf: &[u8]) -> u32 {
    buf.iter().filter(|&&b| b == b'\n').count() as u32
}

/// Drop the first `k` complete lines (each newline-terminated) from the
/// front of `buf`. If `buf` holds fewer complete lines than `k` — e.g. `k`
/// lines plus a trailing unterminated one — everything is dropped. Used by
/// `tail` to trim its window at EOF, where an unterminated final line
/// counts as a line.
fn drop_lines(buf: &[u8], k: u32) -> &[u8] {
    let mut skip = 0usize;
    let mut dropped = 0u32;
    while dropped < k {
        match buf[skip..].iter().position(|&b| b == b'\n') {
            Some(i) => {
                skip += i + 1;
                dropped += 1;
            }
            None => return &[], // fewer complete lines than k: drop all
        }
    }
    &buf[skip..]
}

/// Split `raw` into lines, each kept with its trailing newline; a final
/// unterminated fragment (input not ending in `\n`) is a line without
/// one. An empty input yields no lines.
fn split_lines(raw: &[u8]) -> Vec<Vec<u8>> {
    let mut lines = Vec::new();
    let mut cur = Vec::new();
    for &b in raw {
        cur.push(b);
        if b == b'\n' {
            lines.push(core::mem::take(&mut cur));
        }
    }
    if !cur.is_empty() {
        lines.push(cur); // unterminated final line
    }
    lines
}

/// The sort key of a line: its bytes minus a single trailing newline (the
/// newline is a line terminator, not part of the key).
fn line_key(line: &[u8]) -> &[u8] {
    if line.ends_with(b"\n") {
        &line[..line.len() - 1]
    } else {
        line
    }
}

/// Parse one character of a `tr` SET: a literal byte, or a backslash
/// escape (`\n` `\t` `\r` `\\`; an unknown escape is the literal
/// character — `\x` is `x`). Returns the byte and the index after it;
/// a trailing backslash is malformed (`None`).
fn parse_set_char(b: &[u8], i: usize) -> Option<(u8, usize)> {
    match *b.get(i)? {
        b'\\' => {
            let e = *b.get(i + 1)?;
            let c = match e {
                b'n' => b'\n',
                b't' => b'\t',
                b'r' => b'\r',
                b'\\' => b'\\',
                other => other,
            };
            Some((c, i + 2))
        }
        c => Some((c, i + 1)),
    }
}

/// Expand a `tr` SET string into its byte sequence: literal characters,
/// ranges `c1-c2` (ascending; a descending range like `z-a` is an
/// error), and backslash escapes. A `-` at the start or end of the set
/// is a literal minus, and a range never extends past an escape — so
/// `a-\\n` is `a`, `-`, newline. A trailing backslash is malformed
/// (`None`).
fn expand_set(set: &str) -> Option<Vec<u8>> {
    let b = set.as_bytes();
    let mut out = Vec::new();
    let mut i = 0;
    while i < b.len() {
        let (c1, next) = parse_set_char(b, i)?;
        // A range only when a `-` follows and the char after it is a
        // literal (not an escape and not the end of the set).
        if next < b.len() && b[next] == b'-' && next + 1 < b.len() && b[next + 1] != b'\\' {
            let (c2, after) = parse_set_char(b, next + 1)?;
            if c1 > c2 {
                return None;
            }
            out.extend(c1..=c2);
            i = after;
        } else {
            out.push(c1);
            i = next;
        }
    }
    Some(out)
}

/// Build `tr`'s map table: byte → its translation. Bytes in `s1` map to
/// the corresponding `s2` byte, with `s2`'s last byte repeated for a
/// longer `s1` (GNU semantics); everything else is the identity. `s2`
/// must be non-empty (the caller rejects an empty second set).
fn build_map(s1: &[u8], s2: &[u8]) -> [u8; 256] {
    let mut t = [0u8; 256];
    for (i, slot) in t.iter_mut().enumerate() {
        *slot = i as u8;
    }
    for (i, &b) in s1.iter().enumerate() {
        t[b as usize] = s2[i.min(s2.len() - 1)];
    }
    t
}

/// Build `tr -d`'s membership table: 1 for the bytes in `s1`, else 0.
fn build_delete(s1: &[u8]) -> [u8; 256] {
    let mut t = [0u8; 256];
    for &b in s1 {
        t[b as usize] = 1;
    }
    t
}

/// Apply the table to a chunk: map mode (`delete` false) translates
/// every byte through the table; delete mode drops the bytes the table
/// marks.
fn tr_chunk(chunk: &[u8], table: &[u8], delete: bool) -> Vec<u8> {
    if delete {
        chunk
            .iter()
            .filter(|&&b| table[b as usize] == 0)
            .copied()
            .collect()
    } else {
        chunk.iter().map(|&b| table[b as usize]).collect()
    }
}

/// Parse `cut`'s `-f`/`-c` list: comma-separated positions and ranges,
/// each `N` (a single position), `N-M`, `N-` (open-ended to the end),
/// `-M` (from the start), or `-` (everything). Positions are 1-based;
/// an open end is stored as `end == 0`. Errors (`None`): an empty
/// list, a zero position, a descending range, or an unparsable member.
fn parse_cut_ranges(list: &str) -> Option<Vec<(u32, u32)>> {
    if list.is_empty() {
        return None;
    }
    let mut ranges = Vec::new();
    for spec in list.split(',') {
        if spec.is_empty() {
            return None;
        }
        if spec == "-" {
            ranges.push((1, 0));
            continue;
        }
        if let Some((a, b)) = spec.split_once('-') {
            let start = if a.is_empty() { 1 } else { a.parse().ok()? };
            let end = if b.is_empty() { 0 } else { b.parse().ok()? };
            if start == 0 || (end != 0 && end < start) {
                return None;
            }
            ranges.push((start, end));
        } else {
            let n: u32 = spec.parse().ok()?;
            if n == 0 {
                return None;
            }
            ranges.push((n, n));
        }
    }
    Some(ranges)
}

/// Apply `cut`'s selection to one line (without its newline) and return
/// the output line, newline included. Field mode (`mode` 0) splits the
/// line on `delim` and emits the selected fields joined with `delim`;
/// char mode (`mode` 1) emits the selected byte positions concatenated.
/// An open range (`end == 0`) extends to the last field / last byte.
/// Out-of-range selections contribute nothing — so a fully out-of-range
/// selection yields a bare newline, and a missing field never joins.
fn cut_line(line: &[u8], mode: u8, delim: u8, ranges: &[(u32, u32)]) -> Vec<u8> {
    let mut out = Vec::new();
    if mode == 0 {
        // Split into fields; an empty line is one empty field.
        let fields: Vec<&[u8]> = line.split(|&b| b == delim).collect();
        let mut first = true;
        for &(start, end) in ranges {
            let last = if end == 0 { fields.len() as u32 } else { end };
            for pos in start..=last {
                if let Some(f) = fields.get(pos as usize - 1) {
                    if !first {
                        out.push(delim);
                    }
                    out.extend_from_slice(f);
                    first = false;
                }
            }
        }
    } else {
        for &(start, end) in ranges {
            let last = if end == 0 { line.len() as u32 } else { end };
            for pos in start..=last {
                if let Some(&b) = line.get(pos as usize - 1) {
                    out.push(b);
                }
            }
        }
    }
    out.push(b'\n');
    out
}

/// `wc`: counts lines, words and bytes of its input — stdin, or the
/// named files in order. Output is `{:>7}`-aligned, one line per source
/// with the filename for files (v1: no total line). Exits 0 on success,
/// 2 on error. Reads via `read_blocking`, so an empty pipe stdin parks
/// instead of spinning — and the count terminates only on EOF, which
/// pipe fd closure delivers once every writer (the shell's own copies
/// and the writing child) is gone: `echo hi | wc` proves it. Data
/// layout: `[lines u32, words u32, bytes u32, prev_ws, phase, file_idx,
/// fd]`.
pub fn wc<K: Kernel, A: BufferAlloc>(ctx: &mut Ctx<'_, K, A>) -> Step {
    let task = ctx.task;
    if ctx.data.is_empty() {
        // lines, words, bytes (u32 LE), prev_ws, phase, file_idx, fd
        ctx.data.extend_from_slice(&[0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0]);
    }
    loop {
        match ctx.data[13] {
            // Choose the next source: stdin when no files were given,
            // otherwise the file at file_idx.
            0 => {
                let src = if ctx.data[14] == 0 && ctx.argv().len() <= 1 {
                    0
                } else {
                    let path = match ctx.argv().get(1 + ctx.data[14] as usize) {
                        Some(p) => p.clone(),
                        None => return Step::Exit(0),
                    };
                    match ctx.vfs().open(task, &path, O_RDONLY, 0) {
                        Ok(fd) => fd,
                        Err(_) => return Step::Exit(2),
                    }
                };
                ctx.data[13] = 1;
                ctx.data[15] = src as u8;
            }
            1 => {
                let fd = ctx.data[15] as u32;
                let mut buf = [0u8; 16];
                match ctx.read_blocking(fd, &mut buf) {
                    ReadBlock::Data(0) => {
                        // EOF (all writers gone): print this source's
                        // counts, then move to the next source.
                        let mut text = alloc::format!(
                            "{:>7} {:>7} {:>7}",
                            read_u32(&ctx.data, 0),
                            read_u32(&ctx.data, 4),
                            read_u32(&ctx.data, 8)
                        );
                        if ctx.data[14] > 0 || ctx.argv().len() > 1 {
                            if let Some(p) = ctx.argv().get(1 + ctx.data[14] as usize) {
                                text.push(' ');
                                text.push_str(p);
                            }
                        }
                        text.push('\n');
                        if ctx.vfs().write(task, 1, text.as_bytes()).is_err() {
                            return Step::Exit(2);
                        }
                        if fd != 0 {
                            ctx.vfs().close(task, fd).ok();
                        }
                        // Reset the counters for the next source.
                        ctx.data[0..12].fill(0);
                        ctx.data[12] = 1; // prev_ws
                        ctx.data[13] = 0; // phase: next source
                        ctx.data[14] += 1;
                        ctx.data[15] = 0;
                    }
                    ReadBlock::Data(n) => {
                        let mut lines = read_u32(&ctx.data, 0);
                        let mut words = read_u32(&ctx.data, 4);
                        let bytes = read_u32(&ctx.data, 8) + n as u32;
                        let mut prev_ws = ctx.data[12] == 1;
                        count_chunk(&buf[..n], &mut lines, &mut words, &mut prev_ws);
                        ctx.data[0..4].copy_from_slice(&lines.to_le_bytes());
                        ctx.data[4..8].copy_from_slice(&words.to_le_bytes());
                        ctx.data[8..12].copy_from_slice(&bytes.to_le_bytes());
                        ctx.data[12] = prev_ws as u8;
                    }
                    ReadBlock::WouldBlock => {
                        return Step::Blocked(BlockReason::Readable(fd));
                    }
                    ReadBlock::Err(_) => return Step::Exit(2),
                }
            }
            _ => return Step::Exit(2),
        }
    }
}

/// `head [-n N] [file...]`: prints the first N lines (default 10) of each
/// source (stdin, or the named files in order), then exits immediately —
/// WITHOUT draining the rest of the input. That is the partial-pipe
/// semantics: the reader leaves the pipe while the writer is still
/// producing, so once head's fd table is dropped (the shell already
/// closed its own pipe copies) the writer's next write observes `EPIPE`;
/// the shell reaps both and `$?` is head's status (0). `-n 0` prints
/// nothing and exits at once. Data layout: `[phase, file_idx, fd,
/// remaining u32 LE, files, partial...]` — the trailing partial line
/// survives the parks and is flushed at EOF without a newline.
pub fn head<K: Kernel, A: BufferAlloc>(ctx: &mut Ctx<'_, K, A>) -> Step {
    let task = ctx.task;
    if ctx.data.is_empty() {
        let (remaining, files) = match parse_n(ctx.argv()) {
            Some(x) => x,
            None => return Step::Exit(2),
        };
        ctx.data.extend_from_slice(&[0, 0, 0]); // phase, file_idx, fd
        ctx.data.extend_from_slice(&remaining.to_le_bytes());
        ctx.data.push(files as u8);
    }
    loop {
        if read_u32(&ctx.data, 3) == 0 {
            return Step::Exit(0); // `-n 0` (or exhausted): never read
        }
        match ctx.data[0] {
            // Choose the next source: stdin when no files were given,
            // otherwise the file at file_idx.
            0 => {
                let files = ctx.data[7] as usize;
                let src = if ctx.data[1] == 0 && ctx.argv().len() <= files {
                    0
                } else {
                    let path = match ctx.argv().get(files + ctx.data[1] as usize) {
                        Some(p) => p.clone(),
                        None => return Step::Exit(0),
                    };
                    match ctx.vfs().open(task, &path, O_RDONLY, 0) {
                        Ok(fd) => fd,
                        Err(_) => return Step::Exit(2),
                    }
                };
                ctx.data[0] = 1;
                ctx.data[2] = src as u8;
            }
            1 => {
                let fd = ctx.data[2] as u32;
                let mut buf = [0u8; 16];
                match ctx.read_blocking(fd, &mut buf) {
                    ReadBlock::Data(0) => {
                        // EOF: flush the trailing partial line (no
                        // newline, like head), then the next source.
                        if ctx.data.len() > 8 {
                            let tail = ctx.data[8..].to_vec();
                            if ctx.vfs().write(task, 1, &tail).is_err() {
                                return Step::Exit(2);
                            }
                        }
                        if fd != 0 {
                            ctx.vfs().close(task, fd).ok();
                        }
                        ctx.data.truncate(8);
                        ctx.data[0] = 0; // next source
                        ctx.data[1] += 1;
                        ctx.data[2] = 0;
                    }
                    ReadBlock::Data(n) => {
                        ctx.data.extend_from_slice(&buf[..n]);
                        // Extract the complete lines (newline included)
                        // into owned buffers — the writes below need no
                        // borrow of ctx.data. Each line runs from just
                        // after the previous newline to this one (a
                        // chunk can hold several); the trailing partial
                        // stays in the data.
                        let mut lines: Vec<Vec<u8>> = Vec::new();
                        let mut consumed = 0;
                        let mut line_start = 0;
                        for (i, &b) in ctx.data[8..].iter().enumerate() {
                            if b == b'\n' {
                                lines.push(ctx.data[8 + line_start..=8 + i].to_vec());
                                consumed = i + 1;
                                line_start = i + 1;
                            }
                        }
                        if consumed > 0 {
                            ctx.data.drain(8..8 + consumed);
                        }
                        let mut remaining = read_u32(&ctx.data, 3);
                        for line in &lines {
                            if remaining == 0 {
                                break;
                            }
                            if ctx.vfs().write(task, 1, line).is_err() {
                                return Step::Exit(2);
                            }
                            remaining -= 1;
                        }
                        ctx.data[3..7].copy_from_slice(&remaining.to_le_bytes());
                        if remaining == 0 {
                            // The N lines are out: leave now, without
                            // draining the input (partial-pipe semantics
                            // — the writer's next write fails EPIPE).
                            return Step::Exit(0);
                        }
                    }
                    ReadBlock::WouldBlock => {
                        return Step::Blocked(BlockReason::Readable(fd));
                    }
                    ReadBlock::Err(_) => return Step::Exit(2),
                }
            }
            _ => return Step::Exit(2),
        }
    }
}

/// `tail [-n N] [file...]`: prints the last N lines (default 10) of each
/// source (stdin, or the named files in order). Tail is head's opposite —
/// it must see EOF to know which lines are last, so it reads the whole
/// stream (parking on empty pipes like `wc`) and keeps only a sliding
/// window of the last N complete lines, dropping the oldest as new ones
/// arrive; an unterminated final line counts as a line at EOF. Data
/// layout: `[phase, file_idx, fd, n u32 LE, seen u32 LE, files, lines...]`
/// — `lines` is the raw buffered text; complete lines (newline-terminated)
/// count into `seen`, and the window keeps `seen <= n`.
pub fn tail<K: Kernel, A: BufferAlloc>(ctx: &mut Ctx<'_, K, A>) -> Step {
    let task = ctx.task;
    if ctx.data.is_empty() {
        let (n, files) = match parse_n(ctx.argv()) {
            Some(x) => x,
            None => return Step::Exit(2),
        };
        ctx.data.extend_from_slice(&[0, 0, 0]); // phase, file_idx, fd
        ctx.data.extend_from_slice(&n.to_le_bytes());
        ctx.data.extend_from_slice(&0u32.to_le_bytes()); // seen
        ctx.data.push(files as u8);
    }
    loop {
        match ctx.data[0] {
            // Choose the next source: stdin when no files were given,
            // otherwise the file at file_idx.
            0 => {
                let files = ctx.data[11] as usize;
                let src = if ctx.data[1] == 0 && ctx.argv().len() <= files {
                    0
                } else {
                    let path = match ctx.argv().get(files + ctx.data[1] as usize) {
                        Some(p) => p.clone(),
                        None => return Step::Exit(0),
                    };
                    match ctx.vfs().open(task, &path, O_RDONLY, 0) {
                        Ok(fd) => fd,
                        Err(_) => return Step::Exit(2),
                    }
                };
                ctx.data[0] = 1;
                ctx.data[2] = src as u8;
            }
            1 => {
                let fd = ctx.data[2] as u32;
                let mut buf = [0u8; 16];
                match ctx.read_blocking(fd, &mut buf) {
                    ReadBlock::Data(0) => {
                        // EOF: an unterminated final line counts as one
                        // more; drop any overflow, then emit the window.
                        let n = read_u32(&ctx.data, 3);
                        let seen = read_u32(&ctx.data, 7);
                        let partial =
                            ctx.data.len() > 12 && ctx.data[ctx.data.len() - 1] != b'\n';
                        let total = seen + partial as u32;
                        let drop = total.saturating_sub(n);
                        let out = if drop > 0 {
                            drop_lines(&ctx.data[12..], drop).to_vec()
                        } else {
                            ctx.data[12..].to_vec()
                        };
                        if ctx.vfs().write(task, 1, &out).is_err() {
                            return Step::Exit(2);
                        }
                        if fd != 0 {
                            ctx.vfs().close(task, fd).ok();
                        }
                        ctx.data.truncate(12);
                        ctx.data[0] = 0; // next source
                        ctx.data[1] += 1;
                        ctx.data[2] = 0;
                    }
                    ReadBlock::Data(n) => {
                        ctx.data.extend_from_slice(&buf[..n]);
                        let n_total = read_u32(&ctx.data, 3);
                        // Re-count the complete lines now in the buffer and
                        // trim the window from the front.
                        let mut seen = count_lines(&ctx.data[12..]);
                        while seen > n_total {
                            let idx = ctx.data[12..]
                                .iter()
                                .position(|&b| b == b'\n')
                                .expect("seen counts newlines");
                            ctx.data.drain(12..=12 + idx);
                            seen -= 1;
                        }
                        ctx.data[7..11].copy_from_slice(&seen.to_le_bytes());
                    }
                    ReadBlock::WouldBlock => {
                        return Step::Blocked(BlockReason::Readable(fd));
                    }
                    ReadBlock::Err(_) => return Step::Exit(2),
                }
            }
            _ => return Step::Exit(2),
        }
    }
}

/// `sort [file...]`: reads its input to EOF — stdin, or the named files
/// concatenated (GNU semantics), buffering every line across reads — then
/// emits the lines in lexicographic byte order (the C-locale order: the
/// newline is a terminator, not part of the key; a final unterminated
/// line is emitted as stored). Reads park on an empty pipe like `wc`;
/// the emit phase writes with `write_blocking`, so a full output pipe
/// parks instead of failing — and if the reader leaves early (e.g. a
/// downstream `head`), the retry observes `EPIPE` and sort exits 2.
/// v1: no options (a leading `-` is treated as a file name) and the
/// whole input is buffered in the applet data (the pipeline tests feed
/// it a few KiB). Data layout: read phases `[phase, file_idx, fd,
/// files, raw...]`; the sort phase rebuilds it to `[3, off u32 LE,
/// count u32 LE, sorted...]` where `off` survives blocked writes.
pub fn sort<K: Kernel, A: BufferAlloc>(ctx: &mut Ctx<'_, K, A>) -> Step {
    let task = ctx.task;
    if ctx.data.is_empty() {
        ctx.data.extend_from_slice(&[0, 0, 0, 1]); // phase, file_idx, fd, files
    }
    loop {
        match ctx.data[0] {
            // Choose the next source: stdin when no files were given,
            // otherwise the file at file_idx.
            0 => {
                let files = ctx.data[3] as usize;
                let src = if ctx.data[1] == 0 && ctx.argv().len() <= files {
                    0
                } else {
                    let path = match ctx.argv().get(files + ctx.data[1] as usize) {
                        Some(p) => p.clone(),
                        None => return Step::Exit(2),
                    };
                    match ctx.vfs().open(task, &path, O_RDONLY, 0) {
                        Ok(fd) => fd,
                        Err(_) => return Step::Exit(2),
                    }
                };
                ctx.data[0] = 1;
                ctx.data[2] = src as u8;
            }
            // Read chunks into the raw buffer until EOF.
            1 => {
                let fd = ctx.data[2] as u32;
                let mut buf = [0u8; 16];
                match ctx.read_blocking(fd, &mut buf) {
                    ReadBlock::Data(0) => {
                        if fd != 0 {
                            ctx.vfs().close(task, fd).ok();
                        }
                        ctx.data[1] += 1; // next source index
                        ctx.data[2] = 0;
                        let files = ctx.data[3] as usize;
                        if ctx.argv().len() > files
                            && ctx.argv().get(files + ctx.data[1] as usize).is_some()
                        {
                            ctx.data[0] = 0; // choose the next file
                        } else {
                            ctx.data[0] = 2; // all sources read: sort
                        }
                    }
                    ReadBlock::Data(n) => {
                        ctx.data.extend_from_slice(&buf[..n]);
                    }
                    ReadBlock::WouldBlock => {
                        return Step::Blocked(BlockReason::Readable(fd));
                    }
                    ReadBlock::Err(_) => return Step::Exit(2),
                }
            }
            // Sort: split the accumulated raw buffer into lines, sort
            // them, and rebuild the data as the sorted output with an
            // emit cursor (`off`).
            2 => {
                let raw = ctx.data[4..].to_vec();
                let mut lines = split_lines(&raw);
                lines.sort_by(|a, b| line_key(a).cmp(line_key(b)));
                ctx.data.clear();
                ctx.data.push(3); // phase: emit
                ctx.data.extend_from_slice(&0u32.to_le_bytes()); // off
                ctx.data.extend_from_slice(&(lines.len() as u32).to_le_bytes()); // count
                for line in &lines {
                    ctx.data.extend_from_slice(line);
                }
            }
            // Emit the sorted text, parking on a full output pipe.
            3 => {
                let off = read_u32(&ctx.data, 1) as usize;
                let count = read_u32(&ctx.data, 5);
                if count == 0 || off >= ctx.data.len() - 9 {
                    return Step::Exit(0);
                }
                // The copy keeps ctx.data unborrowed across the write.
                let pending = ctx.data[9 + off..].to_vec();
                match ctx.write_blocking(1, &pending) {
                    WriteBlock::Data(k) => {
                        ctx.data[1..5]
                            .copy_from_slice(&((off + k) as u32).to_le_bytes());
                    }
                    WriteBlock::WouldBlock => return Step::Blocked(BlockReason::Writable(1)),
                    WriteBlock::Err(_) => return Step::Exit(2),
                }
            }
            _ => return Step::Exit(2),
        }
    }
}

/// `seq [FIRST [STEP]] LAST`: prints the integers FIRST, FIRST+STEP, ...,
/// up to LAST, one per line (the one-arg form starts at 1, the two-arg
/// form steps by 1). It is a pure producer — it never reads — so a
/// pipeline gets a fast writer that fills the 4 KiB pipe and parks on
/// `Writable`, waking to `EPIPE` if the reader leaves early
/// (`seq 10000 | head -n 3`). `FIRST > LAST` prints nothing and exits 0;
/// an unparsable value or a zero STEP (which would never terminate) exits
/// 2. v1: non-negative integers only. Data layout: `[phase, i u64 LE,
/// last u64 LE, step u64 LE, done, pending_len, pending...]` — a line
/// left by a short pipe write survives the parks, and `done` stops the
/// loop if advancing past `u64::MAX`.
pub fn seq<K: Kernel, A: BufferAlloc>(ctx: &mut Ctx<'_, K, A>) -> Step {
    if ctx.data.is_empty() {
        let (first, last, step) = match parse_seq_args(ctx.argv()) {
            Some(x) => x,
            None => return Step::Exit(2),
        };
        ctx.data.push(1); // phase
        ctx.data.extend_from_slice(&first.to_le_bytes());
        ctx.data.extend_from_slice(&last.to_le_bytes());
        ctx.data.extend_from_slice(&step.to_le_bytes());
        ctx.data.push(0); // done
        ctx.data.push(0); // pending_len
    }
    loop {
        let i = read_u64(&ctx.data, 1);
        let last = read_u64(&ctx.data, 9);
        if ctx.data[25] == 1 || i > last {
            return Step::Exit(0);
        }
        if ctx.data[26] > 0 {
            // Finish a line left by a short write; advancing happens
            // only once the whole line is out.
            let pending = ctx.data[27..].to_vec();
            let plen = ctx.data[26] as usize;
            match ctx.write_blocking(1, &pending) {
                WriteBlock::Data(k) => {
                    if k == plen {
                        ctx.data.truncate(27);
                        ctx.data[26] = 0;
                        advance_seq(&mut ctx.data);
                    } else {
                        ctx.data.drain(27..27 + k);
                        ctx.data[26] -= k as u8;
                    }
                }
                WriteBlock::WouldBlock => return Step::Blocked(BlockReason::Writable(1)),
                WriteBlock::Err(_) => return Step::Exit(2),
            }
        } else {
            let text = alloc::format!("{}\n", i);
            let n = text.len();
            match ctx.write_blocking(1, text.as_bytes()) {
                WriteBlock::Data(k) => {
                    if k == n {
                        advance_seq(&mut ctx.data);
                    } else {
                        // A short write (unreachable for lines ≤ 21 B in
                        // a 4 KiB pipe, but kept correct): stash the tail.
                        ctx.data[26] = (n - k) as u8;
                        ctx.data.extend_from_slice(&text.as_bytes()[k..n]);
                    }
                }
                WriteBlock::WouldBlock => return Step::Blocked(BlockReason::Writable(1)),
                WriteBlock::Err(_) => return Step::Exit(2),
            }
        }
    }
}

/// Advance `seq`'s counter by its step; if the addition would overflow
/// `u64`, set the `done` flag instead so the next loop stops.
fn advance_seq(data: &mut Vec<u8>) {
    let i = read_u64(data, 1);
    let step = read_u64(data, 17);
    match i.checked_add(step) {
        Some(next) => data[1..9].copy_from_slice(&next.to_le_bytes()),
        None => data[25] = 1,
    }
}

/// `tee [file...]`: copies stdin to stdout AND to each named file,
/// fanning every chunk out to all destinations (no files = plain
/// stdin→stdout copy). The chunk is written to the files first, then to
/// stdout with `write_blocking` — so a full stdout pipe parks with the
/// chunk still pending, and the files are NOT rewritten when the park
/// ends. If the downstream reader leaves early (`seq 10000 | tee
/// /tmp/out | head -n 3`), the stdout retry wakes to `EPIPE` and tee
/// exits 2, leaving the file with everything written so far. Data
/// layout: `[phase, fd, nfiles, file_fds..., pending...]` — `pending`
/// is the chunk read but not yet fully written to stdout.
pub fn tee<K: Kernel, A: BufferAlloc>(ctx: &mut Ctx<'_, K, A>) -> Step {
    let task = ctx.task;
    if ctx.data.is_empty() {
        // Copy the paths out: argv is borrowed from the manager, and
        // open needs the vfs mutably.
        let paths: Vec<String> = ctx.argv().iter().skip(1).map(|s| s.clone()).collect();
        ctx.data.push(1); // phase: read a chunk
        ctx.data.push(0); // fd: stdin
        ctx.data.push(paths.len() as u8); // nfiles
        for p in &paths {
            match ctx.vfs().open(task, p, O_CREAT | O_WRONLY | O_TRUNC, 0o644) {
                Ok(fd) => ctx.data.push(fd as u8),
                Err(_) => return Step::Exit(2),
            }
        }
    }
    loop {
        let nfiles = ctx.data[2] as usize;
        let hdr = 3 + nfiles; // file fds at 3..hdr, pending at hdr..
        match ctx.data[0] {
            // Read a chunk into the pending buffer.
            1 => {
                let mut buf = [0u8; 16];
                match ctx.read_blocking(0, &mut buf) {
                    ReadBlock::Data(0) => {
                        let fds: Vec<u32> =
                            ctx.data[3..hdr].iter().map(|&b| b as u32).collect();
                        for fd in &fds {
                            ctx.vfs().close(task, *fd).ok();
                        }
                        return Step::Exit(0);
                    }
                    ReadBlock::Data(n) => {
                        ctx.data.extend_from_slice(&buf[..n]);
                        ctx.data[0] = 2; // fan out to the files first
                    }
                    ReadBlock::WouldBlock => return Step::Blocked(BlockReason::Readable(0)),
                    ReadBlock::Err(_) => return Step::Exit(2),
                }
            }
            // Write the pending chunk to every file (files never block;
            // this runs once per chunk, before the stdout write).
            2 => {
                let pending = ctx.data[hdr..].to_vec();
                let fds: Vec<u32> =
                    ctx.data[3..hdr].iter().map(|&b| b as u32).collect();
                for fd in &fds {
                    if ctx.vfs().write(task, *fd, &pending).is_err() {
                        return Step::Exit(2);
                    }
                }
                ctx.data[0] = 3; // then stdout, which can park
            }
            // Write the pending chunk to stdout, parking on a full
            // pipe. The chunk stays pending (and the files are not
            // rewritten) until it fully lands.
            3 => {
                let pending = ctx.data[hdr..].to_vec();
                let plen = pending.len();
                match ctx.write_blocking(1, &pending) {
                    WriteBlock::Data(k) => {
                        if k == plen {
                            ctx.data.truncate(hdr);
                            ctx.data[0] = 1; // back to reading
                        } else {
                            ctx.data.drain(hdr..hdr + k);
                        }
                    }
                    WriteBlock::WouldBlock => return Step::Blocked(BlockReason::Writable(1)),
                    WriteBlock::Err(_) => return Step::Exit(2),
                }
            }
            _ => return Step::Exit(2),
        }
    }
}

/// `tr SET1 SET2` / `tr -d SET1`: the streaming character filter. Map
/// mode translates each input byte through a 256-byte table built once
/// at init (`SET1` → corresponding `SET2` byte, `SET2`'s last byte
/// repeated for a longer `SET1`); delete mode drops the `SET1` bytes.
/// Sets support ranges (`a-z`) and backslash escapes (`\n` `\t` `\r`
/// `\\`); a descending range (`z-a`), an empty `SET2`, or a trailing
/// backslash is a usage error (exit 2). Reads via `read_blocking` and
/// writes via `write_blocking` — an empty pipe parks on `Readable`, a
/// full one on `Writable` with the translated chunk still pending, and
/// an early-exiting reader (`tr | head`) wakes the retry to `EPIPE`
/// (exit 2). Data layout: `[phase, mode, table (256 B), pending...]` —
/// `mode` 0 = map, 1 = delete; the pending buffer is the translated
/// chunk not yet fully written.
pub fn tr<K: Kernel, A: BufferAlloc>(ctx: &mut Ctx<'_, K, A>) -> Step {
    if ctx.data.is_empty() {
        // Copy the args out: argv borrows the manager, and building the
        // table pushes into ctx.data.
        let (delete, s1, s2) = match ctx.argv().len() {
            3 if ctx.argv()[1] == "-d" => (true, ctx.argv()[2].clone(), String::new()),
            3 => (false, ctx.argv()[1].clone(), ctx.argv()[2].clone()),
            _ => return Step::Exit(2),
        };
        let s1 = match expand_set(&s1) {
            Some(v) => v,
            None => return Step::Exit(2),
        };
        let table: Vec<u8> = if delete {
            build_delete(&s1).to_vec()
        } else {
            if s2.is_empty() {
                return Step::Exit(2); // empty SET2: nothing to map to
            }
            let s2 = match expand_set(&s2) {
                Some(v) => v,
                None => return Step::Exit(2),
            };
            build_map(&s1, &s2).to_vec()
        };
        ctx.data.push(0); // phase: read a chunk
        ctx.data.push(delete as u8); // mode
        ctx.data.extend_from_slice(&table); // table at 2..258
    }
    let delete = ctx.data[1] == 1;
    loop {
        match ctx.data[0] {
            // Read a chunk, translate it, append to pending.
            0 => {
                let mut buf = [0u8; 16];
                match ctx.read_blocking(0, &mut buf) {
                    ReadBlock::Data(0) => return Step::Exit(0),
                    ReadBlock::Data(n) => {
                        let table = ctx.data[2..258].to_vec();
                        let out = tr_chunk(&buf[..n], &table, delete);
                        ctx.data.extend_from_slice(&out);
                        ctx.data[0] = 1;
                    }
                    ReadBlock::WouldBlock => return Step::Blocked(BlockReason::Readable(0)),
                    ReadBlock::Err(_) => return Step::Exit(2),
                }
            }
            // Write the pending chunk, parking on a full pipe. The copy
            // keeps ctx.data unborrowed across the write.
            1 => {
                let pending = ctx.data[258..].to_vec();
                match ctx.write_blocking(1, &pending) {
                    WriteBlock::Data(k) => {
                        if k == pending.len() {
                            ctx.data.truncate(258);
                            ctx.data[0] = 0;
                        } else {
                            ctx.data.drain(258..258 + k);
                        }
                    }
                    WriteBlock::WouldBlock => return Step::Blocked(BlockReason::Writable(1)),
                    WriteBlock::Err(_) => return Step::Exit(2),
                }
            }
            _ => return Step::Exit(2),
        }
    }
}

/// Read `cut`'s ranges back out of the data header (`[start u32 LE, end
/// u32 LE]` each, `end` 0 = open).
fn read_cut_ranges(data: &[u8]) -> Vec<(u32, u32)> {
    let n = data[6] as usize;
    let mut r = Vec::new();
    for i in 0..n {
        let base = 7 + 8 * i; // each range is a (start u32, end u32) pair
        r.push((read_u32(data, base), read_u32(data, base + 4)));
    }
    r
}

/// Write `cut`'s pending-output length into the data header at `h`.
fn write_out_len(data: &mut [u8], h: usize, len: usize) {
    data[h..h + 4].copy_from_slice(&(len as u32).to_le_bytes());
}

/// `cut -d DELIM -f LIST [file...]` / `cut -c LIST [file...]` (and the
/// `-b` alias): the column extractor. Field mode splits each line on the
/// delimiter (default tab) and emits the selected fields joined with it;
/// char mode emits the selected byte positions concatenated. Lists are
/// comma-separated 1-based positions and ranges (`1`, `3-5`, `2-`,
/// `-3`, `-`), parsed once at init; an open range extends to the last
/// field/byte and out-of-range selections contribute nothing. Options
/// may be attached (`-d:`) or separate (`-d :`); exactly one of
/// `-f`/`-c`/`-b` is required (`-d` only with `-f`), a descending range
/// or unknown option is a usage error (exit 2). Lines are buffered
/// across reads (a line may span chunks; the partial line survives
/// parks), each complete line is extracted and written with
/// `write_blocking` — a full pipe parks with the pending output intact
/// and an early-exiting reader (`cut | head`) wakes the retry to
/// `EPIPE`. Data layout: `[phase, file_idx, first_file_arg, fd, mode,
/// delim, nranges, ranges..., out_len u32, in (partial line)..., out
/// (pending output)...]` — `first_file_arg` is the argv index of the
/// first file (255 for stdin only), `in` is everything between the
/// header and the pending output.
pub fn cut<K: Kernel, A: BufferAlloc>(ctx: &mut Ctx<'_, K, A>) -> Step {
    let task = ctx.task;
    if ctx.data.is_empty() {
        // Parse the options: attached or separate forms, in any order;
        // non-option args are files. `mode` is None until one of
        // -f/-c/-b appears; a second one (or -d without -f) is an error.
        let argv = ctx.argv();
        let mut mode: Option<u8> = None;
        let mut delim = b'\t';
        let mut ranges: Vec<(u32, u32)> = Vec::new();
        let mut first_file_arg = 255u8;
        let mut i = 1;
        while i < argv.len() {
            let a = &argv[i];
            if let Some(rest) = a.strip_prefix("-d") {
                let d = if rest.is_empty() {
                    i += 1;
                    match argv.get(i) {
                        Some(s) if s.len() == 1 => s.as_bytes()[0],
                        _ => return Step::Exit(2),
                    }
                } else if rest.len() == 1 {
                    rest.as_bytes()[0]
                } else {
                    return Step::Exit(2); // multi-char delimiter
                };
                delim = d;
            } else if let Some(rest) = a.strip_prefix("-f") {
                if mode.is_some() {
                    return Step::Exit(2); // -b/-c/-f are exclusive
                }
                let list = if rest.is_empty() {
                    i += 1;
                    match argv.get(i) {
                        Some(s) => s.clone(),
                        None => return Step::Exit(2),
                    }
                } else {
                    rest.to_string()
                };
                ranges = match parse_cut_ranges(&list) {
                    Some(r) => r,
                    None => return Step::Exit(2),
                };
                mode = Some(0);
            } else if let Some(rest) = a.strip_prefix("-c") {
                if mode.is_some() {
                    return Step::Exit(2);
                }
                let list = if rest.is_empty() {
                    i += 1;
                    match argv.get(i) {
                        Some(s) => s.clone(),
                        None => return Step::Exit(2),
                    }
                } else {
                    rest.to_string()
                };
                ranges = match parse_cut_ranges(&list) {
                    Some(r) => r,
                    None => return Step::Exit(2),
                };
                mode = Some(1);
            } else if let Some(rest) = a.strip_prefix("-b") {
                // -b is a byte alias of -c in this byte-oriented world.
                if mode.is_some() {
                    return Step::Exit(2);
                }
                let list = if rest.is_empty() {
                    i += 1;
                    match argv.get(i) {
                        Some(s) => s.clone(),
                        None => return Step::Exit(2),
                    }
                } else {
                    rest.to_string()
                };
                ranges = match parse_cut_ranges(&list) {
                    Some(r) => r,
                    None => return Step::Exit(2),
                };
                mode = Some(1);
            } else if a.starts_with('-') && a.len() > 1 {
                return Step::Exit(2); // unknown option
            } else if first_file_arg == 255 {
                first_file_arg = i as u8;
            }
            i += 1;
        }
        let mode = match mode {
            Some(m) => m,
            None => return Step::Exit(2), // one of -f/-c/-b is required
        };
        if delim != b'\t' && mode != 0 {
            return Step::Exit(2); // -d only makes sense with -f
        }
        if ranges.len() > 255 {
            return Step::Exit(2); // header byte budget
        }
        ctx.data.push(0); // phase
        ctx.data.push(0); // file_idx
        ctx.data.push(first_file_arg);
        ctx.data.push(0); // fd
        ctx.data.push(mode);
        ctx.data.push(delim);
        ctx.data.push(ranges.len() as u8); // nranges
        for (s, e) in &ranges {
            ctx.data.extend_from_slice(&s.to_le_bytes());
            ctx.data.extend_from_slice(&e.to_le_bytes());
        }
        ctx.data.extend_from_slice(&0u32.to_le_bytes()); // out_len
    }
    loop {
        let nranges = ctx.data[6] as usize;
        let h = 7 + 8 * nranges; // out_len at h..h + 4
        let in_start = h + 4; // in = data[in_start..len - out_len]
        let out_len = read_u32(&ctx.data, h) as usize;
        match ctx.data[0] {
            // Pick the next source: stdin when no files were given,
            // otherwise the file at argv[first_file_arg + file_idx].
            0 => {
                if ctx.data[2] == 255 {
                    ctx.data[0] = 1; // stdin
                } else {
                    let path = match ctx.argv().get(ctx.data[2] as usize + ctx.data[1] as usize) {
                        Some(p) => p.clone(),
                        None => return Step::Exit(2),
                    };
                    match ctx.vfs().open(task, &path, O_RDONLY, 0) {
                        Ok(fd) => {
                            ctx.data[3] = fd as u8;
                            ctx.data[0] = 1;
                        }
                        Err(_) => return Step::Exit(2),
                    }
                }
            }
            // Read into the partial-line region; on a complete line,
            // extract it and move to the write phase. The newline is
            // the terminator, not a selected byte.
            1 => {
                let src = ctx.data[3] as u32;
                let mut buf = [0u8; 16];
                match ctx.read_blocking(src, &mut buf) {
                    ReadBlock::Data(0) => {
                        if ctx.data.len() - out_len > in_start {
                            // EOF: flush the final line — the in region
                            // minus a trailing newline, so a terminated
                            // last line is not double-newlined.
                            let mut line = ctx.data[in_start..ctx.data.len() - out_len].to_vec();
                            if line.ends_with(b"\n") {
                                line.pop();
                            }
                            ctx.data.truncate(in_start);
                            let ranges = read_cut_ranges(&ctx.data);
                            let out = cut_line(&line, ctx.data[4], ctx.data[5], &ranges);
                            ctx.data.extend_from_slice(&out);
                            write_out_len(&mut ctx.data, h, out.len());
                            ctx.data[0] = 2;
                        } else {
                            // Advance to the next source.
                            let fd = ctx.data[3];
                            if fd != 0 {
                                ctx.vfs().close(task, fd as u32).ok();
                                ctx.data[3] = 0;
                            }
                            if ctx.data[2] == 255 {
                                return Step::Exit(0);
                            }
                            let nfiles = ctx.argv().len() as u8 - ctx.data[2];
                            ctx.data[1] += 1;
                            if ctx.data[1] >= nfiles {
                                return Step::Exit(0);
                            }
                            ctx.data[0] = 0;
                        }
                    }
                    ReadBlock::Data(n) => {
                        ctx.data.extend_from_slice(&buf[..n]);
                        if let Some(nl) = ctx.data[in_start..ctx.data.len() - out_len]
                            .iter()
                            .position(|&b| b == b'\n')
                        {
                            // Extract the first complete line (newline
                            // excluded), leaving the rest in the region.
                            let line = ctx.data[in_start..in_start + nl].to_vec();
                            ctx.data.drain(in_start..in_start + nl + 1);
                            let ranges = read_cut_ranges(&ctx.data);
                            let out = cut_line(&line, ctx.data[4], ctx.data[5], &ranges);
                            ctx.data.extend_from_slice(&out);
                            write_out_len(&mut ctx.data, h, out.len());
                            ctx.data[0] = 2;
                        }
                    }
                    ReadBlock::WouldBlock => return Step::Blocked(BlockReason::Readable(src)),
                    ReadBlock::Err(_) => return Step::Exit(2),
                }
            }
            // Write the pending output line, parking on a full pipe.
            // The copy keeps ctx.data unborrowed across the write.
            2 => {
                let pending = ctx.data[ctx.data.len() - out_len..].to_vec();
                match ctx.write_blocking(1, &pending) {
                    WriteBlock::Data(k) => {
                        if k == pending.len() {
                            ctx.data.truncate(ctx.data.len() - out_len);
                            write_out_len(&mut ctx.data, h, 0);
                            ctx.data[0] = 1;
                        } else {
                            let out_start = ctx.data.len() - out_len;
                            ctx.data.drain(out_start..out_start + k);
                            write_out_len(&mut ctx.data, h, out_len - k);
                        }
                    }
                    WriteBlock::WouldBlock => return Step::Blocked(BlockReason::Writable(1)),
                    WriteBlock::Err(_) => return Step::Exit(2),
                }
            }
            _ => return Step::Exit(2),
        }
    }
}

/// Build the output for a completed run — the run's representative line
/// plus a newline — honoring the mode: default prints every run, `-c`
/// prefixes a `{:>7}` count (GNU's `%7d `), `-d` prints only runs longer
/// than one line, `-u` only runs of exactly one. An empty output is a
/// valid no-op (`-d`/`-u` on a non-matching run).
fn uniq_run_output(data: &[u8], mode: u8) -> Vec<u8> {
    let rep_len = read_u32(data, 9) as usize;
    let count = read_u32(data, 5);
    let rep = &data[18..18 + rep_len];
    let mut out = Vec::new();
    match mode {
        1 => out.extend_from_slice(alloc::format!("{:>7} ", count).as_bytes()),
        2 if count <= 1 => return out,
        3 if count != 1 => return out,
        _ => {}
    }
    out.extend_from_slice(rep);
    out.push(b'\n');
    out
}

/// Process one complete line (content, newline excluded) against the
/// run state in `data`: an empty representative (no run yet) makes the
/// line the representative, an equal line grows the run's count, and a
/// different line emits the completed run's output and starts a new run
/// with this line. Emission sets phase 2 and the pending out_len; the
/// new representative replaces the old in place (the partial input after
/// it shifts, sizes are tiny). The `have_rep` flag distinguishes an
/// empty representative line from "no run yet".
fn uniq_process_line(data: &mut Vec<u8>, line: Vec<u8>) {
    let mode = data[4];
    if data[17] == 0 {
        // First line of the run: it becomes the representative, inserted
        // at the fixed rep slot (the partial region may hold leftover
        // input after it, which shifts — sizes are tiny).
        data.splice(18..18, line.iter().copied());
        data[9..13].copy_from_slice(&(line.len() as u32).to_le_bytes());
        data[5..9].copy_from_slice(&1u32.to_le_bytes());
        data[17] = 1;
    } else {
        let rep_len = read_u32(data, 9) as usize;
        if data[18..18 + rep_len] == line[..] {
            // Same content: grow the run.
            let count = read_u32(data, 5) + 1;
            data[5..9].copy_from_slice(&count.to_le_bytes());
        } else {
            // Different: emit the completed run, then start a new one.
            let out = uniq_run_output(data, mode);
            data.splice(18..18 + rep_len, line.iter().copied());
            data[9..13].copy_from_slice(&(line.len() as u32).to_le_bytes());
            data[5..9].copy_from_slice(&1u32.to_le_bytes());
            data.extend_from_slice(&out);
            data[13..17].copy_from_slice(&(out.len() as u32).to_le_bytes());
            data[0] = 2;
        }
    }
}

/// `uniq [OPTION]... [FILE]`: filters adjacent duplicate lines — a line
/// equal to the previous one is folded into its run, and only the run's
/// representative is printed. Default prints every run once; `-c`
/// prefixes each line with the `{:>7}` run count; `-d` prints only
/// runs longer than one line; `-u` only runs of exactly one. The
/// options are mutually exclusive (like GNU), an unknown option or more
/// than one file is a usage error (exit 2). Comparison is on the line
/// content without the newline (so a final unterminated line matches a
/// terminated equal line), and only ADJACENT lines dedup — `a b a`
/// keeps both `a` lines. Reads via `read_blocking`, so an empty pipe
/// stdin parks; a line may span reads (the partial line survives
/// parks), and each run's output is written with `write_blocking` — a
/// full pipe parks with the pending output intact and an early-exiting
/// reader (`uniq | head`) wakes the retry to `EPIPE`. Data layout:
/// `[phase, file_idx, first_file_arg, fd, mode, count u32, rep_len u32,
/// out_len u32, have_rep, rep..., in (partial line)..., out (pending)...]`.
pub fn uniq<K: Kernel, A: BufferAlloc>(ctx: &mut Ctx<'_, K, A>) -> Step {
    let task = ctx.task;
    if ctx.data.is_empty() {
        let argv = ctx.argv();
        let mut mode = 0u8;
        let mut first_file_arg = 255u8;
        let mut i = 1;
        while i < argv.len() {
            let a = &argv[i];
            if a == "-c" || a == "-d" || a == "-u" {
                if mode != 0 {
                    return Step::Exit(2); // -c/-d/-u are exclusive
                }
                mode = if a == "-c" { 1 } else if a == "-d" { 2 } else { 3 };
            } else if a.starts_with('-') && a.len() > 1 {
                return Step::Exit(2); // unknown option
            } else if first_file_arg == 255 {
                first_file_arg = i as u8;
            }
            i += 1;
        }
        if first_file_arg != 255 && argv.len() - first_file_arg as usize > 1 {
            return Step::Exit(2); // v1: stdin or one file
        }
        ctx.data.extend_from_slice(&[0, 0, first_file_arg, 0, mode]);
        ctx.data.extend_from_slice(&0u32.to_le_bytes()); // count
        ctx.data.extend_from_slice(&0u32.to_le_bytes()); // rep_len
        ctx.data.extend_from_slice(&0u32.to_le_bytes()); // out_len
        ctx.data.push(0); // have_rep
    }
    loop {
        let rep_len = read_u32(&ctx.data, 9) as usize;
        let in_start = 18 + rep_len; // in = data[in_start..len - out_len]
        let out_len = read_u32(&ctx.data, 13) as usize;
        match ctx.data[0] {
            // Pick the source: stdin when no file, else the file.
            0 => {
                if ctx.data[2] == 255 {
                    ctx.data[0] = 1; // stdin
                } else {
                    let path = match ctx.argv().get(ctx.data[2] as usize) {
                        Some(p) => p.clone(),
                        None => return Step::Exit(2),
                    };
                    match ctx.vfs().open(task, &path, O_RDONLY, 0) {
                        Ok(fd) => {
                            ctx.data[3] = fd as u8;
                            ctx.data[0] = 1;
                        }
                        Err(_) => return Step::Exit(2),
                    }
                }
            }
            // Read into the partial-line region; on a complete line,
            // process it. At EOF, process a pending final line (content
            // minus a trailing newline), then flush the final run, then
            // advance to the next source.
            1 => {
                let src = ctx.data[3] as u32;
                let mut buf = [0u8; 16];
                match ctx.read_blocking(src, &mut buf) {
                    ReadBlock::Data(0) => {
                        // Drain the remaining complete lines one per
                        // visit — a chunk can hold several, and
                        // process_line may emit (phase 2), parking the
                        // write while the leftover stays in the region.
                        let rep_len = read_u32(&ctx.data, 9) as usize;
                        let in_start = 18 + rep_len;
                        if let Some(nl) = ctx.data[in_start..ctx.data.len() - out_len]
                            .iter()
                            .position(|&b| b == b'\n')
                        {
                            let line = ctx.data[in_start..in_start + nl].to_vec();
                            ctx.data.drain(in_start..in_start + nl + 1);
                            uniq_process_line(&mut ctx.data, line);
                        } else if ctx.data.len() - out_len > in_start {
                            // A final unterminated fragment is a line.
                            let line = ctx.data[in_start..ctx.data.len() - out_len].to_vec();
                            ctx.data.truncate(in_start);
                            uniq_process_line(&mut ctx.data, line);
                        } else if ctx.data[17] == 1 {
                            // EOF: emit the final run.
                            let out = uniq_run_output(&ctx.data, ctx.data[4]);
                            ctx.data.truncate(18);
                            ctx.data[5..9].copy_from_slice(&0u32.to_le_bytes());
                            ctx.data[9..13].copy_from_slice(&0u32.to_le_bytes());
                            ctx.data[17] = 0;
                            ctx.data.extend_from_slice(&out);
                            ctx.data[13..17].copy_from_slice(&(out.len() as u32).to_le_bytes());
                            ctx.data[0] = 2;
                        } else {
                            // Advance to the next source (at most one
                            // file, so this exits).
                            let fd = ctx.data[3];
                            if fd != 0 {
                                ctx.vfs().close(task, fd as u32).ok();
                            }
                            return Step::Exit(0);
                        }
                    }
                    ReadBlock::Data(n) => {
                        ctx.data.extend_from_slice(&buf[..n]);
                        if let Some(nl) = ctx.data[in_start..ctx.data.len() - out_len]
                            .iter()
                            .position(|&b| b == b'\n')
                        {
                            // Process the first complete line, leaving the
                            // rest of the chunk in the partial region.
                            let line = ctx.data[in_start..in_start + nl].to_vec();
                            ctx.data.drain(in_start..in_start + nl + 1);
                            uniq_process_line(&mut ctx.data, line);
                        }
                    }
                    ReadBlock::WouldBlock => return Step::Blocked(BlockReason::Readable(src)),
                    ReadBlock::Err(_) => return Step::Exit(2),
                }
            }
            // Write the pending run output, parking on a full pipe. The
            // copy keeps ctx.data unborrowed across the write.
            2 => {
                let pending = ctx.data[ctx.data.len() - out_len..].to_vec();
                match ctx.write_blocking(1, &pending) {
                    WriteBlock::Data(k) => {
                        if k == pending.len() {
                            ctx.data.truncate(ctx.data.len() - out_len);
                            ctx.data[13..17].copy_from_slice(&0u32.to_le_bytes());
                            ctx.data[0] = 1;
                        } else {
                            let out_start = ctx.data.len() - out_len;
                            ctx.data.drain(out_start..out_start + k);
                            ctx.data[13..17]
                                .copy_from_slice(&((out_len - k) as u32).to_le_bytes());
                        }
                    }
                    WriteBlock::WouldBlock => return Step::Blocked(BlockReason::Writable(1)),
                    WriteBlock::Err(_) => return Step::Exit(2),
                }
            }
            _ => return Step::Exit(2),
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

/// Append a length-prefixed env region to `data`: `[n, (name_len, name,
/// val_len, val)...]` — the shell's variable table (see `sh`). Accepts
/// either `(&str, &str)` or `(String, String)` pairs.
fn env_push<S: AsRef<str>>(data: &mut Vec<u8>, entries: &[(S, S)]) {
    data.push(entries.len() as u8);
    for (k, v) in entries {
        let (k, v) = (k.as_ref(), v.as_ref());
        data.push(k.len() as u8);
        data.extend_from_slice(k.as_bytes());
        data.push(v.len() as u8);
        data.extend_from_slice(v.as_bytes());
    }
}

/// The index just past the env region starting at `start` — where the
/// input queue begins. A malformed or truncated region is treated as
/// consuming the rest of the data (defensive; the shell never builds one).
fn env_end(data: &[u8], start: usize) -> usize {
    let Some(&n) = data.get(start) else {
        return data.len();
    };
    let mut p = start + 1;
    for _ in 0..n {
        let Some(&nl) = data.get(p) else {
            return data.len();
        };
        p += 1 + nl as usize;
        let Some(&vl) = data.get(p) else {
            return data.len();
        };
        p += 1 + vl as usize;
    }
    p
}

/// Decode the env region starting at `start` into name/value pairs (a
/// malformed region yields what was decodable).
fn env_pairs(data: &[u8], start: usize) -> Vec<(String, String)> {
    let mut out = Vec::new();
    let Some(&n) = data.get(start) else {
        return out;
    };
    let mut p = start + 1;
    for _ in 0..n {
        let Some(&nl) = data.get(p) else {
            break;
        };
        let nl = nl as usize;
        let Some(name) = data
            .get(p + 1..p + 1 + nl)
            .and_then(|b| core::str::from_utf8(b).ok())
        else {
            break;
        };
        p += 1 + nl;
        let Some(&vl) = data.get(p) else {
            break;
        };
        let vl = vl as usize;
        let Some(val) = data
            .get(p + 1..p + 1 + vl)
            .and_then(|b| core::str::from_utf8(b).ok())
        else {
            break;
        };
        p += 1 + vl;
        out.push((name.to_string(), val.to_string()));
    }
    out
}

/// Upsert a variable in an env pair list: replace in place (keeping the
/// region order) or append a new entry.
fn set_var(env: &mut Vec<(String, String)>, name: String, value: String) {
    match env.iter_mut().find(|(k, _)| *k == name) {
        Some(slot) => slot.1 = value,
        None => env.push((name, value)),
    }
}

/// A valid shell variable name: letters, digits and `_`, starting with a
/// letter or `_` (the same rule the tokenizer's `$NAME` expansion uses).
fn valid_name(name: &str) -> bool {
    let mut cs = name.chars();
    match cs.next() {
        Some(c) if c.is_ascii_alphabetic() || c == '_' => {}
        _ => return false,
    }
    cs.all(|c| c.is_ascii_alphanumeric() || c == '_')
}

/// A command the shell runs itself instead of forking: builtins mutate
/// the shell's persistent env region, so they must execute in the shell
/// task. v1: recognized only as the bare name (no `/`), standalone
/// (no pipeline) — see `run_line`.
fn is_builtin(name: &str) -> bool {
    matches!(name, "export" | "setenv" | "unset" | "unsetenv" | "kill" | "cd" | "pwd")
}

/// Apply a stage's `<` / `>` redirects at fd 0 / fd 1. Returns false on
/// failure — the caller picks the exit status (127 for a program child,
/// 2 for a builtin).
fn apply_redirects<K: Kernel, A: BufferAlloc>(ctx: &mut Ctx<'_, K, A>, stage: &Stage) -> bool {
    let task = ctx.task;
    if let Some(path) = &stage.out_redir {
        let fd = match ctx.vfs().open(task, path, O_CREAT | O_WRONLY | O_TRUNC, 0o644) {
            Ok(fd) => fd,
            Err(_) => return false,
        };
        if ctx.vfs().dup2(task, fd, 1).is_err() {
            return false;
        }
        ctx.vfs().close(task, fd).ok();
    }
    if let Some(path) = &stage.in_redir {
        let fd = match ctx.vfs().open(task, path, O_RDONLY, 0) {
            Ok(fd) => fd,
            Err(_) => return false,
        };
        if ctx.vfs().dup2(task, fd, 0).is_err() {
            return false;
        }
        ctx.vfs().close(task, fd).ok();
    }
    true
}

/// `sh`: the minimal interactive shell. Data layout: `data[0]` = phase,
/// `data[1]` = last exit status (`$?`), `data[2..]` = the env region
/// (length-prefixed name/value list — `env_end` finds where it ends), and
/// after that the input batch queue (unconsumed console bytes, possibly
/// several lines). Phase 0 prints the prompt (initializing the default
/// environment: `PATH=/bin`, `HOME=/root`), phase 1 consumes a complete
/// buffered line or reads more console input (parking on an empty console
/// via `read_blocking`), phase 2 runs the first buffered line, phase 3
/// reaps the pipeline's children in order — the env region and the queued
/// remainder survive all of it (rebuilt after the wait state), so a
/// multi-line read runs line by line with a single prompt around the
/// batch and `$VAR` expansion keeps working. A pipeline child's fork
/// snapshot carries `[2, status, stage, n_stages, nfd, pipes..., in_len,
/// in..., out_len, out..., prog_len, prog..., n_args, (arg_len,
/// arg...)..., queue, FORK_MARKER]` — argv is already expanded and the
/// program resolved on the parent side, so the child needs no env — see
/// `sh_child`.
pub fn sh<K: Kernel, A: BufferAlloc>(ctx: &mut Ctx<'_, K, A>) -> Step {
    let task = ctx.task;
    if ctx.data.is_empty() {
        // Default environment: PATH=/bin (the default search path for
        // bare command names) and HOME=/root, encoded as the env region.
        ctx.data.extend_from_slice(&[0, 0]); // phase, last status
        env_push(&mut ctx.data, &[("PATH", "/bin"), ("HOME", "/root")]);
        return Step::Yield;
    }
    if is_child(&ctx.data) {
        return sh_child(ctx);
    }
    if let Some(sig) = ctx.check_signal() {
        if sig == SIGINT {
            if ctx.data[0] == 3 {
                ctx.kill_group(SIGINT);
            }
            let qstart = env_end(&ctx.data, 2);
            let env_reg = ctx.data[2..qstart].to_vec();
            let status = signal_exit_code(SIGINT) as u8;
            ctx.data.clear();
            ctx.data.push(0);
            ctx.data.push(status);
            ctx.data.extend_from_slice(&env_reg);
            return Step::Yield;
        }
        return Step::Exit(signal_exit_code(sig));
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
            let qstart = env_end(&ctx.data, 2);
            // A buffered complete line runs without touching the console.
            if ctx.data[qstart..].contains(&b'\n') {
                ctx.data[0] = 2;
                return Step::Yield;
            }
            let mut buf = [0u8; 16];
            match ctx.read_blocking(0, &mut buf) {
                // EOF: run any partial buffered line, then exit.
                ReadBlock::Data(0) => {
                    if ctx.data.len() > qstart {
                        ctx.data[0] = 2;
                        Step::Yield
                    } else {
                        Step::Exit(0)
                    }
                }
                ReadBlock::Data(n) => {
                    // Scan for Ctrl-C (\x03) in the console input. If
                    // found, discard the partial line, send SIGINT to
                    // the process group, and re-prompt.
                    let chunk = &buf[..n];
                    if chunk.contains(&0x03) {
                        ctx.kill_group(SIGINT);
                        ctx.data.truncate(qstart);
                        ctx.data[0] = 0; // re-prompt
                        return Step::Yield;
                    }
                    ctx.data.extend_from_slice(chunk);
                    if ctx.data[qstart..].contains(&b'\n') {
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

/// Phase 2: run the first complete line of the buffered batch. A
/// standalone `export`/`setenv` command is intercepted as a builtin and
/// runs in the shell task (it mutates the env region); anything else
/// forks one child per pipeline stage. The queued remainder is appended
/// after every fork snapshot and the wait state, so multi-line input
/// runs line by line and nothing is dropped. See `sh_child` for the
/// fork-snapshot layout.
fn run_line<K: Kernel, A: BufferAlloc>(ctx: &mut Ctx<'_, K, A>) -> Step {
    let task = ctx.task;
    let status = ctx.data[1];
    // The env region sits between phase/status and the queue; both the
    // line and the unconsumed remainder live after it.
    let qstart = env_end(&ctx.data, 2);
    let env_region = ctx.data[2..qstart].to_vec();
    let nl = ctx.data[qstart..].iter().position(|&b| b == b'\n');
    let (line, remainder) = match nl {
        Some(i) => (ctx.data[qstart..qstart + i].to_vec(), ctx.data[qstart + i + 1..].to_vec()),
        None => (ctx.data[qstart..].to_vec(), Vec::new()),
    };
    let line = match core::str::from_utf8(&line) {
        Ok(s) => s.trim(),
        Err(_) => "", // garbage: drop the line like a comment
    };
    let env = env_pairs(&ctx.data, 2);
    let stages = parse_line(line, status, &env);
    if stages.is_empty() {
        // Blank or comment line: drop it; re-prompt only if the batch is
        // drained, otherwise continue straight to the next buffered line.
        // The env region survives the rebuild.
        ctx.data.clear();
        ctx.data.push(if remainder.is_empty() { 0 } else { 1 });
        ctx.data.push(status);
        ctx.data.extend_from_slice(&env_region);
        ctx.data.extend_from_slice(&remainder);
        return Step::Yield;
    }
    // A standalone line with no command word: leading assignments persist
    // in the shell (POSIX); a builtin (export/setenv/unset/unsetenv) runs
    // in the shell itself — both mutate the env region, so neither can be
    // a forked child. In a pipeline they resolve as a program and fail
    // 127 like any unknown command.
    if stages.len() == 1 {
        let st = &stages[0];
        if st.argv.is_empty() && !st.assigns.is_empty() {
            return assign_line(ctx, st, &remainder, &env);
        }
        if !st.argv.is_empty() && is_builtin(&st.argv[0]) {
            return run_builtin(ctx, st, &remainder, &env);
        }
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
    // The search path for bare command names, from the shell's env
    // (v1 default /bin when unset).
    let path = env
        .iter()
        .find(|(k, _)| *k == "PATH")
        .map(|(_, v)| v.clone())
        .unwrap_or_else(|| "/bin".to_string());
    // Fork one child per stage; each snapshot carries its stage index, the
    // pipe fds, the `<` / `>` redirect targets (length-prefixed), the
    // program file resolved through PATH (length-prefixed — computed
    // here in the parent, which has the env), the argv (one
    // length-prefixed field per argument, so words containing spaces —
    // quoted or backslash-escaped — arrive at the child exactly as
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
        // A stage's assignments scope its own resolution: a leading
        // `PATH=/usr/bin cmd` searches /usr/bin for this command only
        // (the shell's PATH is untouched).
        let mut stage_path = path.clone();
        for (k, v) in &stage.assigns {
            if k == "PATH" {
                stage_path = v.clone();
            }
        }
        // An empty argv (assigns-only stage inside a pipeline) has no
        // program: resolve to nothing, the child exits 127.
        let prog = if stage.argv.is_empty() {
            String::new()
        } else {
            resolve_path(ctx.vfs(), task, &stage.argv[0], &stage_path)
        };
        ctx.data.push(prog.len() as u8);
        ctx.data.extend_from_slice(prog.as_bytes());
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
    // Wait state: [3, status, n, reaped, children (u32 LE each)..., env...,
    // remainder]. Child ids are 4 bytes: task ids come from a monotonic
    // counter, so a long session exceeds 255 forks.
    ctx.data.clear();
    ctx.data.push(3);
    ctx.data.push(status);
    ctx.data.push(children.len() as u8);
    ctx.data.push(0); // reaped count
    for c in &children {
        ctx.data.extend_from_slice(&c.to_le_bytes());
    }
    ctx.data.extend_from_slice(&env_region);
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
        // Children ids sit at data[4..4+4n] (u32 LE); the env region and
        // the queued remainder survive after them.
        let qstart = env_end(&ctx.data, 4 + 4 * n);
        let env_region = ctx.data[4 + 4 * n..qstart].to_vec();
        let queue = ctx.data[qstart..].to_vec();
        let status = ctx.data[1];
        ctx.data.clear();
        ctx.data.push(if queue.is_empty() { 0 } else { 1 });
        ctx.data.push(status);
        ctx.data.extend_from_slice(&env_region);
        ctx.data.extend_from_slice(&queue);
        return Step::Yield;
    }
    let off = 4 + 4 * reaped;
    let c = u32::from_le_bytes(ctx.data[off..off + 4].try_into().unwrap());
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

/// Run a builtin command in the shell task: apply the stage's redirects
/// (so `export > file` writes the listing there), apply the stage's
/// `NAME=value` assignments FIRST (they persist for special builtins,
/// per POSIX — `FOO=bar export` exports FOO=bar), execute the builtin
/// against the env copy, and rebuild the persistent data — the new env
/// region, the exit status (for the next `$?`), and the unconsumed batch
/// — then continue to the next buffered line or the prompt.
fn run_builtin<K: Kernel, A: BufferAlloc>(
    ctx: &mut Ctx<'_, K, A>,
    stage: &Stage,
    remainder: &[u8],
    env: &[(String, String)],
) -> Step {
    let task = ctx.task;
    if !apply_redirects(ctx, stage) {
        return builtin_done(ctx, remainder, env, 2);
    }
    let mut new_env = env.to_vec();
    for (k, v) in &stage.assigns {
        set_var(&mut new_env, k.clone(), v.clone());
    }
    let code = match stage.argv[0].as_str() {
        "export" => do_export(&mut new_env, &stage.argv[1..], task, ctx),
        "setenv" => do_setenv(&mut new_env, &stage.argv[1..]),
        "unset" => do_unset(&mut new_env, &stage.argv[1..], false),
        "unsetenv" => do_unset(&mut new_env, &stage.argv[1..], true),
        "kill" => do_kill(ctx, &stage.argv[1..]),
        "cd" => do_cd(ctx, &stage.argv[1..]),
        "pwd" => do_pwd(ctx),
        _ => 2, // unreachable: is_builtin guards the call
    };
    builtin_done(ctx, remainder, &new_env, code)
}

/// A bare-assignment line (`FOO=bar` with no command): the assignments
/// affect the shell itself (POSIX — with no command name they persist),
/// silently, with status 0.
fn assign_line<K: Kernel, A: BufferAlloc>(
    ctx: &mut Ctx<'_, K, A>,
    stage: &Stage,
    remainder: &[u8],
    env: &[(String, String)],
) -> Step {
    let mut new_env = env.to_vec();
    for (k, v) in &stage.assigns {
        set_var(&mut new_env, k.clone(), v.clone());
    }
    builtin_done(ctx, remainder, &new_env, 0)
}

/// Rebuild the shell's persistent data after a builtin: `[phase, status,
/// env..., remainder...]` — phase 0 (prompt) if the batch drained,
/// else phase 1 (next buffered line).
fn builtin_done<K: Kernel, A: BufferAlloc>(
    ctx: &mut Ctx<'_, K, A>,
    remainder: &[u8],
    env: &[(String, String)],
    code: u8,
) -> Step {
    ctx.data.clear();
    ctx.data.push(if remainder.is_empty() { 0 } else { 1 });
    ctx.data.push(code);
    env_push(&mut ctx.data, env);
    ctx.data.extend_from_slice(remainder);
    Step::Yield
}

/// `export` builtin: no args lists the environment (NAME=value lines in
/// region order); `export NAME=value` sets a variable (upsert); `export
/// NAME` is a no-op success — v1 exports everything implicitly, so there
/// is nothing to mark. A bad name is a usage error (2).
fn do_export<K: Kernel, A: BufferAlloc>(
    env: &mut Vec<(String, String)>,
    args: &[String],
    task: u32,
    ctx: &mut Ctx<'_, K, A>,
) -> u8 {
    if args.is_empty() {
        let mut text = String::new();
        for (k, v) in env.iter() {
            text.push_str(k);
            text.push('=');
            text.push_str(v);
            text.push('\n');
        }
        return if ctx.vfs().write(task, 1, text.as_bytes()).is_err() {
            2
        } else {
            0
        };
    }
    for a in args {
        match a.find('=') {
            Some(eq) => {
                let (name, value) = a.split_at(eq);
                if !valid_name(name) {
                    return 2;
                }
                set_var(env, name.to_string(), value[1..].to_string());
            }
            None => {
                if !valid_name(a) {
                    return 2;
                }
                // `export NAME` without `=` — mark-export is a no-op.
            }
        }
    }
    0
}

/// `setenv` builtin: `setenv NAME value` sets a variable (upsert). Any
/// other arity, or a bad name, is a usage error (2).
fn do_setenv(env: &mut Vec<(String, String)>, args: &[String]) -> u8 {
    if args.len() != 2 || !valid_name(&args[0]) {
        return 2;
    }
    set_var(env, args[0].clone(), args[1].clone());
    0
}

/// `unset` / `unsetenv` builtin: removes variables from the env region.
/// `unset NAME...` takes one or more names; `unsetenv` (csh-style) takes
/// exactly one. A missing variable is a no-op success; a bad name or
/// wrong arity is a usage error (2).
fn do_unset(env: &mut Vec<(String, String)>, args: &[String], one_only: bool) -> u8 {
    if args.is_empty() || (one_only && args.len() != 1) {
        return 2;
    }
    for a in args {
        if !valid_name(a) {
            return 2;
        }
    }
    env.retain(|(k, _)| !args.iter().any(|a| a == k));
    0
}

/// Parse a signal number from a string: a bare decimal (`9`), or a name
/// with optional `-`/`SIG` prefix (`-9`, `SIGINT`, `-SIGTERM`, `INT`).
fn parse_signal(s: &str) -> Option<i32> {
    let s = s.trim_start_matches('-');
    let s = s.strip_prefix("SIG").unwrap_or(s);
    match s {
        "HUP" => Some(1), "INT" => Some(2), "QUIT" => Some(3),
        "KILL" => Some(9), "PIPE" => Some(13), "TERM" => Some(15),
        _ => s.parse().ok(),
    }
}

/// `kill` builtin: `kill [-SIGNAL] PID...` sends a signal to each PID.
/// Default signal is SIGTERM (15). A PID of 0 sends to the shell's
/// process group (`kill_group`). Returns 0 on success, 1 if any
/// signal could not be delivered.
fn do_kill<K: Kernel, A: BufferAlloc>(
    ctx: &mut Ctx<'_, K, A>,
    args: &[String],
) -> u8 {
    if args.is_empty() {
        return 2; // usage error
    }
    let mut sig = 15i32; // default: SIGTERM
    let mut pid_start = 0;
    if args[0].starts_with('-') && args[0].len() > 1 {
        match parse_signal(&args[0]) {
            Some(s) => {
                sig = s;
                pid_start = 1;
            }
            None => return 2,
        }
    }
    if pid_start >= args.len() {
        return 2;
    }
    let mut ok = true;
    for a in &args[pid_start..] {
        if a == "0" {
            ctx.kill_group(sig);
        } else if let Ok(pid) = a.parse::<u32>() {
            if ctx.kill(pid, sig).is_err() {
                ok = false;
            }
        } else {
            ok = false;
        }
    }
    if ok { 0 } else { 1 }
}

/// `cd` builtin: `cd [DIR]` changes the working directory. With no
/// arguments, changes to `$HOME` (default `/`). Returns 0 on success, 1
/// on failure.
fn do_cd<K: Kernel, A: BufferAlloc>(
    ctx: &mut Ctx<'_, K, A>,
    args: &[String],
) -> u8 {
    let dir = if args.is_empty() {
        "/".to_string()
    } else {
        args[0].clone()
    };
    match ctx.chdir(&dir) {
        Ok(()) => 0,
        Err(_) => 1,
    }
}

/// `pwd` builtin: prints the current working directory to stdout.
/// Returns 0 on success, 1 on failure.
fn do_pwd<K: Kernel, A: BufferAlloc>(
    ctx: &mut Ctx<'_, K, A>,
) -> u8 {
    let task = ctx.task;
    match ctx.getcwd() {
        Ok(cwd) => {
            let mut out = String::from(cwd);
            out.push('\n');
            if ctx.vfs().write(task, 1, out.as_bytes()).is_err() {
                1
            } else {
                0
            }
        }
        Err(_) => 1,
    }
}

/// A pipeline stage child: its snapshot is `[2, status, stage, n_stages,
/// nfd, pipes..., in_len, in_path..., out_len, out_path..., prog_len,
/// prog..., n_args, (arg_len, arg...)..., queue, FORK_MARKER]` — one
/// length-prefixed field per argument, so each argv word (even one
/// containing spaces, from quoting or backslash escapes) arrives exactly
/// as parsed and the child stops before the shell's queued input batch;
/// `prog` is the program file the parent already resolved through PATH.
/// Wire stdin/stdout to the pipe ends (or the console for the first/last
/// stage), apply the stage's `<` / `>` redirects (which override the
/// pipeline connection at fd 0 / fd 1, like POSIX), drop every pipe fd,
/// and exec — on any failure the child exits 127, which the shell reaps.
fn sh_child<K: Kernel, A: BufferAlloc>(ctx: &mut Ctx<'_, K, A>) -> Step {
    let task = ctx.task;
    // The fork marker is the last byte; copy the snapshot out so the
    // parse borrows a local, not ctx.data.
    let d = ctx.data[..ctx.data.len() - 1].to_vec();
    let stage = d[2] as usize;
    let n = d[3] as usize;
    let nfd = d[4] as usize;
    if d.len() < 5 + nfd {
        return Step::Exit(127);
    }
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
    let prog_len = d[p] as usize;
    let prog = match core::str::from_utf8(&d[p + 1..p + 1 + prog_len]) {
        Ok(s) => s.to_string(),
        Err(_) => return Step::Exit(127),
    };
    p += 1 + prog_len;
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
    // argv[0] stays as typed; the program file was resolved through PATH
    // in the parent (run_line), which holds the shell's environment.
    match ctx.exec(&prog, &argv) {
        Ok(()) => Step::Yield,
        Err(_) => Step::Exit(127),
    }
}

/// The candidate paths for `tok` under a `:`-separated search `path`: a
/// token containing `/` is used as-is with no lookup; otherwise each
/// non-empty directory yields `<dir>/<tok>` in order. An empty `path`
/// yields no candidates (the command is unresolvable → 127).
fn path_candidates(tok: &str, path: &str) -> Vec<String> {
    if tok.contains('/') {
        return alloc::vec![tok.to_string()];
    }
    let mut out = Vec::new();
    for dir in path.split(':') {
        if !dir.is_empty() {
            out.push(alloc::format!("{}/{}", dir, tok));
        }
    }
    out
}

/// Resolve a command name through the shell's `PATH` by probing each
/// candidate with an open — the fs is a script store, so a readable file
/// is executable. A token containing `/` is used as-is; a bare name with
/// no hit returns `tok` itself, and the child's exec fails 127, which the
/// shell reaps. Runs in the parent (which holds the env); the resolved
/// path travels in the fork snapshot.
fn resolve_path<K: Kernel, A: BufferAlloc>(
    vfs: &mut Vfs<K, A>,
    task: u32,
    tok: &str,
    path: &str,
) -> String {
    for candidate in path_candidates(tok, path) {
        if let Ok(fd) = vfs.open(task, &candidate, O_RDONLY, 0) {
            vfs.close(task, fd).ok();
            return candidate;
        }
    }
    tok.to_string()
}

/// One pipeline stage: the command's argv plus optional `<` stdin / `>`
/// stdout redirect targets (v1: truncating `>`, no `>>`) and leading
/// `NAME=value` assignment words (scoped to the stage; persisted only for
/// special builtins or a bare-assignment line — see `run_line`).
#[derive(Debug, PartialEq, Eq)]
pub(crate) struct Stage {
    pub argv: Vec<String>,
    pub in_redir: Option<String>,
    pub out_redir: Option<String>,
    pub assigns: Vec<(String, String)>,
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

/// Expand a `$...` variable reference starting at `chars[i]` (a `$`) into
/// `word`, returning the index just past the expansion. `$?` / `${?}` is
/// the last exit status; `$NAME` / `${NAME}` resolves through `env` (a
/// name is letters, digits and `_`, starting with a letter or `_`; an
/// unset name expands to nothing — POSIX). A `$` not followed by a name
/// or `{` is literal; an unterminated `${` is literal too (v1 lenient).
fn expand_var(
    chars: &[char],
    i: usize,
    last_status: u8,
    env: &[(String, String)],
    word: &mut String,
) -> usize {
    debug_assert_eq!(chars[i], '$');
    let next = match chars.get(i + 1) {
        Some(&c) => c,
        None => {
            word.push('$');
            return i + 1;
        }
    };
    if next == '{' {
        match chars[i + 2..].iter().position(|&c| c == '}') {
            Some(rel) => {
                let name: String = chars[i + 2..i + 2 + rel].iter().collect();
                let val = if name == "?" {
                    Some(last_status.to_string())
                } else {
                    env.iter().find(|(k, _)| *k == name).map(|(_, v)| v.clone())
                };
                word.push_str(&val.unwrap_or_default());
                i + 3 + rel
            }
            None => {
                // Unterminated `${` — literal (v1 lenient).
                word.push('$');
                word.push('{');
                i + 2
            }
        }
    } else if next == '?' {
        word.push_str(&last_status.to_string());
        i + 2
    } else if next.is_ascii_alphabetic() || next == '_' {
        let mut j = i + 1;
        while j < chars.len() && (chars[j].is_ascii_alphanumeric() || chars[j] == '_') {
            j += 1;
        }
        let name: String = chars[i + 1..j].iter().collect();
        let val = env
            .iter()
            .find(|(k, _)| *k == name)
            .map(|(_, v)| v.as_str())
            .unwrap_or("");
        word.push_str(val);
        j
    } else {
        word.push('$');
        i + 1
    }
}

/// Tokenize a command line into words and metacharacters, honoring single
/// and double quotes, backslash escapes, and `$` variable expansion. A
/// quoted section is a literal part of its word, so quotes group
/// whitespace — and, quoted, the metacharacters `|` `<` `>` — into a
/// single argument. `$?` expands to the last exit status and `$NAME` /
/// `${NAME}` expand through `env` (the shell's environment), wherever
/// they appear: unquoted, and inside double quotes (POSIX). Single
/// quotes are fully literal, so `'$?'` stays `$?` and `'\'` is a
/// backslash. Outside quotes, backslash removes the special meaning of
/// the next character (POSIX): `\ ` is a literal space, `\$` suppresses
/// expansion, and `\|` is a word, not a pipe; a trailing backslash is
/// dropped (v1 lenient). Inside double quotes, backslash escapes only
/// `$`, `"` and `\` (POSIX); elsewhere it stays literal. An unterminated
/// quote runs to the end of the line (v1 is lenient — no syntax error).
fn tokenize(line: &str, last_status: u8, env: &[(String, String)]) -> Vec<Tok> {
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
                    } else if d == '$' {
                        i = expand_var(&chars, i, last_status, env, &mut word);
                    } else {
                        word.push(d);
                        i += 1;
                    }
                }
            }
            '$' => {
                i = expand_var(&chars, i, last_status, env, &mut word);
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
/// words and metacharacters (see `tokenize` for the quote, escape and
/// variable semantics); a lone unquoted `|` token separates stages; `>` /
/// `<` mark the next word as the stdout / stdin redirect target (the
/// target is not part of argv; a trailing or metachar redirect records an
/// empty path, which the stage child fails to open → exit 127). Empty
/// stages are skipped (a stray `|` cannot produce a stage with no
/// command). Returns [] for a blank line. v1: no `>>`, and `|` /
/// redirects need surrounding spaces (a quoted or escaped version is a
/// literal word).
fn parse_line(line: &str, last_status: u8, env: &[(String, String)]) -> Vec<Stage> {
    let toks = tokenize(line, last_status, env);
    let mut stages: Vec<Stage> = Vec::new();
    let mut argv: Vec<String> = Vec::new();
    let mut in_redir: Option<String> = None;
    let mut out_redir: Option<String> = None;
    let mut assigns: Vec<(String, String)> = Vec::new();
    let mut command_seen = false;
    let mut i = 0;
    while i < toks.len() {
        match &toks[i] {
            Tok::Pipe => {
                if !argv.is_empty() || in_redir.is_some() || out_redir.is_some() || !assigns.is_empty()
                {
                    stages.push(Stage {
                        argv: core::mem::take(&mut argv),
                        in_redir: in_redir.take(),
                        out_redir: out_redir.take(),
                        assigns: core::mem::take(&mut assigns),
                    });
                }
                command_seen = false;
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
                // Leading NAME=value words with a valid name are scoped
                // assignments, not argv (POSIX); the first non-assignment
                // word is the command. Redirects may sit in between.
                if !command_seen {
                    if let Some(eq) = w.find('=') {
                        let name = &w[..eq];
                        if valid_name(name) {
                            assigns.push((name.to_string(), w[eq + 1..].to_string()));
                            i += 1;
                            continue;
                        }
                    }
                    command_seen = true;
                }
                argv.push(w.clone());
                i += 1;
            }
        }
    }
    if !argv.is_empty() || in_redir.is_some() || out_redir.is_some() || !assigns.is_empty() {
        stages.push(Stage {
            argv,
            in_redir,
            out_redir,
            assigns,
        });
    }
    stages
}

/// `nc` (netcat): minimal TCP client/server. Two modes:
///
/// - **Client**: `nc <host> <port>` — connect, relay stdin↔socket.
/// - **Listen**: `nc -l <port>` — bind, listen, accept one client,
///   relay stdin↔accepted socket.
///
/// Data layout (`ctx.data`):
///
/// ```text
/// [0]    phase:
///        1   = connect (client)
///        2   = relay: read socket → write stdout
///        3   = relay: write pending chunk to stdout
///        0xFF = relay: read stdin → send to socket
///        4   = done (exit)
///        10  = bind + listen
///        11  = poll for incoming + accept
/// [1]    sock_id (driver-side socket ID)
/// [2..6] dest_ip / port  (client: connect target; listen: unused)
/// [6..8] dest_port (u16 LE)
/// [8..]  pending I/O data (read buffer or write remainder)
/// ```
/// `nc` (netcat): minimal TCP/UDP client/server. Three modes:
///
/// - **TCP client**: `nc <host> <port>` — connect, relay stdin↔socket.
/// - **TCP listen**: `nc -l <port>` — bind, listen, accept, relay.
/// - **UDP client**: `nc -u <host> <port>` — create SOCK_DGRAM, connect,
///   relay. No listen/accept (UDP is connectionless).
/// - **UDP listen**: `nc -u -l <port>` — create SOCK_DGRAM, bind, relay
///   (recv gives datagrams, send replies to last sender).
///
/// Data layout (`ctx.data`):
///
/// ```text
/// [0]    phase:
///        1   = TCP connect
///        2   = relay: read socket → write stdout (TCP + UDP)
///        3   = write pending chunk to stdout
///        0xFF = read stdin → send to socket
///        4   = done (exit)
///        10  = TCP bind + listen
///        11  = TCP poll + accept
///        12  = UDP bind (listen mode)
/// [1]    sock_id (driver-side socket ID)
/// [2..6] dest_ip   (client: connect target; listen: unused)
/// [6..8] dest_port (u16 LE)
/// [8]    flags: bit 0 = UDP mode
/// [9..]  pending I/O data (read buffer or write remainder)
/// ```
pub fn nc<K: Kernel, A: BufferAlloc>(ctx: &mut Ctx<'_, K, A>) -> Step {
    if ctx.data.is_empty() {
        let argv = ctx.argv().to_vec();
        if ctx.net().is_none() {
            return Step::Exit(1);
        }

        // Scan argv for flags: -l (listen), -u (UDP).
        let mut flags: u8 = 0;
        let mut positional: Vec<&str> = Vec::new();
        for arg in argv.iter().skip(1) {
            match arg.as_str() {
                "-l" => flags |= 0x02,
                "-u" => flags |= 0x01,
                other => positional.push(other),
            }
        }
        let listen = flags & 0x02 != 0;
        let udp = flags & 0x01 != 0;
        let sock_type = if udp {
            aerosls_proto::SOCK_DGRAM
        } else {
            aerosls_proto::SOCK_STREAM
        };

        if listen {
            // Listen mode: nc [-u] -l <port>
            let port_str = match positional.first() {
                Some(p) => p,
                None => return Step::Exit(2),
            };
            let port: u16 = match port_str.parse() {
                Ok(p) => p,
                Err(_) => return Step::Exit(2),
            };
            let sock_id = match ctx.net().unwrap().socket(sock_type) {
                Ok(id) => id,
                Err(_) => return Step::Exit(1),
            };
            ctx.data.resize(9, 0);
            ctx.data[1] = sock_id as u8;
            ctx.data[6..8].copy_from_slice(&port.to_le_bytes());
            ctx.data[8] = flags;
            if udp {
                // UDP: just bind, no listen.
                ctx.data[0] = 12;
            } else {
                ctx.data[0] = 10; // TCP: bind + listen
            }
            return Step::Yield;
        }

        // Client mode: nc [-u] <host> <port>
        let host_str = match positional.first() {
            Some(h) => h,
            None => return Step::Exit(2),
        };
        let port_str = match positional.get(1) {
            Some(p) => p,
            None => return Step::Exit(2),
        };
        let port: u16 = match port_str.parse() {
            Ok(p) => p,
            Err(_) => return Step::Exit(2),
        };
        let mut ip_bytes = [0u8; 4];
        for (idx, octet) in host_str.split('.').enumerate() {
            if idx >= 4 {
                return Step::Exit(2);
            }
            ip_bytes[idx] = match octet.parse() {
                Ok(v) => v,
                Err(_) => return Step::Exit(2),
            };
        }
        let ip = u32::from_le_bytes(ip_bytes);
        let sock_id = match ctx.net().unwrap().socket(sock_type) {
            Ok(id) => id,
            Err(_) => return Step::Exit(1),
        };
        ctx.data.resize(9, 0);
        ctx.data[1] = sock_id as u8;
        ctx.data[2..6].copy_from_slice(&ip.to_le_bytes());
        ctx.data[6..8].copy_from_slice(&port.to_le_bytes());
        ctx.data[8] = flags;
        if udp {
            // UDP client: connect (sets default destination), then relay.
            ctx.data[0] = 1;
        } else {
            ctx.data[0] = 1; // TCP: connect
        }
    }

    let udp = ctx.data.get(8).copied().unwrap_or(0) & 0x01 != 0;

    match ctx.data[0] {
        // ── TCP/UDP connect ───────────────────────────────────────────
        1 => {
            let sock_id = ctx.data[1] as u32;
            let ip = u32::from_le_bytes([
                ctx.data[2], ctx.data[3], ctx.data[4], ctx.data[5],
            ]);
            let port = u16::from_le_bytes([ctx.data[6], ctx.data[7]]);
            match ctx.net().unwrap().connect(sock_id, ip, port) {
                Ok(()) => {
                    ctx.data[0] = 2; // → relay
                    Step::Yield
                }
                Err(_) => Step::Exit(1),
            }
        }
        // ── TCP listen: bind + listen ─────────────────────────────────
        10 => {
            let sock_id = ctx.data[1] as u32;
            let port = u16::from_le_bytes([ctx.data[6], ctx.data[7]]);
            let net = ctx.net().unwrap();
            if net.bind(sock_id, 0, port).is_err() {
                return Step::Exit(1);
            }
            if net.listen(sock_id).is_err() {
                return Step::Exit(1);
            }
            ctx.data[0] = 11; // → accept
            Step::Yield
        }
        // ── TCP listen: poll + accept ─────────────────────────────────
        11 => {
            let sock_id = ctx.data[1] as u32;
            match ctx.net().unwrap().poll(sock_id) {
                Ok(events) if events & 0x01 != 0 => {
                    let accepted = match ctx.net().unwrap().accept(sock_id) {
                        Ok(id) => id,
                        Err(_) => return Step::Exit(1),
                    };
                    ctx.data[1] = accepted as u8;
                    ctx.data[0] = 2; // → relay
                    Step::Yield
                }
                _ => Step::Yield,
            }
        }
        // ── UDP listen: bind only ─────────────────────────────────────
        12 => {
            let sock_id = ctx.data[1] as u32;
            let port = u16::from_le_bytes([ctx.data[6], ctx.data[7]]);
            match ctx.net().unwrap().bind(sock_id, 0, port) {
                Ok(()) => {
                    ctx.data[0] = 2; // → relay (recv/send datagrams)
                    Step::Yield
                }
                Err(_) => Step::Exit(1),
            }
        }
        // ── relay: read socket → write stdout ─────────────────────────
        2 => {
            let sock_id = ctx.data[1] as u32;
            match ctx.net().unwrap().poll(sock_id) {
                Ok(events) if events & 0x01 != 0 => {
                    let mut buf = [0u8; 16];
                    let n = match ctx.net().unwrap().recv(sock_id, &mut buf) {
                        Ok(n) => n,
                        Err(_) => {
                            ctx.data[0] = 4;
                            return Step::Yield;
                        }
                    };
                    if n == 0 {
                        ctx.data[0] = 4;
                        return Step::Yield;
                    }
                    ctx.data.truncate(9);
                    ctx.data.extend_from_slice(&buf[..n]);
                    ctx.data[0] = 3;
                    Step::Yield
                }
                _ => {
                    let mut buf = [0u8; 16];
                    match ctx.read_blocking(0, &mut buf) {
                        ReadBlock::Data(0) => {
                            if !udp {
                                let sid = ctx.data[1] as u32;
                                let _ = ctx.net().unwrap().shutdown(sid, 1);
                            }
                            ctx.data[0] = 4; // done
                            Step::Yield
                        }
                        ReadBlock::Data(n) => {
                            let send_result = {
                                let sid = ctx.data[1] as u32;
                                ctx.net().unwrap().send(sid, &buf[..n])
                            };
                            match send_result {
                                Ok(_) => {
                                    ctx.data.truncate(9);
                                    ctx.data[0] = 2;
                                }
                                Err(_) => {
                                    ctx.data[0] = 4;
                                }
                            }
                            Step::Yield
                        }
                        ReadBlock::WouldBlock => {
                            Step::Blocked(BlockReason::Readable(0))
                        }
                        ReadBlock::Err(_) => Step::Exit(1),
                    }
                },
            }
        }
        // ── relay: write pending chunk to stdout ──────────────────────
        3 => {
            let pending = ctx.data[9..].to_vec();
            match ctx.write_blocking(1, &pending) {
                WriteBlock::Data(k) => {
                    if k == pending.len() {
                        ctx.data.truncate(9);
                        ctx.data[0] = 2;
                    } else {
                        ctx.data.drain(9..9 + k);
                    }
                    Step::Yield
                }
                WriteBlock::WouldBlock => {
                    Step::Blocked(BlockReason::Writable(1))
                }
                WriteBlock::Err(_) => Step::Exit(1),
            }
        }
        // ── done: close socket and exit ───────────────────────────────
        4 => {
            let sock_id = ctx.data[1] as u32;
            let _ = ctx.net().unwrap().close_socket(sock_id);
            Step::Exit(0)
        }
        _ => Step::Exit(1),
    }
}

/// Install the system applets into a proc manager (called by `boot()`).
pub fn register_default_applets<K: Kernel, A: BufferAlloc>(pm: &mut ProcManager<K, A>) {
    pm.register_applet("init", init);
    pm.register_applet("cat", cat);
    pm.register_applet("grep", grep);
    pm.register_applet("wc", wc);
    pm.register_applet("head", head);
    pm.register_applet("tail", tail);
    pm.register_applet("sort", sort);
    pm.register_applet("seq", seq);
    pm.register_applet("tee", tee);
    pm.register_applet("tr", tr);
    pm.register_applet("cut", cut);
    pm.register_applet("uniq", uniq);
    pm.register_applet("echo", echo);
    pm.register_applet("nc", nc);
    pm.register_applet("sh", sh);
    pm.register_applet("true", do_true);
    pm.register_applet("false", do_false);
}

#[cfg(test)]
mod tests {
    use super::{parse_line as parse_line_raw, Stage};

    fn st(argv: &[&str]) -> Stage {
        Stage {
            argv: argv.iter().map(|a| a.to_string()).collect(),
            in_redir: None,
            out_redir: None,
            assigns: Vec::new(),
        }
    }

    /// Parse with an empty environment (all `$VAR` expand to nothing) —
    /// the default for the existing tests, which predate variables.
    fn parse_line(line: &str, status: u8) -> Vec<Stage> {
        parse_line_raw(line, status, &[])
    }

    /// Build an environment from `(name, value)` string pairs.
    fn vars(pairs: &[(&str, &str)]) -> Vec<(String, String)> {
        pairs
            .iter()
            .map(|(k, v)| (k.to_string(), v.to_string()))
            .collect()
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
                assigns: Vec::new(),
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
                assigns: Vec::new(),
            }]
        );
        assert_eq!(
            parse_line("cat < /etc/passwd | grep root > /tmp/out", 0),
            vec![
                Stage {
                    argv: vec!["cat".to_string()],
                    in_redir: Some("/etc/passwd".to_string()),
                    out_redir: None,
                    assigns: Vec::new(),
                },
                Stage {
                    argv: vec!["grep".to_string(), "root".to_string()],
                    in_redir: None,
                    out_redir: Some("/tmp/out".to_string()),
                    assigns: Vec::new(),
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
                assigns: Vec::new(),
            }]
        );
        assert_eq!(
            parse_line("echo hi >", 0),
            vec![Stage {
                argv: vec!["echo".to_string(), "hi".to_string()],
                in_redir: None,
                out_redir: Some(String::new()),
                assigns: Vec::new(),
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
                assigns: Vec::new(),
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

    #[test]
    fn parse_line_expands_variables() {
        let env = vars(&[("PATH", "/bin"), ("HOME", "/root")]);
        // $NAME and ${NAME} both resolve; ${?} is the status.
        assert_eq!(
            parse_line_raw("echo $PATH ${HOME}", 0, &env),
            vec![st(&["echo", "/bin", "/root"])]
        );
        // An unset name expands to nothing — and an unquoted empty
        // expansion produces no field at all (POSIX), while a quoted one
        // is a real empty argument.
        assert_eq!(
            parse_line_raw("echo $UNSET ${?}", 7, &env),
            vec![st(&["echo", "7"])]
        );
        assert_eq!(
            parse_line_raw("echo \"$UNSET\"", 0, &env),
            vec![st(&["echo", ""])]
        );                // An unset name mid-word expands to nothing (POSIX). Name chars
        // are greedy, so `a$UNSETb` is `$UNSETb` (unset → empty); a
        // boundary like `.` keeps the name short.
        assert_eq!(
            parse_line_raw("echo a$UNSETb", 0, &env),
            vec![st(&["echo", "a"])]
        );
        assert_eq!(
            parse_line_raw("echo a$UNSET.b", 0, &env),
            vec![st(&["echo", "a.b"])]
        );
        // A lone `$` or a `$` before a non-name stays literal.
        assert_eq!(
            parse_line_raw("echo $ a$b", 3, &env),
            vec![st(&["echo", "$", "a"])]
        );
        // $PATH expands inside double quotes, not inside single quotes or
        // after a backslash escape.
        assert_eq!(
            parse_line_raw("echo \"$PATH\" '$PATH' \\$PATH", 0, &env),
            vec![st(&["echo", "/bin", "$PATH", "$PATH"])]
        );
    }

    #[test]
    fn parse_line_extracts_assignments() {
        // Leading NAME=value words become scoped assignments, not argv.
        assert_eq!(
            parse_line("FOO=bar echo hi", 0),
            vec![Stage {
                argv: vec!["echo".to_string(), "hi".to_string()],
                in_redir: None,
                out_redir: None,
                assigns: vec![("FOO".to_string(), "bar".to_string())],
            }]
        );
        // Several assignments accumulate; a word after the command is an
        // ordinary argument, even if it looks like an assignment.
        assert_eq!(
            parse_line("A=1 B=2 echo x A=3", 0),
            vec![Stage {
                argv: vec!["echo".to_string(), "x".to_string(), "A=3".to_string()],
                in_redir: None,
                out_redir: None,
                assigns: vec![
                    ("A".to_string(), "1".to_string()),
                    ("B".to_string(), "2".to_string()),
                ],
            }]
        );
        // An invalid name is a command word, not an assignment.
        assert_eq!(parse_line("1BAD=x cmd", 0), vec![st(&["1BAD=x", "cmd"])]);
        // A bare assignment line is a kept stage (it persists in the
        // shell — run_line's assign_line path).
        assert_eq!(
            parse_line("FOO=bar", 0),
            vec![Stage {
                argv: Vec::new(),
                in_redir: None,
                out_redir: None,
                assigns: vec![("FOO".to_string(), "bar".to_string())],
            }]
        );
        // Redirects may sit between assignments and the command.
        assert_eq!(
            parse_line("FOO=bar > /tmp/x echo hi", 0),
            vec![Stage {
                argv: vec!["echo".to_string(), "hi".to_string()],
                in_redir: None,
                out_redir: Some("/tmp/x".to_string()),
                assigns: vec![("FOO".to_string(), "bar".to_string())],
            }]
        );
    }

    #[test]
    fn grep_matches_patterns() {
        use super::is_match;
        // Fixed substring, matching anywhere in the line.
        assert!(is_match("root", b"root:x:0:0"));
        assert!(is_match("root", b"x root y"));
        assert!(!is_match("nope", b"root:x:0:0"));
        // `.` matches exactly one character.
        assert!(is_match("r..t", b"root:x"));
        assert!(!is_match("r..t", b"rt"));
        // `*` matches any run, including none.
        assert!(is_match("r*t", b"root"));
        assert!(is_match("r*t", b"rt"));
        assert!(is_match("a*b", b"xaxxb"));
        assert!(!is_match("a*b", b"xb"));
        // Backslash escapes make `*` / `.` literal.
        assert!(is_match("\\*", b"a*b"));
        assert!(!is_match("\\*", b"abc"));
        assert!(is_match("a\\.b", b"xa.b"));
        assert!(!is_match("a\\.b", b"xaxb"));
        // An empty pattern matches everything (like grep).
        assert!(is_match("", b"anything"));
    }

    #[test]
    fn wc_counts_across_chunk_boundaries() {
        use super::count_chunk;
        // A word split across chunks keeps its count; the whitespace
        // state carries the boundary.
        let mut lines = 0u32;
        let mut words = 0u32;
        let mut prev_ws = true;
        count_chunk(b"hello wor", &mut lines, &mut words, &mut prev_ws);
        assert_eq!((lines, words, prev_ws), (0, 2, false));
        count_chunk(b"ld\nnext", &mut lines, &mut words, &mut prev_ws);
        assert_eq!((lines, words, prev_ws), (1, 3, false));
        // Leading/tab whitespace and trailing newlines.
        let mut lines = 0u32;
        let mut words = 0u32;
        let mut prev_ws = true;
        count_chunk(b"   hi \t yo\n", &mut lines, &mut words, &mut prev_ws);
        assert_eq!((lines, words), (1, 2));
        // An empty chunk is a no-op.
        let mut lines = 0u32;
        let mut words = 0u32;
        let mut prev_ws = false;
        count_chunk(b"", &mut lines, &mut words, &mut prev_ws);
        assert_eq!((lines, words, prev_ws), (0, 0, false));
    }

    #[test]
    fn parse_n_reads_the_line_count() {
        use super::parse_n;
        let av = |a: &[&str]| a.iter().map(|s| s.to_string()).collect::<Vec<_>>();
        // Default: 10 lines, first file arg at argv[1].
        assert_eq!(parse_n(&av(&["head"])), Some((10, 1)));
        assert_eq!(parse_n(&av(&["head", "file"])), Some((10, 1)));
        // `-n N` (two-arg form) and `-nN` (attached) shift the file args.
        assert_eq!(parse_n(&av(&["head", "-n", "5"])), Some((5, 3)));
        assert_eq!(parse_n(&av(&["tail", "-n5"])), Some((5, 2)));
        assert_eq!(parse_n(&av(&["head", "-n", "0"])), Some((0, 3)));
        assert_eq!(
            parse_n(&av(&["head", "-n", "5", "a", "b"])),
            Some((5, 3))
        );
        // An unparsable count or an unknown option is a usage error.
        assert_eq!(parse_n(&av(&["head", "-n", "abc"])), None);
        assert_eq!(parse_n(&av(&["head", "-x"])), None);
        assert_eq!(parse_n(&av(&["head", "-n"])), None); // missing count
        // A negative count is not supported (usage error).
        assert_eq!(parse_n(&av(&["head", "-n", "-5"])), None);
    }

    #[test]
    fn drop_lines_trims_the_window() {
        use super::drop_lines;
        assert_eq!(drop_lines(b"a\nb\nc\n", 0), b"a\nb\nc\n");
        assert_eq!(drop_lines(b"a\nb\nc\n", 1), b"b\nc\n");
        assert_eq!(drop_lines(b"a\nb\nc\n", 2), b"c\n");
        assert_eq!(drop_lines(b"a\nb\nc\n", 3), b"");
        // Dropping more complete lines than exist drops everything,
        // including a trailing unterminated line.
        assert_eq!(drop_lines(b"a\nb\nc\n", 5), b"");
        // A partial (unterminated) final line survives the drop of the
        // complete lines before it...
        assert_eq!(drop_lines(b"a\npartial", 1), b"partial");
        // ...but is gone when it must be dropped too.
        assert_eq!(drop_lines(b"a\npartial", 2), b"");
        // A buffer with no newline at all is one (unterminated) line.
        assert_eq!(drop_lines(b"partial", 1), b"");
        assert_eq!(drop_lines(b"", 1), b"");
    }

    #[test]
    fn count_lines_counts_newlines() {
        use super::count_lines;
        assert_eq!(count_lines(b""), 0);
        assert_eq!(count_lines(b"ab"), 0);
        assert_eq!(count_lines(b"a\nb\n"), 2);
        assert_eq!(count_lines(b"a\nb"), 1); // trailing partial is not a line
    }

    #[test]
    fn split_lines_keeps_terminators_and_the_final_fragment() {
        use super::split_lines;
        assert_eq!(split_lines(b"a\nb\n"), vec![b"a\n".to_vec(), b"b\n".to_vec()]);
        // An unterminated final fragment is a line without a newline.
        assert_eq!(split_lines(b"a\nb"), vec![b"a\n".to_vec(), b"b".to_vec()]);
        // A lone newline is one empty line; empty input yields none.
        assert_eq!(split_lines(b"\n"), vec![b"\n".to_vec()]);
        assert!(split_lines(b"").is_empty());
    }

    #[test]
    fn parse_seq_args_reads_the_range() {
        use super::parse_seq_args;
        let av = |a: &[&str]| a.iter().map(|s| s.to_string()).collect::<Vec<_>>();
        // The three forms: LAST, FIRST LAST, FIRST STEP LAST.
        assert_eq!(parse_seq_args(&av(&["seq", "5"])), Some((1, 5, 1)));
        assert_eq!(parse_seq_args(&av(&["seq", "3", "7"])), Some((3, 7, 1)));
        assert_eq!(parse_seq_args(&av(&["seq", "2", "2", "10"])), Some((2, 10, 2)));
        // Zero is a valid start; FIRST > LAST is valid (prints nothing).
        assert_eq!(parse_seq_args(&av(&["seq", "0", "5"])), Some((0, 5, 1)));
        assert_eq!(parse_seq_args(&av(&["seq", "9", "5"])), Some((9, 5, 1)));
        // Errors: wrong arity, unparsable values, zero step.
        assert_eq!(parse_seq_args(&av(&["seq"])), None);
        assert_eq!(parse_seq_args(&av(&["seq", "1", "2", "3", "4"])), None);
        assert_eq!(parse_seq_args(&av(&["seq", "x"])), None);
        assert_eq!(parse_seq_args(&av(&["seq", "1", "0", "5"])), None);
        // Negative values are rejected (v1 is non-negative integers).
        assert_eq!(parse_seq_args(&av(&["seq", "-5"])), None);
    }

    #[test]
    fn expand_set_expands_ranges_and_escapes() {
        use super::expand_set;
        // Literals pass through; ranges expand ascending.
        assert_eq!(expand_set("abc"), Some(b"abc".to_vec()));
        assert_eq!(expand_set("a-cx-z"), Some(b"abcxyz".to_vec()));
        assert_eq!(expand_set("a-z"), Some(b"abcdefghijklmnopqrstuvwxyz".to_vec()));
        // A leading or trailing `-` is a literal minus.
        assert_eq!(expand_set("-a"), Some(b"-a".to_vec()));
        assert_eq!(expand_set("a-"), Some(b"a-".to_vec()));
        assert_eq!(expand_set("a-z-"), Some(b"abcdefghijklmnopqrstuvwxyz-".to_vec()));
        // Escapes: newline, tab, CR, backslash.
        assert_eq!(expand_set("\\n\\t\\r\\\\"), Some(vec![b'\n', b'\t', b'\r', b'\\']));
        // A range never extends past an escape: `a-\n` is a, -, newline.
        assert_eq!(expand_set("a-\\n"), Some(b"a-\n".to_vec()));
        // Malformed sets: descending range, trailing backslash.
        assert_eq!(expand_set("z-a"), None);
        assert_eq!(expand_set("a\\"), None);
    }

    #[test]
    fn build_table_and_tr_chunk_translate_and_delete() {
        use super::{build_delete, build_map, tr_chunk};
        // `tr a-z A-Z`: lower-case maps to upper, everything else passes.
        let t = build_map(b"abcdefghijklmnopqrstuvwxyz", b"ABCDEFGHIJKLMNOPQRSTUVWXYZ");
        assert_eq!(tr_chunk(b"hello world", &t, false), b"HELLO WORLD".to_vec());
        // A longer SET1 repeats SET2's last byte (GNU semantics).
        let t = build_map(b"abc", b"x");
        assert_eq!(tr_chunk(b"abcabc", &t, false), b"xxxxxx".to_vec());
        // Bytes outside SET1 are the identity.
        let t = build_map(b"aeiou", b"AEIOU");
        assert_eq!(tr_chunk(b"hello", &t, false), b"hEllO".to_vec());
        // `tr -d`: set members drop, everything else passes.
        let t = build_delete(b"l");
        assert_eq!(tr_chunk(b"hello", &t, true), b"heo".to_vec());
        let t = build_delete(b"aeiou");
        assert_eq!(tr_chunk(b"hello world", &t, true), b"hll wrld".to_vec());
        // An empty delete set keeps everything.
        let t = build_delete(b"");
        assert_eq!(tr_chunk(b"abc", &t, true), b"abc".to_vec());
    }

    #[test]
    fn parse_cut_ranges_reads_lists() {
        use super::parse_cut_ranges;
        // A lone position, mixed lists, open and from-start ranges.
        assert_eq!(parse_cut_ranges("1"), Some(vec![(1, 1)]));
        assert_eq!(parse_cut_ranges("1,3-5"), Some(vec![(1, 1), (3, 5)]));
        assert_eq!(parse_cut_ranges("-3"), Some(vec![(1, 3)]));
        assert_eq!(parse_cut_ranges("2-"), Some(vec![(2, 0)]));
        assert_eq!(parse_cut_ranges("-"), Some(vec![(1, 0)]));
        assert_eq!(parse_cut_ranges("5-7,1,9-"), Some(vec![(5, 7), (1, 1), (9, 0)]));
        // Errors: empty list, zero, descending, unparsable, empty member.
        assert_eq!(parse_cut_ranges(""), None);
        assert_eq!(parse_cut_ranges("0"), None);
        assert_eq!(parse_cut_ranges("3-2"), None);
        assert_eq!(parse_cut_ranges("abc"), None);
        assert_eq!(parse_cut_ranges("1,"), None);
        assert_eq!(parse_cut_ranges("1-2-"), None);
    }

    #[test]
    fn cut_line_selects_fields_and_chars() {
        use super::cut_line;
        // Field mode: single, multiple, and open ranges; the join uses
        // the delimiter; out-of-range fields are omitted entirely.
        assert_eq!(cut_line(b"a:b:c", 0, b':', &[(2, 2)]), b"b\n");
        assert_eq!(cut_line(b"a:b:c", 0, b':', &[(1, 1), (3, 3)]), b"a:c\n");
        assert_eq!(cut_line(b"a:b:c", 0, b':', &[(2, 0)]), b"b:c\n");
        assert_eq!(cut_line(b"a:b:c", 0, b':', &[(1, 0)]), b"a:b:c\n");
        assert_eq!(cut_line(b"a:b:c", 0, b':', &[(1, 1), (9, 9)]), b"a\n");
        assert_eq!(cut_line(b"a::c", 0, b':', &[(2, 2)]), b"\n"); // empty field
        assert_eq!(cut_line(b"a::c", 0, b':', &[(1, 2)]), b"a:\n");
        assert_eq!(cut_line(b"hello", 0, b'\t', &[(1, 1)]), b"hello\n"); // no delim
        // Char mode: positions concatenated, open range to line end.
        assert_eq!(cut_line(b"hello", 1, b'\t', &[(1, 3)]), b"hel\n");
        assert_eq!(cut_line(b"hello", 1, b'\t', &[(2, 0)]), b"ello\n");
        assert_eq!(cut_line(b"hello", 1, b'\t', &[(1, 2)]), b"he\n");
        assert_eq!(cut_line(b"hello", 1, b'\t', &[(1, 1), (3, 3), (5, 5)]), b"hlo\n");
        assert_eq!(cut_line(b"hello", 1, b'\t', &[(9, 9)]), b"\n"); // all out of range
        // An empty line yields a bare newline in both modes.
        assert_eq!(cut_line(b"", 1, b'\t', &[(1, 1)]), b"\n");
        assert_eq!(cut_line(b"", 0, b':', &[(1, 1)]), b"\n");
    }

    #[test]
    fn uniq_run_output_formats_runs() {
        use super::{read_u32, uniq_run_output};
        // A data vec holding the representative "pear" with a count.
        let build = |count: u32| {
            let mut d = vec![0u8; 18];
            d[5..9].copy_from_slice(&count.to_le_bytes());
            d[9..13].copy_from_slice(&4u32.to_le_bytes());
            d[17] = 1;
            d.extend_from_slice(b"pear");
            d
        };
        // Default: every run prints, count ignored.
        assert_eq!(uniq_run_output(&build(3), 0), b"pear\n");
        assert_eq!(uniq_run_output(&build(1), 0), b"pear\n");
        // -c: the {:>7} count prefix.
        assert_eq!(uniq_run_output(&build(3), 1), b"      3 pear\n");
        assert_eq!(uniq_run_output(&build(12), 1), b"     12 pear\n");
        // -d: only runs longer than one line.
        assert_eq!(uniq_run_output(&build(3), 2), b"pear\n");
        assert_eq!(uniq_run_output(&build(1), 2), b"");
        // -u: only runs of exactly one.
        assert_eq!(uniq_run_output(&build(1), 3), b"pear\n");
        assert_eq!(uniq_run_output(&build(3), 3), b"");
        // The helper reads rep_len/count from the header itself.
        assert_eq!(read_u32(&build(7), 5), 7);
    }

    #[test]
    fn uniq_process_line_tracks_runs() {
        use super::{read_u32, uniq_process_line};
        // Empty header: phase 0, mode 0, no run yet.
        let mut d = vec![0u8; 18];
        // The first line becomes the representative, no emission.
        uniq_process_line(&mut d, b"a".to_vec());
        assert_eq!(&d[18..19], b"a");
        assert_eq!(read_u32(&d, 5), 1); // count
        assert_eq!(read_u32(&d, 9), 1); // rep_len
        assert_eq!(d[17], 1); // have_rep
        assert_eq!(d[0], 0); // phase unchanged
        // An equal line grows the run, still no emission.
        uniq_process_line(&mut d, b"a".to_vec());
        assert_eq!(read_u32(&d, 5), 2);
        assert_eq!(d[0], 0);
        // A different line emits the run (phase 2, pending set) and
        // becomes the new representative.
        uniq_process_line(&mut d, b"b".to_vec());
        assert_eq!(d[0], 2);
        assert_eq!(read_u32(&d, 13), 2); // out_len = "a\n"
        let out_len = read_u32(&d, 13) as usize;
        assert_eq!(&d[d.len() - out_len..], b"a\n");
        assert_eq!(&d[18..19], b"b");
        assert_eq!(read_u32(&d, 5), 1);
        // An empty line is a valid representative: have_rep keeps it
        // distinct from "no run yet", so a second empty line matches.
        let mut d = vec![0u8; 18];
        uniq_process_line(&mut d, Vec::new());
        assert_eq!(d[17], 1);
        assert_eq!(read_u32(&d, 9), 0); // rep_len 0, but have_rep set
        uniq_process_line(&mut d, Vec::new());
        assert_eq!(read_u32(&d, 5), 2);
        assert_eq!(d[0], 0);
    }

    #[test]
    fn sort_orders_lines_lexicographically() {
        use super::{line_key, split_lines};
        let mut lines = split_lines(b"pear\napple\nfig\ndate\n");
        lines.sort_by(|a, b| line_key(a).cmp(line_key(b)));
        let out: Vec<Vec<u8>> = vec![b"apple\n".to_vec(), b"date\n".to_vec(), b"fig\n".to_vec(), b"pear\n".to_vec()];
        assert_eq!(lines, out);
        // Byte (C-locale) order, not numeric: "a10" sorts before "a2".
        let mut lines = split_lines(b"a2\na10\n");
        lines.sort_by(|a, b| line_key(a).cmp(line_key(b)));
        assert_eq!(lines, vec![b"a10\n".to_vec(), b"a2\n".to_vec()]);
        // Empty lines sort first; the unterminated final line sorts by
        // its content (the key strips the terminator).
        let mut lines = split_lines(b"z\n\nb\na");
        lines.sort_by(|a, b| line_key(a).cmp(line_key(b)));
        assert_eq!(
            lines,
            vec![
                b"\n".to_vec(),
                b"a".to_vec(),
                b"b\n".to_vec(),
                b"z\n".to_vec()
            ]
        );
    }

    #[test]
    fn path_candidates_searches_path_dirs() {
        use super::path_candidates;
        // A slash token skips the search path entirely.
        assert_eq!(
            path_candidates("/bin/cat", "/usr/bin:/bin"),
            vec!["/bin/cat"]
        );
        // Bare names try each directory in order.
        assert_eq!(
            path_candidates("cat", "/usr/bin:/bin"),
            vec!["/usr/bin/cat", "/bin/cat"]
        );
        // Empty directories are skipped (double colons, trailing colon);
        // an empty PATH yields no candidates (unresolvable → 127).
        assert_eq!(
            path_candidates("cat", "/bin::/usr"),
            vec!["/bin/cat", "/usr/cat"]
        );
        // PATH dirs are used literally, so a trailing slash doubles up.
        assert_eq!(
            path_candidates("cat", "/usr/bin/"),
            alloc::vec!["/usr/bin//cat"]
        );
        assert_eq!(path_candidates("cat", ""), Vec::<String>::new());
    }
}
