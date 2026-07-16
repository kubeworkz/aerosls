#!/usr/bin/env python3
"""
Upload a local file as an AeroSLS STREAM object (text, PDF, image, binary, etc.)

Flow:
  1. POST /api/stream/create  — allocate the named STREAM slot (up to 8 total)
  2. POST /api/stream/upload  — send file bytes as chunked hex, 4 KiB at a time
     Last chunk sets last=1, which flushes all frames to NVMe and marks status=ready.

Usage:
  python3 stream_upload.py --file ./report.pdf --name report.pdf [options]

Options:
  --host   HOST:PORT   Kernel API address (default: localhost:3001)
  --token  TOKEN       Bearer auth token  (default: dave@gridworkz.com DB_ADMIN token)
  --name   NAME        STREAM object name to create in the kernel
  --file   PATH        Local file to upload
  --mime   TYPE        MIME type string (auto-detected from extension if omitted)

After upload the stream is NVMe-persisted and survives reboots.
Download it back with:
  GET /api/stream/<name>    (returns file with Content-Disposition: attachment)

Common MIME types:
  text/plain  text/csv  application/json  application/pdf
  image/png   image/jpeg  application/octet-stream  application/zip
"""

import argparse
import json
import math
import mimetypes
import sys
import urllib.error
import urllib.request

CHUNK_BYTES = 4096                             # raw bytes per upload chunk
DEFAULT_TOKEN = "deadbeef01234567cafebabe76543210"  # dave@gridworkz.com — DB_ADMIN


# ─── helpers ─────────────────────────────────────────────────────────────────

def _post(base: str, path: str, payload: dict, token: str) -> dict:
    url = base + path
    body = json.dumps(payload).encode()
    req = urllib.request.Request(url, data=body, method="POST")
    req.add_header("Content-Type", "application/json")
    req.add_header("Authorization", f"Bearer {token}")
    try:
        with urllib.request.urlopen(req, timeout=30) as r:
            return json.loads(r.read())
    except urllib.error.HTTPError as exc:
        detail = exc.read().decode(errors="replace")
        print(f"  HTTP {exc.code}: {detail}", file=sys.stderr)
        sys.exit(1)
    except urllib.error.URLError as exc:
        print(f"  Connection error: {exc.reason}", file=sys.stderr)
        sys.exit(1)


def _base_url(host: str) -> str:
    host = host.rstrip("/")
    if not host.startswith(("http://", "https://")):
        host = "http://" + host
    return host


def _detect_mime(path: str) -> str:
    mime, _ = mimetypes.guess_type(path)
    return mime or "application/octet-stream"


# ─── main ─────────────────────────────────────────────────────────────────────

def main() -> None:
    parser = argparse.ArgumentParser(
        description="Upload a file as an AeroSLS STREAM object",
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument("--file",  required=True, metavar="PATH",
                        help="Local file to upload")
    parser.add_argument("--name",  required=True, metavar="NAME",
                        help="STREAM object name in the kernel")
    parser.add_argument("--host",  default="localhost:3001", metavar="HOST:PORT",
                        help="Kernel API address (default: localhost:3001)")
    parser.add_argument("--token", default=DEFAULT_TOKEN, metavar="TOKEN",
                        help="Bearer auth token (default: dave@gridworkz.com)")
    parser.add_argument("--mime",  default="", metavar="TYPE",
                        help="MIME type (auto-detected from file extension if omitted)")
    args = parser.parse_args()

    base = _base_url(args.host)
    mime = args.mime or _detect_mime(args.file)

    # Read file
    try:
        with open(args.file, "rb") as fh:
            data = fh.read()
    except OSError as exc:
        print(f"Cannot read '{args.file}': {exc}", file=sys.stderr)
        sys.exit(1)

    total = len(data)
    print(f"File  : {args.file} ({total:,} bytes)")
    print(f"Stream: {args.name}  mime={mime}")
    print(f"Host  : {base}")

    # ── Step 1: create ────────────────────────────────────────────────────────
    print(f"\n[1/2] Creating STREAM object '{args.name}'...")
    resp = _post(base, "/api/stream/create", {"name": args.name, "mime": mime}, args.token)
    if resp.get("ok") != "true":
        err = resp.get("error", str(resp))
        print(f"  Failed: {err}", file=sys.stderr)
        if "already exists" in err:
            print("  Tip: streams cannot be overwritten. Use a different name.", file=sys.stderr)
        sys.exit(1)
    print(f"  Created")

    # ── Step 2: upload chunks ─────────────────────────────────────────────────
    n_chunks = math.ceil(total / CHUNK_BYTES) if total else 1
    print(f"\n[2/2] Uploading {total:,} bytes in {n_chunks} chunk(s) ({CHUNK_BYTES} B each)...")

    offset = 0
    idx = 0
    while True:
        chunk = data[offset : offset + CHUNK_BYTES]
        is_last = 1 if (offset + len(chunk) >= total) else 0
        resp = _post(base, "/api/stream/upload", {
            "name":   args.name,
            "hex":    chunk.hex(),
            "offset": offset,
            "last":   is_last,
        }, args.token)
        idx += 1
        if resp.get("ok") != "true":
            print(f"  Chunk {idx} failed: {resp}", file=sys.stderr)
            sys.exit(1)
        print(f"  [{idx:>{len(str(n_chunks))}}/ {n_chunks}] offset={offset:>8}  "
              f"bytes={resp.get('bytes_written', 0):>5}  "
              f"final={resp.get('final')}")
        offset += len(chunk)
        if is_last or not chunk:
            break

    print(f"\nDone. '{args.name}' is NVMe-persisted and ready.")
    print(f"Download: GET {base}/api/stream/{args.name}")


if __name__ == "__main__":
    main()
