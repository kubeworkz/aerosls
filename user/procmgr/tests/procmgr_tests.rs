//! Integration tests for the proc manager, driven end to end: the *real*
//! ramdisk driver on the fake kernel, the VFS mounted on it, and the proc
//! manager running cooperative programs against that VFS. The fork /
//! CLONE_FILES / exec / exit-wait semantics are exercised with real fds and
//! real files, so shared offsets and shared tables are observable.

use aerosls_blockcache::{BlockCache, BufferAlloc};
use aerosls_kernel_sim::{FakeClient, FakeKernel, DRIVER_CONSOLE, DRIVER_STORAGE};
use aerosls_proto::kabi::SendCap;
use aerosls_proto::*;
use aerosls_ramdisk::endpoints::EndpointSet;
use aerosls_ramdisk::server::{self, Device};
use aerosls_vfs::{CharNode, Errno, ImageBuilder, Vfs, O_APPEND, O_CREAT, O_RDONLY, O_RDWR, O_WRONLY};
use aerosls_procmgr::{is_child, BlockReason, Ctx, ProcManager, Program, Step, WaitOutcome};
use std::sync::Arc;
use std::thread::JoinHandle;

type FC = FakeClient;
type FA = FakeAlloc;

fn pm_image() -> Vec<u8> {
    let mut b = ImageBuilder::new();
    b.add_dir("/etc", 0o755);
    b.add_dir("/bin", 0o755);
    b.add_file("/etc/passwd", b"root:x:0:0:root:/root:/bin/sh\n", 0o644);
    b.add_file("/etc/group", b"root:x:0:\n", 0o644);
    b.add_file("/bin/applet", b"hello\n", 0o755);
    b.add_file("/bin/cat", b"cat\n", 0o755);
    b.add_file("/bin/grep", b"grep\n", 0o755);
    b.build()
}

fn boot_driver(fake: FakeKernel) -> JoinHandle<()> {
    let (base, len) = fake.storage_info();
    let dev = Device {
        storage_slot: DRIVER_STORAGE,
        storage_base: base,
        storage_len: len,
        storage_writable: false,
    };
    let mut eps = EndpointSet::new(DRIVER_CONSOLE);
    for h in fake.initial_chan_caps() {
        eps.adopt(h);
    }
    std::thread::spawn(move || {
        let _ = server::run(&fake, &mut eps, &dev);
    })
}

#[derive(Clone)]
struct FakeAlloc(FC);

impl BufferAlloc for FakeAlloc {
    fn alloc(&mut self, len: usize) -> Result<(SendCap, u64), i32> {
        let handle = self.0.new_region(vec![0u8; len], R | W);
        let info = self.0.cap_info(handle)?;
        Ok((
            SendCap {
                slot: handle,
                offset: 0,
                len: len as u32,
                rights: R | W,
                flags: 0,
            },
            info.base,
        ))
    }
}

/// A full proc manager: VFS with `/` (aerofs) + `/tmp` (ramfs), init
/// spawned. The caller owns the driver thread and the client (for kills).
fn pm() -> (ProcManager<FC, FA>, JoinHandle<()>, FakeClient) {
    let (fake, client) = FakeKernel::new(pm_image(), 1);
    let t = boot_driver(fake);
    let cache = BlockCache::connect(client.clone(), 0, FakeAlloc(client.clone())).unwrap();
    let mut vfs = Vfs::new();
    vfs.mount_aerofs("/", cache).unwrap();
    vfs.mount_ramfs("/tmp").unwrap();
    vfs.mount_devfs("/dev", Arc::new(CharNode::console())).unwrap();
    (ProcManager::new(vfs), t, client)
}

/// Read a whole file through a fresh task (init's fds are gone after exit).
fn read_all(vfs: &mut Vfs<FC, FA>, path: &str) -> Vec<u8> {
    let t = vfs.add_task().unwrap();
    let fd = vfs.open(t, path, O_RDONLY, 0).unwrap();
    let mut out = Vec::new();
    let mut buf = [0u8; 64];
    loop {
        let n = vfs.read(t, fd, &mut buf).unwrap();
        if n == 0 {
            break;
        }
        out.extend_from_slice(&buf[..n]);
    }
    vfs.close(t, fd).unwrap();
    out
}

// ── programs ────────────────────────────────────────────────────────────────

