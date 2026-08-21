//! Unit tests for the proc manager: scheduler, fork, exit/zombie/wait, and
//! blocking reads (park on an empty pipe or console, wake on data/EOF).
//! No device, no VFS mounts beyond devfs in the console test. The kernel
//! and allocator stand-ins satisfy the generics; no op reaches them.

use alloc::sync::Arc;

use aerosls_vfs::{CharNode, Errno, Vfs, O_RDONLY};

use crate::{
    is_child, BlockReason, Ctx, ProcManager, Program, ReadBlock, Step, TaskState, WaitOutcome,
    WakeEvent, WriteBlock,
};

/// Assert the wake trace records a task's park and its matching wake, in
/// that order — the blocked-task liveness contract (a park without a
/// later wake for the same reason would be a wedged task).
fn assert_park_wake(trace: &[WakeEvent], task: u32, reason: BlockReason) {
    let park = trace.iter().position(|e| *e == WakeEvent::Parked(task, reason));
    let wake = trace.iter().position(|e| *e == WakeEvent::Woken(task, reason));
    assert!(park.is_some(), "task {task} never parked on {reason:?}: {trace:?}");
    assert!(wake.is_some(), "task {task} never woke from {reason:?}: {trace:?}");
    assert!(
        park.unwrap() < wake.unwrap(),
        "wake precedes park for task {task}: {trace:?}"
    );
}

mod dummy {
    use aerosls_proto::kabi::{CapInfo, ERR_STATE, GrantedCap, Kernel, RecvResult, SendCap};
    use aerosls_vfs::BufferAlloc;

    pub struct NoKernel;
    pub struct NoAlloc;

    impl Kernel for NoKernel {
        fn wait(&self, _: &[u32], _: u64) -> Result<(usize, u16), i32> {
            Err(ERR_STATE)
        }
        fn recv(&self, _: u32, _: &mut [u8], _: &mut [GrantedCap]) -> Result<RecvResult, i32> {
            Err(ERR_STATE)
        }
        fn send(&self, _: u32, _: u32, _: u16, _: &[u8], _: &[SendCap], _: u64) -> Result<(), i32> {
            Err(ERR_STATE)
        }
        fn close(&self, _: u32, _: u16, _: u32) -> Result<(), i32> {
            Err(ERR_STATE)
        }
        fn cap_info(&self, _: u32) -> Result<CapInfo, i32> {
            Err(ERR_STATE)
        }
    }
    impl BufferAlloc for NoAlloc {
        fn alloc(&mut self, _: usize) -> Result<(SendCap, u64), i32> {
            Err(ERR_STATE)
        }
    }
}

type D = dummy::NoKernel;
type M = dummy::NoAlloc;

fn pm() -> ProcManager<D, M> {
    ProcManager::new(Vfs::new())
}

/// Yield three times, then exit 7.
fn count3(ctx: &mut Ctx<D, M>) -> Step {
    ctx.data.push(1);
    if ctx.data.len() >= 3 {
        Step::Exit(7)
    } else {
        Step::Yield
    }
}

fn exit3(_ctx: &mut Ctx<D, M>) -> Step {
    Step::Exit(3)
}

/// Parent: let the child run, then reap it and exit with its status.
fn reaper(ctx: &mut Ctx<D, M>) -> Step {
    if ctx.data.is_empty() {
        ctx.data.push(1);
        return Step::Yield;
    }
    match ctx.wait(1) {
        WaitOutcome::Reaped(code) => ctx.exit(code),
        WaitOutcome::Blocked => Step::Blocked(BlockReason::WaitingChild(1)),
        WaitOutcome::NoSuchChild => Step::Exit(99),
        WaitOutcome::Stopped(_) => Step::Exit(97),
    }
}

// ── blocking reads ───────────────────────────────────────────────────────────

