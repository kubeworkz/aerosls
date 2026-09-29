//! aerobackup — the snapshot-and-retain core of `backup/backup.sh`, as a static
//! single-threaded Rust binary. The E7 Linux ABI shim's designated census
//! target (`tools/linux_abi_candidate.conf`).
//!
//! ─── What this is a rewrite OF, and what it deliberately is not ────────────
//! `backup/backup.sh` (Operational-MVP Phase D) does four things: stop the
//! kernel under pm2, copy `sls_storage.img` aside, restart it, and poll
//! `/api/health` before declaring success. Only the **copy and the retention
//! prune** are the workload. The pm2 calls are process-manager orchestration and
//! the health poll is an HTTP request — and that poll is precisely the
//! networking that eliminated the *monitoring* candidate from E7's shortlist
//! (design §5.5.1), so it cannot be smuggled back in here: a census target has
//! to be reachable with the v1 syscall core, which has no sockets.
//!
//! ─── Three deliberate differences from the shell original ──────────────────
//! 1. **No partial backup can ever wear the final name.** The original copies
//!    straight to `sls_storage-<ts>.img`, so an interrupted copy leaves a
//!    truncated file that looks exactly like a good backup — and retention will
//!    happily keep it and prune a real one. Here the copy lands on
//!    `<name>.img.partial`, is `fsync`ed, and is `rename`d into place; rename is
//!    atomic within a filesystem, so the final name only ever exists complete.
//!    Retention ignores anything not ending in `.img`, so a stale `.partial` is
//!    always a crash artefact and never a candidate for "keeping".
//! 2. **The copy is durable before it is named.** The original relies on `cp`'s
//!    writeback; a backup that is renamed before its bytes reach the disk can be
//!    absent after a crash, which is the one situation a backup exists for.
//! 3. **A zero retention count is refused.** The original's
//!    `tail -n +$((keep + 1))` with `keep=0` deletes every backup including the
//!    one it just took. It is never what anyone means, so the CLI rejects it
//!    with a message rather than performing it faithfully.
//!
//! Two smaller notes, stated so they are not discovered later: the timestamp in
//! the file name is **UTC** (`date` gave local time) — retention does not depend
//! on it, and UTC cannot repeat a name across a DST fall-back — and the copy is
//! sparse-preserving by detection rather than by `cp --sparse=always`, so an
//! all-zero region becomes a hole in the destination.
//!
//! ─── No dependencies, on purpose ───────────────────────────────────────────
//! Everything here is `std`. A census target should measure the syscalls of a
//! workload, not of a dependency tree, and `#![forbid(unsafe_code)]` means the
//! measured binary contains no hand-written memory or ABI code that the shim's
//! own audit would then have to account for.

#![forbid(unsafe_code)]

use std::fs::{self, File, OpenOptions};
use std::io::{self, Read, Seek, SeekFrom, Write};
use std::path::{Path, PathBuf};
use std::time::{SystemTime, UNIX_EPOCH};

/// Backups are named `sls_storage-<YYYYMMDD-HHMMSS>.img`, as the script did.
pub const NAME_PREFIX: &str = "sls_storage-";
pub const NAME_SUFFIX: &str = ".img";
/// Suffix on an in-flight copy, never matched by the retention filter.
pub const PARTIAL_SUFFIX: &str = ".partial";
/// Copy granularity. One chunk of all-zero bytes becomes one hole.
pub const COPY_CHUNK: usize = 64 * 1024;

const SECS_PER_DAY: u64 = 86_400;

/// The two retention tiers `backup.sh` ran from two cron entries.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum Kind {
    Hourly,
    Daily,
}

impl Kind {
    pub fn parse(s: &str) -> Option<Kind> {
        match s {
            "hourly" => Some(Kind::Hourly),
            "daily" => Some(Kind::Daily),
            _ => None,
        }
    }