/// Phase 1 is the fork point: the child resumes past it (fork marker), the
/// parent forks. Then parent writes "PARENT", child writes "CHILD" — both
/// through the same inherited fd, whose offset is *shared*.
fn writer(ctx: &mut Ctx<FC, FA>) -> Step {
    let task = ctx.task;
    if ctx.data.is_empty() {
        let fd = ctx
            .vfs()
            .open(task, "/tmp/log", O_CREAT | O_RDWR, 0o644)
            .unwrap();
        ctx.data.push(1);
        ctx.data.push(fd as u8);
        return Step::Yield;
    }
    match ctx.data[0] {
        1 => {
            if is_child(&ctx.data) {
                ctx.data[0] = 3; // child: resume past the fork point
            } else {
                ctx.fork().unwrap();
                ctx.data[0] = 2; // parent
            }
            Step::Yield
        }
        2 => {
            let fd = ctx.data[1] as u32;
            ctx.vfs().write(task, fd, b"PARENT").unwrap();
            Step::Exit(0)
        }
        3 => {
            let fd = ctx.data[1] as u32;
            ctx.vfs().write(task, fd, b"CHILD").unwrap();
            Step::Exit(7)
        }
        _ => Step::Exit(1),
    }
}

/// Parent forks a thread (shared fd table), closes the shared fd, then
/// reaps the child. The child reads the fd — because the table is *shared*,
/// the parent's close is visible and the read fails EBADF (exit 42). A
/// copied table would let the read succeed (exit 1).
fn threader(ctx: &mut Ctx<FC, FA>) -> Step {
    let task = ctx.task;
    if ctx.data.is_empty() {
        let fd = ctx.vfs().open(task, "/tmp/f", O_CREAT | O_RDWR, 0o644).unwrap();
        ctx.data.push(1);
        ctx.data.push(fd as u8);
        return Step::Yield;
    }
    match ctx.data[0] {
        1 => {
            if is_child(&ctx.data) {
                ctx.data[0] = 3;
            } else {
                let child = ctx.fork_thread().unwrap();
                ctx.data.push(child as u8);
                let fd = ctx.data[1] as u32;
                ctx.vfs().close(task, fd).unwrap();
                ctx.data[0] = 2;
            }
            Step::Yield
        }
        2 => {
            let child = ctx.data[2] as u32;
            match ctx.wait(child) {
                WaitOutcome::Reaped(c) => ctx.exit(c),
                WaitOutcome::Blocked => Step::Blocked(BlockReason::WaitingChild(child)),
                WaitOutcome::NoSuchChild => Step::Exit(90),
            }
        }
        3 => {
            let fd = ctx.data[1] as u32;
            let mut buf = [0u8; 8];
            match ctx.vfs().read(task, fd, &mut buf) {
                Err(Errno::EBadf) => Step::Exit(42),
                _ => Step::Exit(1),
            }
        }
        _ => Step::Exit(1),
    }
}

/// The applet that /bin/applet ("hello") names.
fn hello(ctx: &mut Ctx<FC, FA>) -> Step {
    let task = ctx.task;
    if ctx.data.is_empty() {
        let fd = ctx.vfs().open(task, "/tmp/out", O_CREAT | O_RDWR, 0o644).unwrap();
        ctx.vfs().write(task, fd, b"hello from applet").unwrap();
        ctx.vfs().close(task, fd).unwrap();
        return Step::Exit(3);
    }
    Step::Exit(1)
}

fn execer(ctx: &mut Ctx<FC, FA>) -> Step {
    if ctx.data.is_empty() {
        ctx.exec("/bin/applet").unwrap();
        return Step::Yield; // next step runs the applet (fresh data)
    }
    Step::Exit(1)
}

fn failed_exec(ctx: &mut Ctx<FC, FA>) -> Step {
    if ctx.data.is_empty() {
        if ctx.exec("/no/such/applet").is_err() {
            return Step::Exit(11);
        }
        return Step::Exit(12);
    }
    Step::Exit(12)
}

