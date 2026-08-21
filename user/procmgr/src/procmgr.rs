//! The POSIX sidecar's proc manager: cooperative tasks over the VFS.
//!
//! Maps the POSIX process model onto the sidecar (Phase 2 design §3.2):
//! a *task* is the sidecar-internal process — `{fd_table, cwd, euid/egid,
//! state}` lives in the VFS, the scheduling/relationship state lives here.
//! One sidecar = one trust domain; many tasks; the MVP runs them all
//! cooperatively on the sidecar's single kernel thread (§3.3).
//!
//! Key semantics, pinned by the docs:
//!
//! - **`fork`** (design §4): a proc-manager operation — the child gets a
//!   *copy* of the parent's fd table whose entries share their
//!   `Arc<FileNode>`, so parent and child correctly share open-file
//!   offsets. cwd and credentials are copied. The child's program state is
//!   a snapshot of the parent's at fork time, with a `FORK_MARKER` appended
//!   so the child can tell its fork-return from the parent's (the crate's
//!   stand-in for the copied register file).
//! - **`fork_thread` (CLONE_FILES)**: the child *shares* the parent's fd
//!   table object itself — `close`/`dup2` in one holder is visible in all
//!   (POSIX threads).
//! - **exit/wait**: an exited task becomes a *zombie* (its registry entry
//!   survives, fds dropped) until its parent reaps it with `wait`. Orphans
//!   are reparented to init. A parent that waits on a live child blocks;
//!   the child's exit wakes it.
//! - **`exec`**: replaces a task's image. At this crate's level the image
//!   is a BusyBox-style applet: `exec(path)` resolves and reads the file
//!   through the VFS (the full mount chain), the first line names an
//!   applet from the registry, and the task's program is replaced. Open fds
//!   are preserved (no CLOEXEC in v1).
//! - **Blocking reads and writes**: `Ctx::read_blocking` turns a
//!   would-block (an empty pipe, a console with no input) into
//!   `Vfs::wait_readable` registration plus `Step::Blocked(Readable(fd))`;
//!   `Ctx::write_blocking` mirrors it for a full pipe with
//!   `Vfs::wait_writable` + `Step::Blocked(Writable(fd))`. The scheduler's
//!   unified wake drain re-checks each parked task against a pure
//!   readiness predicate at every step (data arrived, last writer gone →
//!   EOF, free space, last reader gone → EPIPE, console input pushed
//!   between runs) and requeues the task; the program just retries the
//!   I/O, re-parking if it would-block again. No per-event notification
//!   needed in a cooperative model — any other task's step or external
//!   input is visible at the next drain.
//!
//! The proc manager **owns the VFS** (in-core call path). The
//! architecture's internal-bus message exchange between components is the
//! future split; the call path here is direct and synchronous.

use alloc::boxed::Box;
use alloc::collections::{BTreeMap, BTreeSet, VecDeque};
use alloc::string::String;
use alloc::string::ToString;
use alloc::vec::Vec;

use aerosls_proto::kabi::Kernel;
use aerosls_proto::sockops::SocketOps;
use aerosls_vfs::{BufferAlloc, Errno, PollFd, SelectFdSet, SelectResult, Vfs, CLONE_FILES, O_RDONLY};

// ── Signals ────────────────────────────────────────────────────────────────

/// POSIX signal numbers used by the sidecar. v1: a small set; extensible.
pub const SIGHUP: i32 = 1;
pub const SIGINT: i32 = 2;
pub const SIGQUIT: i32 = 3;
pub const SIGKILL: i32 = 9;
pub const SIGPIPE: i32 = 13;
pub const SIGTERM: i32 = 15;
pub const SIGCONT: i32 = 18;
pub const SIGTSTP: i32 = 20;

/// The "raised by signal" exit-status convention: a task killed by signal
/// N exits with status `128 + N`. This encodes the POSIX `WIFSIGNALED` /
/// `WTERMSIG` convention without needing a separate channel.
pub fn signal_exit_code(sig: i32) -> i32 {
    128 + sig
}

/// A compact pending-signal set stored as a bitmask (bit N = signal N).
/// v1: only signals 1..=31 fit; the mask is 32 bits.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct SignalSet(u32);

impl SignalSet {
    pub const fn empty() -> Self {
        Self(0)
    }

    pub const fn is_empty(self) -> bool {
        self.0 == 0
    }

    /// Enqueue a signal. Silently ignores out-of-range signal numbers.
    pub fn add(&mut self, sig: i32) {
        if sig > 0 && sig <= 31 {
            self.0 |= 1 << sig;
        }
    }

    /// Is a specific signal pending?
    pub const fn contains(self, sig: i32) -> bool {
        sig > 0 && sig <= 31 && (self.0 & (1 << sig)) != 0
    }

    /// Dequeue a specific signal (clear the bit).
    pub fn remove(&mut self, sig: i32) {
        if sig > 0 && sig <= 31 {
            self.0 &= !(1 << sig);
        }
    }

    /// Take the highest-priority pending signal and clear it.
    /// Priority: lower signal number first (POSIX: lower = more urgent).
    pub fn take_one(&mut self) -> Option<i32> {
        if self.0 == 0 {
            return None;
        }
        let bit = self.0.trailing_zeros() as i32;
        self.0 &= !(1 << bit);
        Some(bit)
    }
}

/// Appended to a forked child's cloned program data so it can distinguish
/// its fork-return from the parent's (the crate's stand-in for the copied
/// register file: `fork()` returns the child id to the parent and "0" to
/// the child, modeled as a marker byte the child's snapshot carries).
/// Programs must not clear their data after forking without re-checking
/// `is_child`.
pub const FORK_MARKER: u8 = 0xFF;

/// Is this task's data the child half of a fork? (`fork()` appended the
/// marker to the child's snapshot.)
pub fn is_child(data: &[u8]) -> bool {
    data.contains(&FORK_MARKER)
}

/// Task state (design §3.2: `state` field of the task control block).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum TaskState {
    Runnable,
    Blocked(BlockReason),
    /// Zombie until the parent reaps it.
    Exited(i32),
}