    /// The subdirectory name, which is also the CLI/env spelling.
    pub fn name(self) -> &'static str {
        match self {
            Kind::Hourly => "hourly",
            Kind::Daily => "daily",
        }
    }

    /// `KEEP_HOURLY=4` / `KEEP_DAILY=7` from `backup/README.md`.
    pub fn default_retain(self) -> usize {
        match self {
            Kind::Hourly => 4,
            Kind::Daily => 7,
        }
    }
}

// ─── The name ──────────────────────────────────────────────────────────────
/// `sls_storage-YYYYMMDD-HHMMSS.img` for an epoch second, in UTC.
pub fn backup_name(unix_secs: u64) -> String {
    let days = (unix_secs / SECS_PER_DAY) as i64;
    let secs = unix_secs % SECS_PER_DAY;
    let (y, m, d) = civil_from_days(days);
    format!(
        "{}{:04}{:02}{:02}-{:02}{:02}{:02}{}",
        NAME_PREFIX,
        y,
        m,
        d,
        secs / 3600,
        (secs % 3600) / 60,
        secs % 60,
        NAME_SUFFIX
    )
}

/// Does this directory entry name look like a retention candidate?
///
/// Deliberately a name test and not a metadata test: a `.partial` copy is not a
/// backup, and this is the single place that decision is made so the writer
/// (step 1 above) and the pruner cannot disagree about it.
pub fn is_backup_name(name: &str) -> bool {
    name.starts_with(NAME_PREFIX) && name.ends_with(NAME_SUFFIX)
}

/// Days since 1970-01-01 → (year, month, day) in the proleptic Gregorian
/// calendar. Howard Hinnant's `civil_from_days`; the epoch is a leap-year
/// boundary so it is checked directly in the tests rather than trusted.
fn civil_from_days(z: i64) -> (i64, u32, u32) {
    let z = z + 719_468;
    let era = if z >= 0 { z } else { z - 146_096 } / 146_097;
    let doe = (z - era * 146_097) as u64; // [0, 146096]
    let yoe = (doe - doe / 1460 + doe / 36524 - doe / 146_096) / 365; // [0, 399]
    let y = yoe as i64 + era * 400;
    let doy = doe - (365 * yoe + yoe / 4 - yoe / 100); // [0, 365]
    let mp = (5 * doy + 2) / 153; // [0, 11]
    let d = (doy - (153 * mp + 2) / 5 + 1) as u32; // [1, 31]
    let m = if mp < 10 { mp + 3 } else { mp - 9 } as u32; // [1, 12]
    (if m <= 2 { y + 1 } else { y }, m, d)
}

pub fn system_now_unix() -> u64 {
    SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map(|d| d.as_secs())
        .unwrap_or(0)
}

// ─── The prune decision, as a pure function ────────────────────────────────
/// Which files to delete, given `(path, mtime)` pairs and a retention count.
///
/// This is `ls -1t | tail -n +$((keep + 1))` — **newest by mtime survives** —
/// with the one thing the shell pipeline leaves undefined made explicit: `ls -1t`
/// orders equal mtimes by directory order, which is not a contract, so two
/// backups taken in the same second (a manual re-run, a restored copy) prune
/// by whatever `readdir` happened to return. Ties here break on the file name,
/// descending, so the decision is deterministic and re-running the pruner
/// cannot change its mind.
///
/// `keep == 0` returns everything, which is the shell's behaviour and is why the
/// CLI refuses a zero count instead of reaching this.
pub fn plan_prune(entries: &[(PathBuf, SystemTime)], keep: usize) -> Vec<PathBuf> {
    let mut ordered: Vec<&(PathBuf, SystemTime)> = entries.iter().collect();
    ordered.sort_by(|a, b| b.1.cmp(&a.1).then_with(|| b.0.cmp(&a.0)));
    ordered.into_iter().skip(keep).map(|(p, _)| p.clone()).collect()
}

