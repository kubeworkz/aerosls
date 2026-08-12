#!/usr/bin/env bash
# tests/tls_gate_evidence.sh — capture what a Phase 3 handshake actually
# negotiated, and compare two captures.
#
# ─── Why capture rather than assert ────────────────────────────────────────
# The Phase 3 gate is Chrome, Firefox and curl completing a handshake. A gate
# is a yes/no, and yes/no is exactly what this project has repeatedly found is
# not enough: stack_frame_budget_check.sh said "109 files, all within budget"
# while scanning none of mbedTLS, and 31 host tests passed against an entropy
# subsystem that was inert in the booted kernel. Both were green. Both were
# green for the wrong reason.
#
# So this records the parameters rather than judging them, and one thing it
# does judge is the only judgement a browser cannot make.
#
# ─── The serial comparison is the point ────────────────────────────────────
# A browser completes a handshake against a node whose CSPRNG is broken and
# shows a padlock. Debian, Netscape and ROCA all shipped correct protocol
# implementations that interoperated with everything. Two boots producing the
# SAME certificate serial is the visible symptom of that failure, it costs one
# comparison, and no protocol test substitutes for it (§0, §2.5 test 3).
#
#   tests/tls_gate_evidence.sh --tls localhost:8444 --out /tmp/boot1.txt
#   ...reboot the node...
#   tests/tls_gate_evidence.sh --tls localhost:8444 --out /tmp/boot2.txt
#   tests/tls_gate_evidence.sh --compare /tmp/boot1.txt /tmp/boot2.txt
#
# ─── What --cacert here does and does not establish ────────────────────────
# The payload fetch uses the certificate THE SERVER JUST PRESENTED as its own
# CA. That verifies the chain is self-consistent and that the name matches --
# it does NOT establish trust, because the anchor came from the thing being
# checked. Trust is the browser's decision and only the browser can make it.
# Stated because a green line here would otherwise read as more than it is.
set -u

pass=0; fail=0
ok()  { echo "ok:   $1"; pass=$((pass+1)); }
bad() { echo "FAIL: $1"; fail=$((fail+1)); }
note(){ echo "      $1"; }

usage() {
    echo "usage: $0 --tls HOST:PORT [--plain HOST:PORT] [--path /] [--out FILE]"
    echo "       $0 --compare FILE_A FILE_B"
    exit 2
}

# ─── compare mode ──────────────────────────────────────────────────────────
if [ "${1:-}" = "--compare" ]; then
    A="${2:-}"; B="${3:-}"
    [ -r "$A" ] && [ -r "$B" ] || usage
    echo "=== two captures compared ==="
    sa="$(awk -F': ' '/^serial: /{print $2}' "$A")"
    sb="$(awk -F': ' '/^serial: /{print $2}' "$B")"
    note "A serial: ${sa:-<none>}"
    note "B serial: ${sb:-<none>}"
    if [ -z "$sa" ] || [ -z "$sb" ]; then
        bad "one of the captures has no serial -- it did not complete"
    elif [ "$sa" = "$sb" ]; then
        bad "*** IDENTICAL SERIALS ACROSS TWO CAPTURES ***
        Serials come from entropy_get(). If these are two separate boots,
        the CSPRNG is producing the same bytes every time and every key this
        node generates is predictable. Every client still sees a padlock.
        This is the Debian OpenSSL failure and it is invisible to the
        protocol. Run tests/entropy_boot_diversity_check.sh."
    else
        ok "serials differ between captures (entropy is not stuck)"
    fi
    ka="$(awk -F': ' '/^pubkey_sha256: /{print $2}' "$A")"
    kb="$(awk -F': ' '/^pubkey_sha256: /{print $2}' "$B")"
    if [ -n "$ka" ] && [ "$ka" = "$kb" ]; then
        bad "*** IDENTICAL PUBLIC KEYS ACROSS TWO CAPTURES ***
        Worse than a repeated serial: the KEY PAIR is the same. If these are
        separate boots, every node built from this image shares one private
        key."
    elif [ -n "$ka" ]; then
        ok "public keys differ between captures"
    fi
    echo; echo "---- passed=$pass failed=$fail"
    [ "$fail" -eq 0 ] || exit 1
    exit 0