/// One entry in the wake trace: a task parked or was woken, with the
/// reason. `Woken` carries the reason the task was blocked on, so a park
/// and its wake pair up. This is the blocked-task liveness observable:
/// every park has a matching wake (or the task is wedged — the tests
/// assert the pairs, and a live system can log them for the debug
/// channel).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum WakeEvent {
    /// The task transitioned to `Blocked(reason)` (it left the run queue).
    Parked(u32, BlockReason),
    /// The task left `Blocked(reason)` and was requeued (data/EOF/space
    /// arrived, a waited child exited, or the console closed).
    Woken(u32, BlockReason),
}

/// Why a task is blocked.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum BlockReason {
    /// Waiting for a specific child to exit.
    WaitingChild(u32),
    /// Blocked reading `fd` until it becomes readable (data, EOF, or
    /// console input). The VFS registered the wait; the scheduler's
    /// wake drain requeues the task and it retries the read.
    Readable(u32),
    /// Blocked writing `fd` until space frees up (a reader drained the
    /// pipe) or the last reader closes (the retry observes `EPIPE`).
    Writable(u32),
    /// The program chose to block (I/O wait, etc.).
    User,
    /// Sleep for N scheduler ticks (cooperative).
    Sleep(u32),
    /// Wait for a specific child OR for N ticks, whichever comes first.
    /// Payload: (child_id, remaining_ticks).
    WaitChildTimeout(u32, u32),
    /// Task stopped by SIGTSTP (Ctrl-Z).  Resumed by SIGCONT.
    Stopped,
}

/// Outcome of a blocking read (`Ctx::read_blocking`).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum ReadBlock {
    /// Bytes read (`0` = EOF).
    Data(usize),
    /// The object would block; the wait is registered with the VFS. The
    /// program must return `Step::Blocked(BlockReason::Readable(fd))` —
    /// the scheduler parks it and wakes it when the object becomes
    /// readable.
    WouldBlock,
    /// A hard error (`EBADF`, `EIO`, ...).
    Err(Errno),
}

/// Outcome of a blocking write (`Ctx::write_blocking`).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum WriteBlock {
    /// Bytes written (a short write is allowed — the program retries the
    /// remainder).
    Data(usize),
    /// The object would block; the wait is registered with the VFS. The
    /// program must return `Step::Blocked(BlockReason::Writable(fd))` —
    /// the scheduler parks it and wakes it when space frees up or the
    /// last reader closes.
    WouldBlock,
    /// A hard error (`EBADF`, `EPIPE`, `EIO`, ...).
    Err(Errno),
}

/// What one cooperative step of a program produced.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Step {
    /// Requeue at the tail of the run queue.
    Yield,
    /// Block until woken (a waited child's exit, or the program's own
    /// reason).
    Blocked(BlockReason),
    /// Terminate with a status; the task becomes a zombie.
    Exit(i32),
    /// Terminate with status 0.
    Done,
}

/// POSIX wait options.
pub const WNOHANG: u32 = 1;

/// Result of `wait` / `waitpid`.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum WaitOutcome {
    /// The child was reaped; its status.
    Reaped(i32),
    /// The child is alive; the caller should block and retry.
    /// With `WNOHANG` this is a non-blocking "not yet" — the caller
    /// can continue instead of parking.
    Blocked,
    /// No such child.
    NoSuchChild,
    /// The child was stopped (SIGTSTP).  The caller should note it
    /// in the job table and re-prompt (POSIX SIGTSTP/SIGCONT).
    /// Payload: child_id.
    Stopped(u32),
}

/// A program image. At this crate's level: a step function plus its state
/// (the stand-in for an address space's registers/stack) and argv (design
/// §6.2's `ExecSpec.argv`). `fork` clones the whole thing — that is the
/// register/stack copy of design §4.
pub struct Program<K: Kernel, A: BufferAlloc> {
    pub name: String,
    /// The argument vector, as set by the spawner or the last `exec`.
    /// `argv[0]` conventionally names the program (the exec'd script path);
    /// applets read their arguments from `Ctx::argv()[1..]`.
    pub argv: Vec<String>,
    /// Program state (the child of a fork carries a snapshot of the
    /// parent's, with the fork marker appended).
    pub data: Vec<u8>,
    pub step: AppletStep<K, A>,
    /// Image generation: bumped by `exec` so the scheduler can tell that a
    /// program was replaced mid-step (and must not clobber the new image's
    /// fresh data with the old one's).
    pub gen: u64,
}