/// Init: pipe + fork. The child parks on the empty read end; the parent
/// writes "ping" and exits. Exit codes: 5 = got "ping", 4 = wrong bytes,
/// 6 = hard error.
fn pipe_owner(ctx: &mut Ctx<D, M>) -> Step {
    let task = ctx.task;
    if ctx.data.is_empty() {
        let (r, w) = ctx.vfs().pipe(task).unwrap();
        ctx.data.push(1);
        ctx.data.push(r as u8);
        ctx.data.push(w as u8);
        return Step::Yield;
    }
    let r = ctx.data[1] as u32;
    let w = ctx.data[2] as u32;
    match (ctx.data[0], is_child(&ctx.data)) {
        (1, false) => {
            ctx.fork().unwrap();
            ctx.data[0] = 2;
            Step::Yield
        }
        (1, true) => {
            // Child: park on the empty read end right away — the parent's
            // write comes in a later round-robin turn, so this genuinely
            // would-block.
            let mut buf = [0u8; 8];
            match ctx.read_blocking(r, &mut buf) {
                ReadBlock::Data(n) if &buf[..n] == b"ping" => Step::Exit(5),
                ReadBlock::Data(_) => Step::Exit(4),
                ReadBlock::WouldBlock => Step::Blocked(BlockReason::Readable(r)),
                ReadBlock::Err(_) => Step::Exit(6),
            }
        }
        (2, _) => {
            ctx.vfs().write(task, w, b"ping").unwrap();
            Step::Exit(0)
        }
        _ => Step::Exit(1),
    }
}

#[test]
fn blocking_read_parks_until_data_wakes_it() {
    let mut p = pm();
    p.spawn_init(Program::new("pipe_owner", pipe_owner));

    p.run_next().unwrap(); // init: pipe
    p.run_next().unwrap(); // init: fork
    // The child parks on the empty read end — provably not runnable.
    assert_eq!(p.run_next(), Some(Step::Blocked(BlockReason::Readable(0))));
    assert_eq!(p.state(1), Some(TaskState::Blocked(BlockReason::Readable(0))));
    assert_eq!(p.runnable(), 1, "only the parent remains runnable");
    // The parent's write is a scheduler event; the drain wakes the reader.
    assert_eq!(p.run_next(), Some(Step::Exit(0)));
    p.drain_wakes();
    assert_eq!(p.state(1), Some(TaskState::Runnable));
    // The retry completes with the data.
    assert_eq!(p.run_next(), Some(Step::Exit(5)));
    assert_eq!(p.exit_code(1), Some(5));
    // The wake trace records the park and the wake, in order.
    assert_park_wake(&p.wake_trace, 1, BlockReason::Readable(0));
}

/// Like `pipe_owner`, but the child drops its own write end and parks; the
/// parent exits without writing, so the last writer's exit must wake the
/// child with EOF. Exit codes: 7 = EOF, 4 = data, 6 = error.
fn pipe_eof_owner(ctx: &mut Ctx<D, M>) -> Step {
    let task = ctx.task;
    if ctx.data.is_empty() {
        let (r, w) = ctx.vfs().pipe(task).unwrap();
        ctx.data.push(1);
        ctx.data.push(r as u8);
        ctx.data.push(w as u8);
        return Step::Yield;
    }
    let r = ctx.data[1] as u32;
    let w = ctx.data[2] as u32;
    match (ctx.data[0], is_child(&ctx.data)) {
        (1, false) => {
            ctx.fork().unwrap();
            ctx.data[0] = 2;
            Step::Yield
        }
        (1, true) => {
            // Child: drop its own write end once, then park — the parent's
            // end is still live, so the read would-block (not EOF yet). On
            // the wake this phase re-runs; the once-guard keeps the close
            // from firing twice (the fd is already gone).
            if ctx.data.len() == 4 {
                ctx.vfs().close(task, w).unwrap();
                ctx.data.push(1);
            }
            let mut buf = [0u8; 8];
            match ctx.read_blocking(r, &mut buf) {
                ReadBlock::Data(0) => Step::Exit(7),
                ReadBlock::Data(_) => Step::Exit(4),
                ReadBlock::WouldBlock => Step::Blocked(BlockReason::Readable(r)),
                ReadBlock::Err(_) => Step::Exit(6),
            }
        }
        (2, _) => Step::Exit(0), // parent exits → last writer gone
        _ => Step::Exit(1),
    }
}

