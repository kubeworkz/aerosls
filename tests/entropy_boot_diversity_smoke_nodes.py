#!/usr/bin/env python3
"""Fake AeroSLS nodes for tests/entropy_boot_diversity_smoke.sh.

Stands up three HTTP servers shaped like /api/entropy so the guard can be
driven through every verdict without a real cluster. Not a simulator of the
kernel -- only of the four response shapes the guard must tell apart.
"""
import http.server, json, sys, threading, time

mode = sys.argv[1]
base = int(sys.argv[2])

def body(fp, ready="true"):
    return json.dumps({"ready": ready, "rdseed": "true", "rdrand": "true",
                       "jitter": "true", "pool_bits": 256, "reseed_count": 1,
                       "health_failures": 0, "rdseed_retries": 0,
                       "boot_fingerprint": fp})

def serve(port, status, payload):
    class H(http.server.BaseHTTPRequestHandler):
        def do_GET(s):
            s.send_response(status)
            s.send_header("Content-Type", "application/json")
            s.send_header("Content-Length", str(len(payload)))
            s.end_headers()
            s.wfile.write(payload.encode())
        def log_message(s, *a): pass
    # ThreadingHTTPServer, not HTTPServer: the plain server handles ONE
    # request at a time, so three nodes polled concurrently (and beside a
    # loaded host) can queue behind each other and turn a client timeout
    # into "this node never answered". Full story in
    # tests/failover_adoption_smoke_nodes.py.
    srv = http.server.ThreadingHTTPServer(("127.0.0.1", port), H)
    threading.Thread(target=srv.serve_forever, daemon=True).start()

for i in (1, 2, 3):
    if mode == "distinct":   serve(base + i, 200, body(chr(96 + i) * 64))
    elif mode == "identical":serve(base + i, 200, body("a" * 64))
    elif mode == "unseeded": serve(base + i, 200, body("", ready="false"))
    elif mode == "denied":   serve(base + i, 401, '{"error":"Unauthorized"}')
    else:                    sys.exit(f"unknown mode {mode}")

time.sleep(60)