/// Every backup currently in `dir`, as `(path, mtime)`. A missing directory is
/// empty rather than an error: the first run of a tier has no directory yet.
pub fn list_backups(dir: &Path) -> io::Result<Vec<(PathBuf, SystemTime)>> {
    let mut out = Vec::new();
    let rd = match fs::read_dir(dir) {
        Ok(rd) => rd,
        Err(e) if e.kind() == io::ErrorKind::NotFound => return Ok(out),
        Err(e) => return Err(e),
    };
    for entry in rd {
        let entry = entry?;
        let name = entry.file_name();
        let name = name.to_string_lossy();
        if !is_backup_name(&name) {
            continue;
        }
        let meta = entry.metadata()?;
        if !meta.is_file() {
            continue;
        }
        out.push((entry.path(), meta.modified()?));
    }
    Ok(out)
}

// ─── The copy ──────────────────────────────────────────────────────────────
#[derive(Debug, Default, Clone, Copy, PartialEq, Eq)]
pub struct CopyStats {
    /// Bytes in the source — the destination's length.
    pub bytes: u64,
    /// Bytes actually written; `bytes - written` is the hole total.
    pub written: u64,
    /// All-zero chunks turned into holes.
    pub holes: u64,
}

/// Copy `src` to `dst`, turning all-zero chunks into holes.
///
/// `cp --sparse=always` detects zero runs; this does the same thing by looking
/// at each chunk, which needs no `SEEK_HOLE` support and no libc, and is
/// therefore testable as ordinary code. The destination is `fsync`ed before
/// this returns — see difference 2 in the module docs.
pub fn copy_sparse(src: &Path, dst: &Path) -> io::Result<CopyStats> {
    let mut r = File::open(src)?;
    let mut w = OpenOptions::new()
        .create(true)
        .write(true)
        .truncate(true)
        .open(dst)?;
    let mut buf = vec![0u8; COPY_CHUNK];
    let mut stats = CopyStats::default();
    let mut hole_run: u64 = 0;
    loop {
        let n = r.read(&mut buf)?;
        if n == 0 {
            break;
        }
        stats.bytes += n as u64;
        if buf[..n].iter().all(|b| *b == 0) {
            hole_run += n as u64;
            stats.holes += 1;
            continue;
        }
        if hole_run > 0 {
            w.seek(SeekFrom::Current(hole_run as i64))?;
            hole_run = 0;
        }
        w.write_all(&buf[..n])?;
        stats.written += n as u64;
    }
    if hole_run > 0 {
        w.seek(SeekFrom::Current(hole_run as i64))?;
    }
    // A file ending in a hole has no following write to extend it, so the length
    // is set explicitly. Without this the copy is short and the backup is a lie
    // that a size check would catch and nothing else would.
    w.set_len(stats.bytes)?;
    w.sync_all()?;
    Ok(stats)
}

// ─── Errors ────────────────────────────────────────────────────────────────
#[derive(Debug)]
pub enum Error {
    KindInvalid(String),
    RetainZero,
    StorageMissing(PathBuf),
    Io { ctx: String, source: io::Error },
}

impl std::fmt::Display for Error {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            Error::KindInvalid(k) => write!(
                f,
                "BACKUP_KIND/'--kind' must be 'hourly' or 'daily', got '{k}'"
            ),
            Error::RetainZero => write!(
                f,
                "a retention count of 0 would delete every backup including the one just taken -- refusing"
            ),
            Error::StorageMissing(p) => write!(
                f,
                "{} not found (run from the repo root, or set --storage/STORAGE_IMG)",
                p.display()
            ),
            Error::Io { ctx, source } => write!(f, "{ctx}: {source}"),
        }
    }
}

impl std::error::Error for Error {
    fn source(&self) -> Option<&(dyn std::error::Error + 'static)> {
        match self {
            Error::Io { source, .. } => Some(source),
            _ => None,
        }
    }
}

fn ioe(ctx: impl Into<String>, source: io::Error) -> Error {
    Error::Io {
        ctx: ctx.into(),
        source,
    }
}

