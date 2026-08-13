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

CRT="$BUILD/crt.der"; KEY="$BUILD/key.der"; CA="$BUILD/ca.der"
rm -f "$CRT" "$KEY" "$CA"
"$BUILD/oracle" --crt "$CRT" --key "$KEY" --ca "$CA" || { echo "FAIL  generation failed"; exit 1; }
openssl x509 -inform DER -in "$CA" -out "$BUILD/ca.pem" 2>/dev/null
CATXT="$(openssl x509 -in "$BUILD/ca.pem" -noout -text)"

echo
echo "=== what OpenSSL ($(openssl version | cut -d' ' -f1-2)) makes of it ==="
TXT="$(openssl x509 -inform DER -in "$CRT" -noout -text)"
echo "$TXT" | sed 's/^/    /'
echo

grep -q 'Signature Algorithm: ecdsa-with-SHA256' <<<"$TXT" && ok "signed ecdsa-with-SHA256" || bad "wrong signature algorithm"
grep -q 'NIST CURVE: P-256'                      <<<"$TXT" && ok "P-256 public key"          || bad "wrong curve"
# ─── the two certificates have OPPOSITE constraints, and must ─────────────
# One certificate could not satisfy both browsers: Chrome will not anchor
# CA:FALSE, Firefox rejects CA:TRUE at the end-entity position
# (MOZILLA_PKIX_ERROR_CA_CERT_USED_AS_END_ENTITY). So the CA asserts CA:TRUE
# and signs certificates; the leaf asserts CA:FALSE and signs handshakes.
# Checking both is the only way to notice if they ever collapse back into one.
grep -q 'CA:FALSE'  <<<"$TXT"   && ok "leaf is CA:FALSE (Firefox rejects a CA at the end entity)" || bad "leaf CA flag wrong"
grep -q 'CA:TRUE'   <<<"$CATXT" && ok "CA is CA:TRUE (Chrome will not anchor anything else)"      || bad "CA flag wrong"
grep -q 'pathlen:0' <<<"$CATXT" && ok "CA pathlen 0 -- it may sign leaves, not further CAs"       || bad "CA pathlen not 0"
grep -A1 'Key Usage' <<<"$TXT"   | grep -q 'Digital Signature' && ok "leaf keyUsage is digitalSignature" || bad "leaf keyUsage wrong"
grep -A1 'Key Usage' <<<"$CATXT" | grep -q 'Certificate Sign'  && ok "CA keyUsage is keyCertSign"        || bad "CA keyUsage wrong"

# A leaf whose subject equals its issuer reads as self-signed to a path builder
# however it was signed, which is the failure this split exists to escape.
LSUB="$(openssl x509 -in "$BUILD/crt.pem" -noout -subject 2>/dev/null || openssl x509 -inform DER -in "$CRT" -noout -subject)"
LISS="$(openssl x509 -inform DER -in "$CRT" -noout -issuer)"
[ "${LSUB#subject=}" != "${LISS#issuer=}" ] && ok "the leaf's issuer is not itself" || bad "leaf is self-issued -- the split has collapsed"
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
    openssl verify -CAfile "$BUILD/ca.pem" -verify_hostname "$h" \
        "$BUILD/crt.pem" >/dev/null 2>&1 || { name_ok=0; echo "      (rejected DNS $h)"; }
done
for a in 127.0.0.1 10.0.2.15; do
    openssl verify -CAfile "$BUILD/ca.pem" -verify_ip "$a" \
        "$BUILD/crt.pem" >/dev/null 2>&1 || { name_ok=0; echo "      (rejected IP $a)"; }
done
[ "$name_ok" = 1 ] && ok "a verifier accepts localhost, 127.0.0.1 and 10.0.2.15" \
                   || bad "a verifier rejected a name this node is reached by"

if openssl verify -CAfile "$BUILD/ca.pem" -verify_hostname evil.example \
       "$BUILD/crt.pem" >/dev/null 2>&1; then
    bad "a verifier accepted evil.example -- the name check is not being applied"
elif openssl verify -CAfile "$BUILD/ca.pem" -verify_ip 8.8.8.8 \
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