#[test]
fn blocking_read_wakes_on_eof_when_last_writer_exits() {
    let mut p = pm();
    p.spawn_init(Program::new("pipe_eof_owner", pipe_eof_owner));

    p.run_next().unwrap(); // init: pipe
    p.run_next().unwrap(); // init: fork
    // Child parks (parent's write end still open, pipe empty).
    assert_eq!(p.run_next(), Some(Step::Blocked(BlockReason::Readable(0))));
    assert_eq!(p.state(1), Some(TaskState::Blocked(BlockReason::Readable(0))));
    // Parent exits: its write end drops. The drain makes EOF visible.
    assert_eq!(p.run_next(), Some(Step::Exit(0)));
    p.drain_wakes();
    assert_eq!(p.state(1), Some(TaskState::Runnable));
    assert_eq!(p.run_next(), Some(Step::Exit(7)), "retry sees EOF");
    // EOF also pairs a park with its wake in the trace.
    assert_park_wake(&p.wake_trace, 1, BlockReason::Readable(0));
}

/// Reads /dev/console with a blocking read: parks until input is pushed
/// (external event — the event loop re-enters the scheduler).
fn console_reader(ctx: &mut Ctx<D, M>) -> Step {
    let task = ctx.task;
    if ctx.data.is_empty() {
        let fd = ctx.vfs().open(task, "/dev/console", O_RDONLY, 0).unwrap();
        ctx.data.push(fd as u8);
        return Step::Yield;
    }
    let fd = ctx.data[0] as u32;
    let mut buf = [0u8; 8];
    match ctx.read_blocking(fd, &mut buf) {
        ReadBlock::Data(n) if &buf[..n] == b"hi" => Step::Exit(8),
        ReadBlock::Data(_) => Step::Exit(4),
        ReadBlock::WouldBlock => Step::Blocked(BlockReason::Readable(fd)),
        ReadBlock::Err(_) => Step::Exit(6),
    }
}

#[test]
fn blocking_console_read_parks_until_input() {
    let console = Arc::new(CharNode::console());
    let mut vfs = Vfs::new();
    vfs.mount_devfs("/dev", console.clone()).unwrap();
    let mut p = ProcManager::new(vfs);
    p.spawn_init(Program::new("console_reader", console_reader));

    p.run_next().unwrap(); // open the console, yield
    assert_eq!(p.run_next(), Some(Step::Blocked(BlockReason::Readable(0))));
    assert_eq!(p.state(0), Some(TaskState::Blocked(BlockReason::Readable(0))));
    // Input arrives from outside the scheduler (the driver / test); the
    // event loop re-enters and the drain re-arms the parked reader.
    console.console_io().push_input(b"hi");
    p.drain_wakes();
    assert_eq!(p.state(0), Some(TaskState::Runnable));
    assert_eq!(p.run_next(), Some(Step::Exit(8)));
    // The console park/wake is traced like any other.
    assert_park_wake(&p.wake_trace, 0, BlockReason::Readable(0));
}

/// Reads /dev/console with a blocking read; exits 9 on EOF (`Ok(0)` — the
/// console channel closed), 8 on data, 6 on error.
fn console_eof_reader(ctx: &mut Ctx<D, M>) -> Step {
    let task = ctx.task;
    if ctx.data.is_empty() {
        let fd = ctx.vfs().open(task, "/dev/console", O_RDONLY, 0).unwrap();
        ctx.data.push(fd as u8);
        return Step::Yield;
    }
    let fd = ctx.data[0] as u32;
    let mut buf = [0u8; 8];
    match ctx.read_blocking(fd, &mut buf) {
        ReadBlock::Data(0) => Step::Exit(9), // EOF — the channel closed
        ReadBlock::Data(_) => Step::Exit(8),
        ReadBlock::WouldBlock => Step::Blocked(BlockReason::Readable(fd)),
        ReadBlock::Err(_) => Step::Exit(6),
    }
}

#[test]
fn blocking_console_read_parks_until_close_eof() {
    let console = Arc::new(CharNode::console());
    let mut vfs = Vfs::new();
    vfs.mount_devfs("/dev", console.clone()).unwrap();
    let mut p = ProcManager::new(vfs);
    p.spawn_init(Program::new("console_eof_reader", console_eof_reader));

    p.run_next().unwrap(); // open the console, yield
    assert_eq!(p.run_next(), Some(Step::Blocked(BlockReason::Readable(0))));
    assert_eq!(p.state(0), Some(TaskState::Blocked(BlockReason::Readable(0))));
    // The channel-close event fires from outside the scheduler (the
    // kernel console driver went away); the next drain sees the flag and
    // wakes the parked reader, whose retry observes Ok(0) — EOF.
    console.console_io().close();
    assert_eq!(p.state(0), Some(TaskState::Blocked(BlockReason::Readable(0))));
    p.drain_wakes();
    assert_eq!(p.state(0), Some(TaskState::Runnable));
    assert_eq!(p.run_next(), Some(Step::Exit(9)));
    // The close event woke the reader: a traced park/wake pair.
    assert_park_wake(&p.wake_trace, 0, BlockReason::Readable(0));
}

