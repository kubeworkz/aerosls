//! aerobackup — CLI entry point. See `src/lib.rs` for what this is a rewrite
//! of, and what it deliberately leaves out.
//!
//! Flags mirror the environment variables `backup/backup.sh` read, so an
//! operator's existing knowledge and existing cron lines both transfer:
//!
//!   --storage      STORAGE_IMG    default sls_storage.img
//!   --backup-dir   BACKUP_DIR     default /var/backups/aerosls
//!   --kind         BACKUP_KIND    hourly | daily, default hourly
//!   --retain       KEEP_HOURLY / KEEP_DAILY, default 4 / 7
//!
//! Exit: 0 success, 1 the backup or the prune failed, 2 the invocation was
//! wrong. The script exited 1 for every failure including a bad `BACKUP_KIND`;
//! a bad value for a named option is a usage error, and 2 is what the rest of
//! this tree already uses for "could not run".
#![forbid(unsafe_code)]

use std::path::PathBuf;
use std::process::ExitCode;

use aerobackup::{run, Config, Error, Kind};

const USAGE: &str = "\
aerobackup — snapshot-and-retain for sls_storage.img (Operational Phase D)

Usage: aerobackup [options]

Options:
  --storage <path>     image to back up          (env STORAGE_IMG, default sls_storage.img)
  --backup-dir <dir>   root of the tiers         (env BACKUP_DIR, default /var/backups/aerosls)
  --kind <tier>        hourly | daily            (env BACKUP_KIND, default hourly)
  --retain <n>         keep the newest n         (env KEEP_HOURLY / KEEP_DAILY, default 4 / 7)
  --now <epoch-secs>   timestamp to use instead of the clock
  --dry-run            report what would happen; touch nothing
  -h, --help           this text
  -V, --version        print the version

Backups land in <backup-dir>/<kind>/ as sls_storage-<UTC timestamp>.img. The
copy is written to <name>.img.partial, synced, and renamed into place, so a
name ending in .img is always a complete backup. Retaining the newest n prunes
by modification time, as the script's `ls -1t | tail -n +n+1` did.

Not done here, deliberately: stopping or starting the kernel under pm2, and
polling /api/health afterwards. Those are host orchestration, not the workload,
and the health poll needs sockets this candidate must not use.";

struct Args {
    storage: Option<PathBuf>,
    backup_dir: Option<PathBuf>,
    kind: Option<Kind>,
    retain: Option<usize>,
    now: Option<u64>,
    dry_run: bool,
}

/// Returns `Err(message)` for a usage problem — the caller exits 2 for those,
/// so the distinction between "you invoked this wrongly" and "the backup
/// failed" survives to the shell that ran it from cron.
fn parse_args(argv: &[String]) -> Result<Option<Args>, String> {
    let mut a = Args {
        storage: None,
        backup_dir: None,
        kind: None,
        retain: None,
        now: None,
        dry_run: false,
    };
    let mut i = 0;
    while i < argv.len() {
        let arg = argv[i].as_str();
        // Every value-taking option reads argv[i + 1]; a missing value is a
        // usage error rather than a silent default.
        let mut value = |name: &str| -> Result<String, String> {
            i += 1;
            argv.get(i)
                .cloned()
                .ok_or_else(|| format!("{name} needs a value"))
        };
        match arg {
            "--storage" => a.storage = Some(PathBuf::from(value("--storage")?)),
            "--backup-dir" => a.backup_dir = Some(PathBuf::from(value("--backup-dir")?)),
            "--kind" => {
                let v = value("--kind")?;
                a.kind = Some(Kind::parse(&v).ok_or_else(|| Error::KindInvalid(v).to_string())?);
            }
            "--retain" => {
                let v = value("--retain")?;
                a.retain = Some(
                    v.parse::<usize>()
                        .map_err(|_| format!("--retain needs a whole number, got '{v}'"))?,
                );
            }
            "--now" => {
                let v = value("--now")?;
                a.now = Some(
                    v.parse::<u64>()
                        .map_err(|_| format!("--now needs an epoch second, got '{v}'"))?,
                );
            }
            "--dry-run" => a.dry_run = true,
            "-h" | "--help" => {
                println!("{USAGE}");
                return Ok(None);
            }
            "-V" | "--version" => {
                println!("aerobackup {}", env!("CARGO_PKG_VERSION"));
                return Ok(None);
            }
            other => return Err(format!("unknown option '{other}' (try --help)")),
        }
        i += 1;
    }
    Ok(Some(a))
}