/// The step signature: run one slice of the program against the context.
/// The program's state lives in `ctx.data`.
pub type AppletStep<K, A> = fn(&mut Ctx<'_, K, A>) -> Step;

impl<K: Kernel, A: BufferAlloc> Program<K, A> {
    pub fn new(name: &str, step: AppletStep<K, A>) -> Program<K, A> {
        Program {
            name: name.to_string(),
            argv: Vec::new(),
            data: Vec::new(),
            step,
            gen: 0,
        }
    }

    /// Spawn/exec a program with an argument vector (design §6.2's
    /// `ExecSpec`). `argv[0]` conventionally names the program.
    pub fn with_argv(name: &str, step: AppletStep<K, A>, argv: Vec<String>) -> Program<K, A> {
        Program {
            name: name.to_string(),
            argv,
            data: Vec::new(),
            step,
            gen: 0,
        }
    }
}

impl<K: Kernel, A: BufferAlloc> Clone for Program<K, A> {
    fn clone(&self) -> Self {
        Program {
            name: self.name.clone(),
            argv: self.argv.clone(),
            data: self.data.clone(),
            step: self.step,
            gen: self.gen,
        }
    }
}

/// The task control block.
pub struct TaskCtl<K: Kernel, A: BufferAlloc> {
    pub state: TaskState,
    pub parent: Option<u32>,
    pub children: BTreeSet<u32>,
    pub program: Program<K, A>,
    /// Signals delivered but not yet checked by the task's cooperative
    /// step. Checked by `check_signals` at blocking I/O and between
    /// pipeline stages. Clear + delivered atomically to the task.
    pub pending: SignalSet,
    /// Inherited environment variables. Copied from parent on fork so
    /// exec'd programs can read PATH, HOME, etc. The shell syncs its
    /// own env region into this table after builtins modify it.
    pub env: Vec<(String, String)>,
    /// The foreground pipeline's child ids.  Ctrl-C / SIGINT is delivered
    /// only to these — background jobs are not killed by the terminal
    /// interrupt.  Cleared when the pipeline completes.
    pub foreground: BTreeSet<u32>,
    /// Set by `drain_wakes` when a `WaitChildTimeout` expires.
    /// The task checks and clears this on its next step to detect
    /// that it was woken by a timeout rather than a child exit.
    pub wait_timed_out: bool,
}

/// The proc manager. Owns the VFS and the task registry + run queue.
pub struct ProcManager<K: Kernel, A: BufferAlloc> {
    pub vfs: Vfs<K, A>,
    /// Scheduler trace (task ids in run order) — the sidecar's scheduler
    /// observability; tests assert round-robin interleaving with it.
    pub sched_trace: Vec<u32>,
    /// Wake trace: every `Parked`/`Woken` event with its reason, in
    /// order. Blocked-task liveness is observable through it — a task
    /// that parks without a later `Woken` for the same reason is wedged.
    pub wake_trace: Vec<WakeEvent>,
    tasks: BTreeMap<u32, TaskCtl<K, A>>,
    run: VecDeque<u32>,
    applets: BTreeMap<String, AppletStep<K, A>>,
    /// Optional network driver client for socket I/O. Set after boot
    /// when a network channel is available.
    pub net: Option<Box<dyn SocketOps>>,
}

/// The execution context handed to a program's step: the manager, the
/// task's id, and the task's program data (so fork can snapshot it).
pub struct Ctx<'a, K: Kernel, A: BufferAlloc> {
    pub pm: &'a mut ProcManager<K, A>,
    pub task: u32,
    pub data: &'a mut Vec<u8>,
}

impl<K: Kernel, A: BufferAlloc> ProcManager<K, A> {
    pub fn new(vfs: Vfs<K, A>) -> ProcManager<K, A> {
        ProcManager {
            vfs,
            sched_trace: Vec::new(),
            wake_trace: Vec::new(),
            tasks: BTreeMap::new(),
            run: VecDeque::new(),
            applets: BTreeMap::new(),
            net: None,
        }
    }

    /// Install a network driver client after boot.
    pub fn set_net(&mut self, net: Box<dyn SocketOps>) {
        self.net = Some(net);
    }

    /// Register task 0 (the VFS's initial task) as init, with `program`.
    pub fn spawn_init(&mut self, program: Program<K, A>) -> u32 {
        debug_assert!(!self.tasks.contains_key(&0), "init already spawned");
        self.tasks.insert(
            0,
            TaskCtl {
                state: TaskState::Runnable,
                parent: None,
                children: BTreeSet::new(),
                program,
                pending: SignalSet::empty(),
                env: Vec::new(),
                foreground: BTreeSet::new(),
                wait_timed_out: false,
            },
        );
        self.run.push_back(0);
        0
    }

    /// Spawn a new task with a fresh (empty) fd table.
    pub fn spawn(&mut self, program: Program<K, A>) -> Result<u32, Errno> {
        let id = self.vfs.add_task()?;
        self.tasks.insert(
            id,
            TaskCtl {
                state: TaskState::Runnable,
                parent: Some(0),
                children: BTreeSet::new(),
                program,
                pending: SignalSet::empty(),
                env: Vec::new(),
                foreground: BTreeSet::new(),
                wait_timed_out: false,
            },
        );
        self.tasks.get_mut(&0).map(|t| t.children.insert(id));
        self.run.push_back(id);
        Ok(id)
    }

    /// Register an exec-able applet (BusyBox argv[0]-dispatch, miniature).
    pub fn register_applet(&mut self, name: &str, step: AppletStep<K, A>) {
        self.applets.insert(name.to_string(), step);
    }

    /// `fork`: the child is a new task whose fd table is a *copy* of the
    /// parent's (shared `FileNode`s → shared offsets), cwd/cred copied, and
    /// whose program state is the parent's snapshot plus the fork marker.
    /// Returns the child id (to the caller — the parent).
    pub fn fork(&mut self, parent: u32) -> Result<u32, Errno> {
        let child = self.vfs.clone_task(parent, 0)?;
        let data = self
            .tasks
            .get(&parent)
            .map(|t| t.program.data.clone())
            .ok_or(Errno::EInval)?;
        self.register_child(parent, child, data)?;
        Ok(child)
    }

    /// `fork_thread`: like `fork`, but the child *shares* the parent's fd
    /// table object (CLONE_FILES — POSIX threads).
    pub fn fork_thread(&mut self, parent: u32) -> Result<u32, Errno> {
        let child = self.vfs.clone_task(parent, CLONE_FILES)?;
        let data = self
            .tasks
            .get(&parent)
            .map(|t| t.program.data.clone())
            .ok_or(Errno::EInval)?;
        self.register_child(parent, child, data)?;
        Ok(child)
    }

    /// Register a child task: cloned program (with the fork marker), parent
    /// link, queued.
    fn register_child(&mut self, parent: u32, child: u32, data: Vec<u8>) -> Result<(), Errno> {
        let (program, parent_env) = self
            .tasks
            .get(&parent)
            .map(|t| {
                let p = Program {
                    name: t.program.name.clone(),
                    argv: t.program.argv.clone(),
                    data,
                    step: t.program.step,
                    gen: t.program.gen,
                };
                (p, t.env.clone())
            })
            .ok_or(Errno::EInval)?;
        self.tasks.insert(
            child,
            TaskCtl {
                state: TaskState::Runnable,
                parent: Some(parent),
                children: BTreeSet::new(),
                program,
                pending: SignalSet::empty(),
                env: parent_env,
                foreground: BTreeSet::new(),
                wait_timed_out: false,
            },
        );
        if let Some(tc) = self.tasks.get_mut(&parent) {
            tc.children.insert(child);
        }
        // The child's snapshot carries the fork-return marker.
        if let Some(tc) = self.tasks.get_mut(&child) {
            tc.program.data.push(FORK_MARKER);
        }
        self.run.push_back(child);
        Ok(())
    }

