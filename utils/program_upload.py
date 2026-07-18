#!/usr/bin/env python3
"""
Upload a local binary/ELF file as an AeroSLS PROGRAM object.

Flow:
  1. POST /api/program/create  — allocate the named PROGRAM object
  2. POST /api/program/upload  — send file bytes as chunked hex, 4 KiB at a time

Usage:
  python3 program_upload.py --file ./myapp --name myapp [options]

Options:
  --host   HOST:PORT   Kernel API address (default: localhost:3001)
  --token  TOKEN       Bearer auth token  (default: dave@gridworkz.com DB_ADMIN token)
  --name   NAME        PROGRAM object name to create in the kernel
  --file   PATH        Local file to upload (ELF or flat binary)
  --pages  N           Virtual address pages to reserve (default: auto from file size)

After a successful upload the object status becomes "ready" and can be spawned:
  POST /api/program/spawn  {"name": "<name>"}
"""

import argparse
import json
import math
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


# ─── main ─────────────────────────────────────────────────────────────────────

def main() -> None:
    parser = argparse.ArgumentParser(
        description="Upload a binary/ELF file as an AeroSLS PROGRAM object",
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument("--file",  required=True, metavar="PATH",
                        help="Local binary/ELF file to upload")
    parser.add_argument("--name",  required=True, metavar="NAME",
                        help="PROGRAM object name in the kernel")
    parser.add_argument("--host",  default="localhost:3001", metavar="HOST:PORT",
                        help="Kernel API address (default: localhost:3001)")
    parser.add_argument("--token", default=DEFAULT_TOKEN, metavar="TOKEN",
                        help="Bearer auth token (default: dave@gridworkz.com)")
    parser.add_argument("--pages", type=int, default=0, metavar="N",
                        help="Virtual pages to allocate (default: derived from file size)")
    args = parser.parse_args()

    base = _base_url(args.host)

    # Read file
    try:
        with open(args.file, "rb") as fh:
            data = fh.read()
    except OSError as exc:
        print(f"Cannot read '{args.file}': {exc}", file=sys.stderr)
        sys.exit(1)

    total = len(data)
    pages = args.pages or max(4, math.ceil(total / 4096))

    # Detect ELF
    is_elf = data[:4] == b"\x7fELF"
    fmt = "ELF64" if is_elf else "flat"

    print(f"File  : {args.file} ({total:,} bytes, {fmt})")
    print(f"Object: {args.name}  pages={pages}")
    print(f"Host  : {base}")

    # ── Step 1: create ────────────────────────────────────────────────────────
    print(f"\n[1/2] Creating PROGRAM object '{args.name}'...")
    resp = _post(base, "/api/program/create", {"name": args.name, "pages": pages}, args.token)
    if resp.get("ok") != "true":
        print(f"  Failed: {resp}", file=sys.stderr)
        sys.exit(1)
    print(f"  Created — object_id={resp.get('object_id')}")

    # ── Step 2: upload chunks ─────────────────────────────────────────────────
    n_chunks = math.ceil(total / CHUNK_BYTES) if total else 1
    print(f"\n[2/2] Uploading {total:,} bytes in {n_chunks} chunk(s) ({CHUNK_BYTES} B each)...")

    offset = 0
    idx = 0
    while True:
        chunk = data[offset : offset + CHUNK_BYTES]
        is_last = 1 if (offset + len(chunk) >= total) else 0
        resp = _post(base, "/api/program/upload", {
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

    print(f"\nDone. '{args.name}' uploaded and persisted ({fmt}).")
    print(f"Spawn:  POST {base}/api/program/spawn  {{\"name\": \"{args.name}\"}}")


if __name__ == "__main__":
    main()
