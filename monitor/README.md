# AeroSLS health monitoring (Operational Phase C)

See `docs/AeroSLS-Operational-MVP-Roadmap-v0.1.md` for the full writeup.

## 1. `health_check.sh` via cron

Run on the server, on a schedule, against the **public** URL (this checks
the whole chain — nginx, Cloudflare, the kernel — not just the kernel in
isolation; `deploy/deploy.sh` already covers that narrower check as part of
a deploy).

```
crontab -e
```

Add (every 2 minutes; adjust to taste):

```
*/2 * * * * ALERT_WEBHOOK_URL="https://hooks.slack.com/services/…" /path/to/aerosls2/monitor/health_check.sh >> /var/log/aerosls-health-check.log 2>&1
```

`ALERT_WEBHOOK_URL` is optional — point it at a Slack/Discord incoming
webhook (or anything that accepts a JSON POST) for real notifications.
Without it, a non-zero exit still relies on cron's own default behavior
(emailing the job's output to the local mail user) — worth confirming that
mail actually goes somewhere on this server before relying on it as the
only channel.

Tested against a mock server standing in for the kernel's real `jb_uint`
JSON output format (compact, no space after `:` — confirmed by reading
`net/http.c`'s `jb_uint()` directly, not assumed): unreachable, stuck
`uptime_ticks`, recovering `uptime_ticks`, and the happy path all produce
the correct alert/exit-code behavior.

## 2. Free external uptime monitor (complementary, not a replacement)

`health_check.sh` catches the wedged-but-answering case a plain uptime
monitor can't. A plain monitor is still worth having as a second, simpler,
off-server check (catches the case where the server itself, or its network
path, is the thing that's down — which a script running ON that same server
obviously can't detect about itself). Signing up is a step Dave needs to do
himself, not something to automate here. Suggested config, e.g. for
UptimeRobot's free tier:

- Monitor type: HTTP(s)
- URL: `https://aerosls.kubeworkz.io/api/health`
- Interval: 5 minutes (free tier's minimum)
- Expected: HTTP 200 (optionally: keyword monitoring for `"status":"ok"` in
  the body, for a slightly stronger check than status code alone)

## 3. Log rotation

The kernel's serial debug log (`-serial file:sls_kernel_debug.log` in the
Makefile's `x86-run` target) grows forever with no rotation — confirmed via
that target; the actual production launch command (whatever pm2 runs)
wasn't found in either repo, so the real log path/filename on the server
needs confirming rather than assumed. Two complementary fixes:

- `pm2 install pm2-logrotate` — rotates whatever pm2 itself captures as the
  managed process's stdout/stderr. Zero config, works today regardless of
  the exact log path question above.
- If the kernel's QEMU invocation still uses `-serial file:<path>` in
  production (writing directly to a file, bypassing pm2's own stdout
  capture entirely), that file needs its own system `logrotate` entry,
  since `pm2-logrotate` won't see it. Template:

  ```
  # /etc/logrotate.d/aerosls-kernel
  /path/to/sls_kernel_debug.log {
      daily
      rotate 7
      compress
      missingok
      notifempty
      copytruncate
  }
  ```

  `copytruncate` matters here specifically: QEMU has the file open
  continuously via `-serial file:`, so a plain rename-based rotation would
  leave QEMU still writing into the renamed (now-invisible) file instead of
  a fresh one — `copytruncate` avoids that by copying the current content
  out and truncating the original in place, which QEMU keeps writing into
  correctly.