    /// Terminate a task: drop its fd table, mark it a zombie, reparent its
    /// orphans to init, and wake a parent blocked on it.
    pub fn exit(&mut self, task: u32, code: i32) {
        self.do_exit(task, code);
    }

    fn do_exit(&mut self, id: u32, code: i32) {
        self.vfs.exit_task(id).ok();
        let parent = self.tasks.get(&id).and_then(|t| t.parent);
        // Reparent orphans to init (task 0), unless we *are* init.
        if id != 0 {
            let orphans: Vec<u32> = self
                .tasks
                .get(&id)
                .map(|t| t.children.iter().copied().collect())
                .unwrap_or_default();
            for c in orphans {
                if let Some(tc) = self.tasks.get_mut(&c) {
                    tc.parent = Some(0);
                }
                if let Some(t0) = self.tasks.get_mut(&0) {
                    t0.children.insert(c);
                }
            }
        }
        if let Some(tc) = self.tasks.get_mut(&id) {
            tc.state = TaskState::Exited(code);
        }
        if let Some(p) = parent {
            let waiting = matches!(
                self.tasks.get(&p).map(|t| t.state),
                Some(TaskState::Blocked(BlockReason::WaitingChild(c))) if c == id
            );
            if waiting {
                if let Some(tc) = self.tasks.get_mut(&p) {
                    tc.state = TaskState::Runnable;
                }
                self.run.push_back(p);
                self.wake_trace
                    .push(WakeEvent::Woken(p, BlockReason::WaitingChild(id)));
            }
        }
    }

    /// `waitpid`: reap a child with optional flags. `options & WNOHANG`
    /// makes the call non-blocking — when the child is still alive the
    /// return is `Blocked` without the caller needing to park.
    ///
    /// Without `WNOHANG`, `Blocked` means the caller should park
    /// (`Step::Blocked(WaitingChild(child))`) and retry on wake.
    pub fn waitpid(&mut self, task: u32, child: u32, _options: u32) -> WaitOutcome {
        let state = self.tasks.get(&child).map(|t| t.state);
        match state {
            Some(TaskState::Exited(code)) => {
                self.tasks.remove(&child);
                if let Some(tc) = self.tasks.get_mut(&task) {
                    tc.children.remove(&child);
                }
                WaitOutcome::Reaped(code)
            }
            Some(TaskState::Blocked(BlockReason::Stopped)) => WaitOutcome::Stopped(child),
            Some(_) => WaitOutcome::Blocked,
            None => WaitOutcome::NoSuchChild,
        }
    }

    /// Blocking `wait` — same as `waitpid` with no flags. The caller
    /// should park on `Blocked` and retry on wake.
    pub fn wait(&mut self, task: u32, child: u32) -> WaitOutcome {
        self.waitpid(task, child, 0)
    }

    /// `exec`: replace the task's image with the applet its file names, and
    /// set the new image's argv (design §6.2's `ExecSpec`: the caller — a
    /// shell, init, a spawner — supplies the argument vector; `argv[0]`
    /// conventionally names the program). Resolves and reads the file
    /// through the VFS; the first non-empty line names the applet. Fds
    /// with the `cloexec` flag are closed before the image is replaced.
    pub fn exec(&mut self, task: u32, path: &str, argv: &[&str]) -> Result<(), Errno> {
        // Close every fd with the cloexec flag before replacing the image.
        // This is the POSIX O_CLOEXEC / FD_CLOEXEC semantics: pipe fds
        // and other internal descriptors marked cloexec don't leak into
        // the exec'd program.
        self.vfs.close_cloexec(task);
        let fd = self.vfs.open(task, path, O_RDONLY, 0)?;
        let mut buf = [0u8; 4096];
        let n = self.vfs.read(task, fd, &mut buf)?;
        self.vfs.close(task, fd)?;
        let name = core::str::from_utf8(&buf[..n])
            .ok()
            .and_then(|s| s.lines().next())
            .map(str::trim)
            .filter(|s| !s.is_empty())
            .ok_or(Errno::ENoexec)?;
        let step = *self.applets.get(name).ok_or(Errno::ENoexec)?;
        let argv = argv.iter().map(|a| a.to_string()).collect::<Vec<_>>();
        if let Some(tc) = self.tasks.get_mut(&task) {
            let gen = tc.program.gen.wrapping_add(1);
            tc.program = Program {
                name: name.to_string(),
                argv,
                data: Vec::new(),
                step,
                gen,
            };
        }
        Ok(())
    }

    /// Run one cooperative step: pop the head of the run queue, run its
    /// program, and apply the outcome. Returns the outcome.
    pub fn run_next(&mut self) -> Option<Step> {
        let id = self.run.pop_front()?;
        let (step, gen, mut data) = {
            let tc = self.tasks.get_mut(&id)?;
            (tc.program.step, tc.program.gen, core::mem::take(&mut tc.program.data))
        };
        self.sched_trace.push(id);
        let mut ctx = Ctx {
            pm: self,
            task: id,
            data: &mut data,
        };
        let outcome = step(&mut ctx);
        // Restore the program's data (the zombie keeps its state). If the
        // step `exec`'d, the program was replaced (generation bumped) — its
        // fresh data must not be clobbered by the old image's.
        if let Some(tc) = self.tasks.get_mut(&id) {
            if tc.program.gen == gen {
                tc.program.data = data;
            }
        }
        let outcome = match outcome {
            Step::Done => Step::Exit(0),
            o => o,
        };
        // After each step, check for pending fatal signals. If one is
        // pending and the task did not already exit, override the outcome
        // to a signal death — this is the cooperative delivery path.
        let outcome = if !matches!(outcome, Step::Exit(_)) {
            if let Some(sig) = self.check_signals(id) {
                if matches!(sig, SIGINT | SIGQUIT | SIGTERM | SIGKILL) {
                    Step::Exit(signal_exit_code(sig))
                } else if sig == SIGTSTP {
                    // SIGTSTP stops the task.  Park it and do not re-enqueue.
                    if let Some(tc) = self.tasks.get_mut(&id) {
                        tc.state = TaskState::Blocked(BlockReason::Stopped);
                    }
                    self.wake_trace.push(WakeEvent::Parked(id, BlockReason::Stopped));
                    return Some(Step::Blocked(BlockReason::Stopped));
                } else {
                    outcome
                }
            } else {
                outcome
            }
        } else {
            outcome
        };
        match outcome {
            Step::Yield => {
                if let Some(tc) = self.tasks.get_mut(&id) {
                    tc.state = TaskState::Runnable;
                }
                self.run.push_back(id);
            }
            Step::Blocked(reason) => {
                if let Some(tc) = self.tasks.get_mut(&id) {
                    tc.state = TaskState::Blocked(reason);
                }
                self.wake_trace.push(WakeEvent::Parked(id, reason));
            }
            Step::Exit(code) => {
                let already = matches!(
                    self.tasks.get(&id).map(|t| t.state),
                    Some(TaskState::Exited(_))
                );
                if !already {
                    self.do_exit(id, code);
                }
            }
            Step::Done => unreachable!(),
        }
        Some(outcome)
    }