/// An env var, treating empty as unset so `STORAGE_IMG=` cannot mean the empty
/// path.
fn env_var(name: &str) -> Option<String> {
    match std::env::var(name) {
        Ok(v) if !v.is_empty() => Some(v),
        _ => None,
    }
}

/// Resolve flags over environment over defaults, exactly as the script did.
/// Split out from `main` so the precedence is unit-testable.
fn resolve(a: &Args, env: &dyn Fn(&str) -> Option<String>) -> Result<Config, Error> {
    let kind = match &a.kind {
        Some(k) => *k,
        None => match env("BACKUP_KIND") {
            Some(v) => Kind::parse(&v).ok_or(Error::KindInvalid(v))?,
            None => Kind::Hourly,
        },
    };
    let storage = a
        .storage
        .clone()
        .or_else(|| env("STORAGE_IMG").map(PathBuf::from))
        .unwrap_or_else(|| PathBuf::from("sls_storage.img"));
    let backup_dir = a
        .backup_dir
        .clone()
        .or_else(|| env("BACKUP_DIR").map(PathBuf::from))
        .unwrap_or_else(|| PathBuf::from("/var/backups/aerosls"));
    let retain = match a.retain {
        Some(n) => n,
        None => {
            let var = match kind {
                Kind::Hourly => "KEEP_HOURLY",
                Kind::Daily => "KEEP_DAILY",
            };
            match env(var) {
                Some(v) => v
                    .parse::<usize>()
                    .map_err(|_| Error::KindInvalid(format!("{var}={v} is not a number")))?,
                None => kind.default_retain(),
            }
        }
    };
    // A zero count is knowable here, so it is a usage error rather than a
    // failed run. `run()` keeps the same guard for library callers, because a
    // backup tool whose prune step can delete the backup it just took must not
    // depend on its CLI being the only caller.
    if retain == 0 {
        return Err(Error::RetainZero);
    }
    Ok(Config {
        storage,
        backup_dir,
        kind,
        retain,
        now_unix: a.now,
        dry_run: a.dry_run,
    })
}

