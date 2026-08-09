#!/usr/bin/env python3
"""
Download an AeroSLS STREAM object back to a local file.

The kernel sends the stream content with the stored MIME type and
a Content-Disposition: attachment header.  Frames are lazy-loaded
from NVMe on first access after a reboot.

Usage:
  python3 stream_download.py --name report.pdf --output ./report.pdf [options]

Options:
  --host    HOST:PORT   Kernel API address (default: localhost:3001)
  --token   TOKEN       Bearer auth token  (default: dave@gridworkz.com DB_ADMIN token)
  --name    NAME        STREAM object name in the kernel
  --output  PATH        Local output file path (use "-" to write to stdout)

List all streams:
  GET /api/streams

Example:
  python3 stream_download.py --name report.pdf --output report_local.pdf
  python3 stream_download.py --name data.csv   --output - | head
"""

import argparse
import sys
import urllib.error
import urllib.request

DEFAULT_TOKEN = "deadbeef01234567cafebabe76543210"  # dave@gridworkz.com — DB_ADMIN


# ─── helpers ─────────────────────────────────────────────────────────────────

def _base_url(host: str) -> str:
    host = host.rstrip("/")
    if not host.startswith(("http://", "https://")):
        host = "http://" + host
    return host


# ─── main ─────────────────────────────────────────────────────────────────────

def main() -> None:
    parser = argparse.ArgumentParser(
        description="Download an AeroSLS STREAM object to a local file",
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument("--name",   required=True, metavar="NAME",
                        help="STREAM object name in the kernel")
    parser.add_argument("--output", required=True, metavar="PATH",
                        help="Local output file path (use '-' for stdout)")
    parser.add_argument("--host",   default="localhost:3001", metavar="HOST:PORT",
                        help="Kernel API address (default: localhost:3001)")
    parser.add_argument("--token",  default=DEFAULT_TOKEN, metavar="TOKEN",
                        help="Bearer auth token (default: dave@gridworkz.com)")
    args = parser.parse_args()

    base = _base_url(args.host)
    url  = f"{base}/api/stream/{args.name}"

    print(f"Stream : {args.name}", file=sys.stderr)
    print(f"URL    : {url}", file=sys.stderr)

    req = urllib.request.Request(url)
    req.add_header("Authorization", f"Bearer {args.token}")

    try:
        with urllib.request.urlopen(req, timeout=60) as resp:
            content_type = resp.headers.get("Content-Type", "application/octet-stream")
            content_disp = resp.headers.get("Content-Disposition", "")
            data = resp.read()
    except urllib.error.HTTPError as exc:
        detail = exc.read().decode(errors="replace")
        print(f"HTTP {exc.code}: {detail}", file=sys.stderr)
        if exc.code == 404:
            print(f"Stream '{args.name}' not found. List available streams:", file=sys.stderr)
            print(f"  GET {base}/api/streams", file=sys.stderr)
        sys.exit(1)
    except urllib.error.URLError as exc:
        print(f"Connection error: {exc.reason}", file=sys.stderr)
        sys.exit(1)

    # Write output
    if args.output == "-":
        sys.stdout.buffer.write(data)
    else:
        try:
            with open(args.output, "wb") as fh:
                fh.write(data)
        except OSError as exc:
            print(f"Cannot write '{args.output}': {exc}", file=sys.stderr)
            sys.exit(1)
        print(f"Saved  : {args.output} ({len(data):,} bytes)", file=sys.stderr)

    print(f"MIME   : {content_type}", file=sys.stderr)
    if content_disp:
        print(f"Header : {content_disp}", file=sys.stderr)


if __name__ == "__main__":
    main()