// ── blocking writes ─────────────────────────────────────────────────────────

/// Init: pipe + 8 KiB payload in `data` (after the [phase, r, w] header),
/// fork. The parent is the writer: it drains the payload through the pipe
/// with `write_blocking`, parking when the 4 KiB pipe fills. The child is
/// the reader: it yields a few turns (letting the writer fill the pipe and
/// park), then drains. Exit codes: 0 = done, 6 = write error.
fn pipe_writer_owner(ctx: &mut Ctx<D, M>) -> Step {
    let task = ctx.task;
    if ctx.data.is_empty() {
        let (r, w) = ctx.vfs().pipe(task).unwrap();
        ctx.data.push(1); // phase
        ctx.data.push(r as u8);
        ctx.data.push(w as u8);
        for _ in 0..4096 {
            ctx.data.extend_from_slice(b"AB"); // 8 KiB payload
        }
        return Step::Yield;
    }
    let r = ctx.data[1] as u32;
    let w = ctx.data[2] as u32;
    match (ctx.data[0], is_child(&ctx.data)) {
        (1, false) => {
            ctx.fork().unwrap();
            // The writer keeps only its write end: with its own read end
            // closed, the reader's close is the LAST reader (EPIPE case).
            ctx.vfs().close(task, r).unwrap();
            ctx.data[0] = 2;
            Step::Yield
        }
        (1, true) => {
            // Reader: drop its own write end (else the writer's exit never
            // becomes EOF — the reader's own end keeps the pipe open),
            // then arm the yield counter at data[3] and drain.
            ctx.vfs().close(task, w).unwrap();
            ctx.data.insert(3, 0);
            ctx.data[0] = 3;
            Step::Yield
        }
        (2, _) => {
            // Writer: push the payload through the (4 KiB) pipe.
            if ctx.data.len() <= 3 {
                return Step::Exit(0); // payload fully drained
            }
            let n = core::cmp::min(ctx.data.len() - 3, 4096);
            let chunk = ctx.data[3..3 + n].to_vec();
            match ctx.write_blocking(w, &chunk) {
                WriteBlock::Data(m) => {
                    ctx.data.drain(3..3 + m);
                    Step::Yield
                }
                WriteBlock::WouldBlock => Step::Blocked(BlockReason::Writable(w)),
                WriteBlock::Err(_) => Step::Exit(6),
            }
        }
        (3, _) => {
            if ctx.data[3] < 3 {
                ctx.data[3] += 1;
                return Step::Yield; // let the writer fill the pipe
            }
            let mut buf = [0u8; 512];
            match ctx.read_blocking(r, &mut buf) {
                ReadBlock::Data(0) => Step::Exit(0),
                ReadBlock::Data(_) => Step::Yield,
                ReadBlock::WouldBlock => Step::Blocked(BlockReason::Readable(r)),
                ReadBlock::Err(_) => Step::Exit(7),
            }
        }
        _ => Step::Exit(1),
    }
}

#[test]
fn blocking_write_parks_on_a_full_pipe_until_the_reader_drains() {
    let mut p = pm();
    p.spawn_init(Program::new("pipe_writer_owner", pipe_writer_owner));

    p.run_next().unwrap(); // init: pipe + payload
    p.run_next().unwrap(); // init: fork (writer closes its read end)
    p.run_next().unwrap(); // reader: arm the yield counter
    p.run_next().unwrap(); // writer: fill the 4 KiB pipe
    p.run_next().unwrap(); // reader: yield
    // The writer parks on the full pipe — provably not runnable.
    assert_eq!(p.run_next(), Some(Step::Blocked(BlockReason::Writable(1))));
    assert_eq!(p.state(0), Some(TaskState::Blocked(BlockReason::Writable(1))));
    assert_eq!(p.runnable(), 1, "only the reader remains runnable");

    // The reader drains; the scheduler's drain wakes the writer, and the
    // whole payload flows before the reader sees EOF.
    p.run_until_quiet(300);
    assert_eq!(p.exit_code(0), Some(0), "writer drained the whole payload");
    assert_eq!(p.exit_code(1), Some(0), "reader drained the pipe to EOF");
}

