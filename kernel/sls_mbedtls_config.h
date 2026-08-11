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
 * ON, and routed to kernel/rtc.c through sls_mbedtls_time() in
 * kernel/tls_platform.c. Without this X.509 skips notBefore/notAfter entirely
 * and an EXPIRED CERTIFICATE VERIFIES -- which is what this config shipped
 * with for one commit, recorded here at the time so it could not be forgotten.
 *
 * There is no time_t in a freestanding build, so the type is named explicitly.
 * `long long` rather than a 32-bit type: signed 32-bit seconds overflows in
 * 2038, and a certificate-validity clock that wraps is exactly the quiet wrong
 * answer kernel/rtc.c exists to refuse.
 *
 * TIME_MACRO, not TIME_ALT. The first attempt used TIME_ALT with a runtime
 * mbedtls_platform_set_time() call, and mbedTLS's own check_config.h rejected
 * it: TIME_TYPE_MACRO and TIME_ALT are mutually exclusive (check_config.h:528),
 * because ALT keeps mbedtls_time_t as libc's time_t and a freestanding build
 * has none. The compile-time macro is the better fit here regardless -- the
 * time source is fixed at build time in a kernel, and a function pointer that
 * can be left unset is one more way to boot into a broken state. */
#define MBEDTLS_HAVE_TIME
#define MBEDTLS_HAVE_TIME_DATE
#define MBEDTLS_PLATFORM_TIME_TYPE_MACRO long long
#define MBEDTLS_PLATFORM_TIME_MACRO      sls_mbedtls_time

/* Declared here because mbedtls_time expands to it inside library sources
 * that include no header of ours. */
long long sls_mbedtls_time(long long *t);

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

/* ─── 4b. The hosted symbols the platform layer still reaches for ─────────
 * Measured, not guessed: with the config above, memory_buffer_alloc.o still
 * wants exit(), and platform_util.o wants clock_gettime()/gmtime_r()/time().
 * PLATFORM_NO_STD_FUNCTIONS removes the DEFAULTS but leaves these three hooks
 * needing a target, and a freestanding link fails on each one by name.
 *
 * EXIT_MACRO: mbedTLS calls mbedtls_exit() on unrecoverable internal state.
 * In a kernel there is nothing to exit TO, so it routes to a function that
 * says so on the console and halts rather than returning into corrupted state.
 *
 * MS_TIME_ALT: mbedtls_ms_time() is a MONOTONIC millisecond counter used for
 * timeouts -- deliberately not the same thing as the wall clock in §2, and
 * conflating them would put certificate validity on a counter that restarts
 * every boot. kernel_tick_counter is the right source. */
#define MBEDTLS_PLATFORM_EXIT_MACRO sls_mbedtls_exit
#define MBEDTLS_PLATFORM_MS_TIME_ALT

/* GMTIME_R_ALT excludes platform_util.c's whole gmtime_r block -- including
 * its #include <time.h>, which a freestanding build cannot satisfy. Nothing
 * in the linked set calls mbedtls_platform_gmtime_r (x509.c uses its own
 * mbedtls_x509_time_gmtime), so excluding it removes the symbol rather than
 * requiring a struct tm we have no header for. Verified by nm, not assumed. */
#define MBEDTLS_PLATFORM_GMTIME_R_ALT

/* ZEROIZE_ALT: platform_util.c detects "platforms known to support
 * explicit_bzero()" and calls it (line 98). On this toolchain gcc rewrites
 * that to glibc's fortified __explicit_bzero_chk, which does not exist in a
 * freestanding link -- and the detection has no idea it is building for one.
 *
 * I called this a host-gcc artefact that would not appear under the
 * cross-compiler. That was a guess stated as a finding, and the build proved
 * it wrong: it is the FIRST symbol the real linker complained about after the
 * others were routed.
 *
 * So mbedtls_platform_zeroize() becomes ours. It has one job that memset
 * cannot be trusted with -- the compiler may delete a memset over a buffer it
 * can prove is dead, which is exactly how key material survives on a stack.
 * The volatile pointer is what stops that, and it is the same construction
 * kernel/sha256.c already uses for the same reason. */
#define MBEDTLS_PLATFORM_ZEROIZE_ALT

void sls_mbedtls_exit(int status);

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

/* ─── 5b. No CPU crypto acceleration ───────────────────────────────────────
 * AES-NI (x86) and the ARMv8 crypto extensions use SSE/NEON registers. This
 * kernel is compiled -mno-sse -mno-sse2 -mno-mmx, and that is not a
 * preference: using vector registers in kernel code requires saving and
 * restoring FPU/SSE state across context switches and interrupts, and this
 * kernel has no such path. Without it, a TLS handshake interrupted mid-AES
 * would corrupt whatever else was using xmm -- silently, and only under load.
 *
 * Found by compiling: aesni.c fails with "the register 'xmm0' cannot be
 * clobbered in 'asm' for the current target". That is the toolchain refusing
 * an unsound combination rather than a build annoyance, and the fix is to turn
 * the feature off, not to relax the flag.
 *
 * The cost is real and should be measured rather than assumed: software AES-GCM
 * is several times slower than AES-NI. If that proves to matter, the answer is
 * ChaCha20-Poly1305 (already enabled, fast in pure software, and what the
 * config would prefer on a Pi anyway) -- not enabling SSE in a kernel that
 * cannot save it. */
#undef MBEDTLS_AESNI_C
#undef MBEDTLS_AESCE_C
#undef MBEDTLS_SHA256_USE_A64_CRYPTO_IF_PRESENT
#undef MBEDTLS_SHA512_USE_A64_CRYPTO_IF_PRESENT
#undef MBEDTLS_SHA256_USE_ARMV8_A_CRYPTO_IF_PRESENT
#undef MBEDTLS_SHA512_USE_ARMV8_A_CRYPTO_IF_PRESENT
#undef MBEDTLS_PADLOCK_C

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
