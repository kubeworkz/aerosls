//! aerobackup's host test — the retention prune, over a real filesystem with
//! real mtimes, plus the two things that make the prune trustworthy: that
//! retention reads mtimes and not names, and that a `.partial` copy is never
//! treated as a backup.
//!
//! ─── Why this is a real-filesystem test and not a unit test ────────────────
//! `plan_prune` is pure and unit-tested in `src/lib.rs`. What those tests cannot
//! prove is that `run()` wires it to the directory at all — the classic failure
//! of a beautifully tested decision function that nothing calls. Every test here
//! goes through `run()` or through the built binary, so deleting the call to
//! `plan_prune` from `run()` reddens this file and nothing else has to.
//!
//! ─── And why the binary is invoked, not just the library ───────────────────
//! aerobackup is E7's designated census target: the artifact is a `strace` of
//! *this executable*. A test that only exercises the library would leave the
//! entry point, the flag precedence and the exit codes unmeasured — the parts
//! the census and the cron line actually depend on.

use std::env;
use std::fs;
use std::io::{Read, Seek, SeekFrom, Write};
use std::path::{Path, PathBuf};
use std::process::Command;
use std::sync::atomic::{AtomicUsize, Ordering};
use std::time::{Duration, SystemTime, UNIX_EPOCH};

use aerobackup::{copy_sparse, list_backups, plan_prune, run, Config, Kind};

// ─── A temp directory, with no dependency to provide one ───────────────────
struct TempDir(PathBuf);

impl TempDir {
    fn new(label: &str) -> TempDir {
        static N: AtomicUsize = AtomicUsize::new(0);
        let n = N.fetch_add(1, Ordering::Relaxed);
        let p = env::temp_dir().join(format!("aerobackup-{}-{}-{}", label, std::process::id(), n));
        let _ = fs::remove_dir_all(&p);
        fs::create_dir_all(&p).expect("create temp dir");
        TempDir(p)
    }

    fn path(&self) -> &Path {
        &self.0
    }
}

impl Drop for TempDir {
    fn drop(&mut self) {
        let _ = fs::remove_dir_all(&self.0);
    }
}

/// A file in `dir` whose mtime is `mtime_secs` since the epoch. The mtime is the
/// input the retention rule reads, so it has to be set rather than hoped for.
fn stamp(dir: &Path, name: &str, mtime_secs: u64) -> PathBuf {
    fs::create_dir_all(dir).expect("create tier");
    let p = dir.join(name);
    fs::write(&p, b"x").expect("write");
    // Opened for WRITE rather than read: set_modified needs the write-attributes
    // permission on Windows, and a test that only runs on the Linux dev box is
    // a test that stops being run.
    fs::OpenOptions::new()
        .write(true)
        .open(&p)
        .expect("open for set_modified")
        .set_modified(UNIX_EPOCH + Duration::from_secs(mtime_secs))
        .expect("set mtime");
    p
}

fn names(v: &[PathBuf]) -> Vec<String> {
    let mut s: Vec<String> = v
        .iter()
        .map(|p| p.file_name().unwrap().to_string_lossy().into_owned())
        .collect();
    s.sort();
    s
}

// ─── The prune ─────────────────────────────────────────────────────────────
#[test]
fn run_prunes_to_the_retention_count_and_keeps_the_backup_it_just_took() {
    let td = TempDir::new("prune");
    let tier = td.path().join("hourly");
    // Four old backups whose NAMES run opposite to their MTIMES, so a prune
    // that secretly ordered by name passes the count assertion and fails the
    // survival assertion below.
    stamp(&tier, "sls_storage-19700101-000000.img", 100);
    stamp(&tier, "sls_storage-20000229-000000.img", 500); // newest of the old
    stamp(&tier, "sls_storage-20240101-000000.img", 300);
    stamp(&tier, "sls_storage-20990101-000000.img", 200);
    // A stale in-flight copy. Nothing may prune it, and it must not count
    // against the retention budget.
    stamp(&tier, "sls_storage-20200101-000000.img.partial", 400);

    let storage = td.path().join("sls_storage.img");
    fs::write(&storage, b"payload").unwrap();

    let cfg = Config {
        storage: storage.clone(),
        backup_dir: td.path().to_path_buf(),
        kind: Kind::Hourly,
        retain: 2,
        now_unix: Some(1_700_000_000),
        dry_run: false,
    };
    let r = run(&cfg).expect("run");

    assert!(r.dest.exists(), "the backup just taken must exist");
    assert_eq!(r.pruned.len(), 3, "4 old backups, keep 2, so 3 go");
    assert_eq!(r.kept, 2);
    assert_eq!(
        names(&r.pruned),
        vec![
            "sls_storage-19700101-000000.img", // mtime 100
            "sls_storage-20240101-000000.img", // mtime 300
            "sls_storage-20990101-000000.img", // mtime 200 (newest name, 3rd mtime)
        ]
    );
    for p in &r.pruned {
        assert!(!p.exists(), "{} should be gone", p.display());
    }
    // The survivor that is not this run's backup, by mtime.
    assert!(tier.join("sls_storage-20000229-000000.img").exists());
    // The in-flight copy was neither pruned nor counted.
    assert!(
        tier.join("sls_storage-20200101-000000.img.partial").exists(),
        "a .partial file is not a backup and must not be pruned by the retention rule"
    );
    // And the only files left in the tier are: the new backup, the newest old
    // one, and the .partial. Three, exactly.
    let mut left: Vec<String> = fs::read_dir(&tier)
        .unwrap()
        .map(|e| e.unwrap().file_name().to_string_lossy().into_owned())
        .collect();
    left.sort();
    assert_eq!(left.len(), 3, "tier contents: {left:?}");
}

