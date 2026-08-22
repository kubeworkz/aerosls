//! Two-process Polyglot e2e: three real OS processes on one real channel.
//!
//! ```text
//!   sls-kerneld  (arena file + cap refcounts + channel queues, TCP)
//!      ▲ 302/303/304 wire ABI                ▲
//!      │                                      │
//!   wasm-sidecar (wasmi embedding)      lisp-sidecar (real SBCL)
//!      │  shared arena mmap ◄───────────────►│
//! ```
//!
//! The Wasm sidecar's guest module drives two CalculatorService calls
//! (add + sqrt_batch over a 4096-element f64 array) into the Lisp sidecar,
//! which runs the generated `calculator.lisp` dispatch. The array travels
//! through the shared arena by MEM cap — never across the wire.
//!
//! Prerequisites (skipped with a clear message when absent):
//!   - Linux binaries in `POLYGLOT_TARGET_DIR` (built by run_e2e.sh via WSL)
//!   - a `sbcl` reachable from the Linux side (WSL on Windows, PATH on Linux)

use std::io::{BufRead, BufReader};
use std::net::TcpListener;
use std::process::{Child, Command, Stdio};
use std::sync::mpsc;
use std::time::{Duration, Instant};

fn repo_root() -> String {
    let manifest = env!("CARGO_MANIFEST_DIR");
    std::path::Path::new(manifest)
        .parent()
        .and_then(|p| p.parent())
        .unwrap()
        .to_str()
        .unwrap()
        .to_string()
}

/// The repo root as the Linux side sees it (WSL path on Windows, plain on Linux).
fn repo_root_linux() -> String {
    let root = repo_root();
    #[cfg(windows)]
    {
        let s = root.replace('\\', "/");
        let drive = s.chars().next().unwrap().to_ascii_lowercase();
        format!("/mnt/{drive}{}", &s[2..])
    }
    #[cfg(not(windows))]
    {
        root
    }
}

fn using_wsl() -> bool {
    #[cfg(windows)]
    {
        Command::new("wsl")
            .arg("--status")
            .stdout(Stdio::null())
            .stderr(Stdio::null())
            .status()
            .map(|s| s.success())
            .unwrap_or(false)
    }
    #[cfg(not(windows))]
    {
        false
    }
}

/// A command that runs `script` in the Linux environment (bash -lc on WSL,
/// plain bash on native Linux).
fn linux_sh(script: &str) -> Command {
    if using_wsl() {
        let mut c = Command::new("wsl");
        c.arg("-e").arg("bash").arg("-lc").arg(script).arg("sh");
        c
    } else {
        let mut c = Command::new("bash");
        c.arg("-lc").arg(script);
        c
    }
}

/// Spawn a Linux program, echoing its stdout lines to stderr and a channel.
fn spawn_linux(program: &str, args: &[&str], envs: &[(&str, &str)]) -> (Child, mpsc::Receiver<String>) {
    let mut c = if using_wsl() {
        let script = format!("exec {program} {}", args.join(" "));
        let mut c = Command::new("wsl");
        c.arg("-e").arg("bash").arg("-lc").arg(&script).arg("sh");
        c
    } else {
        let mut c = Command::new(program);
        c.args(args);
        c
    };
    for (k, v) in envs {
        c.env(k, v);
    }
    c.stdout(Stdio::piped()).stderr(Stdio::piped());
    let mut child = c.spawn().expect("spawn");
    let stdout = child.stdout.take().unwrap();
    let (tx, rx) = mpsc::channel::<String>();
    let program = program.to_string();
    std::thread::spawn(move || {
        let reader = BufReader::new(stdout);
        for line in reader.lines() {
            if let Ok(l) = line {
                eprintln!("[{program}] {l}");
                if tx.send(l).is_err() {
                    break;
                }
            }
        }
    });
    (child, rx)
}

fn wait_for_marker(rx: &mpsc::Receiver<String>, marker: &str, timeout: Duration) -> Option<String> {
    let deadline = Instant::now() + timeout;
    while Instant::now() < deadline {
        match rx.recv_timeout(Duration::from_millis(250)) {
            Ok(line) if line.contains(marker) => return Some(line),
            Ok(_) => {}
            Err(mpsc::RecvTimeoutError::Timeout) => {}
            Err(_) => break,
        }
    }
    None
}

fn free_port() -> u16 {
    TcpListener::bind(("127.0.0.1", 0))
        .unwrap()
        .local_addr()
        .unwrap()
        .port()
}

fn kill_child(child: &mut Child) {
    let _ = child.kill();
    let _ = child.wait();
}