fi

# ─── capture mode ──────────────────────────────────────────────────────────
TLS=""; PLAIN=""; OUT=""; PATH_="/"
while [ $# -gt 0 ]; do
    case "$1" in
        --tls)   TLS="${2:-}"; shift 2 ;;
        --plain) PLAIN="${2:-}"; shift 2 ;;
        --out)   OUT="${2:-}"; shift 2 ;;
        --path)  PATH_="${2:-}"; shift 2 ;;
        *) usage ;;
    esac
done
[ -n "$TLS" ] || usage
for t in openssl curl sha256sum; do
    command -v "$t" >/dev/null 2>&1 || { echo "SKIP  tls_gate_evidence: $t not available"; exit 0; }
done

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

echo "=== handshake against $TLS ==="
# -alpn is not optional here: s_client offers nothing by default, so a server
# that selects http/1.1 correctly still reports "<none negotiated>" and the
# capture cannot tell that apart from a server with no ALPN at all.
if ! openssl s_client -connect "$TLS" -showcerts -alpn http/1.1 \
        </dev/null >"$WORK/hs.txt" 2>&1; then
    bad "no handshake -- $(grep -m1 -i 'error\|refused\|unable' "$WORK/hs.txt" || echo 'see below')"
    sed 's/^/      /' "$WORK/hs.txt" | head -12
    echo; echo "---- passed=$pass failed=$fail"; exit 1
fi
openssl x509 -in "$WORK/hs.txt" -out "$WORK/crt.pem" 2>/dev/null \
    || { bad "the peer sent no parseable certificate"; exit 1; }
ok "handshake completed and a certificate was presented"

# Negotiated parameters, verbatim rather than interpreted.
# Two sources, because the SSL-Session summary block is not always emitted --
# a server that closes promptly after responding can leave s_client printing
# only the one-line "New, TLSv1.3, Cipher is ..." banner. Parsing only the
# block reported version and cipher as "unknown" against the real node while
# working perfectly against the openssl s_server this script was developed
# against: a parser tuned to a specimen the test itself created. That is the
# same mistake tls_cert_oracle_smoke.sh made asserting its own SAN instead of
# the kernel's, and it hid exactly the field the design doc wanted settled.
VER="$(awk -F': ' '/^ *Protocol *:/{print $2; exit}' "$WORK/hs.txt")"
CIPH="$(awk -F': ' '/^ *Cipher *:/{print $2; exit}' "$WORK/hs.txt")"
if [ -z "$VER" ] || [ -z "$CIPH" ]; then
    NEW="$(grep -m1 '^New, ' "$WORK/hs.txt")"
    [ -n "$NEW" ] && {
        [ -n "$VER"  ] || VER="$(echo "$NEW"  | sed -E 's/^New, ([^,]+),.*/\1/')"
        [ -n "$CIPH" ] || CIPH="$(echo "$NEW" | sed -E 's/.*Cipher is (.*)$/\1/')"
    }
fi
GROUP="$(awk -F': ' '/Negotiated TLS1.3 group:/{print $2; exit}' "$WORK/hs.txt")"
[ -n "$GROUP" ] || GROUP="$(awk -F': ' '/^Server Temp Key:/{print $2; exit}' "$WORK/hs.txt")"
ALPN="$(awk -F': ' '/^ALPN protocol:/{print $2; exit}' "$WORK/hs.txt")"
note "version : ${VER:-unknown}"
note "cipher  : ${CIPH:-unknown}"
note "group   : ${GROUP:-<not reported by this openssl>}"
note "alpn    : ${ALPN:-<none negotiated>}"