#[test]
fn the_copy_lands_under_the_final_name_only_after_it_is_complete() {
    // The invariant the module docs claim: a `.img` name is always a complete
    // file, because the copy is written to `.img.partial` and renamed. If a
    // future refactor copies straight to `dest`, a crash mid-copy leaves a
    // truncated file that retention will keep in preference to a good, older
    // backup. This asserts the observable half of that: after a successful run
    // there is no `.partial` left anywhere in the tier.
    let td = TempDir::new("partial");
    let storage = td.path().join("sls_storage.img");
    fs::write(&storage, vec![7u8; 128 * 1024]).unwrap();
    let cfg = Config {
        storage,
        backup_dir: td.path().to_path_buf(),
        kind: Kind::Hourly,
        retain: 4,
        now_unix: Some(1_700_000_000),
        dry_run: false,
    };
    let r = run(&cfg).expect("run");
    let leftovers: Vec<String> = fs::read_dir(td.path().join("hourly"))
        .unwrap()
        .map(|e| e.unwrap().file_name().to_string_lossy().into_owned())
        .filter(|n| n.ends_with(".partial"))
        .collect();
    assert!(leftovers.is_empty(), "leftover partials: {leftovers:?}");
    assert_eq!(fs::metadata(&r.dest).unwrap().len(), 128 * 1024);
}

#[test]
fn list_backups_sees_only_backups_and_the_prune_agrees_with_it() {
    // The filter and the decision have to be the same opinion: `run()` prunes
    // exactly what `list_backups()` returned, so a file the filter hides can
    // never be pruned and a file it shows can never be ignored.
    let td = TempDir::new("filter");
    let tier = td.path().join("daily");
    stamp(&tier, "sls_storage-20240101-000000.img", 10);
    stamp(&tier, "sls_storage-20240102-000000.img", 20);
    stamp(&tier, "sls_storage-20240103-000000.img.partial", 30);
    fs::write(tier.join("README.txt"), b"not a backup").unwrap();

    let listed = list_backups(&tier).unwrap();
    assert_eq!(listed.len(), 2, "listed: {listed:?}");
    assert_eq!(plan_prune(&listed, 1).len(), 1);
}

#[test]
fn a_missing_tier_directory_is_empty_rather_than_an_error() {
    let td = TempDir::new("missing");
    assert!(list_backups(&td.path().join("nothing-here"))
        .unwrap()
        .is_empty());
}

#[test]
fn a_missing_storage_image_fails_before_anything_is_created() {
    let td = TempDir::new("nostorage");
    let cfg = Config {
        storage: td.path().join("absent.img"),
        backup_dir: td.path().to_path_buf(),
        kind: Kind::Hourly,
        retain: 4,
        now_unix: Some(1_700_000_000),
        dry_run: false,
    };
    assert!(run(&cfg).is_err());
    assert!(
        !td.path().join("hourly").exists(),
        "a failed backup must not leave an empty tier behind"
    );
}

// ─── The copy ──────────────────────────────────────────────────────────────
#[test]
fn the_copy_preserves_length_and_turns_zero_runs_into_holes() {
    const HOLE_AT: u64 = 2 * 1024 * 1024;
    let td = TempDir::new("sparse");
    let src = td.path().join("src.img");
    let dst = td.path().join("dst.img");

    let mut f = fs::File::create(&src).unwrap();
    f.write_all(&[0xAB; 4096]).unwrap();
    f.seek(SeekFrom::Start(HOLE_AT)).unwrap();
    f.write_all(&[0xCD; 4096]).unwrap();
    drop(f);
    let expected_len = HOLE_AT + 4096;

    let stats = copy_sparse(&src, &dst).unwrap();
    assert_eq!(stats.bytes, expected_len, "the copy must be the whole source");
    assert_eq!(fs::metadata(&dst).unwrap().len(), expected_len);
    assert!(stats.holes >= 1, "the zero run should have become holes");
    assert!(
        stats.written * 4 < stats.bytes,
        "wrote {} of {} bytes — the copy is not sparse",
        stats.written,
        stats.bytes
    );

    // Content, not just length: a hole that lost a byte and a header that
    // gained one would still pass the size assertion.
    let mut a = Vec::new();
    fs::File::open(&src).unwrap().read_to_end(&mut a).unwrap();
    let mut b = Vec::new();
    fs::File::open(&dst).unwrap().read_to_end(&mut b).unwrap();
    assert_eq!(a, b);
}

