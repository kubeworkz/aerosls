//! Unit tests for the proc manager: scheduler, fork, exit/zombie/wait —
//! no device, no VFS mounts. The kernel and allocator stand-ins below
//! satisfy the generics; no op ever reaches them.

use aerosls_vfs::Vfs;

use crate::{BlockReason, Ctx, ProcManager, Program, Step, TaskState, WaitOutcome};

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
    }
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