fn main() -> ExitCode {
    let argv: Vec<String> = std::env::args().skip(1).collect();
    let args = match parse_args(&argv) {
        Ok(Some(a)) => a,
        Ok(None) => return ExitCode::SUCCESS, // --help / --version
        Err(msg) => {
            eprintln!("aerobackup: {msg}");
            eprintln!("Try 'aerobackup --help'.");
            return ExitCode::from(2);
        }
    };
    // Everything resolvable from flags and environment is a usage error, so it
    // reports like one: a bad BACKUP_KIND from cron should look like a mistake
    // in the cron line, not like a backup that failed.
    let cfg = match resolve(&args, &env_var) {
        Ok(c) => c,
        Err(e) => {
            eprintln!("aerobackup: {e}");
            eprintln!("Try 'aerobackup --help'.");
            return ExitCode::from(2);
        }
    };

    if cfg.dry_run {
        println!(
            "[backup] dry run: kind={} retain={} storage={} backup-dir={}",
            cfg.kind.name(),
            cfg.retain,
            cfg.storage.display(),
            cfg.backup_dir.display()
        );
    } else {
        println!(
            "[backup] Copying {} -> {}",
            cfg.storage.display(),
            cfg.backup_dir.join(cfg.kind.name()).display()
        );
    }

    match run(&cfg) {
        Ok(r) => {
            if r.dry_run {
                println!("[backup] dry run: would write {}", r.dest.display());
                for p in &r.pruned {
                    println!("[backup] dry run: would prune {}", p.display());
                }
                println!("[backup] dry run complete, nothing was touched");
            } else {
                println!(
                    "[backup] Pruning {} backups (keeping newest {})...",
                    cfg.kind.name(),
                    cfg.retain
                );
                for p in &r.pruned {
                    println!("[backup] pruned {}", p.display());
                }
                println!(
                    "[backup] Done: {} ({} bytes, {} sparse holes, {} kept)",
                    r.dest.display(),
                    r.bytes,
                    r.holes,
                    r.kept
                );
            }
            ExitCode::SUCCESS
        }
        Err(e) => {
            eprintln!("[backup] FAILED: {e}");
            ExitCode::from(1)
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn no_env(_: &str) -> Option<String> {
        None
    }

    fn args(argv: &[&str]) -> Vec<String> {
        argv.iter().map(|s| s.to_string()).collect()
    }

    #[test]
    fn defaults_match_the_scripts() {
        let a = parse_args(&[]).unwrap().unwrap();
        let c = resolve(&a, &no_env).unwrap();
        assert_eq!(c.storage, PathBuf::from("sls_storage.img"));
        assert_eq!(c.backup_dir, PathBuf::from("/var/backups/aerosls"));
        assert_eq!(c.kind, Kind::Hourly);
        assert_eq!(c.retain, 4);
        assert!(!c.dry_run);
    }

    #[test]
    fn the_tier_picks_its_own_retention_default() {
        let a = parse_args(&args(&["--kind", "daily"])).unwrap().unwrap();
        assert_eq!(resolve(&a, &no_env).unwrap().retain, 7);
    }

    #[test]
    fn flags_beat_the_environment_and_the_environment_beats_the_default() {
        let env = |n: &str| match n {
            "STORAGE_IMG" => Some("from-env.img".to_string()),
            "KEEP_HOURLY" => Some("9".to_string()),
            _ => None,
        };
        let a = parse_args(&args(&["--storage", "from-flag.img"])).unwrap().unwrap();
        let c = resolve(&a, &env).unwrap();
        assert_eq!(c.storage, PathBuf::from("from-flag.img")); // flag wins
        assert_eq!(c.retain, 9); // env wins over the default

        let a = parse_args(&[]).unwrap().unwrap();
        assert_eq!(resolve(&a, &env).unwrap().storage, PathBuf::from("from-env.img"));
    }

    #[test]
    fn an_empty_env_var_is_unset_rather_than_the_empty_path() {
        let env = |n: &str| match n {
            "STORAGE_IMG" => env_var_for_test(""),
            _ => None,
        };
        let a = parse_args(&[]).unwrap().unwrap();
        assert_eq!(resolve(&a, &env).unwrap().storage, PathBuf::from("sls_storage.img"));
    }

    fn env_var_for_test(v: &str) -> Option<String> {
        // Mirrors env_var's rule without touching the process environment.
        if v.is_empty() {
            None
        } else {
            Some(v.to_string())
        }
    }

    #[test]
    fn a_bad_tier_is_a_usage_error_in_both_spellings() {
        assert!(parse_args(&args(&["--kind", "weekly"])).is_err());
        let a = parse_args(&[]).unwrap().unwrap();
        let env = |n: &str| (n == "BACKUP_KIND").then(|| "weekly".to_string());
        assert!(resolve(&a, &env).is_err());
    }

    #[test]
    fn a_missing_option_value_is_a_usage_error_not_a_default() {
        assert!(parse_args(&args(&["--storage"])).is_err());
        assert!(parse_args(&args(&["--retain", "four"])).is_err());
        assert!(parse_args(&args(&["--nope"])).is_err());
    }

    #[test]
    fn help_and_version_are_not_runs() {
        assert!(parse_args(&args(&["--help"])).unwrap().is_none());
        assert!(parse_args(&args(&["-V"])).unwrap().is_none());
    }

    #[test]
    fn a_non_numeric_env_retention_is_an_error_rather_than_a_silent_default() {
        let a = parse_args(&[]).unwrap().unwrap();
        let env = |n: &str| (n == "KEEP_HOURLY").then(|| "many".to_string());
        assert!(resolve(&a, &env).is_err());
    }
}