/// Append one byte to /tmp/order (O_APPEND) each step, three steps, then
/// exit. `ctx.task` distinguishes the tasks ('A' = task 0, 'B', 'C').
fn spinner(ctx: &mut Ctx<FC, FA>) -> Step {
    let task = ctx.task;
    if ctx.data.is_empty() {
        ctx.data.push(0);
    }
    let n = ctx.data[0];
    if n >= 3 {
        return Step::Exit(0);
    }
    let fd = ctx
        .vfs()
        .open(task, "/tmp/order", O_CREAT | O_RDWR | O_APPEND, 0o644)
        .unwrap();
    let ch = [b'A' + task as u8];
    ctx.vfs().write(task, fd, &ch).unwrap();
    ctx.vfs().close(task, fd).unwrap();
    ctx.data[0] = n + 1;
    Step::Yield
}

/// The `cat` applet: copies /etc/passwd to fd 1 (its stdout — the pipe
/// write end the shell dup2'd there before exec'ing it).
fn cat(ctx: &mut Ctx<FC, FA>) -> Step {
    let task = ctx.task;
    if ctx.data.is_empty() {
        let fd = ctx.vfs().open(task, "/etc/passwd", O_RDONLY, 0).unwrap();
        ctx.data.push(fd as u8);
        return Step::Yield;
    }
    let fd = ctx.data[0] as u32;
    let mut buf = [0u8; 16];
    match ctx.vfs().read(task, fd, &mut buf) {
        Ok(0) => {
            ctx.vfs().close(task, fd).unwrap();
            Step::Exit(0)
        }
        Ok(n) => {
            ctx.vfs().write(task, 1, &buf[..n]).unwrap();
            Step::Yield
        }
        Err(_) => Step::Exit(2),
    }
}

/// The `grep` applet: reads fd 0 (its stdin — the pipe read end) to EOF,
/// keeps the lines containing "root", writes them to /tmp/out.
fn grep(ctx: &mut Ctx<FC, FA>) -> Step {
    let task = ctx.task;
    let mut buf = [0u8; 16];
    match ctx.vfs().read(task, 0, &mut buf) {
        Ok(0) => {
            // EOF: the writer side is gone. Filter what we accumulated
            // (owned, so the borrow of `ctx.data` ends before I/O).
            let text = core::str::from_utf8(&ctx.data).unwrap_or("");
            let matches: Vec<String> = text
                .lines()
                .filter(|l| l.contains("root"))
                .map(|l| l.to_string())
                .collect();
            let out = ctx
                .vfs()
                .open(task, "/tmp/out", O_CREAT | O_WRONLY, 0o644)
                .unwrap();
            for l in &matches {
                ctx.vfs().write(task, out, l.as_bytes()).unwrap();
                ctx.vfs().write(task, out, b"\n").unwrap();
            }
            ctx.vfs().close(task, out).unwrap();
            Step::Exit(0)
        }
        Ok(n) => {
            ctx.data.extend_from_slice(&buf[..n]);
            Step::Yield
        }
        // Nothing in the pipe yet — the producer hasn't run; yield.
        Err(Errno::EAgain) => Step::Yield,
        Err(_) => Step::Exit(3),
    }
}