# ─── the leaf verifies against the CA, which is what a browser does ────────
openssl x509 -inform DER -in "$CRT" -out "$BUILD/crt.pem" 2>/dev/null
if openssl verify -CAfile "$BUILD/ca.pem" "$BUILD/crt.pem" >/dev/null 2>&1; then
    ok "the leaf verifies against the CA (a full chain, not a partial one)"
else
    bad "openssl verify could not build leaf -> CA: $(openssl verify -CAfile "$BUILD/ca.pem" "$BUILD/crt.pem" 2>&1 | tail -1)"
fi
# And the CA alone must NOT verify the leaf if the leaf is swapped in as its
# own anchor -- that would mean the split bought nothing.
if openssl verify -CAfile "$BUILD/crt.pem" "$BUILD/crt.pem" >/dev/null 2>&1; then
    bad "the leaf still verifies as its own anchor -- it is not really issued by the CA"
else
    ok "the leaf does NOT verify standalone (it genuinely needs the CA)"
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

# ─── every pinned suite is actually in this build ──────────────────────────
# kernel/tls_server.c pins three ciphersuites. A pinned suite that is not
# compiled in is dropped silently -- the server just never offers it. ChaCha20
# is in that list specifically as the fallback for parts with no hardware AES,
# and nothing has ever negotiated it on a real node: every handshake so far
# picked AES-256. So without this the fallback's existence rested entirely on
# it being written down.
#
# The oracle reads the list from kernel/tls_server.h rather than keeping its
# own, so this cannot pass by agreeing with a stale copy of itself.
echo
echo "=== the pinned ciphersuites exist in this configuration ==="
echo
SOUT="$("$BUILD/oracle" --suites 2>&1)"; src=$?
echo "$SOUT" | grep -E "^(ok|FAIL|      )" | sed 's/^ok:   /    ok:   /; s/^FAIL: /    FAIL: /'
pass=$(( pass + $(echo "$SOUT" | grep -c '^ok:') ))
sf=$(echo "$SOUT" | grep -c '^FAIL:')
fail=$(( fail + sf ))
if [ "$src" -ne 0 ] && [ "$sf" -eq 0 ]; then
    bad "the suite check exited $src without reporting a failure"
fi

# ─── the CA outliving the boot that made it ────────────────────────────────
# Everything above judges ONE generation. The whole point of storing the CA is
# that a LATER boot signs a new leaf with it and the operator's import stays
# good -- and nothing above can see that, because nothing above involves a
# second boot.
#
# The oracle's --persist mode makes a CA, moves its clock forward twice, and
# signs a leaf at each stop. It prints its own ok:/FAIL: lines for the return
# codes (OpenSSL cannot be asked "this call must fail, with THIS code"); they
# are folded into this script's tally below so a failure there fails here.
# The DER it leaves behind is judged by OpenSSL, as everything else is.
echo
echo "=== the CA across three boots ==="
P="$BUILD/persist"
rm -rf "$P"; mkdir -p "$P"
POUT="$("$BUILD/oracle" --persist "$P" 2>&1)"; prc=$?
echo "$POUT" | grep -E '^(ok|FAIL):' | sed 's/^/    /'
pass=$(( pass + $(echo "$POUT" | grep -c '^ok:') ))
fail=$(( fail + $(echo "$POUT" | grep -c '^FAIL:') ))
if [ "$prc" -ne 0 ] && [ "$(echo "$POUT" | grep -c '^FAIL:')" -eq 0 ]; then
    # Exited non-zero with no FAIL line: it died before it could report.
    echo "$POUT" | sed 's/^/    /'
    bad "the persist-mode oracle exited $prc without reporting"
fi