    /// Wake tasks parked on a readable or writable fd whose wait is now
    /// satisfied (pipe data/EOF, console input, pipe space freed, last
    /// reader gone). Called at every scheduler step; also public so an
    /// event-loop driver can re-arm blocked tasks after external input
    /// arrives between runs.
    pub fn drain_wakes(&mut self) {
        let mut woken = self.vfs.take_woken_readers();
        woken.extend(self.vfs.take_woken_writers());
        for t in woken {
            if let Some(tc) = self.tasks.get_mut(&t) {
                // Capture the blocked reason for the trace before the
                // transition; only read/write-fd parks are woken here.
                let reason = match tc.state {
                    TaskState::Blocked(
                        r @ (BlockReason::Readable(_) | BlockReason::Writable(_)),
                    ) => r,
                    _ => continue,
                };
                tc.state = TaskState::Runnable;
                self.run.push_back(t);
                self.wake_trace.push(WakeEvent::Woken(t, reason));
            }
        }
        // Tick down sleep timers. Any task blocked on Sleep(n) gets
        // n decremented; when it reaches 0 the task is woken.
        let mut sleepers = Vec::new();
        for (&id, tc) in self.tasks.iter_mut() {
            if let TaskState::Blocked(BlockReason::Sleep(n)) = &mut tc.state {
                if *n == 0 {
                    sleepers.push(id);
                } else {
                    *n -= 1;
                }
            }
        }
        for id in sleepers {
            if let Some(tc) = self.tasks.get_mut(&id) {
                tc.state = TaskState::Runnable;
                self.run.push_back(id);
                self.wake_trace
                    .push(WakeEvent::Woken(id, BlockReason::Sleep(0)));
            }
        }
        // Wake blocked tasks that have pending fatal signals (SIGINT,
        // SIGTERM, SIGKILL).  Without this, a task blocked on
        // WaitingChild would never observe a Ctrl-C until the child
        // exits on its own.
        let mut sig_woken = Vec::new();
        for (&id, tc) in self.tasks.iter() {
            if let TaskState::Blocked(_) = tc.state {
                if tc.pending.contains(SIGINT)
                    || tc.pending.contains(SIGTERM)
                    || tc.pending.contains(SIGKILL)
                {
                    sig_woken.push(id);
                }
            }
        }
        for id in sig_woken {
            if let Some(tc) = self.tasks.get_mut(&id) {
                let reason = match tc.state {
                    TaskState::Blocked(r) => r,
                    _ => continue,
                };
                tc.state = TaskState::Runnable;
                self.run.push_back(id);
                self.wake_trace.push(WakeEvent::Woken(id, reason));
            }
        }
        // Terminal interrupt: when the console has a Ctrl-C (0x03) in its
        // input buffer, deliver SIGINT to foreground children.  Falls
        // back to ALL children when no foreground set exists (e.g. init,
        // which doesn't use the shell's foreground mechanism).
        if self.vfs.has_console_byte(0x03) {
            self.vfs.discard_console_byte(0x03);
            let fg_children: Vec<u32> = self.tasks.values()
                .flat_map(|tc| tc.foreground.iter().copied())
                .collect();
            let targets: Vec<u32> = if fg_children.is_empty() {
                self.tasks.values()
                    .flat_map(|tc| tc.children.iter().copied())
                    .collect()
            } else {
                fg_children
            };
            for c in targets {
                self.kill(c, SIGINT).ok();
            }
        }
        // Quit: Ctrl-\\ (0x1c) sends SIGQUIT to foreground children,
        // terminating them with exit 128+3=131.  The parent is woken
        // from WaitingChild so the shell can re-prompt.  Falls back to
        // all children when no foreground set exists (init mode).
        if self.vfs.has_console_byte(0x1c) {
            self.vfs.discard_console_byte(0x1c);
            let fg_children: Vec<u32> = self.tasks.values()
                .flat_map(|tc| tc.foreground.iter().copied())
                .collect();
            for c in &fg_children {
                self.kill(*c, SIGQUIT).ok();
            }
            // Wake parents blocked on WaitingChild for the killed children.
            let mut wake_parents: Vec<u32> = Vec::new();
            for (&id, tc) in self.tasks.iter() {
                if let TaskState::Blocked(BlockReason::WaitingChild(c)) = tc.state {
                    if fg_children.contains(&c) {
                        wake_parents.push(id);
                    }
                }
            }
            for id in wake_parents {
                if let Some(tc) = self.tasks.get_mut(&id) {
                    tc.state = TaskState::Runnable;
                    self.run.push_back(id);
                }
            }
        }
                // Terminal stop: when the console has a Ctrl-Z (0x1a) in its
        // input buffer, deliver SIGTSTP to every task's foreground children
        // immediately.  This suspends the foreground pipeline; the shell
        // observes the stopped children and re-prompts.
        if self.vfs.has_console_byte(0x1a) {
            self.vfs.discard_console_byte(0x1a);
            let fg_children: Vec<u32> = self.tasks.values()
                .flat_map(|tc| tc.foreground.iter().copied())
                .collect();
            for c in fg_children {
                self.kill(c, SIGTSTP).ok();
            }
            // Wake any parent blocked on WaitingChild for a now-stopped
            // child so the shell can observe the stop and re-prompt.
            let stopped_set: Vec<u32> = self.tasks.iter()
                .filter(|(_, tc)| matches!(tc.state, TaskState::Blocked(BlockReason::Stopped)))
                .map(|(&id, _)| id)
                .collect();
            let mut wake_parents: Vec<u32> = Vec::new();
            for (&id, tc) in self.tasks.iter() {
                if let TaskState::Blocked(BlockReason::WaitingChild(c)) = tc.state {
                    if stopped_set.contains(&c) {
                        wake_parents.push(id);
                    }
                }
            }
            for id in wake_parents {
                if let Some(tc) = self.tasks.get_mut(&id) {
                    tc.state = TaskState::Runnable;
                    self.run.push_back(id);
                }
            }
        }
        // Handle WaitChildTimeout: check if the child exited or the
        // timer expired.  This is how `fg` avoids hanging forever on a
        // background job that never exits.
        //
        // Phase 1: collect decisions without borrowing self.tasks mutably.
        let mut wake_list: Vec<(u32, BlockReason)> = Vec::new();
        let mut decrement_list: Vec<u32> = Vec::new();
        let mut kill_list: Vec<u32> = Vec::new();
        for (&id, tc) in self.tasks.iter() {
            if let TaskState::Blocked(BlockReason::WaitChildTimeout(child, ticks)) = tc.state {
                let child_done = self.tasks.get(&child)
                    .map(|c| matches!(c.state, TaskState::Exited(_)))
                    .unwrap_or(true);
                if child_done || ticks == 0 {
                    wake_list.push((id, BlockReason::WaitChildTimeout(child, 0)));
                    if !child_done {
                        kill_list.push(child);
                    }
                } else {
                    decrement_list.push(id);
                }
            }
        }
        // Phase 2: mutate (kills, decrements, wakes).
        for child in kill_list {
            self.kill(child, SIGTERM).ok();
        }
        for id in decrement_list {
            if let Some(tc) = self.tasks.get_mut(&id) {
                if let TaskState::Blocked(BlockReason::WaitChildTimeout(_, ref mut t)) = tc.state {
                    *t -= 1;
                }
            }
        }
        for (id, reason) in wake_list {
            if let Some(tc) = self.tasks.get_mut(&id) {
                let timed_out = matches!(reason, BlockReason::WaitChildTimeout(_, 0));
                tc.wait_timed_out = timed_out;
                tc.state = TaskState::Runnable;
                self.run.push_back(id);
                self.wake_trace.push(WakeEvent::Woken(id, reason));
            }
        }
    }