/// The shell: `cat /etc/passwd | grep root > /tmp/out`. Creates the pipe,
/// forks cat (stdout → write end) and grep (stdin → read end), closes its
/// own copies, waits for both, exits with grep's status.
fn pipeline(ctx: &mut Ctx<FC, FA>) -> Step {
    let task = ctx.task;
    if ctx.data.is_empty() {
        // Shell stdio is the console: 0,1,2 = /dev/console. The pipe then
        // lands at 3,4, so the children's dup2 actually *moves* an end
        // into place instead of no-op'ing onto itself.
        for _ in 0..3 {
            ctx.vfs().open(task, "/dev/console", O_RDWR, 0).unwrap();
        }
        let (r, w) = ctx.vfs().pipe(task).unwrap();
        ctx.data.push(1); // phase (data[0])
        ctx.data.push(r as u8);
        ctx.data.push(w as u8);
        return Step::Yield;
    }
    let r = ctx.data[1] as u32;
    let w = ctx.data[2] as u32;
    match (ctx.data[0], is_child(&ctx.data)) {
        (1, false) => {
            // Fork cat. The child resumes at (1, true); we record its id.
            let c = ctx.fork().unwrap();
            ctx.data.push(c as u8);
            ctx.data[0] = 2;
            Step::Yield
        }
        (1, true) => {
            ctx.data[0] = 3; // the cat child
            Step::Yield
        }
        (2, false) => {
            let c = ctx.fork().unwrap();
            ctx.data.push(c as u8);
            ctx.data[0] = 5;
            Step::Yield
        }
        (2, true) => {
            ctx.data[0] = 4; // the grep child
            Step::Yield
        }
        (3, _) => {
            // cat: stdout → pipe write end; drop its own pipe fds.
            ctx.vfs().dup2(task, w, 1).unwrap();
            ctx.vfs().close(task, r).unwrap();
            ctx.vfs().close(task, w).unwrap();
            ctx.exec("/bin/cat").unwrap();
            Step::Yield
        }
        (4, _) => {
            // grep: stdin → pipe read end; drop its own pipe fds.
            ctx.vfs().dup2(task, r, 0).unwrap();
            ctx.vfs().close(task, r).unwrap();
            ctx.vfs().close(task, w).unwrap();
            ctx.exec("/bin/grep").unwrap();
            Step::Yield
        }
        (5, _) => {
            // Shell: drop its own pipe copies, then wait for both.
            ctx.vfs().close(task, r).unwrap();
            ctx.vfs().close(task, w).unwrap();
            ctx.data[0] = 6;
            Step::Yield
        }
        (6, _) => {
            let cat_c = ctx.data[3] as u32;
            let grep_c = ctx.data[4] as u32;
            match ctx.wait(cat_c) {
                WaitOutcome::Reaped(_) => match ctx.wait(grep_c) {
                    WaitOutcome::Reaped(code) => ctx.exit(code),
                    WaitOutcome::Blocked => Step::Blocked(BlockReason::WaitingChild(grep_c)),
                    WaitOutcome::NoSuchChild => Step::Exit(90),
                },
                WaitOutcome::Blocked => Step::Blocked(BlockReason::WaitingChild(cat_c)),
                WaitOutcome::NoSuchChild => Step::Exit(90),
            }
        }
        _ => Step::Exit(1),
    }
}

/// Set cwd `/etc` + creds, fork; the child resolves relative paths (cwd
/// inherited), then the parent switches ITS cwd — which must not leak.
fn cwd_child(ctx: &mut Ctx<FC, FA>) -> Step {
    let task = ctx.task;
    if ctx.data.is_empty() {
        ctx.vfs().set_cwd(task, "/etc").unwrap();
        ctx.vfs().set_cred(task, 1000, 1000).unwrap();
        ctx.data.push(1); // phase 1 = the fork point
        return Step::Yield;
    }
    match ctx.data[0] {
        1 => {
            // The child resumes at the fork point already "returned"; the
            // parent forks here.
            if is_child(&ctx.data) {
                ctx.data[0] = 5;
            } else {
                ctx.fork().unwrap();
                ctx.data[0] = 9;
            }
            Step::Yield
        }
        5 => {
            // Relative to the inherited /etc cwd, with the inherited creds
            // (1000): passwd is 0644 (world-readable), so this opens.
            match ctx.vfs().open(task, "passwd", O_RDONLY, 0) {
                Ok(f) => {
                    ctx.vfs().close(task, f).unwrap();
                    Step::Exit(5)
                }
                Err(_) => Step::Exit(21),
            }
        }
        9 => {
            ctx.vfs().set_cwd(task, "/tmp").unwrap();
            Step::Exit(9)
        }
        _ => Step::Exit(1),
    }
}

// ── tests ───────────────────────────────────────────────────────────────────

#[test]
fn fork_shares_open_file_offset() {
    let (mut p, t, client) = pm();
    p.spawn_init(Program::new("writer", writer));
    p.run_until_quiet(100);

    // The two writes went through the *same* FileNode: adjacent, never
    // overlapping. Content is "PARENTCHILD" or "CHILDPARENT".
    let out = read_all(&mut p.vfs, "/tmp/log");
    assert_eq!(out.len(), 11, "both writes landed: {out:?}");
    assert!(
        out == b"PARENTCHILD" || out == b"CHILDPARENT",
        "shared offset, no overlap: {out:?}"
    );
    assert_eq!(p.exit_code(0), Some(0), "parent exited 0");
    assert_eq!(p.exit_code(1), Some(7), "child exited 7");

    client.kill_driver(0);
    t.join().unwrap();
}