#[cfg(unix)]
#[test]
fn the_sparse_copy_really_allocates_less_than_a_dense_one() {
    // st_blocks is the only way to see the difference the sparse copy exists
    // for; `len` cannot. Unix-only, since Windows has no equivalent in std.
    use std::os::unix::fs::MetadataExt;
    const HOLE_AT: u64 = 8 * 1024 * 1024;
    let td = TempDir::new("blocks");
    let src = td.path().join("src.img");
    let dst = td.path().join("dst.img");
    let mut f = fs::File::create(&src).unwrap();
    f.write_all(&[1u8; 4096]).unwrap();
    f.seek(SeekFrom::Start(HOLE_AT)).unwrap();
    f.write_all(&[2u8; 4096]).unwrap();
    drop(f);

    copy_sparse(&src, &dst).unwrap();
    let meta = fs::metadata(&dst).unwrap();
    let allocated = meta.blocks() * 512;
    assert!(
        allocated * 2 < meta.len(),
        "allocated {allocated} bytes for a {} byte file — the holes were not preserved",
        meta.len()
    );
}

// ─── The binary, because the census traces this and not the library ────────
fn bin() -> &'static str {
    env!("CARGO_BIN_EXE_aerobackup")
}

#[test]
fn the_cli_reports_usage_errors_separately_from_failed_backups() {
    let td = TempDir::new("cli");
    let run_cli = |args: &[&str]| {
        let out = Command::new(bin()).args(args).output().expect("spawn");
        (
            out.status.code(),
            String::from_utf8_lossy(&out.stdout).into_owned(),
            String::from_utf8_lossy(&out.stderr).into_owned(),
        )
    };

    // A bad option value: usage error, 2.
    let (code, _, err) = run_cli(&["--kind", "weekly"]);
    assert_eq!(code, Some(2), "stderr: {err}");
    assert!(err.contains("hourly"), "stderr: {err}");

    // Retain 0 deletes the backup just taken; refused up front, as a usage
    // error, before the image is touched.
    let storage = td.path().join("sls_storage.img");
    fs::write(&storage, b"payload").unwrap();
    let (code, _, err) = run_cli(&[
        "--storage",
        storage.to_str().unwrap(),
        "--backup-dir",
        td.path().to_str().unwrap(),
        "--retain",
        "0",
    ]);
    assert_eq!(code, Some(2), "stderr: {err}");
    assert!(err.contains("delete every backup"), "stderr: {err}");
    assert!(!td.path().join("hourly").exists());

    // A missing image: a failed backup, 1, not a usage error.
    let (code, _, err) = run_cli(&[
        "--storage",
        td.path().join("absent.img").to_str().unwrap(),
        "--backup-dir",
        td.path().to_str().unwrap(),
    ]);
    assert_eq!(code, Some(1), "stderr: {err}");
    assert!(err.starts_with("[backup] FAILED:"), "stderr: {err}");

    // A real run: 0, and the named output is the pinned timestamp.
    let (code, out, err) = run_cli(&[
        "--storage",
        storage.to_str().unwrap(),
        "--backup-dir",
        td.path().to_str().unwrap(),
        "--kind",
        "hourly",
        "--retain",
        "1",
        "--now",
        "1700000000",
    ]);
    assert_eq!(code, Some(0), "stdout: {out} stderr: {err}");
    assert!(out.contains("sls_storage-20231114-221320.img"), "stdout: {out}");
    assert!(td
        .path()
        .join("hourly/sls_storage-20231114-221320.img")
        .exists());
}

#[test]
fn the_cli_dry_run_touches_nothing() {
    let td = TempDir::new("dryrun");
    let storage = td.path().join("sls_storage.img");
    fs::write(&storage, b"payload").unwrap();
    let out = Command::new(bin())
        .args([
            "--storage",
            storage.to_str().unwrap(),
            "--backup-dir",
            td.path().to_str().unwrap(),
            "--now",
            "1700000000",
            "--dry-run",
        ])
        .output()
        .expect("spawn");
    assert_eq!(out.status.code(), Some(0));
    assert!(!td.path().join("hourly").exists(), "dry run created the tier");
    let s = String::from_utf8_lossy(&out.stdout);
    assert!(s.contains("would write"), "stdout: {s}");
}
