#!/usr/bin/env bash
# tests/tls_cert_oracle_smoke.sh — generate a certificate with the shipping
# kernel/tls_cert.c and let OpenSSL, not this project, say whether it is right.
#
# ─── Why this exists ───────────────────────────────────────────────────────
# Phase 3's gate is Chrome, Firefox and curl completing a handshake. That is
# the right gate and the wrong FIRST oracle: a browser reports only that the
# connection is not private and leaves you to guess which field it disliked.
#
# Nothing inside this project can tell us the DER is correct.
# mbedtls_x509write_crt_set_validity() checks a string's LENGTH and nothing
# else; set_serial_raw() is a bounds check and a memcpy. Every way of being
# wrong that keeps the length at 14 or the serial at 20 bytes passes straight
# through into a signed certificate. So the verdict has to come from a parser
# that has never seen this code.
#
# ─── Why it is smoke.sh, not *_check.sh ────────────────────────────────────
# run_checks.sh globs tests/*_check.sh and deploy.sh gates on that set. This
# needs a HOST compiler, OpenSSL, and a host build of vendor/mbedtls (~40s the
# first time, cached after). It must not sit in a deploy gate. Run it with the
# other build-host smokes.
#
# Artifacts go under $BUILD (default /tmp/aerosls-tls-oracle), never into the
# tree -- so an interrupted run leaves nothing behind for `make` to compile.
# That is not hypothetical: entropy_source_smoke.sh planted a .c file in
# kernel/ and an interrupted run left it there, on a filesystem whose rm was
# refused.
set -u

BUILD="${BUILD:-/tmp/aerosls-tls-oracle}"
CC="${CC:-gcc}"
pass=0; fail=0
ok()  { echo "ok:   $1"; pass=$((pass+1)); }
bad() { echo "FAIL: $1"; fail=$((fail+1)); }
have() { command -v "$1" >/dev/null 2>&1; }

for t in "$CC" openssl ar python3; do
    have "$t" || { echo "SKIP  tls_cert_oracle_smoke: $t not available"; exit 0; }
done
[ -d vendor/mbedtls/library ] || { echo "SKIP  tls_cert_oracle_smoke: vendor/mbedtls absent"; exit 0; }

mkdir -p "$BUILD/obj"