SERIAL="$(openssl x509 -in "$WORK/crt.pem" -noout -serial | cut -d= -f2)"
SUBJ="$(openssl x509 -in "$WORK/crt.pem" -noout -subject | sed 's/^subject=//')"
NB="$(openssl x509 -in "$WORK/crt.pem" -noout -startdate | cut -d= -f2)"
NA="$(openssl x509 -in "$WORK/crt.pem" -noout -enddate | cut -d= -f2)"
SAN="$(openssl x509 -in "$WORK/crt.pem" -noout -ext subjectAltName 2>/dev/null | tail -n +2 | tr -d ' ' | tr '\n' ' ')"
PUBSHA="$(openssl x509 -in "$WORK/crt.pem" -pubkey -noout | openssl dgst -sha256 | awk '{print $NF}')"
note "serial  : $SERIAL"
note "subject : $SUBJ"
note "validity: $NB  ->  $NA"
note "san     : ${SAN:-<NONE>}"

if [ -n "$VER" ] && [ -n "$CIPH" ] && [ "$VER" != "unknown" ]; then
    ok "negotiated parameters captured ($VER / $CIPH)"
else
    bad "could not read the negotiated version or cipher -- the capture is
        missing the field it exists to record"
fi

[ -n "$SAN" ] && ok "the certificate carries a subjectAltName" \
              || bad "NO subjectAltName -- Chrome has ignored commonName since Chrome 58"

# The certificate is presented as its own CA: self-consistency and a name
# match, NOT trust. See the header.
HOSTONLY="${TLS%%:*}"
if curl -sS --cacert "$WORK/crt.pem" "https://$TLS$PATH_" -o "$WORK/tls.body" 2>"$WORK/curl.err"; then
    ok "curl verified the chain and the name '$HOSTONLY', and fetched the body"
else
    bad "curl --cacert failed: $(head -1 "$WORK/curl.err")"
fi
TLSSHA=""
[ -s "$WORK/tls.body" ] && TLSSHA="$(sha256sum <"$WORK/tls.body" | awk '{print $1}')"
note "body    : ${TLSSHA:-<empty>} ($(wc -c <"$WORK/tls.body" 2>/dev/null || echo 0) bytes)"
[ -s "$WORK/tls.body" ] && ok "the body is non-empty" \
                        || bad "empty body -- a completed handshake serving nothing is a passing handshake and a failing gate"

# The payload comparison. A handshake that completes and then serves a
# truncated body is the failure this catches, and it needs a second opinion
# from a path with no TLS in it.
if [ -n "$PLAIN" ]; then
    if curl -sS "http://$PLAIN$PATH_" -o "$WORK/plain.body" 2>/dev/null; then
        PSHA="$(sha256sum <"$WORK/plain.body" | awk '{print $1}')"
        note "plain   : $PSHA ($(wc -c <"$WORK/plain.body") bytes)"
        [ "$TLSSHA" = "$PSHA" ] && ok "the TLS body is byte-identical to the plaintext body" \
                                || bad "TLS and plaintext bodies DIFFER -- the record layer is losing or altering bytes"
    else
        note "plaintext fetch failed; skipping the payload comparison"
    fi
fi

if [ -n "$OUT" ]; then
    {
        echo "host: $TLS"
        echo "path: $PATH_"
        echo "version: ${VER:-unknown}"
        echo "cipher: ${CIPH:-unknown}"
        echo "group: ${GROUP:-unknown}"
        echo "alpn: ${ALPN:-none}"
        echo "serial: $SERIAL"
        echo "subject: $SUBJ"
        echo "not_before: $NB"
        echo "not_after: $NA"
        echo "san: $SAN"
        echo "pubkey_sha256: $PUBSHA"
        echo "body_sha256: ${TLSSHA:-none}"
    } > "$OUT"
    note "capture written to $OUT"
    note "compare two boots with: $0 --compare A B"
fi

echo
echo "---- passed=$pass failed=$fail"
[ "$fail" -eq 0 ] || exit 1