    /// Run the cooperative scheduler until the run queue is empty (or
    /// `max_steps` steps). Returns the number of steps run. A non-empty
    /// result at `max_steps` with tasks still blocked means the remaining
    /// tasks are wedged (deadlock — the caller's problem).
    /// Return true if any task has an active timer (Sleep or
    /// WaitChildTimeout) that hasn't expired yet.  The scheduler must
    /// keep calling drain_wakes to tick those down.
    fn has_active_timers(&self) -> bool {
        self.tasks.values().any(|tc| matches!(
            tc.state,
            TaskState::Blocked(BlockReason::Sleep(n) | BlockReason::WaitChildTimeout(_, n)) if n > 0
        )) || self.tasks.values().any(|tc| matches!(tc.state, TaskState::Blocked(BlockReason::Stopped)))
    }

    pub fn run_until_quiet(&mut self, max_steps: usize) -> usize {
        let mut steps = 0;
        while steps < max_steps {
            self.drain_wakes();
            match self.run_next() {
                Some(_) => steps += 1,
                None => {
                    // No runnable task.  If timers are still counting
                    // down (Sleep or WaitChildTimeout), keep calling
                    // drain_wakes so they can expire.
                    if self.has_active_timers() {
                        steps += 1;
                        continue;
                    }
                    break;
                }
            }
        }
        steps
    }

    /// A task's state (None once reaped).
    pub fn state(&self, task: u32) -> Option<TaskState> {
        self.tasks.get(&task).map(|t| t.state)
    }

    /// A zombie's exit status.
    pub fn exit_code(&self, task: u32) -> Option<i32> {
        match self.tasks.get(&task).map(|t| t.state) {
            Some(TaskState::Exited(code)) => Some(code),
            _ => None,
        }
    }

    /// Return the program name of a task (for job-table display).
    pub fn task_name(&self, task: u32) -> &str {
        self.tasks.get(&task)
            .map(|t| t.program.name.as_str())
            .unwrap_or("?")
    }

    // ── Signal delivery ──────────────────────────────────────────────────

    /// Enqueue a signal on a task. The signal is delivered cooperatively:
    /// the task observes it at the next `check_signals()` call (at blocking
    /// I/O or between pipeline stages). Returns `Ok(())` on success, or
    /// `Err(ESRCH)` if the task does not exist.
    pub fn kill(&mut self, task: u32, sig: i32) -> Result<(), Errno> {
        let tc = self.tasks.get_mut(&task).ok_or(Errno::ESRCH)?;
        tc.pending.add(sig);
        if sig == SIGTSTP {
            // SIGTSTP stops the task: transition to Stopped regardless of
            // current state (Runnable → Stopped, Blocked → Stopped).
            let was_running = matches!(tc.state, TaskState::Runnable);
            tc.state = TaskState::Blocked(BlockReason::Stopped);
            if was_running {
                // Remove from run queue if present.
                self.run.retain(|&id| id != task);
            }
            self.wake_trace.push(WakeEvent::Parked(task, BlockReason::Stopped));
        } else if sig == SIGCONT {
            // SIGCONT resumes a stopped task.
            if let TaskState::Blocked(BlockReason::Stopped) = tc.state {
                tc.state = TaskState::Runnable;
                self.run.push_back(task);
                self.wake_trace.push(WakeEvent::Woken(task, BlockReason::Stopped));
            }
        } else if matches!(sig, SIGINT | SIGQUIT | SIGTERM | SIGKILL) {
            // A blocked task with a pending fatal signal must not stay parked:
            // wake it so it can observe the signal and exit.
            if let TaskState::Blocked(reason) = tc.state {
                tc.state = TaskState::Runnable;
                self.run.push_back(task);
                self.wake_trace.push(WakeEvent::Woken(task, reason));
            }
        }
        Ok(())
    }

