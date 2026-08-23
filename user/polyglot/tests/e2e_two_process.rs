//! Two-process Polyglot e2e: three real OS processes on one real channel,
//! run over BOTH transports so the zero-copy gain is measured side by side.
//!
//! ```text
//!   sls-kerneld  (arena file + cap refcounts; message path is the choice)
//!      ▲ ring2/3 wasm alloc-free, ring4/5 lisp alloc-free     ▲
//!      │                                                    │
//!   wasm-sidecar (wasmi embedding)                    lisp-sidecar (real SBCL)
//!      │  shared arena mmap ◄─────────────────────────────►│
//!      └── transport: TCP via kerneld  OR  shared-memory ring (shm) ──┘
//! ```
//!
//! Each leg spawns the wasm + lisp sidecars with `--transport tcp` or
//! `--transport shm` and runs the same guest (add + sqrt_batch over a
//! 4096-element f64 array + reverse over a variable-length string, plus the
//! 1000-call add latency bench, the 100-call arena sqrt bench, and the
//! 100-call string reverse bench). In the `shm` leg the ENTIRE sidecar path is
//! shared memory: the request/reply frames travel ring0/ring1 (wasm↔lisp,
//! no kernel) and the arena alloc/free bookkeeping travels ring2/ring3
//! (wasm↔kerneld) and ring4/ring5 (lisp↔kerneld) — no TCP round trip
//! remains on the shm leg. The array data always travels through the
//! shared arena by MEM cap, never copied. Both legs must pass the latency
//! gate; the BENCH_JSON lines carry a `transport` field so CI archives
//! the two transports' medians separately.
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
    let stderr = child.stderr.take().unwrap();
    let (tx, rx) = mpsc::channel::<String>();
    let program = program.to_string();
    // Echo stdout lines to the test's stderr AND the marker channel.
    let program_out = program.clone();
    std::thread::spawn(move || {
        let reader = BufReader::new(stdout);
        for line in reader.lines() {
            if let Ok(l) = line {
                eprintln!("[{program_out}] {l}");
                if tx.send(l).is_err() {
                    break;
                }
            }
        }
    });
    // Echo stderr lines too (panics, guest logs) so a sidecar crash is
    // visible in the test output instead of vanishing into the pipe.
    std::thread::spawn(move || {
        let reader = BufReader::new(stderr);
        for line in reader.lines() {
            if let Ok(l) = line {
                eprintln!("[{program}:stderr] {l}");
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

/// Collect `count` lines containing `marker` (FIFO). Used for the T4-T8
/// sweep reports: the sidecar prints 5 per-size lines per method before
/// the verdict, so the gate needs all of them, not just the first.
fn wait_for_markers(
    rx: &mpsc::Receiver<String>,
    marker: &str,
    count: usize,
    timeout: Duration,
) -> Vec<String> {
    let mut found = Vec::new();
    let deadline = Instant::now() + timeout;
    while found.len() < count && Instant::now() < deadline {
        match rx.recv_timeout(Duration::from_millis(250)) {
            Ok(line) if line.contains(marker) => found.push(line),
            Ok(_) => {}
            Err(mpsc::RecvTimeoutError::Timeout) => {}
            Err(_) => break,
        }
    }
    found
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

/// Parse `median_ns=<u64>` out of a BENCH line emitted by the wasm-sidecar.
fn parse_median_ns(line: &str) -> Option<u64> {
    parse_field(line, "median_ns=")
}

/// Parse `key=<u64>` out of a BENCH line (key includes the trailing '=').
fn parse_field(line: &str, key: &str) -> Option<u64> {
    line.split(key)
        .nth(1)?
        .split_whitespace()
        .next()?
        .parse()
        .ok()
}

/// Run one leg: spawn the Lisp sidecar (TRANSPORT env) + the Wasm sidecar
/// (--transport), assert the latency gate on both bench legs, tear the Lisp
/// sidecar down. Returns Err on any failure (the caller panics with context).
fn run_leg(
    transport: &str,
    port: u16,
    arena_path: &str,
    chan_path: &str,
    lisp_script: &str,
    calc_lisp: &str,
    wasm_bin: &str,
    max_median_ns: u64,
) -> Result<(), String> {
    let port_s = port.to_string();
    let mut lisp_envs: Vec<(&str, &str)> = vec![
        ("KERNEL_PORT", &port_s),
        ("ARENA_PATH", arena_path),
        ("CALC_LISP_PATH", calc_lisp),
        ("TRANSPORT", transport),
    ];
    if transport == "shm" {
        lisp_envs.push(("CHAN_PATH", chan_path));
    }
    // NB: `--noinform --non-interactive --script` together swallow stdout in
    // some SBCL builds — plain `--script` is what actually prints.
    let (mut lisp, lisp_out) = spawn_linux("sbcl", &["--script", lisp_script], &lisp_envs);
    if wait_for_marker(&lisp_out, "LISP READY", Duration::from_secs(20)).is_none() {
        kill_child(&mut lisp);
        return Err("Lisp sidecar did not become ready".to_string());
    }

    let mut wasm_args = vec!["--port", &port_s, "--arena", arena_path, "--transport", transport];
    if transport == "shm" {
        wasm_args.push("--chan");
        wasm_args.push(chan_path);
    }
    let (mut wasm, wasm_out) = spawn_linux(wasm_bin, &wasm_args, &[]);
    // The sidecar prints the rdtsc latency reports first, then the verdict.
    // The channel is FIFO, so consume all six BENCH lines before
    // WASM_SIDECAR (the three baseline legs print before the three
    // cross-sidecar legs, both before the verdict).
    let bench_local = wait_for_marker(&wasm_out, "BENCH_LOCAL", Duration::from_secs(60));
    let bench_pipe = wait_for_marker(&wasm_out, "BENCH_PIPE", Duration::from_secs(60));
    let bench_sock = wait_for_marker(&wasm_out, "BENCH_SOCK", Duration::from_secs(60));
    let bench_add = wait_for_marker(&wasm_out, "BENCH_ADD", Duration::from_secs(60));
    let bench_sqrt = wait_for_marker(&wasm_out, "BENCH_SQRT", Duration::from_secs(60));
    let bench_str = wait_for_marker(&wasm_out, "BENCH_STR", Duration::from_secs(60));
    // T4-T8 payload-size sweep: 5 per-size lines per method, printed after
    // BENCH_STR and before WASM_SIDECAR (FIFO).
    let sweep_sqrt = wait_for_markers(&wasm_out, "BENCH_SWEEP_SQRT:", 5, Duration::from_secs(120));
    let sweep_str = wait_for_markers(&wasm_out, "BENCH_SWEEP_STR:", 5, Duration::from_secs(60));
    let wasm_line = wait_for_marker(&wasm_out, "WASM_SIDECAR", Duration::from_secs(30));
    let async_line = wait_for_marker(&wasm_out, "ASYNC_SIDECAR", Duration::from_secs(10));
    let status = wasm.wait().expect("wasm exit");
    let Some(wasm_line) = wasm_line else {
        kill_child(&mut lisp);
        return Err("wasm-sidecar produced no verdict".to_string());
    };
    if !(status.success() && wasm_line.contains("PASS")) {
        kill_child(&mut lisp);
        return Err(format!("wasm-sidecar failed: {wasm_line} (exit {status})"));
    }
    // T13/T14 async gate: on the shared-ring leg the guest MUST have run the
    // heavy_reduce async section and received the verified result off the
    // dedicated result ring. The sidecar prints ASYNC_SIDECAR only on shm;
    // on tcp there is no result ring (async is skipped there by design).
    if transport == "shm" {
        let Some(async_line) = async_line else {
            kill_child(&mut lisp);
            return Err("shm leg: no ASYNC_SIDECAR verdict — async path skipped or failed".to_string());
        };
        if !async_line.contains("PASS") {
            kill_child(&mut lisp);
            return Err(format!("shm leg async gate failed: {async_line}"));
        }
        eprintln!("[gate] ASYNC_SIDECAR: heavy_reduce result received+verified on result ring (shm) — OK");
    } else {
        eprintln!("[gate] ASYNC_SIDECAR skipped on tcp (no result ring by design) — OK");
    }

    // ── payload-size sweep gate (T4-T8) ─────────────────────────────────
    // The sweep proves the zero-copy claim: the arena bytes never cross the
    // transport, so per-size latency must scale with the *Lisp compute*
    // (which grows with payload), not with a wire copy. Three checks:
    //   (1) all 10 per-size lines (5 sqrt + 5 str) were produced;
    //   (2) workload proof: compute_ns(1MiB) >= 10x compute_ns(4KiB) — the
    //       payload really grew and was processed (a count=0 or
    //       constant-payload regression collapses this ratio to ~1x);
    //   (3) coarse per-size median cap (40ms) — the live medians are
    //       0.07-19ms, so this catches a catastrophic blowup while the
    //       per-size drift (archived via BENCH_JSON legs like sqrt-1024k)
    //       soft-warns on slower degradations.
    if sweep_sqrt.len() != 5 || sweep_str.len() != 5 {
        kill_child(&mut lisp);
        return Err(format!(
            "T4-T8 sweep incomplete: got {} sqrt + {} str lines (need 5 each) (transport={transport})",
            sweep_sqrt.len(),
            sweep_str.len()
        ));
    }
    for (name, lines) in [("SQRT", &sweep_sqrt), ("STR", &sweep_str)] {
        let compute_4k = parse_field(&lines[0], "compute_ns=").unwrap_or(0);
        let compute_1m = parse_field(&lines[4], "compute_ns=").unwrap_or(0);
        if compute_4k == 0 || compute_1m < compute_4k * 10 {
            kill_child(&mut lisp);
            return Err(format!(
                "T4-T8 {name} sweep workload is dead: compute_ns 4KiB={compute_4k} 1MiB={compute_1m} — \
                 the payload never grew (count=0 or constant-payload regression) (transport={transport})"
            ));
        }
        for (i, line) in lines.iter().enumerate() {
            let median_ns = parse_median_ns(line).unwrap_or(0);
            // Catastrophe cap, size-aware: legit compute grows with the
            // payload (a 1MiB SBCL byte-reversal measures tens of ms on
            // cold arena pages), so a flat cap calibrated to the small
            // sizes would false-positive on the largest. Cap each size at
            // max(40ms, 8x its own measured compute + 10ms): a 4KiB
            // payload taking 40ms is a catastrophe, while a 1MiB payload
            // at 62ms compute gets ~0.5s headroom. A transport regression
            // that stalls per-frame still blows the ADD/SQRT/STR median
            // gates (5ms over 1000 samples) long before it hides here.
            let compute_i = parse_field(line, "compute_ns=").unwrap_or(0);
            let cap = 40_000_000u64.max(compute_i * 8 + 10_000_000);
            if median_ns > cap {
                kill_child(&mut lisp);
                return Err(format!(
                    "T4-T8 {name} sweep size[{}] median {median_ns} ns > {cap} ns catastrophe cap (compute_ns={compute_i}, transport={transport})",
                    i
                ));
            }
        }
        eprintln!(
            "[gate] T4-T8 {name} sweep: compute scales {compute_4k} -> {compute_1m} ns (1MiB/4KiB), all sizes within their size-aware caps — OK (transport={transport})"
        );
    }

    // ── baseline gate (once per invocation, transport-independent) ───────
    // The three baseline legs answer "how much faster is a cross-sidecar
    // call than ordinary IPC": a local call is ~ns, a pipe ~1-5us, a Unix
    // socketpair ~2-10us on any modern core. Gate them with wide headroom
    // (local 1000x, pipe/socket 100x) so a host where the kernel IPC path
    // went catastrophically wrong fails the build, while CI noise never
    // trips it.
    for (name, bench, max) in [
        ("BENCH_LOCAL", &bench_local, 10_000),
        ("BENCH_PIPE", &bench_pipe, 500_000),
        ("BENCH_SOCK", &bench_sock, 500_000),
    ] {
        let Some(line) = bench else {
            kill_child(&mut lisp);
            return Err(format!("wasm-sidecar produced no {name} baseline report (transport={transport})"));
        };
        let median_ns = parse_median_ns(line)
            .unwrap_or_else(|| panic!("{name} line missing median_ns: {line}"));
        if median_ns > max {
            kill_child(&mut lisp);
            return Err(format!(
                "{name} baseline median {median_ns} ns exceeds the {max} ns threshold — \
                 the local IPC path is broken (transport={transport})"
            ));
        }
        eprintln!("[gate] {name} median {median_ns} ns <= {max} ns baseline threshold — OK");
    }

    // ── latency regression gate (per transport leg) ──────────────────────
    // Fail if either bench leg's median round trip exceeds the threshold.
    // Measured medians are ~0.7ms (add) and ~1.5ms (sqrt) over TCP; the
    // shared-ring leg is ~200x faster on add (~3us) and ~9x faster on sqrt
    // (~175us, still dominated by 4096 real SBCL sqrts) — all sidecar
    // traffic, message + arena bookkeeping, is shared memory. 5ms default
    // catches an order-of-magnitude transport regression with wide headroom.
    for (name, bench) in [
        ("BENCH_ADD", &bench_add),
        ("BENCH_SQRT", &bench_sqrt),
        ("BENCH_STR", &bench_str),
    ] {
        let Some(line) = bench else {
            kill_child(&mut lisp);
            return Err(format!("wasm-sidecar produced no {name} latency report (transport={transport})"));
        };
        let median_ns = parse_median_ns(line)
            .unwrap_or_else(|| panic!("{name} line missing median_ns: {line}"));
        if median_ns > max_median_ns {
            kill_child(&mut lisp);
            return Err(format!(
                "{name} median {median_ns} ns exceeds the {max_median_ns} ns regression threshold \
                 (transport={transport}) — round-trip latency exploded"
            ));
        }
        eprintln!(
            "[gate] {name} median {median_ns} ns <= {max_median_ns} ns threshold (transport={transport}) — OK"
        );

        if name == "BENCH_ADD" || name == "BENCH_SQRT" || name == "BENCH_STR" {
            // ── compute/transport split guard ──────────────────────────────
            // These legs report total = compute (Lisp-side work, timed on
            // the Lisp side and carried in the reply) + transport (derived
            // as total - compute). A dead or stale split shows compute_ns=0
            // (the Lisp timing or its wire field broke) or transport_ns=0
            // (the Lisp clock runs ahead of the wasm calibration). Both
            // must be live; the floors sit far below the live medians:
            // sqrt's 4096 sqrts cannot finish in <10us, reverse's 32..63 B
            // byte loop (with cold arena page faults) measures ~9us on shm
            // and cannot finish in <1us, and add's timed 2048-iteration sum
            // measures ~3-5us on shm and cannot finish in <1us. The message
            // + arena bookkeeping can never be zero.
            let min_compute = match name {
                "BENCH_SQRT" => 10_000,
                _ => 1_000,
            };
            let compute_ns = parse_field(line, "compute_ns=")
                .unwrap_or_else(|| panic!("{name} line missing compute_ns: {line}"));
            let transport_ns = parse_field(line, "transport_ns=")
                .unwrap_or_else(|| panic!("{name} line missing transport_ns: {line}"));
            if compute_ns < min_compute || transport_ns == 0 {
                kill_child(&mut lisp);
                return Err(format!(
                    "{name} compute/transport split is dead: compute_ns={compute_ns} transport_ns={transport_ns} total={median_ns} (transport={transport}) — the Lisp-side timing or its wire path broke"
                ));
            }
            eprintln!(
                "[gate] {name} split: compute {compute_ns} ns + transport {transport_ns} ns (total {median_ns} ns, transport={transport}) — OK"
            );
        }
    }

    kill_child(&mut lisp);
    eprintln!("leg {transport}: wasm(wasmi) -> lisp(SBCL) -> wasm PASS (arena={arena_path})");
    Ok(())
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
    let (mut kerneld, kerneld_out) = spawn_linux(
        &kerneld_bin,
        // 64 MB: the design-doc arena size AND the bitmap capacity of the
        // real shared-arena allocator (aerosls-shared-arena tracks 16384
        // pages x 4 KiB = 64 MiB). sls-kerneld now drives that allocator, so
        // pages are reclaimed at refcount 0: the T4-T8 sweep (up to 1 MiB
        // arena buffers per call, allocated+freed per iteration across BOTH
        // legs on the same kerneld) only fits in 64 MiB if reclamation
        // actually works — passing this e2e IS the reclamation proof. (The
        // old bump cursor never reclaimed, which is why it needed 256 MB.)
        &["--port", &port.to_string(), "--arena-size", "64"],
        &[],
    );
    let Some(ready) = wait_for_marker(&kerneld_out, "READY arena=", Duration::from_secs(15)) else {
        kill_child(&mut kerneld);
        panic!("sls-kerneld did not become READY");
    };
    // The READY line is `READY arena=PATH chan=PATH` — split each value at
    // the next space so one marker's value can never swallow the other's.
    let field = |marker: &str| -> String {
        ready
            .split_once(marker)
            .map(|(_, p)| {
                p.split_whitespace()
                    .next()
                    .unwrap_or("")
                    .to_string()
            })
            .filter(|v| !v.is_empty())
            .unwrap_or_else(|| panic!("READY line missing {marker}: {ready}"))
    };
    let arena_path = field("arena=");
    let chan_path = field("chan=");

    // ── latency regression gate (shared by both legs) ────────────────────
    let max_median_ns: u64 = std::env::var("POLYGLOT_BENCH_MEDIAN_NS")
        .ok()
        .and_then(|v| v.parse().ok())
        .unwrap_or(5_000_000);

    // ── run both transports and require both to pass ─────────────────────
    let mut failures = Vec::new();
    for transport in ["tcp", "shm"] {
        match run_leg(
            transport,
            port,
            &arena_path,
            &chan_path,
            &lisp_script,
            &calc_lisp,
            &wasm_bin,
            max_median_ns,
        ) {
            Ok(()) => {}
            Err(e) => failures.push(format!("[{transport}] {e}")),
        }
    }

    // ── teardown ─────────────────────────────────────────────────────────
    kill_child(&mut kerneld);
    // Sweep stragglers (WSL does not always propagate SIGKILL to Linux
    // children when the wsl.exe handle dies).
    let sweep = format!(
        "pkill -f 'sls-kerneld --port {port}' 2>/dev/null; pkill -f lisp_sidecar.lisp 2>/dev/null; true"
    );
    let _ = linux_sh(&sweep).status();

    if !failures.is_empty() {
        panic!("polyglot e2e failed:\n{}", failures.join("\n"));
    }
    eprintln!(
        "e2e PASS: wasm(wasmi) -> lisp(SBCL) -> wasm over tcp + shm transports, arena={arena_path}"
    );
}