# ─── host mbedTLS, built with the KERNEL's config ──────────────────────────
# Not the stock config. MBEDTLS_X509_REMOVE_INFO, the fixed-pool allocator and
# the PLATFORM_*_ALT hooks all change what is compiled, and an oracle built
# against a different configuration is judging a different library.
DEFS="-DMBEDTLS_USER_CONFIG_FILE=\"sls_mbedtls_config.h\" -U_FORTIFY_SOURCE -Uunix -U__unix -U__unix__"
built=0
for c in vendor/mbedtls/library/*.c; do
    o="$BUILD/obj/$(basename "${c%.c}").o"
    [ -f "$o" ] && [ "$o" -nt "$c" ] && continue
    # shellcheck disable=SC2086
    $CC -I vendor/mbedtls/include -I vendor/mbedtls/library -I kernel $DEFS \
        -O1 -w -c "$c" -o "$o" || { echo "FAIL  compiling $c"; exit 1; }
    built=$((built+1))
done
[ "$built" -gt 0 ] && echo "      (built $built mbedTLS objects; cached in $BUILD)"
ar rcs "$BUILD/libmbedtls_host.a" "$BUILD"/obj/*.o

# SLS_BUILD_EPOCH is pinned one day below the oracle's clock, NOT taken from
# git. rtc_set_unix() refuses any time before the build epoch -- correctly, it
# is how a typo'd year gets rejected -- so using the real build epoch would
# make this harness start failing the moment a commit landed after its fixed
# clock, which is exactly what happened on the first run. Those refusal rules
# are rtc_host_test.c's subject; here the clock has to be deterministic or the
# expected notBefore/notAfter below drift with every commit.
EPOCH=1786406400   # 2026-08-11T00:00:00Z, one day before ORACLE_NOW
$CC -Wall -Wextra -std=c11 -DSLS_BUILD_EPOCH=${EPOCH}ull \
    -I kernel -I net -I . -I vendor/mbedtls/include -o "$BUILD/oracle" \
    tests/tls_cert_oracle.c tests/tls_cert_oracle_stubs.c \
    kernel/tls_cert.c kernel/rtc.c kernel/tls_platform.c kernel/stubs.c \
    "$BUILD/libmbedtls_host.a" || { echo "FAIL  linking the oracle"; exit 1; }

CRT="$BUILD/crt.der"; KEY="$BUILD/key.der"
rm -f "$CRT" "$KEY"
"$BUILD/oracle" --crt "$CRT" --key "$KEY" || { echo "FAIL  generation failed"; exit 1; }

echo
echo "=== what OpenSSL ($(openssl version | cut -d' ' -f1-2)) makes of it ==="
TXT="$(openssl x509 -inform DER -in "$CRT" -noout -text)"
echo "$TXT" | sed 's/^/    /'
echo

grep -q 'Signature Algorithm: ecdsa-with-SHA256' <<<"$TXT" && ok "signed ecdsa-with-SHA256" || bad "wrong signature algorithm"
grep -q 'NIST CURVE: P-256'                      <<<"$TXT" && ok "P-256 public key"          || bad "wrong curve"
grep -q 'CA:FALSE'                               <<<"$TXT" && ok "basicConstraints CA:FALSE" || bad "CA flag wrong"
grep -q 'X509v3 Subject Key Identifier'          <<<"$TXT" && ok "subject key identifier present" || bad "no SKI"

# ─── validity: the backdate is only visible from outside ───────────────────
# The oracle sets the clock to 2026-08-12T00:00:00Z. notBefore must be the day
# before -- that is TLS_CERT_BACKDATE_SECONDS, and nothing inside the kernel
# can show it applied.
grep -q 'Not Before: Aug 11 00:00:00 2026 GMT' <<<"$TXT" \
    && ok "notBefore is backdated a full day from the clock (Aug 12 -> Aug 11)" \
    || bad "notBefore is not backdated: $(grep 'Not Before' <<<"$TXT" | tr -s ' ')"
grep -q 'Not After : Nov 10 00:00:00 2026 GMT' <<<"$TXT" \
    && ok "notAfter is 90 days out" \
    || bad "notAfter wrong: $(grep 'Not After' <<<"$TXT" | tr -s ' ')"

# ─── the SAN, which is what the Phase 3 gate turns on ──────────────────────
# Chrome has ignored commonName since Chrome 58. Text alone is not enough: an
# IP written as a dotted STRING would still print as something, so check the
# DER tag. [7] with four raw bytes is an iPAddress; anything else is not.
# Three names, because a verifier checks the one the CLIENT used. A node is
# reached as localhost through run-cluster.sh's port forward; 10.0.2.15 is
# slirp's guest address and is only meaningful inside QEMU. The first live
# handshake shipped with the guest address alone and passed only because
# curl -k skips verification.
grep -q 'DNS:localhost'        <<<"$TXT" && ok "SAN carries DNS:localhost"      || bad "no DNS:localhost in SAN"
grep -q 'IP Address:127.0.0.1' <<<"$TXT" && ok "SAN carries IP Address:127.0.0.1" || bad "no loopback IP in SAN"
grep -q 'IP Address:10.0.2.15' <<<"$TXT" && ok "SAN carries the slirp guest address" || bad "no guest IP in SAN"
python3 - "$CRT" <<'PY' && ok "both IPs are context tag [7] with four RAW bytes, not dotted strings" || bad "an iPAddress is not encoded as [7] + 4 bytes"
import sys
d = open(sys.argv[1], 'rb').read()
want = [bytes([0x87, 0x04, 127, 0, 0, 1]), bytes([0x87, 0x04, 10, 0, 2, 15])]
sys.exit(0 if all(d.find(w) >= 0 for w in want) else 1)
PY

# ─── the name check a verifier actually performs ───────────────────────────
# grep proves the string is in the certificate. It does not prove a verifier
# accepts it -- and accepting the name the CLIENT used is the entire job of a
# SAN. openssl -verify_hostname / -verify_ip run the real matcher, and the
# negative cases are what make the positives mean anything: a test that only
# ever asserts success cannot tell you it is capable of failing.
openssl x509 -inform DER -in "$CRT" -out "$BUILD/crt.pem" 2>/dev/null
name_ok=1
for h in localhost; do
    openssl verify -CAfile "$BUILD/crt.pem" -partial_chain -verify_hostname "$h" \
        "$BUILD/crt.pem" >/dev/null 2>&1 || { name_ok=0; echo "      (rejected DNS $h)"; }
done
for a in 127.0.0.1 10.0.2.15; do
    openssl verify -CAfile "$BUILD/crt.pem" -partial_chain -verify_ip "$a" \
        "$BUILD/crt.pem" >/dev/null 2>&1 || { name_ok=0; echo "      (rejected IP $a)"; }
done
[ "$name_ok" = 1 ] && ok "a verifier accepts localhost, 127.0.0.1 and 10.0.2.15" \
                   || bad "a verifier rejected a name this node is reached by"

if openssl verify -CAfile "$BUILD/crt.pem" -partial_chain -verify_hostname evil.example \
       "$BUILD/crt.pem" >/dev/null 2>&1; then
    bad "a verifier accepted evil.example -- the name check is not being applied"
elif openssl verify -CAfile "$BUILD/crt.pem" -partial_chain -verify_ip 8.8.8.8 \
         "$BUILD/crt.pem" >/dev/null 2>&1; then
    bad "a verifier accepted 8.8.8.8 -- the name check is not being applied"
else
    ok "and rejects a name that is not in the SAN (so the check above can fail)"
fi

# ─── the serial ────────────────────────────────────────────────────────────
# mbedTLS prepends 0x00 when the top bit is set, silently making 21 octets --
# one over the RFC 5280 ceiling set_serial_raw() itself enforces.
python3 - "$CRT" <<'PY' && ok "serial is a 20-octet INTEGER, positive and minimally encoded" || bad "serial encoding wrong"
import sys
d = open(sys.argv[1], 'rb').read()
i = d.find(b'\x02\x14')                    # INTEGER, length 20
if i < 0: sys.exit(1)
first = d[i+2]
sys.exit(0 if (first & 0x80) == 0 and first != 0 else 1)
PY

# ─── does it verify, the way curl --cacert verifies ────────────────────────
openssl x509 -inform DER -in "$CRT" -out "$BUILD/crt.pem" 2>/dev/null
if openssl verify -CAfile "$BUILD/crt.pem" -partial_chain "$BUILD/crt.pem" >/dev/null 2>&1; then
    ok "verifies as its own trust anchor (the curl --cacert path)"
else
    bad "openssl verify rejected it"
fi

a="$(openssl pkey -inform DER -in "$KEY" -pubout -outform DER 2>/dev/null | openssl dgst -sha256)"
b="$(openssl x509 -inform DER -in "$CRT" -pubkey -noout 2>/dev/null | openssl pkey -pubin -pubout -outform DER | openssl dgst -sha256)"
[ -n "$a" ] && [ "$a" = "$b" ] && ok "the emitted private key matches the certificate's public key" \
                               || bad "key does not match the certificate"

# ─── fail closed ───────────────────────────────────────────────────────────
# This proves the SYSTEM refuses, not which part of it refused. Mutation
# testing showed why that distinction matters: deleting the return check in
# tls_cert_make_serial() -- the exact bug that would publish uninitialised
# stack as a serial -- does NOT fail this assertion, because key generation
# and signing call sls_mbedtls_rng() and refuse a moment later. The smoke
# stays green while a real disclosure bug is present.
#
# tests/tls_cert_host_test.c covers that specifically, and its mutation run
# catches it. Do not read a green line here as covering the serial path.
# Regenerate afterwards: the fail-closed case below deletes the certificate on
# purpose, and leaving the run with no artefact means the first thing anyone
# does after a green run -- look at what it actually produced -- fails.
cp "$CRT" "$BUILD/crt.last.der" 2>/dev/null
rm -f "$CRT"
if "$BUILD/oracle" --no-entropy --crt "$CRT" --key "$KEY" >/dev/null 2>&1; then
    bad "generation SUCCEEDED with entropy refusing"
elif [ -e "$CRT" ]; then
    bad "entropy refused but a certificate file was written anyway"
else
    ok "entropy refusing means no certificate, and nothing written"
fi

mv -f "$BUILD/crt.last.der" "$CRT" 2>/dev/null

echo
echo "      certificate left at $CRT for inspection:"
echo "        openssl x509 -inform DER -in $CRT -noout -text"
echo "---- passed=$pass failed=$fail"
[ "$fail" -eq 0 ] || exit 1