    /// Deliver a signal to every task in a process group (all tasks
    /// sharing the same parent, minus the sender). Used by the shell to
    /// send SIGINT to an entire pipeline on Ctrl-C.
    pub fn kill_group(&mut self, sender: u32, sig: i32) {
        // Collect the sender's children (the pipeline stages) first to
        // avoid borrowing conflicts with kill().
        let children: Vec<u32> = {
            match self.tasks.get(&sender) {
                Some(tc) => tc.children.iter().copied().collect(),
                None => Vec::new(),
            }
        };
        for c in children {
            self.kill(c, sig).ok();
        }
    }

    /// Deliver a signal only to the sender's foreground children
    /// (the current pipeline).  Background jobs are not affected.
    /// This is what Ctrl-C / SIGINT uses — POSIX job control semantics.
    pub fn kill_group_fg(&mut self, sender: u32, sig: i32) {
        let fg: Vec<u32> = {
            match self.tasks.get(&sender) {
                Some(tc) => tc.foreground.iter().copied().collect(),
                None => Vec::new(),
            }
        };
        for c in fg {
            self.kill(c, sig).ok();
        }
    }

    /// Set the foreground pipeline children for a task.
    pub fn set_foreground(&mut self, task: u32, children: BTreeSet<u32>) {
        if let Some(tc) = self.tasks.get_mut(&task) {
            tc.foreground = children;
        }
    }

    /// Check and consume pending signals for a task. Returns the highest-
    /// priority pending signal (lower number = more urgent), or `None` if
    /// no signal is pending. The caller should act on the returned signal
    /// immediately (e.g. exit with `signal_exit_code(sig)`).
    pub fn check_signals(&mut self, task: u32) -> Option<i32> {
        let tc = self.tasks.get_mut(&task)?;
        tc.pending.take_one()
    }

    pub fn task_count(&self) -> usize {
        self.tasks.len()
    }

    pub fn runnable(&self) -> usize {
        self.run.len()
    }
}

impl<'a, K: Kernel, A: BufferAlloc> Ctx<'a, K, A> {
    pub fn vfs(&mut self) -> &mut Vfs<K, A> {
        &mut self.pm.vfs
    }

    /// Fork: create a child task with a copied fd table and a snapshot of
    /// the caller's program state (plus the fork marker). Returns the child
    /// id in the parent; the child resumes with `is_child(data)` true.
    pub fn fork(&mut self) -> Result<u32, Errno> {
        let child = self.pm.vfs.clone_task(self.task, 0)?;
        self.pm.register_child(self.task, child, self.data.clone())?;
        Ok(child)
    }

    /// Like `fork`, but the child shares the parent's fd table (threads).
    pub fn fork_thread(&mut self) -> Result<u32, Errno> {
        let child = self.pm.vfs.clone_task(self.task, CLONE_FILES)?;
        self.pm.register_child(self.task, child, self.data.clone())?;
        Ok(child)
    }

    /// Terminate the calling task with a status.
    pub fn exit(&mut self, code: i32) -> Step {
        self.pm.do_exit(self.task, code);
        Step::Done
    }

    pub fn wait(&mut self, child: u32) -> WaitOutcome {
        self.pm.wait(self.task, child)
    }

    /// `waitpid` with options (e.g. `WNOHANG` for non-blocking poll).
    /// When `WNOHANG` is set and the child is still alive, returns
    /// `Blocked` without the caller needing to park — the caller can
    /// continue processing instead.
    pub fn waitpid(&mut self, child: u32, options: u32) -> WaitOutcome {
        self.pm.waitpid(self.task, child, options)
    }

    /// Wait for a child with a tick-based timeout.  If the child exits
    /// before `ticks` scheduler steps, returns `Reaped`.  If the timer
    /// expires first, the child is killed with SIGTERM and the caller
    /// is woken with `Blocked(WaitChildTimeout(child, 0))`.
    pub fn wait_timeout(&mut self, child: u32, ticks: u32) -> Step {
        // Fast path: already reaped, gone, or stopped.
        match self.pm.wait(self.task, child) {
            WaitOutcome::Reaped(_code) => {
                return Step::Yield;
            }
            WaitOutcome::NoSuchChild => {
                return Step::Yield;
            }
            WaitOutcome::Stopped(_child_id) => {
                return Step::Yield;
            }
            WaitOutcome::Blocked => {}
        }
        Step::Blocked(BlockReason::WaitChildTimeout(child, ticks))
    }

    pub fn exec(&mut self, path: &str, argv: &[&str]) -> Result<(), Errno> {
        self.pm.exec(self.task, path, argv)
    }

    /// Close every fd with the `cloexec` flag set. Called by exec to
    /// prevent pipe fds from leaking into the new program image.
    pub fn close_cloexec(&mut self) {
        self.pm.vfs.close_cloexec(self.task);
    }

    /// Set or clear the `cloexec` flag on an fd.
    pub fn set_cloexec(&mut self, fd: u32, cloexec: bool) -> Result<(), Errno> {
        self.pm.vfs.set_cloexec(self.task, fd, cloexec)
    }

    /// The calling task's argument vector (set by `exec` or the spawner).
    pub fn argv(&self) -> &[String] {
        self.pm
            .tasks
            .get(&self.task)
            .map(|t| t.program.argv.as_slice())
            .unwrap_or(&[])
    }