if [ -s "$P/p_ca.der" ] && [ -s "$P/p_leaf1.der" ] && [ -s "$P/p_leaf2.der" ]; then
    openssl x509 -inform DER -in "$P/p_ca.der"      -out "$P/ca.pem"   2>/dev/null
    openssl x509 -inform DER -in "$P/p_leaf1.der"   -out "$P/l1.pem"   2>/dev/null
    openssl x509 -inform DER -in "$P/p_leaf2.der"   -out "$P/l2.pem"   2>/dev/null
    openssl x509 -inform DER -in "$P/p_clamped.der" -out "$P/lc.pem"   2>/dev/null

    # The one that matters: an operator imported p_ca.der once. Both leaves --
    # thirty simulated days apart -- must verify against that same import.
    # -attime, because these leaves are dated thirty and sixty days from the
    # oracle's clock and OpenSSL judges against the real one -- without it both
    # verifies fail with "certificate is not yet valid", which is true and has
    # nothing to do with what is being tested. The instants come from the
    # oracle itself so the shell cannot drift from what was signed.
    T2="$(echo "$POUT" | sed -n 's/^attime-boot2: //p')"
    T3="$(echo "$POUT" | sed -n 's/^attime-boot3: //p')"
    if [ -z "$T2" ] || [ -z "$T3" ]; then
        bad "the oracle did not report its simulated boot times"
        T2=0; T3=0
    fi
    openssl verify -attime "$T2" -CAfile "$P/ca.pem" -verify_hostname localhost "$P/l1.pem" >/dev/null 2>&1 \
        && ok "boot 2's leaf verifies against the stored CA" \
        || bad "boot 2's leaf does NOT verify against the stored CA"
    openssl verify -attime "$T3" -CAfile "$P/ca.pem" -verify_hostname localhost "$P/l2.pem" >/dev/null 2>&1 \
        && ok "boot 3's leaf verifies against the SAME stored CA -- the import held" \
        || bad "boot 3's leaf does not verify against the stored CA"

    # The negative that keeps the two above honest: a leaf must NOT verify
    # against a CA it was not signed by. Without this, "verify succeeded" could
    # mean the flags were wrong and openssl checked nothing.
    openssl verify -attime "$T2" -CAfile "$P/l2.pem" "$P/l1.pem" >/dev/null 2>&1 \
        && bad "a leaf verified against something that is not its issuer" \
        || ok "  and does not verify against a CA that did not sign it"

    # Different serials, different keys. If these ever matched, the leaf would
    # be being reused across boots and the fresh-key-per-boot claim would be
    # false -- which is the sort of thing that stays true in a comment long
    # after it stops being true in the code.
    s1="$(openssl x509 -in "$P/l1.pem" -noout -serial)"
    s2="$(openssl x509 -in "$P/l2.pem" -noout -serial)"
    [ "$s1" != "$s2" ] && ok "the two leaves carry different serials" \
                       || bad "the two leaves share a serial"
    k1="$(openssl x509 -in "$P/l1.pem" -noout -pubkey)"
    k2="$(openssl x509 -in "$P/l2.pem" -noout -pubkey)"
    [ "$k1" != "$k2" ] && ok "and different public keys -- a new key each boot" \
                       || bad "the two leaves share a public key"

    # The clamp, in dates rather than return codes. A leaf that asked for ten
    # years from a five-year CA must expire exactly when its issuer does.
    cna="$(openssl x509 -in "$P/ca.pem" -noout -enddate)"
    lna="$(openssl x509 -in "$P/lc.pem" -noout -enddate)"
    [ "$cna" = "$lna" ] \
        && ok "an over-long leaf is clamped to the CA's exact notAfter (${cna#notAfter=})" \
        || bad "leaf notAfter ($lna) is not the CA's ($cna) -- the clamp did not hold"

    # And an ordinary leaf must land INSIDE the issuer, not on its boundary --
    # otherwise the check above would pass for the wrong reason.
    l1na="$(openssl x509 -in "$P/l1.pem" -noout -enddate)"
    [ "$l1na" != "$cna" ] \
        && ok "a normal leaf expires before the CA, so the clamp above meant something" \
        || bad "every leaf is landing on the CA's notAfter -- the clamp test proves nothing"
else
    bad "the persist-mode oracle wrote no certificates"
fi

echo
echo "      certificate left at $CRT for inspection:"
echo "        openssl x509 -inform DER -in $CRT -noout -text"
echo "---- passed=$pass failed=$fail"
[ "$fail" -eq 0 ] || exit 1
