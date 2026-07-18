#!/usr/bin/env python3
"""
Import a CSV file into AeroSLS.

Always creates:
  STREAM  '<name>'          — raw CSV bytes, NVMe-persisted, full fidelity
  DB_TABLE '<name>__schema' — manifest: row_count, col_count, columns, source_stream

With --import-rows also creates (up to 90):
  DB_TABLE '<name>_r000', '<name>_r001', ... — one object per data row,
    one record field per CSV column; queryable via GET /api/objects/<name>_r<NNN>

Usage:
  python3 csv_import.py --file sales.csv --name sales [options]

Options:
  --host         HOST:PORT  Kernel API address (default: localhost:3001)
  --token        TOKEN      Bearer auth token  (default: dave@gridworkz.com DB_ADMIN)
  --name         NAME       Base object name (≤ 55 chars)
  --file         PATH       CSV file to import
  --delimiter    CHAR       Field delimiter (default: ,)
  --import-rows             Also create per-row DB_TABLE objects

Kernel limits (after expansion in commit 31eb5a3):
  CATALOG_MAX_OBJECTS = 128  → ~90 safe row-object slots (others used by system)
  RECORD_MAX_FIELDS   = 32   → max 32 columns per row object
  RECORD_KEY_LEN      = 64   → column headers truncated at 63 chars
  RECORD_VAL_LEN      = 256  → cell values truncated at 255 chars
"""

import argparse
import csv
import json
import math
import sys
import urllib.error
import urllib.request
from datetime import datetime, timezone

CHUNK_BYTES  = 4096
MAX_ROWS     = 90     # ~90 safe row slots (128 total minus ~38 for system objects)
MAX_COLS     = 32     # RECORD_MAX_FIELDS
MAX_KEY_LEN  = 63     # RECORD_KEY_LEN - 1
MAX_VAL_LEN  = 255    # RECORD_VAL_LEN - 1
MAX_NAME_LEN = 55     # OBJECT_NAME_LEN(64) - len("__schema")(8) - 1 null byte

DEFAULT_TOKEN     = "deadbeef01234567cafebabe76543210"
OBJ_TYPE_DB_TABLE = 1


# ─── HTTP helpers ──────────────────────────────────────────────────────────────

def _base_url(host):
    h = host.rstrip("/")
    return h if h.startswith(("http://", "https://")) else "http://" + h


def _post(base, path, payload, token):
    url  = base + path
    body = json.dumps(payload).encode()
    req  = urllib.request.Request(url, data=body, method="POST")
    req.add_header("Content-Type",  "application/json")
    req.add_header("Authorization", f"Bearer {token}")
    try:
        with urllib.request.urlopen(req, timeout=30) as r:
            return json.loads(r.read())
    except urllib.error.HTTPError as exc:
        print(f"  HTTP {exc.code}: {exc.read().decode(errors='replace')}", file=sys.stderr)
        sys.exit(1)
    except urllib.error.URLError as exc:
        print(f"  Connection error: {exc.reason}", file=sys.stderr)
        sys.exit(1)


def _valloc(base, name, token):
    """Create a DB_TABLE object. Returns object_id string or None if already exists."""
    resp = _post(base, "/api/valloc",
                 {"name": name, "type": OBJ_TYPE_DB_TABLE, "pages": 4}, token)
    return resp.get("object_id") if resp.get("ok") == "true" else None


def _insert(base, obj_name, key, value, token):
    _post(base, "/api/record",
          {"object": obj_name, "key": key[:MAX_KEY_LEN], "value": value[:MAX_VAL_LEN]},
          token)


# ─── Stream upload ─────────────────────────────────────────────────────────────

def _upload_stream(base, name, raw_bytes, token):
    resp = _post(base, "/api/stream/create", {"name": name, "mime": "text/csv"}, token)
    if resp.get("ok") != "true":
        print(f"  Stream create failed: {resp}", file=sys.stderr)
        sys.exit(1)
    total    = len(raw_bytes)
    n_chunks = max(1, math.ceil(total / CHUNK_BYTES))
    print(f"  Uploading {total:,} bytes in {n_chunks} chunk(s)...")
    offset = 0
    while True:
        chunk   = raw_bytes[offset: offset + CHUNK_BYTES]
        is_last = 1 if offset + len(chunk) >= total else 0
        _post(base, "/api/stream/upload",
              {"name": name, "hex": chunk.hex(), "offset": offset, "last": is_last},
              token)
        offset += len(chunk)
        if is_last or not chunk:
            break
    print(f"  Created — '{name}' NVMe-persisted.")


# ─── main ──────────────────────────────────────────────────────────────────────