#[test]
fn two_process_wasm_lisp_arena_roundtrip() {
    let root_linux = repo_root_linux();

    // ── prerequisites ────────────────────────────────────────────────────
    let target_dir = std::env::var("POLYGLOT_TARGET_DIR")
        .unwrap_or_else(|_| "/tmp/polyglot-target".to_string());
    let kerneld_bin = format!("{target_dir}/release/sls-kerneld");
    let wasm_bin = format!("{target_dir}/release/wasm-sidecar");
    let lisp_script = format!("{root_linux}/user/polyglot/lisp/lisp_sidecar.lisp");
    let calc_lisp = format!("{root_linux}/tools/aeroidl/gen/calculator.lisp");

    let probe = |script: &str| -> bool {
        linux_sh(script)
            .output()
            .map(|o| String::from_utf8_lossy(&o.stdout).contains("_OK"))
            .unwrap_or(false)
    };
    let have_sbcl = probe("command -v sbcl >/dev/null 2>&1 && echo SBCL_OK || echo NO_SBCL");
    let bins_exist = probe(&format!(
        "test -x {kerneld_bin} && test -x {wasm_bin} && echo BINS_OK || echo NO_BINS"
    ));

    if !using_wsl() && std::env::consts::OS != "linux" {
        eprintln!("SKIP: this e2e needs a Linux environment (WSL on Windows, native on CI)");
        return;
    }
    if !have_sbcl {
        eprintln!("SKIP: no sbcl reachable from the Linux side (apt install sbcl)");
        return;
    }
    if !bins_exist {
        eprintln!(
            "SKIP: Linux binaries not found in {target_dir}/release — run user/polyglot/run_e2e.sh to build them"
        );
        return;
    }

    // ── launch the kernel transport ──────────────────────────────────────
    let port = free_port();
    let (mut kerneld, kerneld_out) = spawn_linux(&kerneld_bin, &["--port", &port.to_string()], &[]);
    let Some(ready) = wait_for_marker(&kerneld_out, "READY arena=", Duration::from_secs(15)) else {
        kill_child(&mut kerneld);
        panic!("sls-kerneld did not become READY");
    };
    let arena_path = ready
        .split_once("arena=")
        .map(|(_, p)| p.trim().to_string())
        .expect("READY line carries arena path");

    // ── launch the Lisp sidecar (real SBCL) ─────────────────────────────
    let port_s = port.to_string();
    let lisp_envs: Vec<(&str, &str)> = vec![
        ("KERNEL_PORT", &port_s),
        ("ARENA_PATH", &arena_path),
        ("CALC_LISP_PATH", &calc_lisp),
    ];
    // NB: `--noinform --non-interactive --script` together swallow stdout in
    // some SBCL builds — plain `--script` is what actually prints.
    let (mut lisp, lisp_out) = spawn_linux("sbcl", &["--script", &lisp_script], &lisp_envs);
    if wait_for_marker(&lisp_out, "LISP READY", Duration::from_secs(20)).is_none() {
        kill_child(&mut kerneld);
        kill_child(&mut lisp);
        panic!("Lisp sidecar did not become ready");
    }

    // ── run the Wasm sidecar (wasmi embedding) ──────────────────────────
    let (mut wasm, wasm_out) = spawn_linux(
        &wasm_bin,
        &["--port", &port.to_string(), "--arena", &arena_path],
        &[],
    );
    // The sidecar prints the rdtsc latency reports first, then the verdict.
    // The channel is FIFO, so consume both BENCH lines before WASM_SIDECAR.
    let bench_add = wait_for_marker(&wasm_out, "BENCH_ADD", Duration::from_secs(60));
    let bench_sqrt = wait_for_marker(&wasm_out, "BENCH_SQRT", Duration::from_secs(60));
    let wasm_line = wait_for_marker(&wasm_out, "WASM_SIDECAR", Duration::from_secs(30));
    let status = wasm.wait().expect("wasm exit");
    let Some(wasm_line) = wasm_line else {
        kill_child(&mut kerneld);
        kill_child(&mut lisp);
        panic!("wasm-sidecar produced no verdict");
    };
    assert!(
        status.success() && wasm_line.contains("PASS"),
        "wasm-sidecar failed: {wasm_line} (exit {status})"
    );
    assert!(
        bench_add.is_some(),
        "wasm-sidecar produced no BENCH_ADD latency report"
    );
    assert!(
        bench_sqrt.is_some(),
        "wasm-sidecar produced no BENCH_SQRT latency report"
    );

    // ── teardown ─────────────────────────────────────────────────────────
    kill_child(&mut lisp);
    kill_child(&mut kerneld);
    // Sweep stragglers (WSL does not always propagate SIGKILL to Linux
    // children when the wsl.exe handle dies).
    let sweep = format!(
        "pkill -f 'sls-kerneld --port {port}' 2>/dev/null; pkill -f lisp_sidecar.lisp 2>/dev/null; true"
    );
    let _ = linux_sh(&sweep).status();

    eprintln!(
        "e2e PASS: wasm(wasmi) -> lisp(SBCL) -> wasm over syscalls 302-304, arena={arena_path}"
    );
}