#[test]
fn clone_files_threads_truly_share_the_table() {
    let (mut p, t, client) = pm();
    p.spawn_init(Program::new("threader", threader));
    p.run_until_quiet(100);

    // The child saw the parent's close → EBADF → 42. If the table had been
    // *copied* (fork semantics), the read would have succeeded (exit 1).
    assert_eq!(p.exit_code(0), Some(42), "parent reaped the child's 42");
    assert_eq!(p.state(1), None, "child reaped");

    client.kill_driver(0);
    t.join().unwrap();
}

#[test]
fn exec_switches_to_the_applet() {
    let (mut p, t, client) = pm();
    p.register_applet("hello", hello);
    p.spawn_init(Program::new("execer", execer));
    p.run_until_quiet(100);

    assert_eq!(p.exit_code(0), Some(3), "the applet's exit status");
    assert_eq!(
        read_all(&mut p.vfs, "/tmp/out"),
        b"hello from applet",
        "the applet wrote through the VFS"
    );

    client.kill_driver(0);
    t.join().unwrap();
}

#[test]
fn exec_failure_keeps_the_task_alive() {
    let (mut p, t, client) = pm();
    p.spawn_init(Program::new("failed_exec", failed_exec));
    p.run_until_quiet(100);
    assert_eq!(p.exit_code(0), Some(11), "exec error surfaced, task continued");

    client.kill_driver(0);
    t.join().unwrap();
}

#[test]
fn round_robin_interleaves_running_tasks() {
    let (mut p, t, client) = pm();
    p.spawn_init(Program::new("spinner", spinner));
    p.spawn(Program::new("spinner", spinner)).unwrap();
    p.spawn(Program::new("spinner", spinner)).unwrap();
    p.run_until_quiet(100);

    let order = read_all(&mut p.vfs, "/tmp/order");
    assert_eq!(order.len(), 9, "3 tasks × 3 steps each: {order:?}");
    for ch in [b'A', b'B', b'C'] {
        assert_eq!(
            order.iter().filter(|&&c| c == ch).count(),
            3,
            "fair share of {ch}: {order:?}"
        );
    }
    // Round-robin: no task ran twice in a row (its writes are not adjacent).
    for w in order.windows(2) {
        assert_ne!(w[0], w[1], "no consecutive same-task writes: {order:?}");
    }

    client.kill_driver(0);
    t.join().unwrap();
}

#[test]
fn fork_inherits_cwd_and_creds_per_task() {
    let (mut p, t, client) = pm();
    p.spawn_init(Program::new("cwd_child", cwd_child));
    p.run_until_quiet(100);

    assert_eq!(p.exit_code(0), Some(9), "parent switched its cwd");
    assert_eq!(
        p.exit_code(1),
        Some(5),
        "child resolved 'passwd' relative to the inherited /etc cwd"
    );

    client.kill_driver(0);
    t.join().unwrap();
}

#[test]
fn cat_grep_pipeline_through_the_proc_manager() {
    let (mut p, t, client) = pm();
    p.register_applet("cat", cat);
    p.register_applet("grep", grep);
    p.spawn_init(Program::new("pipeline", pipeline));
    p.run_until_quiet(100);

    // cat wrote the passwd line into the pipe; grep's EOF read saw the
    // writer side fully closed (cat exited), filtered, and wrote the match.
    assert_eq!(p.exit_code(0), Some(0), "shell exited with grep's status");
    assert_eq!(
        read_all(&mut p.vfs, "/tmp/out"),
        b"root:x:0:0:root:/root:/bin/sh\n",
        "the matching line flowed cat → pipe → grep → file"
    );
    // Both pipeline stages were reaped; nothing left in the run queue.
    assert_eq!(p.state(1), None);
    assert_eq!(p.state(2), None);

    client.kill_driver(0);
    t.join().unwrap();
}

#[test]
fn exit_drops_the_tasks_fds() {
    let (mut p, t, client) = pm();
    p.spawn_init(Program::new("open_then_exit", |ctx: &mut Ctx<FC, FA>| {
        let task = ctx.task;
        if ctx.data.is_empty() {
            let fd = ctx.vfs().open(task, "/tmp/f", O_CREAT | O_RDWR, 0o644).unwrap();
            ctx.data.push(fd as u8);
            return Step::Exit(0);
        }
        Step::Exit(1)
    }));
    p.run_until_quiet(100);
    assert_eq!(p.exit_code(0), Some(0));
    assert_eq!(p.vfs.fd_count(0), None, "exit drops the fd table");

    client.kill_driver(0);
    t.join().unwrap();
}