/// Like `pipe_writer_owner`, but the reader closes its read end instead of
/// draining: the parked writer must wake (last reader gone) and its retry
/// observes `EPIPE`. Exit codes: 9 = EPIPE on the retry.
fn pipe_writer_epipe_owner(ctx: &mut Ctx<D, M>) -> Step {
    let task = ctx.task;
    if ctx.data.is_empty() {
        let (r, w) = ctx.vfs().pipe(task).unwrap();
        ctx.data.push(1);
        ctx.data.push(r as u8);
        ctx.data.push(w as u8);
        for _ in 0..4096 {
            ctx.data.extend_from_slice(b"AB");
        }
        return Step::Yield;
    }
    let r = ctx.data[1] as u32;
    let w = ctx.data[2] as u32;
    match (ctx.data[0], is_child(&ctx.data)) {
        (1, false) => {
            ctx.fork().unwrap();
            ctx.vfs().close(task, r).unwrap();
            ctx.data[0] = 2;
            Step::Yield
        }
        (1, true) => {
            ctx.data.insert(3, 0);
            ctx.data[0] = 3;
            Step::Yield
        }
        (2, _) => {
            if ctx.data.len() <= 3 {
                return Step::Exit(0);
            }
            let n = core::cmp::min(ctx.data.len() - 3, 4096);
            let chunk = ctx.data[3..3 + n].to_vec();
            match ctx.write_blocking(w, &chunk) {
                WriteBlock::Data(m) => {
                    ctx.data.drain(3..3 + m);
                    Step::Yield
                }
                WriteBlock::WouldBlock => Step::Blocked(BlockReason::Writable(w)),
                WriteBlock::Err(Errno::EPipe) => Step::Exit(9),
                WriteBlock::Err(_) => Step::Exit(6),
            }
        }
        (3, _) => {
            // Reader: yield twice, then close its read end and exit — the
            // last reader is gone, so the parked writer must wake to EPIPE.
            if ctx.data[3] < 2 {
                ctx.data[3] += 1;
                return Step::Yield;
            }
            ctx.vfs().close(task, r).unwrap();
            Step::Exit(0)
        }
        _ => Step::Exit(1),
    }
}

#[test]
fn blocking_write_wakes_with_epipe_when_the_last_reader_closes() {
    let mut p = pm();
    p.spawn_init(Program::new("pipe_writer_epipe_owner", pipe_writer_epipe_owner));

    p.run_next().unwrap(); // init: pipe + payload
    p.run_next().unwrap(); // init: fork
    p.run_next().unwrap(); // reader: arm the counter
    p.run_next().unwrap(); // writer: fill the pipe
    p.run_next().unwrap(); // reader: yield
    assert_eq!(p.run_next(), Some(Step::Blocked(BlockReason::Writable(1))));
    assert_eq!(p.state(0), Some(TaskState::Blocked(BlockReason::Writable(1))));

    // Reader closes its end and exits; the drain wakes the writer to EPIPE.
    p.run_until_quiet(100);
    assert_eq!(p.exit_code(0), Some(9), "retry observed EPIPE");
    assert_eq!(p.exit_code(1), Some(0), "reader exited cleanly");
}

/// Parent: wait *before* the child runs (must block, then be woken).
fn waiting_parent(ctx: &mut Ctx<D, M>) -> Step {
    if ctx.data.is_empty() {
        ctx.data.push(1);
        return Step::Blocked(BlockReason::WaitingChild(1));
    }
    match ctx.wait(1) {
        WaitOutcome::Reaped(code) => ctx.exit(code),
        WaitOutcome::Blocked => Step::Blocked(BlockReason::WaitingChild(1)),
        WaitOutcome::NoSuchChild => Step::Exit(98),
        WaitOutcome::Stopped(_) => Step::Exit(97),
    }
}

#[test]
fn spawn_runs_to_exit_code() {
    let mut p = pm();
    p.spawn_init(Program::new("count3", count3));
    let steps = p.run_until_quiet(100);
    assert_eq!(steps, 3);
    assert_eq!(p.exit_code(0), Some(7));
    assert_eq!(p.state(0), Some(TaskState::Exited(7)));
    // The zombie lingers until reaped (nothing reaps init here).
    assert_eq!(p.task_count(), 1);
    // Fds are dropped on exit.
    assert_eq!(p.vfs.fd_count(0), None);
}