def main():
    parser = argparse.ArgumentParser(
        description="Import a CSV file into AeroSLS (STREAM + DB_TABLE schema)",
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument("--file",        required=True, metavar="PATH",
                        help="CSV file to import")
    parser.add_argument("--name",        required=True, metavar="NAME",
                        help=f"Base object name (≤ {MAX_NAME_LEN} chars)")
    parser.add_argument("--host",        default="localhost:3001", metavar="HOST:PORT")
    parser.add_argument("--token",       default=DEFAULT_TOKEN,   metavar="TOKEN")
    parser.add_argument("--delimiter",   default=",",             metavar="CHAR",
                        help="CSV field delimiter (default: ',')")
    parser.add_argument("--import-rows", action="store_true",
                        help=f"Also create per-row DB_TABLE objects (max {MAX_ROWS})")
    args = parser.parse_args()

    base = _base_url(args.host)

    if len(args.name) > MAX_NAME_LEN:
        print(f"Error: --name must be ≤ {MAX_NAME_LEN} chars (got {len(args.name)})",
              file=sys.stderr)
        sys.exit(1)

    # ── Read file ─────────────────────────────────────────────────────────────
    try:
        with open(args.file, "rb") as fh:
            raw_bytes = fh.read()
        # Parse CSV from the decoded content (handle BOM if present)
        text = raw_bytes.decode("utf-8-sig")
        reader  = csv.DictReader(text.splitlines(), delimiter=args.delimiter)
        headers = list(reader.fieldnames or [])
        rows    = list(reader)
    except OSError as exc:
        print(f"Cannot read '{args.file}': {exc}", file=sys.stderr)
        sys.exit(1)

    n_rows = len(rows)
    n_cols = len(headers)

    print(f"File   : {args.file} ({n_rows:,} rows × {n_cols} columns, {len(raw_bytes):,} bytes)")
    print(f"Name   : {args.name}")
    print(f"Host   : {base}")
    print()

    # ── Limit warnings ────────────────────────────────────────────────────────
    if n_cols > MAX_COLS:
        print(f"Warning: {n_cols} columns > max {MAX_COLS} — extra columns skipped in row objects")
        headers = headers[:MAX_COLS]
    long_keys = [h for h in headers if len(h) > MAX_KEY_LEN]
    if long_keys:
        print(f"Warning: {len(long_keys)} column header(s) will be truncated to {MAX_KEY_LEN} chars")
    if args.import_rows and n_rows > MAX_ROWS:
        print(f"Warning: --import-rows limited to first {MAX_ROWS} of {n_rows:,} rows")
        print(f"         Full data is always available via the STREAM object.")
    if n_cols > MAX_COLS or long_keys or (args.import_rows and n_rows > MAX_ROWS):
        print()

    n_steps = 2 + (1 if args.import_rows else 0)

    # ── Step 1: STREAM upload ─────────────────────────────────────────────────
    print(f"[1/{n_steps}] Uploading raw CSV as STREAM '{args.name}'...")
    _upload_stream(base, args.name, raw_bytes, args.token)

    # ── Step 2: Schema manifest ───────────────────────────────────────────────
    schema_name = f"{args.name}__schema"
    print(f"\n[2/{n_steps}] Creating schema manifest '{schema_name}'...")
    oid = _valloc(base, schema_name, args.token)
    if oid:
        col_list = "|".join(h[:MAX_KEY_LEN] for h in headers)
        ts = datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")
        manifest = [
            ("row_count",     str(n_rows)),
            ("col_count",     str(len(headers))),
            ("columns",       col_list[:MAX_VAL_LEN]),
            ("source_stream", args.name[:MAX_VAL_LEN]),
            ("imported_at",   ts),
        ]
        for key, val in manifest:
            _insert(base, schema_name, key, val, args.token)
            print(f"  {key:<16} = {val}")
        print(f"  Created — object_id={oid}")
    else:
        print(f"  Warning: '{schema_name}' already exists — schema not updated")

    # ── Step 3: Per-row objects ───────────────────────────────────────────────
    if args.import_rows:
        import_rows = rows[:MAX_ROWS]
        total_r = len(import_rows)
        width   = len(str(total_r))
        print(f"\n[3/{n_steps}] Creating {total_r} row object(s) as DB_TABLE...")
        skipped = 0
        for idx, row in enumerate(import_rows):
            row_name = f"{args.name}_r{idx:03d}"
            oid = _valloc(base, row_name, args.token)
            if not oid:
                skipped += 1
                continue
            for header in headers:
                val = str(row.get(header, ""))
                _insert(base, row_name, header, val, args.token)
            # Progress every 10 rows and on the last one
            if (idx + 1) % 10 == 0 or idx + 1 == total_r:
                print(f"  [{idx+1:{width}d}/{total_r}] {row_name}")
        if skipped:
            print(f"  Note: {skipped} row(s) skipped (objects already exist)")
        if n_rows > MAX_ROWS:
            print(f"  Note: {n_rows - MAX_ROWS:,} row(s) beyond {MAX_ROWS} not imported.")

    # ── Summary ───────────────────────────────────────────────────────────────
    print(f"\nDone.")
    print(f"  Raw CSV  : GET {base}/api/stream/{args.name}")
    print(f"  Schema   : GET {base}/api/objects/{schema_name}")
    if args.import_rows and n_rows > 0:
        last = min(n_rows, MAX_ROWS) - 1
        print(f"  Rows     : GET {base}/api/objects/{args.name}_r000")
        if last > 0:
            print(f"           : GET {base}/api/objects/{args.name}_r{last:03d}")


if __name__ == "__main__":
    main()
