#ifndef SLS_MBEDTLS_CONFIG_H
#define SLS_MBEDTLS_CONFIG_H

/*
 * sls_mbedtls_config.h — what AeroSLS turns off in mbedTLS, and why.
 *
 * TLS Phase 2. Passed as MBEDTLS_USER_CONFIG_FILE, not MBEDTLS_CONFIG_FILE.
 *
 * ─── Why a USER config and not a replacement config ───────────────────────
 * MBEDTLS_CONFIG_FILE replaces mbedtls_config.h wholesale: we would own the
 * entire option set, ~4000 lines of it, and every upstream release would need
 * that set re-derived by hand. MBEDTLS_USER_CONFIG_FILE is layered on top of
 * upstream's own defaults (see build_info.h:129), so this file contains only
 * the deltas -- which is also the complete, reviewable list of every way this
 * build differs from stock. That list being short and readable is the point.
 *
 * ─── The deltas are all one shape: this kernel is not an operating system ──
 * mbedTLS's defaults assume a hosted C environment with a filesystem, BSD
 * sockets, a clock, and malloc. We have none of those in the sense it means,
 * so each #undef below removes an assumption rather than a feature.
 */

/* ─── 1. No hosted platform ────────────────────────────────────────────────
 * FS_IO wants fopen; NET_C wants BSD sockets (net/tcp.h is our transport and
 * mbedTLS never sees it -- it gets bytes through callbacks); TIMING_C wants
 * gettimeofday and, worse, spawns its own alarm handling. */
#undef MBEDTLS_FS_IO
#undef MBEDTLS_NET_C
#undef MBEDTLS_TIMING_C
#undef MBEDTLS_PSA_ITS_FILE_C
#undef MBEDTLS_PSA_CRYPTO_STORAGE_C

/* No standard library at all. Every mbedtls_calloc/free/printf/snprintf must
 * be one we supply, and with this set a missing one is a LINK error rather
 * than an accidental pull-in of a libc that is not there. */
#define MBEDTLS_PLATFORM_C
#define MBEDTLS_PLATFORM_MEMORY
#define MBEDTLS_PLATFORM_NO_STD_FUNCTIONS

/* ─── 2. Time ──────────────────────────────────────────────────────────────
 * MBEDTLS_HAVE_TIME off for now, deliberately, and this is a REAL limitation
 * rather than a tidy-up: with it off, X.509 does not check notBefore/notAfter,
 * so an expired certificate verifies. kernel/rtc.c exists precisely to fix
 * this and is tested (50 checks), but wiring it means MBEDTLS_PLATFORM_TIME_ALT
 * plus an mbedtls_time_t shim, which is its own commit with its own test.
 *
 * Recorded here, in the file that causes it, so the gap is discoverable from
 * the code rather than only from a document: TLS built with this config has
 * NO CERTIFICATE EXPIRY CHECKING. Do not ship it. */
#undef MBEDTLS_HAVE_TIME
#undef MBEDTLS_HAVE_TIME_DATE

/* ─── 3. Entropy ───────────────────────────────────────────────────────────
 * No /dev/urandom, no CryptGenRandom. mbedtls_hardware_poll() in
 * kernel/tls_platform.c is the only source, and it routes to entropy.c's
 * fail-closed DRBG (see kernel/entropy.h).
 *
 * NO_PLATFORM_ENTROPY without HARDWARE_ALT would leave mbedTLS with no source
 * whatsoever and it would refuse to initialise -- which is the correct
 * behaviour and exactly why both must be set together. */
#define MBEDTLS_NO_PLATFORM_ENTROPY
#define MBEDTLS_ENTROPY_HARDWARE_ALT

/* ─── 4. Memory ────────────────────────────────────────────────────────────
 * The fixed-pool allocator, replacing the bump allocator in tls_platform.c
 * that was named PLACEHOLDER because it never frees anything.
 *
 * This is upstream's own implementation: a real free list over a
 * caller-supplied buffer, so a handshake's memory is returned when the
 * connection closes. It is what makes the pool sustainable rather than a
 * countdown to exhaustion.
 *
 * The property that mattered when mbedTLS was chosen over BearSSL survives:
 * allocation happens inside a buffer WE own, so it cannot fragment or exhaust
 * the arena the database lives in. Worst case is a refused handshake. That is
 * a reduction of BearSSL's no-malloc guarantee, not an equal -- design doc
 * amendment, §"Costs". */
#define MBEDTLS_MEMORY_BUFFER_ALLOC_C

/* ─── 5. TLS 1.3 only, ephemeral only ──────────────────────────────────────
 * 1.2 is disabled: there is no legacy peer to support, both ends of the
 * cluster are ours, and the browser side speaks 1.3. Carrying 1.2 would mean
 * carrying its ciphersuite negotiation and a downgrade surface for nothing.
 *
 * EPHEMERAL alone drops every PSK path, which is code we would otherwise ship
 * and never execute. Upstream notes both PSA_CRYPTO_C and
 * SSL_KEEP_PEER_CERTIFICATE are REQUIRED by 1.3 and must stay in their default
 * enabled state -- see vendor/mbedtls/docs/architecture/tls13-support.md. */
#undef MBEDTLS_SSL_PROTO_TLS1_2
#define MBEDTLS_SSL_PROTO_TLS1_3
#define MBEDTLS_SSL_TLS1_3_KEY_EXCHANGE_MODE_EPHEMERAL_ENABLED
#undef MBEDTLS_SSL_TLS1_3_KEY_EXCHANGE_MODE_PSK_ENABLED
#undef MBEDTLS_SSL_TLS1_3_KEY_EXCHANGE_MODE_PSK_EPHEMERAL_ENABLED

/* 0-RTT stays off. It is replayable by design, the mitigation needs a clock
 * and per-request memory we do not have, and the API gives no way to tell the
 * application which bytes were replayable. */
#undef MBEDTLS_SSL_EARLY_DATA

/* Renegotiation does not exist in 1.3; DTLS is not our transport. */
#undef MBEDTLS_SSL_RENEGOTIATION
#undef MBEDTLS_SSL_PROTO_DTLS
#undef MBEDTLS_SSL_DTLS_CONNECTION_ID
#undef MBEDTLS_SSL_DTLS_ANTI_REPLAY
#undef MBEDTLS_SSL_DTLS_HELLO_VERIFY
#undef MBEDTLS_SSL_DTLS_SRTP

/* ─── 6. Debug output ──────────────────────────────────────────────────────
 * Off. mbedTLS's debug callback prints handshake internals including key
 * material at high verbosity levels, and this kernel's serial console is not
 * a private channel. */
#undef MBEDTLS_DEBUG_C

/* ─── 7. Things we do not have a use for ───────────────────────────────────
 * Not security-critical, just footprint: every one of these is code that would
 * be linked into a 220 MiB image and never called. */
#undef MBEDTLS_SELF_TEST
#undef MBEDTLS_VERSION_FEATURES
#undef MBEDTLS_PKCS12_C
#undef MBEDTLS_PKCS5_C

#endif /* SLS_MBEDTLS_CONFIG_H */
