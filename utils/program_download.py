#!/usr/bin/env python3
"""
Download an AeroSLS PROGRAM object back to a local file.

The kernel returns the raw binary (ELF64 or flat) that was previously uploaded
via program_upload.py.  The X-Binary-Format response header indicates the type.

Usage:
  python3 program_download.py --name myapp --output ./myapp [options]

Options:
  --host    HOST:PORT   Kernel API address (default: localhost:3001)
  --token   TOKEN       Bearer auth token  (default: dave@gridworkz.com DB_ADMIN token)
  --name    NAME        PROGRAM object name in the kernel
  --output  PATH        Local output file path (use "-" to write to stdout)

List all programs:
  GET /api/programs

Example:
  python3 program_download.py --name myapp --output ./myapp_recovered
  python3 program_download.py --name myapp --output - | sha256sum
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
        description="Download an AeroSLS PROGRAM object to a local file",
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument("--name",   required=True, metavar="NAME",
                        help="PROGRAM object name in the kernel")
    parser.add_argument("--output", required=True, metavar="PATH",
                        help="Local output file path (use '-' for stdout)")
    parser.add_argument("--host",   default="localhost:3001", metavar="HOST:PORT",
                        help="Kernel API address (default: localhost:3001)")
    parser.add_argument("--token",  default=DEFAULT_TOKEN, metavar="TOKEN",
                        help="Bearer auth token (default: dave@gridworkz.com)")
    args = parser.parse_args()

    base = _base_url(args.host)
    url  = f"{base}/api/program/{args.name}"

    print(f"Program: {args.name}", file=sys.stderr)
    print(f"URL    : {url}", file=sys.stderr)

    req = urllib.request.Request(url)
    req.add_header("Authorization", f"Bearer {args.token}")

    try:
        with urllib.request.urlopen(req, timeout=60) as resp:
            fmt          = resp.headers.get("X-Binary-Format", "unknown")
            content_disp = resp.headers.get("Content-Disposition", "")
            data         = resp.read()
    except urllib.error.HTTPError as exc:
        detail = exc.read().decode(errors="replace")
        print(f"HTTP {exc.code}: {detail}", file=sys.stderr)
        if exc.code == 404:
            print(f"Program '{args.name}' not found. List available programs:", file=sys.stderr)
            print(f"  GET {base}/api/programs", file=sys.stderr)
        sys.exit(1)
    except urllib.error.URLError as exc:
        print(f"Connection error: {exc.reason}", file=sys.stderr)
        sys.exit(1)

    if args.output == "-":
        sys.stdout.buffer.write(data)
    else:
        with open(args.output, "wb") as fh:
            fh.write(data)
        print(f"Saved  : {args.output} ({len(data):,} bytes)", file=sys.stderr)

    print(f"Format : {fmt}", file=sys.stderr)
    if content_disp:
        print(f"Header : {content_disp}", file=sys.stderr)


if __name__ == "__main__":
    main()