// ─── The run ───────────────────────────────────────────────────────────────
#[derive(Debug, Clone)]
pub struct Config {
    pub storage: PathBuf,
    pub backup_dir: PathBuf,
    pub kind: Kind,
    /// Retention count for this tier. The CLI refuses 0.
    pub retain: usize,
    /// Pin the timestamp instead of reading the clock — what makes the output
    /// name a testable value rather than a race with the wall clock.
    pub now_unix: Option<u64>,
    pub dry_run: bool,
}

#[derive(Debug, Clone)]
pub struct Report {
    pub dest: PathBuf,
    pub bytes: u64,
    pub holes: u64,
    pub pruned: Vec<PathBuf>,
    /// Backups surviving the prune (`min(existing, retain)`).
    pub kept: usize,
    pub dry_run: bool,
}

/// Take one backup and prune its tier. The whole workload, in one call.
pub fn run(cfg: &Config) -> Result<Report, Error> {
    if cfg.retain == 0 {
        return Err(Error::RetainZero);
    }
    if !cfg.storage.is_file() {
        return Err(Error::StorageMissing(cfg.storage.clone()));
    }
    let dir = cfg.backup_dir.join(cfg.kind.name());
    let now = cfg.now_unix.unwrap_or_else(system_now_unix);
    let dest = dir.join(backup_name(now));

    if cfg.dry_run {
        let existing = list_backups(&dir).map_err(|e| ioe(format!("listing {}", dir.display()), e))?;
        let mut planned = existing.clone();
        planned.push((dest.clone(), SystemTime::now()));
        let pruned = plan_prune(&planned, cfg.retain);
        return Ok(Report {
            dest,
            bytes: fs::metadata(&cfg.storage)
                .map_err(|e| ioe(format!("stat {}", cfg.storage.display()), e))?
                .len(),
            holes: 0,
            pruned,
            kept: planned.len().saturating_sub(cfg.retain.min(planned.len())),
            dry_run: true,
        });
    }

    fs::create_dir_all(&dir).map_err(|e| ioe(format!("creating {}", dir.display()), e))?;

    // Copy to `<name>.partial` and rename into place, so the final name is only
    // ever a complete, synced file (difference 1 in the module docs).
    let mut partial = dest.clone().into_os_string();
    partial.push(PARTIAL_SUFFIX);
    let partial = PathBuf::from(partial);
    let stats = match copy_sparse(&cfg.storage, &partial) {
        Ok(s) => s,
        Err(e) => {
            // A failed copy must not leave junk that nothing will ever prune.
            let _ = fs::remove_file(&partial);
            return Err(ioe(
                format!("copying {} -> {}", cfg.storage.display(), partial.display()),
                e,
            ));
        }
    };
    fs::rename(&partial, &dest)
        .map_err(|e| ioe(format!("renaming {} into place", partial.display()), e))?;

    let existing = list_backups(&dir).map_err(|e| ioe(format!("listing {}", dir.display()), e))?;
    let pruned = plan_prune(&existing, cfg.retain);
    for p in &pruned {
        fs::remove_file(p).map_err(|e| ioe(format!("pruning {}", p.display()), e))?;
    }
    // `existing` includes the backup just taken, so this is the count that
    // survives — and if it is less than `retain`, that is old backups, not a
    // pruning bug.
    let kept = existing.len().min(cfg.retain);

    Ok(Report {
        dest,
        bytes: stats.bytes,
        holes: stats.holes,
        pruned,
        kept,
        dry_run: false,
    })
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::time::Duration;

    fn t(secs: u64) -> SystemTime {
        UNIX_EPOCH + Duration::from_secs(secs)
    }

    // ── The name ───────────────────────────────────────────────────────────
    #[test]
    fn name_is_the_script_shape_in_utc() {
        assert_eq!(backup_name(0), "sls_storage-19700101-000000.img");
        // 1700000000 is 2023-11-14T22:13:20Z.
        assert_eq!(backup_name(1_700_000_000), "sls_storage-20231114-221320.img");
    }

    #[test]
    fn name_handles_a_leap_day() {
        // 2000-02-29T00:00:00Z
        assert_eq!(backup_name(951_782_400), "sls_storage-20000229-000000.img");
    }

    #[test]
    fn civil_from_days_has_no_off_by_one_at_the_epoch() {
        assert_eq!(civil_from_days(0), (1970, 1, 1));
        // The algorithm's era arithmetic is the part that breaks if its sign
        // handling is wrong, and the epoch is a decade boundary, so check one
        // day either side of it as well.
        assert_eq!(civil_from_days(-1), (1969, 12, 31));
        assert_eq!(civil_from_days(1), (1970, 1, 2));
    }

    #[test]
    fn zero_padding_is_what_makes_names_sortable() {
        // A single-digit month must still be two characters, or lexical order
        // stops matching chronological order and `ls`-style listing lies.
        let jan = backup_name(1_704_067_200); // 2024-01-01T00:00:00Z
        let oct = backup_name(1_728_000_000); // 2024-10-04T00:00:00Z
        assert!(jan < oct, "{jan} should sort before {oct}");
    }

    // ── The name filter ────────────────────────────────────────────────────
    #[test]
    fn partial_copies_are_not_backups() {
        assert!(is_backup_name("sls_storage-20240101-000000.img"));
        assert!(!is_backup_name("sls_storage-20240101-000000.img.partial"));
        assert!(!is_backup_name("sls_storage.img"));
        assert!(!is_backup_name("other-20240101-000000.img"));
    }

    // ── plan_prune ─────────────────────────────────────────────────────────
    #[test]
    fn keep_n_leaves_the_n_newest_by_mtime() {
        let e = vec![
            (PathBuf::from("d/sls_storage-a.img"), t(100)),
            (PathBuf::from("d/sls_storage-b.img"), t(300)),
            (PathBuf::from("d/sls_storage-c.img"), t(200)),
        ];
        let pruned = plan_prune(&e, 2);
        assert_eq!(pruned, vec![PathBuf::from("d/sls_storage-a.img")]);
    }

    #[test]
    fn mtime_decides_not_the_name() {
        // The whole reason retention is mtime-based: a restored or copied file
        // can carry any name. If this ever switched to name ordering, an
        // operator restoring an old image into the tier would silently get the
        // newest backup pruned instead of the restored one.
        let e = vec![
            (
                PathBuf::from("d/sls_storage-20990101-000000.img"),
                t(100), // newest NAME, oldest mtime
            ),
            (
                PathBuf::from("d/sls_storage-19700101-000000.img"),
                t(300), // oldest NAME, newest mtime
            ),
        ];
        let pruned = plan_prune(&e, 1);
        assert_eq!(
            pruned,
            vec![PathBuf::from("d/sls_storage-20990101-000000.img")]
        );
    }

    #[test]
    fn equal_mtimes_break_on_the_name_so_the_decision_is_deterministic() {
        let e = vec![
            (PathBuf::from("d/sls_storage-aaa.img"), t(100)),
            (PathBuf::from("d/sls_storage-bbb.img"), t(100)),
        ];
        // Name descending: 'bbb' outranks 'aaa', so 'aaa' is the one to drop.
        assert_eq!(plan_prune(&e, 1), vec![PathBuf::from("d/sls_storage-aaa.img")]);
        // And the same input gives the same answer, which is the point.
        assert_eq!(plan_prune(&e, 1), plan_prune(&e, 1));
    }

    #[test]
    fn keeping_more_than_exist_prunes_nothing() {
        let e = vec![(PathBuf::from("d/sls_storage-a.img"), t(1))];
        assert!(plan_prune(&e, 4).is_empty());
        assert!(plan_prune(&e, 100).is_empty());
    }

    #[test]
    fn keep_zero_is_everything_and_that_is_why_the_cli_refuses_it() {
        let e = vec![
            (PathBuf::from("d/sls_storage-a.img"), t(1)),
            (PathBuf::from("d/sls_storage-b.img"), t(2)),
        ];
        assert_eq!(plan_prune(&e, 0).len(), 2);
    }
}
