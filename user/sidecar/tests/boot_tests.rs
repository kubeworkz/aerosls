//! End-to-end tests for the sidecar bootstrap, driven against the *real*
//! ramdisk driver on the fake kernel: the BIB → caps resolution, the
//! manifest contract, and `boot()` itself — handshake, mounts, console
//! stdio, init spawn.

use aerosls_blockcache::BufferAlloc;
use aerosls_kernel_sim::{FakeClient, FakeKernel, DRIVER_CONSOLE, DRIVER_STORAGE};
use aerosls_proto::bootinfo::BootInfo;
use aerosls_proto::kabi::{SendCap, CAP_CHAN, CAP_MEM};
use aerosls_proto::{R, W};
use aerosls_ramdisk::endpoints::EndpointSet;
use aerosls_ramdisk::server::{self, Device};
use aerosls_sidecar::{boot, BootCaps};
use aerosls_vfs::{CharNode, ImageBuilder, O_RDONLY};
use std::sync::Arc;
use std::thread::JoinHandle;

type FC = FakeClient;
type FA = FakeAlloc;

/// The rootfs the booted sidecar runs: applet script files (first line
/// names the applet) plus the boot script init executes. The script
/// exercises init's shared word parser — a quoted argument, a
/// backslash-escaped space, a `<` stdin redirect — and two lines that
/// must fail 127 without stopping init: a nonexistent program and a
/// piped line (init runs one command per line).
fn image() -> Vec<u8> {
    let mut b = ImageBuilder::new();
    b.add_dir("/etc", 0o755);
    b.add_dir("/bin", 0o755);
    b.add_file("/etc/passwd", b"root:x:0:0:root:/root:/bin/sh\n", 0o644);
    b.add_file("/etc/group", b"root:x:0:\n", 0o644);
    b.add_file("/bin/cat", b"cat\n", 0o755);
    b.add_file("/bin/echo", b"echo\n", 0o755);
    b.add_file(
        "/etc/init.rc",
        b"# Boot script: full-path commands, quoted/escaped args, < and > redirects\n\n/bin/echo \"init says hi\"\n/bin/echo spaced\\ out\n/bin/cat < /etc/passwd\n/bin/echo booted > /tmp/out\n/bin/nope\n/bin/echo piped | /bin/cat\n",
        0o644,
    );
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

/// Read a whole file through a *fresh* VFS task (the booted init has
/// exited, so task 0's fd table is gone).
fn read_all(vfs: &mut aerosls_vfs::Vfs<FC, FA>, path: &str) -> Vec<u8> {
    let t = vfs.add_task().unwrap();
    let fd = vfs.open(t, path, O_RDONLY, 0).unwrap();
    let mut buf = [0u8; 128];
    let mut out = Vec::new();
    loop {
        match vfs.read(t, fd, &mut buf) {
            Ok(0) => break,
            Ok(n) => out.extend_from_slice(&buf[..n]),
            Err(_) => break,
        }
    }
    vfs.close(t, fd).unwrap();
    out
}

/// The full boot: BIB-shaped caps → handshake → mounts → the boot script
/// runner as init with console stdio → init forks one child per
/// `/etc/init.rc` line and runs the system to completion.
#[test]
fn boot_mounts_and_runs_init() {
    let (fake, client) = FakeKernel::new(image(), 1);
    let t = boot_driver(fake);

    // The sim's client table: no budget/console caps; the ramdisk endpoint
    // is the first wired handle (0).
    let caps = BootCaps::new(0, 0, 0, None, 0);
    let console = Arc::new(CharNode::console());
    let mut booted = boot(client.clone(), &caps, console.clone(), FakeAlloc(client.clone()))
        .expect("boot");

    booted.run(200);

    assert_eq!(booted.proc.exit_code(0), Some(0), "init exited cleanly");
    // The script's quoted / escaped echo lines print to console stdout
    // (fd 1), then `/bin/cat < /etc/passwd` carries the rootfs read out
    // through the full chain: cache → driver → storage.
    assert_eq!(
        console.console_io().output(),
        b"init says hi\nspaced out\nroot:x:0:0:root:/root:/bin/sh\n",
        "the boot script's quoted/escaped echoes and < redirect reached the console"
    );
    // `/bin/echo booted > /tmp/out`: the script runner's `>` redirect
    // pointed the child's fd 1 at the ramfs file.
    assert_eq!(read_all(&mut booted.proc.vfs, "/tmp/out"), b"booted\n");
    // The root mount is a real aerofs on the cache, not a shadow.
    assert_eq!(
        read_all(&mut booted.proc.vfs, "/etc/passwd"),
        b"root:x:0:0:root:/root:/bin/sh\n"
    );
    // The /bin/nope and piped lines cost two 127-exiting children (the
    // piped line proves the single-stage rule — init runs one command per
    // line); init reaped both and still finished the script cleanly
    // (proved by the exit above).
    assert!(booted
        .proc
        .wake_trace
        .iter()
        .any(|w| matches!(w, aerosls_procmgr::WakeEvent::Parked(0, aerosls_procmgr::BlockReason::WaitingChild(_)))),
        "init parked waiting for at least one script child"
    );

    client.kill_driver(0);
    t.join().unwrap();
}

/// The interactive-shell boot: init runs a script whose last line is
/// `/bin/sh`; the shell prompts on the console, parks for input, and the
/// test types commands — a two-stage pipeline, a failing command, and a
/// `$?` expansion — asserting the console transcript at each step and the
/// parked shell state between them.
#[test]
fn boot_runs_an_interactive_shell() {
    let mut b = ImageBuilder::new();
    b.add_dir("/etc", 0o755);
    b.add_dir("/bin", 0o755);
    b.add_file("/etc/passwd", b"root:x:0:0:root:/root:/bin/sh\n", 0o644);
    b.add_file("/bin/cat", b"cat\n", 0o755);
    b.add_file("/bin/echo", b"echo\n", 0o755);
    b.add_file("/bin/grep", b"grep\n", 0o755);
    b.add_file("/bin/wc", b"wc\n", 0o755);
    b.add_file("/bin/head", b"head\n", 0o755);
    b.add_file("/bin/tail", b"tail\n", 0o755);
    b.add_file("/bin/sort", b"sort\n", 0o755);
    b.add_file("/bin/seq", b"seq\n", 0o755);
    b.add_file("/bin/false", b"false\n", 0o755);
    b.add_file("/bin/sh", b"sh\n", 0o755);
    // A second bin dir so PATH-driven lookup has somewhere to search:
    // greet lives only under /usr/bin.
    b.add_dir("/usr", 0o755);
    b.add_dir("/usr/bin", 0o755);
    b.add_file("/usr/bin/greet", b"echo\n", 0o755);
    // A > 4 KiB file (500 lines), so `cat big.txt | head -n 1` forces cat
    // to fill the pipe buffer and park on a full pipe before head exits
    // early — the partial-pipe case the head session exercises.
    let mut big = Vec::new();
    for i in 0..500 {
        big.extend_from_slice(format!("line-{:03}\n", i).as_bytes());
    }
    b.add_file("/etc/big.txt", &big, 0o644);
    // A small file whose lines are out of order, small enough to span
    // several 16-byte reads but big enough that the buffer grows across
    // them.
    b.add_file("/etc/mixed.txt", b"pear\napple\nfig\ndate\n", 0o644);
    b.add_file("/etc/init.rc", b"/bin/sh\n", 0o644);
    let (fake, client) = FakeKernel::new(b.build(), 1);
    let t = boot_driver(fake);

    let caps = BootCaps::new(0, 0, 0, None, 0);
    let console = Arc::new(CharNode::console());
    let mut booted = boot(client.clone(), &caps, console.clone(), FakeAlloc(client.clone()))
        .expect("boot");

    // Boot: init forks the shell (task 1), which prints its first prompt
    // and parks on console input; init parks waiting for it.
    booted.run(100);
    assert_eq!(
        console.console_io().output(),
        b"$ ",
        "the shell's first prompt"
    );
    assert_eq!(
        booted.proc.state(1),
        Some(aerosls_procmgr::TaskState::Blocked(
            aerosls_procmgr::BlockReason::Readable(0)
        )),
        "the shell parks on the empty console"
    );
    assert_eq!(
        booted.proc.state(0),
        Some(aerosls_procmgr::TaskState::Blocked(
            aerosls_procmgr::BlockReason::WaitingChild(1)
        )),
        "init waits for the shell"
    );

    // `echo hello | cat`: the shell forks a two-stage pipeline; cat (the
    // last stage) writes the payload to the console.
    console.console_io().push_input(b"echo hello | cat\n");
    booted.run(100);
    assert_eq!(
        console.console_io().output(),
        b"$ hello\n$ ",
        "the pipeline's output, then the next prompt"
    );

    // `false` exits 1 silently; the shell tracks it as `$?`.
    console.console_io().push_input(b"false\n");
    booted.run(100);
    assert_eq!(console.console_io().output(), b"$ hello\n$ $ ");

    // `echo $?` expands the last exit status: 1 from the `false`.
    console.console_io().push_input(b"echo $?\n");
    booted.run(100);
    assert_eq!(
        console.console_io().output(),
        b"$ hello\n$ $ 1\n$ ",
        "$? carried false's 1 into the next command"
    );
    assert_eq!(
        booted.proc.state(1),
        Some(aerosls_procmgr::TaskState::Blocked(
            aerosls_procmgr::BlockReason::Readable(0)
        )),
        "the shell parked again awaiting the next command"
    );

    // `>` redirect: echo's stdout lands in /tmp/saved, not on the console
    // (the transcript only gains the prompt).
    console.console_io().push_input(b"echo saved > /tmp/saved\n");
    booted.run(100);
    assert_eq!(
        console.console_io().output(),
        b"$ hello\n$ $ 1\n$ $ ",
        "the redirect kept the payload off the console"
    );
    assert_eq!(
        read_all(&mut booted.proc.vfs, "/tmp/saved"),
        b"saved\n",
        "the shell's > redirect wrote the ramfs file"
    );

    // `<` redirect: cat reads the rootfs file from stdin (fd 0) and the
    // passwd line reaches the console through the shell.
    console.console_io().push_input(b"cat < /etc/passwd\n");
    booted.run(100);
    assert_eq!(
        console.console_io().output(),
        b"$ hello\n$ $ 1\n$ $ root:x:0:0:root:/root:/bin/sh\n$ ",
        "the shell's < redirect fed cat from the rootfs"
    );

    // A pasted batch: two lines arrive in one read. The shell buffers the
    // whole chunk and runs them in order — one prompt around the batch,
    // nothing dropped.
    console.console_io().push_input(b"echo one\necho two\n");
    booted.run(100);
    assert_eq!(
        console.console_io().output(),
        b"$ hello\n$ $ 1\n$ $ root:x:0:0:root:/root:/bin/sh\n$ one\ntwo\n$ ",
        "the batch ran line by line with a single prompt around it"
    );
    assert_eq!(
        booted.proc.state(1),
        Some(aerosls_procmgr::TaskState::Blocked(
            aerosls_procmgr::BlockReason::Readable(0)
        )),
        "the shell parked after draining the batch"
    );

    // Quoting: double quotes keep a space inside one argument, so the
    // two-word payload survives tokenization through the pipeline.
    console.console_io().push_input(b"echo \"hello world\" | cat\n");
    booted.run(100);
    assert_eq!(
        console.console_io().output(),
        b"$ hello\n$ $ 1\n$ $ root:x:0:0:root:/root:/bin/sh\n$ one\ntwo\n$ hello world\n$ ",
        "double quotes grouped the two-word argument through the pipeline"
    );

    // Single quotes keep `$?` literal — no expansion inside, so the shell
    // prints the two characters instead of the last status.
    console.console_io().push_input(b"echo '$?'\n");
    booted.run(100);
    assert_eq!(
        console.console_io().output(),
        b"$ hello\n$ $ 1\n$ $ root:x:0:0:root:/root:/bin/sh\n$ one\ntwo\n$ hello world\n$ $?\n$ ",
        "single quotes kept the $? token literal"
    );

    // Backslash escapes: `\ ` keeps a space inside one word, so the shell
    // prints a single argument with a space instead of splitting it.
    console.console_io().push_input(b"echo hello\\ world\n");
    booted.run(100);
    assert_eq!(
        console.console_io().output(),
        b"$ hello\n$ $ 1\n$ $ root:x:0:0:root:/root:/bin/sh\n$ one\ntwo\n$ hello world\n$ $?\n$ hello world\n$ ",
        "the backslash-escaped space kept the two words as one argument"
    );

    // Exact argv through the fork snapshot: a quoted double space survives
    // the per-argument encoding (a join/split round-trip would collapse it
    // to a single space).
    console.console_io().push_input(b"echo \"a  b\"\n");
    booted.run(100);
    assert_eq!(
        console.console_io().output(),
        b"$ hello\n$ $ 1\n$ $ root:x:0:0:root:/root:/bin/sh\n$ one\ntwo\n$ hello world\n$ $?\n$ hello world\n$ a  b\n$ ",
        "quoted spacing arrived at the applet exactly as typed"
    );

    // Variable expansion: $PATH and ${HOME} resolve from the shell's
    // built-in environment (the env region survives the pipeline waits).
    console.console_io().push_input(b"echo $PATH ${HOME}\n");
    booted.run(100);
    assert_eq!(
        console.console_io().output(),
        b"$ hello\n$ $ 1\n$ $ root:x:0:0:root:/root:/bin/sh\n$ one\ntwo\n$ hello world\n$ $?\n$ hello world\n$ a  b\n$ /bin /root\n$ ",
        "variable expansion reached the applet through the shell"
    );

    // Builtins: `export NAME=value` and `setenv NAME value` mutate the
    // shell's persistent env region, so `$VAR` expands to the new value
    // in the next command.
    console.console_io().push_input(b"export FOO=bar\n");
    booted.run(100);
    console.console_io().push_input(b"echo $FOO\n");
    booted.run(100);
    assert_eq!(
        console.console_io().output(),
        b"$ hello\n$ $ 1\n$ $ root:x:0:0:root:/root:/bin/sh\n$ one\ntwo\n$ hello world\n$ $?\n$ hello world\n$ a  b\n$ /bin /root\n$ $ bar\n$ ",
        "export's variable set was visible to the next command's expansion"
    );

    // setenv takes the value as a separate (quoted) argument.
    console.console_io().push_input(b"setenv GREETING \"hi there\"\n");
    booted.run(100);
    console.console_io().push_input(b"echo $GREETING\n");
    booted.run(100);
    assert_eq!(
        console.console_io().output(),
        b"$ hello\n$ $ 1\n$ $ root:x:0:0:root:/root:/bin/sh\n$ one\ntwo\n$ hello world\n$ $?\n$ hello world\n$ a  b\n$ /bin /root\n$ $ bar\n$ $ hi there\n$ ",
        "setenv's quoted value survived expansion in the next command"
    );

    // A usage error (setenv with one argument) exits 2, which becomes $?.
    console.console_io().push_input(b"export HOME=/home/root\nsetenv onlyname\n");
    booted.run(100);
    console.console_io().push_input(b"echo $?\n");
    booted.run(100);
    assert_eq!(
        console.console_io().output(),
        b"$ hello\n$ $ 1\n$ $ root:x:0:0:root:/root:/bin/sh\n$ one\ntwo\n$ hello world\n$ $?\n$ hello world\n$ a  b\n$ /bin /root\n$ $ bar\n$ $ hi there\n$ $ 2\n$ ",
        "the builtin's usage error became the next $?"
    );

    // `export` with no args lists the environment in region order: PATH
    // first, HOME overwritten in place, then the appended FOO/GREETING.
    console.console_io().push_input(b"export\n");
    booted.run(100);
    assert_eq!(
        console.console_io().output(),
        b"$ hello\n$ $ 1\n$ $ root:x:0:0:root:/root:/bin/sh\n$ one\ntwo\n$ hello world\n$ $?\n$ hello world\n$ a  b\n$ /bin /root\n$ $ bar\n$ $ hi there\n$ $ 2\n$ PATH=/bin\nHOME=/home/root\nFOO=bar\nGREETING=hi there\n$ ",
        "export listed the mutated environment in region order"
    );

    // PATH-driven lookup: with PATH=/usr/bin:/bin, a bare name resolves
    // through the search path — greet lives only in /usr/bin (first hit),
    // echo falls through to /bin.
    console.console_io().push_input(b"export PATH=/usr/bin:/bin\n");
    booted.run(100);
    console.console_io().push_input(b"greet hello\n");
    booted.run(100);
    console.console_io().push_input(b"echo fallback\n");
    booted.run(100);
    assert_eq!(
        console.console_io().output(),
        b"$ hello\n$ $ 1\n$ $ root:x:0:0:root:/root:/bin/sh\n$ one\ntwo\n$ hello world\n$ $?\n$ hello world\n$ a  b\n$ /bin /root\n$ $ bar\n$ $ hi there\n$ $ 2\n$ PATH=/bin\nHOME=/home/root\nFOO=bar\nGREETING=hi there\n$ $ hello\n$ fallback\n$ ",
        "bare commands resolved through the exported PATH (usr/bin hit, /bin fallback)"
    );

    // A PATH miss: with PATH=/usr/bin only, `false` has no candidate and
    // the child exec fails 127. Check it with a slash-qualified command
    // (slash tokens skip the search path — a bare `echo` would fail too).
    console.console_io().push_input(b"export PATH=/usr/bin\n");
    booted.run(100);
    console.console_io().push_input(b"false\n");
    booted.run(100);
    console.console_io().push_input(b"/bin/echo $?\n");
    booted.run(100);
    assert_eq!(
        console.console_io().output(),
        b"$ hello\n$ $ 1\n$ $ root:x:0:0:root:/root:/bin/sh\n$ one\ntwo\n$ hello world\n$ $?\n$ hello world\n$ a  b\n$ /bin /root\n$ $ bar\n$ $ hi there\n$ $ 2\n$ PATH=/bin\nHOME=/home/root\nFOO=bar\nGREETING=hi there\n$ $ hello\n$ fallback\n$ $ $ 127\n$ ",
        "an unresolvable bare command failed 127 and fed $?"
    );

    // Restore PATH=/bin (the earlier PATH-miss test left it /usr/bin, so
    // a bare `echo` would fail 127), then unset GREETING: it vanishes from
    // the env region, `$GREETING` expands to nothing (an unquoted empty
    // expansion leaves echo with no argument → one blank line), and the
    // listing drops it.
    console.console_io().push_input(b"export PATH=/bin\nunset GREETING\n");
    booted.run(100);
    console.console_io().push_input(b"echo $GREETING\n");
    booted.run(100);
    console.console_io().push_input(b"export\n");
    booted.run(100);
    assert_eq!(
        console.console_io().output(),
        b"$ hello\n$ $ 1\n$ $ root:x:0:0:root:/root:/bin/sh\n$ one\ntwo\n$ hello world\n$ $?\n$ hello world\n$ a  b\n$ /bin /root\n$ $ bar\n$ $ hi there\n$ $ 2\n$ PATH=/bin\nHOME=/home/root\nFOO=bar\nGREETING=hi there\n$ $ hello\n$ fallback\n$ $ $ 127\n$ $ \n$ PATH=/bin\nHOME=/home/root\nFOO=bar\n$ ",
        "unset removed GREETING: empty expansion and no listing entry"
    );

    // A bare assignment line persists in the shell (POSIX, no command
    // name); echo sees it. unsetenv removes it again.
    console.console_io().push_input(b"FOO=scoped\n");
    booted.run(100);
    console.console_io().push_input(b"echo $FOO\n");
    booted.run(100);
    console.console_io().push_input(b"unsetenv FOO\nexport\n");
    booted.run(100);
    assert_eq!(
        console.console_io().output(),
        b"$ hello\n$ $ 1\n$ $ root:x:0:0:root:/root:/bin/sh\n$ one\ntwo\n$ hello world\n$ $?\n$ hello world\n$ a  b\n$ /bin /root\n$ $ bar\n$ $ hi there\n$ $ 2\n$ PATH=/bin\nHOME=/home/root\nFOO=bar\nGREETING=hi there\n$ $ hello\n$ fallback\n$ $ $ 127\n$ $ \n$ PATH=/bin\nHOME=/home/root\nFOO=bar\n$ $ scoped\n$ PATH=/bin\nHOME=/home/root\n$ ",
        "a bare assignment persisted for the next command; unsetenv removed it"
    );

    // Assignment-scoped PATH: `PATH=/usr/bin greet hi` resolves greet
    // through /usr/bin for that command only, while the shell's own PATH
    // stays /bin — the follow-up `greet` fails 127.
    console.console_io().push_input(b"PATH=/usr/bin greet hi\n");
    booted.run(100);
    console.console_io().push_input(b"greet\n");
    booted.run(100);
    assert_eq!(
        console.console_io().output(),
        b"$ hello\n$ $ 1\n$ $ root:x:0:0:root:/root:/bin/sh\n$ one\ntwo\n$ hello world\n$ $?\n$ hello world\n$ a  b\n$ /bin /root\n$ $ bar\n$ $ hi there\n$ $ 2\n$ PATH=/bin\nHOME=/home/root\nFOO=bar\nGREETING=hi there\n$ $ hello\n$ fallback\n$ $ $ 127\n$ $ \n$ PATH=/bin\nHOME=/home/root\nFOO=bar\n$ $ scoped\n$ PATH=/bin\nHOME=/home/root\n$ hi\n$ $ ",
        "an assignment scoped PATH to one command; the shell's PATH was untouched"
    );

    // grep filters pipeline output: a fixed substring match prints the
    // line; a no-match exits 1 (which becomes $?); `.` and `*` wildcards
    // match like a small glob.
    console.console_io().push_input(b"echo hello | grep ell\n");
    booted.run(100);
    console.console_io().push_input(b"echo hello | grep zzz\necho $?\n");
    booted.run(100);
    console.console_io().push_input(b"echo hello | grep 'h.llo'\n");
    booted.run(100);
    console.console_io().push_input(b"echo hello | grep 'h*o'\n");
    booted.run(100);
    assert_eq!(
        console.console_io().output(),
        b"$ hello\n$ $ 1\n$ $ root:x:0:0:root:/root:/bin/sh\n$ one\ntwo\n$ hello world\n$ $?\n$ hello world\n$ a  b\n$ /bin /root\n$ $ bar\n$ $ hi there\n$ $ 2\n$ PATH=/bin\nHOME=/home/root\nFOO=bar\nGREETING=hi there\n$ $ hello\n$ fallback\n$ $ $ 127\n$ $ \n$ PATH=/bin\nHOME=/home/root\nFOO=bar\n$ $ scoped\n$ PATH=/bin\nHOME=/home/root\n$ hi\n$ $ hello\n$ 1\n$ hello\n$ hello\n$ ",
        "grep filtered the pipeline: fixed match, no-match exit 1, dot and star wildcards"
    );

    // wc counts lines/words/bytes through the pipe — the count ends only
    // when EOF arrives, which pipe fd closure delivers once the writer
    // child exits and the shell drops its own pipe copies. The spaced
    // argument exercises the word-transition counting through the pipe,
    // and the file form shows the per-source line with its name.
    console.console_io().push_input(b"echo \"hello world\" | wc\n");
    booted.run(100);
    console.console_io().push_input(b"echo \"  spaced   out \" | wc\n");
    booted.run(100);
    console.console_io().push_input(b"wc /etc/passwd\n");
    booted.run(100);
    assert_eq!(
        console.console_io().output(),
        b"$ hello\n$ $ 1\n$ $ root:x:0:0:root:/root:/bin/sh\n$ one\ntwo\n$ hello world\n$ $?\n$ hello world\n$ a  b\n$ /bin /root\n$ $ bar\n$ $ hi there\n$ $ 2\n$ PATH=/bin\nHOME=/home/root\nFOO=bar\nGREETING=hi there\n$ $ hello\n$ fallback\n$ $ $ 127\n$ $ \n$ PATH=/bin\nHOME=/home/root\nFOO=bar\n$ $ scoped\n$ PATH=/bin\nHOME=/home/root\n$ hi\n$ $ hello\n$ 1\n$ hello\n$ hello\n$       1       2      12\n$       1       2      16\n$       1       1      30 /etc/passwd\n$ ",
        "wc counted through closed pipes and named files: 1/2/12, 1/2/16, and the passwd file"
    );

    // head terminates early: `cat big.txt | head -n 1` makes cat fill the
    // 4 KiB pipe and park on a full pipe; head prints the first line and
    // exits WITHOUT draining, so cat's next write wakes to EPIPE and exits
    // 2. The shell reaps both and `$?` is head's status (0) — the partial-
    // pipe read path end to end.
    console.console_io().push_input(b"cat /etc/big.txt | head -n 1\n");
    booted.run(100);
    console.console_io().push_input(b"echo $?\n");
    booted.run(100);
    // The file form with a line count: head reads the 31-byte passwd line
    // across two 16-byte chunks and stops at the count, not at EOF.
    console.console_io().push_input(b"head -n 2 /etc/passwd\n");
    booted.run(100);
    // tail is head's opposite — it must read to EOF to know the last
    // lines: the pipe's fd closure (cat exits, the shell drops its
    // copies) delivers EOF, and the window keeps the last 1 / 2 lines.
    console.console_io().push_input(b"cat /etc/passwd | tail -n 1\n");
    booted.run(100);
    console.console_io().push_input(b"cat /etc/big.txt | tail -n 2\n");
    booted.run(100);
    assert_eq!(
        console.console_io().output(),
        b"$ hello\n$ $ 1\n$ $ root:x:0:0:root:/root:/bin/sh\n$ one\ntwo\n$ hello world\n$ $?\n$ hello world\n$ a  b\n$ /bin /root\n$ $ bar\n$ $ hi there\n$ $ 2\n$ PATH=/bin\nHOME=/home/root\nFOO=bar\nGREETING=hi there\n$ $ hello\n$ fallback\n$ $ $ 127\n$ $ \n$ PATH=/bin\nHOME=/home/root\nFOO=bar\n$ $ scoped\n$ PATH=/bin\nHOME=/home/root\n$ hi\n$ $ hello\n$ 1\n$ hello\n$ hello\n$       1       2      12\n$       1       2      16\n$       1       1      30 /etc/passwd\n$ line-000\n$ 0\n$ root:x:0:0:root:/root:/bin/sh\n$ root:x:0:0:root:/root:/bin/sh\n$ line-498\nline-499\n$ ",
        "head exited early on the count (cat woke to EPIPE, $? stayed 0) and tail buffered the window to EOF"
    );

    // A multi-line chunk exercises the per-line extraction (each line
    // runs from just after the previous newline to this one): mixed.txt's
    // 22 bytes arrive as one chunk holding three complete lines, so grep
    // must print exactly `fig` and head must stop at exactly two.
    console.console_io().push_input(b"cat /etc/mixed.txt | grep fig\n");
    booted.run(100);
    console.console_io().push_input(b"cat /etc/mixed.txt | head -n 2\n");
    booted.run(100);
    // sort buffers everything to EOF, then emits in lexicographic order:
    // the file form sorts the four out-of-order lines, and a three-stage
    // cat | sort | head pipeline proves the accumulation across reads and
    // the sorted emit through a second pipe (head takes two lines and
    // leaves, so sort's next write wakes to EPIPE).
    console.console_io().push_input(b"sort /etc/mixed.txt\n");
    booted.run(100);
    console.console_io().push_input(b"cat /etc/big.txt | sort | head -n 2\n");
    booted.run(100);
    assert_eq!(
        console.console_io().output(),
        b"$ hello\n$ $ 1\n$ $ root:x:0:0:root:/root:/bin/sh\n$ one\ntwo\n$ hello world\n$ $?\n$ hello world\n$ a  b\n$ /bin /root\n$ $ bar\n$ $ hi there\n$ $ 2\n$ PATH=/bin\nHOME=/home/root\nFOO=bar\nGREETING=hi there\n$ $ hello\n$ fallback\n$ $ $ 127\n$ $ \n$ PATH=/bin\nHOME=/home/root\nFOO=bar\n$ $ scoped\n$ PATH=/bin\nHOME=/home/root\n$ hi\n$ $ hello\n$ 1\n$ hello\n$ hello\n$       1       2      12\n$       1       2      16\n$       1       1      30 /etc/passwd\n$ line-000\n$ 0\n$ root:x:0:0:root:/root:/bin/sh\n$ root:x:0:0:root:/root:/bin/sh\n$ line-498\nline-499\n$ fig\n$ pear\napple\n$ apple\ndate\nfig\npear\n$ line-000\nline-001\n$ ",
        "grep and head handled multi-line chunks, sort ordered the file lines and the three-stage pipeline's buffered output"
    );

    // seq is the pure producer: the three range forms print straight to
    // the console, `seq 10000 | head -n 3` fills the 4 KiB pipe and parks
    // before head takes three lines and leaves (seq's next write wakes to
    // EPIPE, `$?` is head's 0), and `seq 10000 | wc` proves the full
    // stream — every number reaches wc through repeated park/wake cycles
    // and the count emits only at seq's EOF.
    console.console_io().push_input(b"seq 5\n");
    booted.run(100);
    console.console_io().push_input(b"seq 3 5\n");
    booted.run(100);
    console.console_io().push_input(b"seq 2 2 8\n");
    booted.run(100);
    console.console_io().push_input(b"seq 10000 | head -n 3\n");
    booted.run(100);
    console.console_io().push_input(b"seq 10000 | wc\n");
    booted.run(100);
    assert_eq!(
        console.console_io().output(),
        b"$ hello\n$ $ 1\n$ $ root:x:0:0:root:/root:/bin/sh\n$ one\ntwo\n$ hello world\n$ $?\n$ hello world\n$ a  b\n$ /bin /root\n$ $ bar\n$ $ hi there\n$ $ 2\n$ PATH=/bin\nHOME=/home/root\nFOO=bar\nGREETING=hi there\n$ $ hello\n$ fallback\n$ $ $ 127\n$ $ \n$ PATH=/bin\nHOME=/home/root\nFOO=bar\n$ $ scoped\n$ PATH=/bin\nHOME=/home/root\n$ hi\n$ $ hello\n$ 1\n$ hello\n$ hello\n$       1       2      12\n$       1       2      16\n$       1       1      30 /etc/passwd\n$ line-000\n$ 0\n$ root:x:0:0:root:/root:/bin/sh\n$ root:x:0:0:root:/root:/bin/sh\n$ line-498\nline-499\n$ fig\n$ pear\napple\n$ apple\ndate\nfig\npear\n$ line-000\nline-001\n$ 1\n2\n3\n4\n5\n$ 3\n4\n5\n$ 2\n4\n6\n8\n$ 1\n2\n3\n$   10000   10000   48894\n$ ",
        "seq produced the three range forms, parked against head, and streamed all 10000 lines to wc"
    );

    client.kill_driver(0);
    t.join().unwrap();
}

/// `BootCaps::from_bib` resolves the initial caps by manifest name from a
/// kernel-filled Boot Info Block.
#[test]
fn from_bib_resolves_initial_caps() {
    let bib = build_bib(&[
        ("budget", CAP_MEM, 0x3, 0x1000_0000, 0x100_0000, 0),
        ("console", CAP_CHAN, 0x7, 0, 0, 1),
        ("ramdisk", CAP_CHAN, 0x7, 0, 0, 2),
    ]);
    let info = unsafe { BootInfo::from_raw(bib.as_ptr()) }.unwrap();
    let caps = BootCaps::from_bib(&info).unwrap();
    assert_eq!(caps.budget_slot, 0);
    assert_eq!(caps.budget_base, 0x1000_0000);
    assert_eq!(caps.budget_len, 0x100_0000);
    assert_eq!(caps.console_chan, Some(1));
    assert_eq!(caps.ramdisk_chan, 2);

    // A BIB missing the ramdisk channel must fail loudly.
    let bib = build_bib(&[("budget", CAP_MEM, 0x3, 0x1000_0000, 0x100_0000, 0)]);
    let info = unsafe { BootInfo::from_raw(bib.as_ptr()) }.unwrap();
    assert_eq!(
        BootCaps::from_bib(&info),
        Err(aerosls_sidecar::BootErr::MissingCap("ramdisk"))
    );
}

/// The manifest and the BIB are the same contract in two formats: the
/// manifest names the initial caps, the kernel builds the table in record
/// order, and the BIB reports it. Assert the names line up end to end.
#[test]
fn manifest_names_line_up_with_bib_caps() {
    use aerosls_proto::manifest::{
        build_manifest, parse_manifest, Budget, CapKind, Image, Limits, Manifest, ManifestCap,
        MAX_MANIFEST_CAPS,
    };
    let m = Manifest {
        version_major: 1,
        version_minor: 0,
        flags: 0,
        personality: Some("aerosls.posix.v1"),
        image: Some(Image {
            offset: 0x4000,
            size: 0xC000,
            entry: 0x4000,
        }),
        budget: Some(Budget {
            mem_bytes: 1 << 25,
            stack_bytes: 1 << 18,
            heap_initial: 1 << 23,
        }),
        cpu: None,
        limits: Some(Limits {
            max_tasks: 64,
            max_fds: 4096,
            max_channels: 128,
            max_open_files: 512,
            chan_queue_depth: 64,
        }),
        caps: [
            Some(ManifestCap {
                name: "budget",
                rights: 0x3,
                kind: CapKind::Mem {
                    base: 0x1000_0000,
                    size: 1 << 25,
                },
            }),
            Some(ManifestCap {
                name: "console",
                rights: 0x7,
                kind: CapKind::Chan {
                    peer: Some("kernel.debug.console"),
                    flags: 0,
                },
            }),
            Some(ManifestCap {
                name: "ramdisk",
                rights: 0x7,
                kind: CapKind::Chan {
                    peer: Some("drv.ramdisk.0"),
                    flags: 0,
                },
            }),
            None,
            None,
            None,
            None,
            None,
            None,
            None,
            None,
            None,
            None,
            None,
            None,
            None,
        ],
        n_caps: 3,
        bootstrap: None,
        flags_value: None,
        signature: None,
    };
    let blob = build_manifest(&m);
    let parsed = parse_manifest(&blob).unwrap();
    assert_eq!(parsed.personality, Some("aerosls.posix.v1"));

    // The kernel fills the table in record order; the BIB reports the same
    // order. Build the BIB from the manifest's caps and resolve them back.
    let slots: Vec<(String, u16, u16, u64, u64, u32)> = parsed
        .caps()
        .iter()
        .enumerate()
        .map(|(i, c)| {
            let c = c.unwrap();
            let (base, len) = match c.kind {
                CapKind::Mem { base, size } => (base, size),
                CapKind::Chan { .. } => (0, 0),
            };
            let ty = match c.kind {
                CapKind::Mem { .. } => CAP_MEM,
                CapKind::Chan { .. } => CAP_CHAN,
            };
            (c.name.to_string(), ty, c.rights, base, len, i as u32)
        })
        .collect();
    let bib = build_bib(
        &slots
            .iter()
            .map(|(n, ty, r, b, l, s)| (n.as_str(), *ty, *r, *b, *l, *s))
            .collect::<Vec<_>>(),
    );
    let info = unsafe { BootInfo::from_raw(bib.as_ptr()) }.unwrap();
    let caps = BootCaps::from_bib(&info).unwrap();
    assert_eq!(caps.budget_slot, 0);
    assert_eq!(caps.console_chan, Some(1));
    assert_eq!(caps.ramdisk_chan, 2);
    let _ = MAX_MANIFEST_CAPS;
}

/// Build a Boot Info Block with the given caps (name, ty, rights, base,
/// len, slot). Layout per `aerosls_proto::bootinfo`.
fn build_bib(caps: &[(&str, u16, u16, u64, u64, u32)]) -> Vec<u8> {
    const HEADER_LEN: usize = 32;
    let mut b = Vec::new();
    b.extend_from_slice(b"AERSLSB1");
    b.extend_from_slice(&1u16.to_le_bytes()); // version
    b.extend_from_slice(&(caps.len() as u16).to_le_bytes()); // cap_count
    b.extend_from_slice(&(1u64 << 20).to_le_bytes()); // budget_bytes
    b.extend_from_slice(&0x2000_0000u64.to_le_bytes()); // stack_top
    let total = HEADER_LEN
        + caps
            .iter()
            .map(|(n, _, _, _, _, _)| 2 + n.len() + 2 + 4 + 16)
            .sum::<usize>();
    b.extend_from_slice(&(total as u32).to_le_bytes()); // total_len
    for (name, ty, rights, base, len, slot) in caps {
        b.extend_from_slice(&(name.len() as u16).to_le_bytes());
        b.extend_from_slice(name.as_bytes());
        b.extend_from_slice(&(*slot as u16).to_le_bytes());
        b.push(*ty as u8);
        b.push(*rights as u8);
        b.extend_from_slice(&base.to_le_bytes());
        b.extend_from_slice(&len.to_le_bytes());
    }
    b
}