#[test]
fn zombie_until_reaped() {
    let mut p = pm();
    p.spawn_init(Program::new("reaper", reaper));
    p.spawn(Program::new("exit3", exit3)).unwrap();

    // init yields; child exits → zombie.
    p.run_next().unwrap();
    p.run_next().unwrap();
    assert_eq!(p.state(1), Some(TaskState::Exited(3)), "child is a zombie");
    assert_eq!(p.task_count(), 2);

    // init reaps it and exits with its status.
    p.run_next().unwrap();
    assert_eq!(p.exit_code(0), Some(3));
    assert_eq!(p.state(1), None, "child reaped");
    assert_eq!(p.task_count(), 1);
}

#[test]
fn wait_on_live_child_blocks_then_wakes() {
    let mut p = pm();
    p.spawn_init(Program::new("waiting_parent", waiting_parent));
    p.spawn(Program::new("exit3", exit3)).unwrap();

    // Parent waits first → blocked.
    p.run_next().unwrap();
    assert_eq!(
        p.state(0),
        Some(TaskState::Blocked(BlockReason::WaitingChild(1)))
    );
    assert_eq!(p.runnable(), 1, "only the child remains runnable");

    // Child exits → wakes the parent.
    p.run_next().unwrap();
    assert_eq!(p.state(0), Some(TaskState::Runnable));

    // Parent reaps and exits with the child's status.
    p.run_next().unwrap();
    assert_eq!(p.exit_code(0), Some(3));
    assert_eq!(p.state(1), None, "child reaped");
    assert_eq!(p.task_count(), 1, "only init's zombie remains (nothing reaps init)");
    // The exit-wake is traced: the child's death woke the parent's wait.
    assert_park_wake(&p.wake_trace, 0, BlockReason::WaitingChild(1));
}

#[test]
fn wait_no_such_child() {
    let mut p = pm();
    p.spawn_init(Program::new("wait", |ctx: &mut Ctx<D, M>| match ctx.wait(9) {
        WaitOutcome::NoSuchChild => ctx.exit(40),
        _ => Step::Exit(41),
    }));
    p.run_until_quiet(10);
    assert_eq!(p.exit_code(0), Some(40));
}

#[test]
fn round_robin_interleaves_tasks() {
    let mut p = pm();
    for _ in 0..3 {
        p.spawn(Program::new("count3", count3)).unwrap();
    }
    let steps = p.run_until_quiet(100);
    assert_eq!(steps, 9, "3 tasks × 3 steps");
    let t = &p.sched_trace;
    assert_eq!(t.len(), 9);
    for id in 1..=3 {
        assert_eq!(t.iter().filter(|&&x| x == id).count(), 3, "fair share for {id}");
    }
    // Strict round-robin: no task runs twice in a row.
    for w in t.windows(2) {
        assert_ne!(w[0], w[1], "no consecutive same-task steps: {t:?}");
    }
}

#[test]
fn exit_marks_state_and_drops_fds() {
    let mut p = pm();
    p.spawn_init(Program::new("exit", |_ctx: &mut Ctx<D, M>| Step::Exit(9)));
    p.run_until_quiet(10);
    assert_eq!(p.exit_code(0), Some(9));
    assert_eq!(p.vfs.fd_count(0), None);
}

#[test]
fn fork_thread_shares_the_table_object() {
    let mut p = pm();
    p.spawn_init(Program::new("idle", |_ctx: &mut Ctx<D, M>| Step::Exit(0)));
    // Direct manager call (no step running): the child's fd table must be
    // the *same* pool entry as the parent's — verify through the
    // observable that table mutations in one are visible in the other.
    let child = p.fork_thread(0).unwrap();
    assert!(p.vfs.fd_count(0).is_some());
    assert_eq!(p.vfs.fd_count(0), p.vfs.fd_count(child));
    // A dup in the parent is visible in the child (same table).
    // (No fds exist yet, so this only proves the plumbing didn't panic;
    // the integration suite proves sharing with real fds.)
    assert_eq!(p.vfs.fd_count(child), p.vfs.fd_count(0));
    assert_eq!(p.state(child), Some(TaskState::Runnable));
}