    /// Blocking read: like `Vfs::read`, but on would-block (`EAGAIN` — an
    /// empty pipe, a console with no input) registers the task with the
    /// VFS and returns `ReadBlock::WouldBlock`. The program must then
    /// return `Step::Blocked(BlockReason::Readable(fd))`; the scheduler
    /// parks the task and the read-wake drain requeues it when the object
    /// becomes readable, at which point the program retries. Mount-table
    /// files never block (EOF reads return 0), so this only parks on
    /// pipes and devices.
    pub fn read_blocking(&mut self, fd: u32, buf: &mut [u8]) -> ReadBlock {
        let task = self.task;
        match self.pm.vfs.read(task, fd, buf) {
            Ok(n) => ReadBlock::Data(n),
            Err(Errno::EAgain) => match self.pm.vfs.wait_readable(task, fd) {
                Ok(()) => ReadBlock::WouldBlock,
                Err(e) => ReadBlock::Err(e),
            },
            Err(e) => ReadBlock::Err(e),
        }
    }

    /// Blocking write: like `Vfs::write`, but on would-block (`EAGAIN` —
    /// a full pipe) registers the task with the VFS and returns
    /// `WriteBlock::WouldBlock`. The program must then return
    /// `Step::Blocked(BlockReason::Writable(fd))`; the scheduler parks the
    /// task and the wake drain requeues it when a reader frees space (or
    /// the last reader closes — the retry observes `EPIPE`), at which
    /// point the program retries. Files and the console never block on
    /// write in v1, so this only parks on pipes.
    pub fn write_blocking(&mut self, fd: u32, buf: &[u8]) -> WriteBlock {
        let task = self.task;
        match self.pm.vfs.write(task, fd, buf) {
            Ok(n) => WriteBlock::Data(n),
            Err(Errno::EAgain) => match self.pm.vfs.wait_writable(task, fd) {
                Ok(()) => WriteBlock::WouldBlock,
                Err(e) => WriteBlock::Err(e),
            },
            Err(e) => WriteBlock::Err(e),
        }
    }

    /// Return `Step::Blocked` for the given reason.
    pub fn block(&self, reason: BlockReason) -> Step {
        Step::Blocked(reason)
    }

    pub fn yield_now(&self) -> Step {
        Step::Yield
    }

    /// Access the network driver client (if one was installed at boot).
    pub fn net(&mut self) -> Option<&mut Box<dyn SocketOps>> {
        self.pm.net.as_mut()
    }

    // ── Signals ──────────────────────────────────────────────────────────

    /// Check for pending signals on the calling task. Returns the highest-
    /// priority signal (lower number = more urgent), or `None`. Applets
    /// should call this at the top of their main loop and before/after
    /// blocking I/O. A returned SIGKILL/SIGINT/SIGTERM means the task
    /// should exit with `signal_exit_code(sig)`.
    pub fn check_signal(&mut self) -> Option<i32> {
        self.pm.check_signals(self.task)
    }

    /// Deliver a signal to a specific task (by task id).
    pub fn kill(&mut self, task: u32, sig: i32) -> Result<(), Errno> {
        self.pm.kill(task, sig)
    }

    /// Deliver a signal to the caller's process group (all siblings,
    /// excluding the sender). Used by the shell for Ctrl-C → SIGINT
    /// to an entire pipeline.
    pub fn kill_group(&mut self, sig: i32) {
        self.pm.kill_group(self.task, sig);
    }

    /// Deliver a signal only to the foreground pipeline children.
    /// Background jobs are not affected — POSIX job control semantics.
    pub fn kill_group_fg(&mut self, sig: i32) {
        self.pm.kill_group_fg(self.task, sig);
    }

    /// Set the foreground pipeline children for this task.
    pub fn set_foreground(&mut self, children: alloc::collections::BTreeSet<u32>) {
        self.pm.set_foreground(self.task, children);
    }

    /// Clear the foreground pipeline children (pipeline completed).
    pub fn clear_foreground(&mut self) {
        self.pm.set_foreground(self.task, alloc::collections::BTreeSet::new());
    }

    /// Change the calling task's working directory.
    pub fn chdir(&mut self, path: &str) -> Result<(), Errno> {
        self.pm.vfs.set_cwd(self.task, path)
    }

    /// Return the calling task's current working directory.
    pub fn getcwd(&self) -> Result<&str, Errno> {
        self.pm.vfs.get_cwd(self.task)
    }

    /// Park the calling task for `ticks` scheduler ticks (cooperative
    /// sleep).  The drain-wake countdown decrements each step and
    /// requeues the task when it reaches zero.
    pub fn sleep(&mut self, ticks: u32) -> Step {
        Step::Blocked(BlockReason::Sleep(ticks))
    }

    /// Check (and clear) the `wait_timed_out` flag.  Returns true if
    /// the task was woken because a `WaitChildTimeout` expired rather
    /// than because the child exited.
    pub fn check_wait_timeout(&mut self) -> bool {
        if let Some(tc) = self.pm.tasks.get_mut(&self.task) {
            let v = tc.wait_timed_out;
            tc.wait_timed_out = false;
            v
        } else {
            false
        }
    }

    /// Return a reference to the calling task's inherited environment
    /// variables. Programs use this to read PATH, HOME, etc.
    pub fn get_env(&self) -> &[(String, String)] {
        self.pm
            .tasks
            .get(&self.task)
            .map(|t| t.env.as_slice())
            .unwrap_or(&[])
    }


    /// Replace the calling task's environment. The shell calls this after
    /// builtins modify the env region so the TaskCtl stays in sync.
    pub fn set_env(&mut self, env: Vec<(String, String)>) {
        if let Some(tc) = self.pm.tasks.get_mut(&self.task) {
            tc.env = env;
        }
    }

    /// select(): check readiness across up to nfds file descriptors.
    /// Delegates to Vfs::select().
    pub fn select(
        &mut self,
        nfds: u32,
        readfds: &SelectFdSet,
        writefds: &SelectFdSet,
        errorfds: &SelectFdSet,
    ) -> Result<SelectResult, Errno> {
        let task = self.task;
        self.pm.vfs.select(task, nfds, readfds, writefds, errorfds)
    }

    /// Unified poll: check readiness across the task's open fds.
    /// Delegates to Vfs::poll(). Socket readiness is derived from
    /// client-side state; the caller can additionally query the network
    /// driver via SocketOps::poll for more accurate socket results.
    pub fn poll(
        &mut self,
        fds: &mut [PollFd],
    ) -> Result<usize, Errno> {
        let task = self.task;
        self.pm.vfs.poll(task, fds)
    }
}
