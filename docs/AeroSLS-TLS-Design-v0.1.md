# AeroSLS TLS — design and sequencing, v0.1

**Status:** Phases 0, 1 and 2 implemented — mbedTLS 3.6.7 now links into the
x86-64 kernel image. Phase 3 (a listener) is next and nothing calls the library
yet. See "Phase 2 result" below. Phase 2 library choice REVERSED — see
the amendment immediately below before reading §3.
**The image-end number has been taken** (2026-08-11) and **confirmed on the
real toolchain**: `_kernel_image_end = 0x0df88000`, 223 MiB, of which mbedTLS
is **512 KiB**. Trimming the module list is housekeeping, not urgent. See
"Image-end measurement" at the foot of this document; the request is closed.
**Phase 3's gate is two-thirds met** (2026-08-12): curl completes a verified
handshake and Chrome serves the Navigator with no interstitial. Firefox is
outstanding. See "Phase 3 gate result" at the foot.
**Decided:** port **mbedTLS 3.6 LTS** (not BearSSL, not write-our-own); private
CA with per-node certificates; first shippable version is server-side TLS
**plus** mutual TLS between nodes.

---

## AMENDMENT (2026-08-11): BearSSL cannot do TLS 1.3

**§3 recommended BearSSL for a document about TLS 1.3. BearSSL does not
implement TLS 1.3.** That recommendation was made without checking, and the
check took one page fetch.

From BearSSL's own site: TLS 1.0/1.1/1.2 only. Its TLS 1.3 roadmap is ten
steps and step 1 is *"find an existing implementation with TLS 1.3 support, for
interoperability tests"* — i.e. not begun. RSA/PSS, EdDSA, the 1.3 record
format and the 1.3 handshake are all unimplemented. Both pages are © 2018;
current version is 0.6, self-described as beta.

A second gap, also missed: **BearSSL cannot issue certificates.** It generates
key pairs, but "production of signed certificate requests and self-signed
certificates" sits under *Not Yet Implemented*. §4's private CA would have
needed certificate encoding written by us or performed off-box.

### What replaced it, and what was verified this time

**mbedTLS 3.6 LTS**, checked against `docs/architecture/tls13-support.md` and
the project README rather than recollection:

| Property | Verified |
|---|---|
| TLS 1.3, client **and** server | Yes — `MBEDTLS_SSL_PROTO_TLS1_3` |
| Suites we want | `TLS_AES_128_GCM_SHA256`, `TLS_CHACHA20_POLY1305_SHA256` |
| Groups | x25519, secp256r1, secp384r1, secp521r1 |
| 1.2 and 1.3 together, independently enableable | Yes, with version negotiation |
| Licence | Dual Apache-2.0 OR GPL-2.0-or-later — take Apache-2.0 |
| Maintenance | TrustedFirmware, security list, `SECURITY.md`, LTS branch |

Costs, stated rather than discovered later:

- **Requires `MBEDTLS_PSA_CRYPTO_C` and `MBEDTLS_SSL_KEEP_PEER_CERTIFICATE`.**
  Both must stay on for TLS 1.3, and the second means the peer certificate is
  retained per connection — RAM, on top of §3.2's record buffers.
- **`MBEDTLS_SSL_MAX_FRAGMENT_LENGTH` and `MBEDTLS_SSL_VARIABLE_BUFFER_LENGTH`
  are unsupported under 1.3.** So the per-connection buffer is fixed at full
  record size; §3.2's memory budget gets worse, not better, and the connection
  limit must be enforced rather than discovered.
- **It wants an allocator.** This was the strongest argument for BearSSL and
  it is not fully neutralised, only reduced: mbedTLS can be given a fixed pool
  rather than a general heap, which keeps TLS allocation out of the database's
  arena. That has to be configured deliberately and verified, not assumed.
- `MBEDTLS_SSL_TLS1_3_KEY_EXCHANGE_MODE_EPHEMERAL_ENABLED` alone drops all PSK
  code, which is the configuration to start from for footprint.

**Vendor the 3.6 LTS release tarball, not 4.0 and not `development`.** 4.0
splits crypto into a separate TF-PSA-Crypto submodule repository; the
development branch requires Python and Perl to generate source files that
release tarballs already contain. 3.6 LTS carries only the framework submodule.

### The rule this cost

§8.1 listed "BearSSL is still the right choice" as an assumption to challenge,
and named the right risk — a quiet upstream — while missing a larger and
entirely checkable one: whether the library implements the protocol the
document is about. **An assumption that can be settled by reading the
project's own front page is not an assumption. It is something nobody
checked.**

Everything in §§0-2 (entropy) and §1.1 (the clock) is unaffected: those were
about the kernel, not the library, and both are implemented and tested.
**Window:** this precedes all other roadmap work (see
`AeroSLS-Roadmap-2026H2-v0.1.md` §2, which recommended deferring this; that
recommendation is superseded and the roadmap extends to 12 months).

---

## Phase 2 result, 2026-08-11: the library links

The whole of mbedTLS 3.6.7 compiles and links into the bare-metal image. Five
platform hooks are supplied by kernel/tls_platform.c — randomness, memory,
time, zeroization, transport — and the TCP bridge is written, tested and
called by nothing yet.

### What the link cost, and what it taught

Eleven rounds of undefined symbols. Worth recording because the pattern was
consistent and is the reusable part:

**Three were ROOT CAUSES that each explained several symbols.**

- `_FORTIFY_SOURCE=2` is on by default in this toolchain, so gcc rewrote
  memcpy/memset/memmove into `__memcpy_chk` and friends. That is four symbols
  from one cause — and it is also what produced `__explicit_bzero_chk` several
  commits earlier, which was "fixed" with `ZEROIZE_ALT`. Right mechanism,
  wrong disease; the ALT is worth keeping anyway.
- `unix`/`__unix`/`__unix__` made mbedTLS believe it was building for Unix, so
  `psa_crypto_random.c` wanted `getpid()` for fork protection. A kernel never
  forks. `-U` removed the feature and the symbol together.
- `MBEDTLS_X509_REMOVE_INFO` deleted the certificate pretty-printers, which
  were the only reason most of the X.509 modules wanted `snprintf`.

**Four were genuinely missing primitives**, each added because the linker
named it *and its call site*: `memmove` (X.509 builds DER back-to-front and
shifts it — overlapping, and `memcpy` is undefined there), `strncpy`, `strstr`,
`strchr`, plus `libgcc` for `__udivti3` (bignum divides `unsigned __int128`;
that helper belongs to the compiler, not to any C file).

**Two were mine.** `mbedtls_platform_gmtime_r` was excluded on the strength of
an `nm` run over a three-file set that did not contain x509.c — a real
measurement over the wrong population. And `sls_snprintf` collided with an
existing symbol in the QEMU-SLS layer, which was the lucky outcome: that one
ignores its format and writes `<nofmt>`, which would have put that literal into
certificate subject names had the link resolved silently.

### Measured, not assumed

- **Frame sizes are fine.** Not one module in bignum, ecp, ssl_tls, ssl_msg,
  the TLS 1.3 handshake code, x509_crt or psa_crypto produces a frame warning
  at 16 KB, let alone the 256 KB advisory. mbedTLS is written for embedded
  targets and it shows. I predicted otherwise.
- **`tests/stack_frame_budget_check.sh` now scans the vendored sources**, and
  only the ones actually linked. It previously reported "109 files scanned,
  all within budget" while scanning none of mbedTLS.

### Owed

- ~~**Trim the module list.**~~ **Answered, and the answer is no.** The
  instrument has now been read: linking all 107 modules costs **512 KiB** of a
  **223 MiB** image — 0.22%. See "Image-end measurement" below. This stays on
  the list as tidiness, not as a task with a deadline, and it should not be
  allowed to block Phase 3. What the number does *not* retire is §3.2's
  per-connection RAM, which is a different quantity that trimming would not
  improve either.
- **Collapse the two snprintfs.** `sls_tls_snprintf` could replace the TCG
  stub, whose own comment says a real one is the fix.
- **The ARM64 entropy gate** (§2.5), still unrun on hardware with no RNG
  instruction.

---

## 0. The one thing to take from this document

**Entropy is the project. TLS is the easy part.**

A TLS stack built on a broken random number generator handshakes correctly,
interoperates with curl and every browser, passes every functional test anyone
would think to write, and provides no security whatsoever. It fails *silently*
and it fails *completely*.

This is not a theoretical concern:

- **Debian OpenSSL, 2006-2008.** A maintainer removed two lines that seeded the
  pool, on the advice of a memory-checker warning. Every key generated on
  Debian and Ubuntu for two years came from a space of 32,767 possibilities.
  Everything worked perfectly for the entire period.
- **Netscape, 1995.** Seeded from time-of-day and process IDs. Broken by two
  graduate students in a few hours. SSL itself was fine.
- **ROCA, 2017.** A flawed prime-generation routine in Infineon smartcards.
  Certificates validated, signatures verified, keys were factorable. Seventeen
  years in the field.

Each of those shipped a *correct protocol implementation*. The failure was
underneath it, and no amount of protocol testing would have caught any of them.

This project's verification culture is unusually well suited to catching that
class of bug — mutation testing, byte-identity guards, figures withdrawn when
the variance did not support them — but only if the entropy source is treated
as a first-class subsystem with its own gates, rather than as a function that
TLS calls. Hence the sequencing in §7: **entropy ships and is verified before a
single byte of BearSSL is compiled.**

---

## 1. What the kernel supplies today

Every prerequisite a TLS stack needs, and whether it exists:

| Requirement | State | Notes |
|---|---|---|
| TCP with listen/accept/send/recv | **Yes** | `net/tcp.h` — clean layer to slot a record layer above |
| Persistent object storage for keys/certs | **Yes** | object catalog + `persist.c` |
| Heap allocation | **Yes** | `sls_heap_*` — though BearSSL needs none |
| Authentication / audit | **Yes** | `kernel/auth.c`, `kernel/security_audit.c` |
| **CSPRNG** | **No** | see §2 — this is the blocker |
| **Wall clock** | **No** | no RTC driver on any arch; cert validity unverifiable |
| **High-resolution counter on ARM64** | **No** | see §2.3 |
| Constant-time primitives | **No** | nothing in-tree; BearSSL supplies these |

Two hard blockers, both invisible from the protocol layer.

### 1.1 The wall clock

`grep -rn "rtc\|cmos\|epoch\|time_t" kernel/` returns nothing relevant. There is
`kernel_tick_counter` — monotonic ticks since boot — which is the right thing
for timeouts and the wrong thing for X.509.

Without wall-clock time, `notBefore` / `notAfter` cannot be checked, which means
**a revoked or expired certificate is indistinguishable from a valid one.** For
a private CA (§5) this is less catastrophic than it sounds — the trust root is
ours and rotation is under our control — but it must be a stated limitation,
not an omission nobody noticed.

Cost: small. CMOS/RTC on x86 is a well-documented port-72h/73h sequence; ARM
`virt` exposes a PL031 RTC at a known address, and the Pi has none (it needs
NTP or an operator-supplied time at provisioning). Budget one week including
the "no trustworthy time available" path, which must **fail closed** rather than
assume 1970.

---

## 2. Entropy: the actual work

### 2.1 What exists today, and why it is worse than nothing

The only randomness in 176,000 lines is in `kernel/vec_index.c`:

```c
static uint32_t vi_rng_state = 0x9E3779B9u;   /* fixed seed */
static uint32_t vi_rand32(void) {             /* xorshift32 */
    uint32_t x = vi_rng_state;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    vi_rng_state = x;
    return x;
}
```

Its own comment is admirably clear that this is not fit for the purpose:
*"never claimed cryptographic or even statistically rigorous randomness"*, and
the fixed seed is **deliberate** — it makes HNSW graph shape reproducible for
the host tests.

That is correct engineering for an approximate index and a catastrophe if
anything ever reaches for it as "the kernel's random number generator." Every
node would produce byte-identical key material on every boot.

**Mitigation, cheap and immediate:** rename it to make misuse hard —
`vi_rand32` → `vi_testonly_rand32`, and add a guard script asserting that no
file outside `vec_index.c` references it. A name is not a security boundary,
but this specific accident is worth making difficult before the temptation
exists.

### 2.2 x86-64 sources

| Source | Detection | Trust |
|---|---|---|
| `RDSEED` | CPUID.07H:EBX.bit18 | True entropy from the on-die noise source. Preferred. Can fail (returns CF=0) under load — must retry with backoff, and must give up rather than spin forever. |
| `RDRAND` | CPUID.01H:ECX.bit30 | A DRBG *seeded* by the above. Fine as an input, not as the only one. |
| TSC jitter | always | `read_tsc()` exists in `kernel/dashboard.h`. Slow, needs health testing, but architecturally always available. |

The kernel already executes CPUID (`kernel/kernel.c:86`) for the vendor string,
so feature-bit detection is a small extension of existing code.

**Do not trust `RDRAND` alone.** Not from paranoia about backdoors, but from
ordinary engineering conservatism: it is an opaque hardware DRBG whose internal
state cannot be inspected, it has had errata (AMD Zen family, `RDRAND`
returning 0xFFFFFFFF after suspend/resume), and mixing costs nothing. Every
available source goes into the pool; no single source is load-bearing.

### 2.3 ARM64 sources — and the finding that shapes this section

**None of the ARM64 hardware in scope has an architectural RNG instruction.**

`RNDR`/`RNDRRS` are `FEAT_RNG`, introduced in **ARMv8.5-A**. The targets:

| Target | Core | Architecture | `RNDR`? |
|---|---|---|---|
| `make arm64-run` | Cortex-A53 | ARMv8.0-A | No |
| Raspberry Pi 4 | Cortex-A72 | ARMv8.0-A | No |
| Oracle Cloud Ampere | Neoverse N1 | ARMv8.2-A | No |

So the x86 plan does not port. The ARM64 answer must be:

1. **Jitter entropy from `CNTVCT_EL0`** — the architectural virtual counter,
   present on every ARMv8 core. Measure the variance in timing of a
   deliberately unpredictable memory/compute workload; this is the mechanism
   Linux's `jitterentropy` uses and it is the only source guaranteed to exist.
2. **`virtio-rng`** where the platform provides it — QEMU `-M virt` can, which
   covers development and the Oracle VM. **No virtio transport exists in this
   kernel at all**, so this is a new MMIO driver, not a configuration change.
3. **Board-specific hardware RNG** where present — BCM2711 has RNG200 at a
   fixed MMIO address on the Pi. Board-specific, so it is a bonus source and
   never the only one.

**And a prerequisite inside the prerequisite:** `read_tsc()` in
`kernel/dashboard.h` has no ARM64 branch — it falls through to `return 0`.
Jitter entropy measured with a counter that always reads zero produces a
perfectly uniform-looking stream of nothing. `CNTVCT_EL0` must be wired in
first, and its addition should be gated on a test that asserts two successive
reads differ.

*Precisely: this is not a live bug today.* `dashboard.h` is not included by any
file in `AR_C_SRC`, so nothing on ARM64 currently calls `read_tsc()` and gets a
zero. It is a **trap**, not a defect — armed and waiting for the first ARM64
source that includes the header, at which point it returns a plausible,
silent, always-zero timing measurement. Fixing it costs three lines
(`mrs %0, cntvct_el0`) and closes it before anything walks into it.

### 2.4 The design

```
   RDSEED / RDRAND  ─┐
   TSC jitter       ─┤
   CNTVCT jitter    ─┼──►  entropy pool  ──►  ChaCha20 DRBG  ──►  consumers
   virtio-rng       ─┤     (mixing + )         (reseed on
   board RNG        ─┘      health tests)       interval + on fork)
```

- **Mixing, not selecting.** Every available source contributes; the pool is
  hashed. A compromised or broken source degrades the pool, it does not own it.
- **Health tests on every source**, continuously, per NIST SP 800-90B: the
  Repetition Count Test and the Adaptive Proportion Test. A source that fails is
  dropped from the pool and the event is written to `security_audit`.
- **ChaCha20 as the DRBG.** Not AES: it is constant-time in software by
  construction, needs no AES-NI (absent on the Pi), and is ~100 lines. BearSSL
  ships one, so this may not even need writing.
- **Fail closed, loudly.** If the pool has not reached its seeding threshold,
  `entropy_get()` returns an error and **TLS refuses to start**. A node that
  cannot generate keys safely must serve nothing rather than serve something
  insecure. This is the single most important line of code in the subsystem.

### 2.5 How this gets verified — the part that matters

Functional tests cannot validate a CSPRNG. These can:

1. **Known-answer tests.** ChaCha20 against RFC 8439 vectors; the DRBG against
   NIST CAVP vectors. Catches implementation error, not entropy failure.
2. **Statistical tests on the raw sources.** Dieharder or NIST STS over
   collected output. Weak evidence — a fixed-seed xorshift passes many of
   these — but it catches gross failure like a stuck bit.
3. **The boot-diversity test, which is the one that would have caught Debian.**
   Boot N nodes from an identical image and assert their first 256 bytes of
   DRBG output are pairwise distinct. If seeding is broken, this fails
   immediately and unmistakably. It is cheap, it runs in CI against
   `run-cluster.sh`, and no protocol test substitutes for it.
4. **Mutation testing on the seeding path**, in this project's established
   style: delete the `RDSEED` mixing, force a health-test pass, skip the
   threshold check — each must fail a named test. If removing the entropy
   source does not break a test, the tests are not testing entropy.
5. **Source-failure injection.** Force `RDSEED` to always fail; assert the node
   still boots, drops that source, logs it, and either proceeds on remaining
   sources or refuses TLS. Never silently proceeds with less than it thinks.

**Gate for Phase 0:** all five green on x86-64 *and* ARM64, plus a boot with
every hardware source disabled that correctly refuses to serve TLS.

### Gate result, 2026-08-11: MET on x86-64

```
AEROSLS_TOKEN=... tests/entropy_boot_diversity_check.sh
  nodes seeded:        4  (ids: 1 2 3 4)
  distinct fingerprints: 4
PASS  every node seeded differently (4/4 distinct)
```

Four nodes, one image, four distinct fingerprints. Test 3 -- the one that
would have caught Debian -- has now actually run against real hardware rather
than being asserted.

**What it found on its first real run was not what it was built for.**
`entropy_init()` was never called. The subsystem compiled, linked, and passed
31 host tests while being completely inert in the booted kernel; every host
test calls `entropy_init()` itself as setup, so none of them could see it. A
unit test cannot detect "the boot path never invokes this" -- the call it needs
is the call under test. Fixed in `83e1134`.

That is the argument for a runtime gate, made better than §2.5 made it. The
value was not the assertion; it was that something finally exercised the real
system instead of a harness that supplied the missing piece.

`tests/entropy_boot_diversity_smoke.sh` now plants teeth against fake nodes and
requires the guard to reach the right verdict for all five shapes -- including
that a node with NO entropy is not blamed on diversity, which is a wording
failure this script shipped twice. Mutation tested: forcing the guard to always
PASS is caught.

**Still owed: the same gate on ARM64.** x86-64 has RDSEED and RDRAND; ARM64 has
neither and runs on jitter alone (§2.3). A pass here says nothing about the
target the roadmap actually optimises for.

---

## 3. BearSSL port

Chosen for reasons that are specific rather than general:

- **No dynamic allocation anywhere.** Contexts are caller-provided structs. In a
  kernel where the heap is shared with the database, that removes an entire
  failure class.
- **Constant-time by construction**, including the big-integer code — designed
  against timing side channels rather than patched for them.
- **Freestanding by design.** No libc dependency beyond `memcpy`/`memset`, no
  filesystem, no sockets. It expects to be embedded in exactly this situation.
- **~20k lines**, small enough to actually read.
- **MIT licensed** — no reciprocal obligation on the kernel.

### 3.1 What the port actually involves

BearSSL is deliberately transport-agnostic: the application feeds it bytes and
it emits bytes. So the work is glue, not cryptography:

| Piece | Work |
|---|---|
| Build integration | New `TLS_C_SRC` in the Makefile, x86 + ARM64 object rules. Its config header trims what is compiled in. |
| Entropy hookup | `br_prng_class` fed from §2. **The port is not complete until this is wired — BearSSL will not invent randomness for you.** |
| TCP bridge | `br_sslio_*` read/write callbacks over `tcp_send`/`tcp_recv`, honouring partial reads and the existing non-blocking HTTP loop. |
| Certificate/key storage | Node key + cert as objects in the catalog. The private key must never be checkpointed to a plaintext NVMe region — see §6.3. |
| Clock | `br_x509_time_check` needs §1.1's RTC. |
| Memory budget | See §3.2. |

### 3.2 The memory constraint, stated early because it bites

TLS 1.3 records are up to 16 KB. A server context with full buffers is ~32-35 KB
of I/O buffer **per connection**, plus ~25 KB of handshake context.

This collides with an existing, deliberate constraint:

```
Makefile:128    -Wframe-larger-than=16384
```

...enforced by `tests/stack_frame_budget_check.sh`. So:

- **No TLS context on the stack.** Contexts are static or heap, always.
- **Per-connection cost is real.** Ten concurrent TLS connections is ~500 KB of
  buffer. On a 1 GB edge box that is fine; it must still be *budgeted*, and the
  connection limit must be enforced rather than discovered.
- BearSSL supports reduced buffer sizes at the cost of interoperability with
  peers that send maximum-size records. Do not take that trade quietly — it
  produces failures that look like network problems.

---

## 4. Certificate model — private CA

The cluster is its own certificate authority.

**Why this and not ACME:** an edge box typically has no public DNS name, no
inbound reachability for an HTTP-01 challenge, and often no internet at all.
ACME solves a problem these deployments do not have and fails at the one they
do.

**Why this and not self-signed + pinning:** with more than one node, pinning is
O(n²) trust decisions and rotation is a manual event on every client. A CA
makes node identity a property the cluster issues and revokes.

### 4.1 Shape

- **Root key generated at cluster initialisation**, on the first node, from the
  §2 CSPRNG. Never leaves that node in plaintext; ideally offline after issuing
  the intermediate.
- **Node certificates issued at provisioning**, CN/SAN carrying the node id and
  cluster id. This makes node identity a cryptographic fact rather than a
  command-line argument — a meaningful upgrade on `node=<n>`, which is
  currently unauthenticated.
- **Operators install the root** in their browser or client trust store once.
  One trust decision per cluster, not per node.
- **Revocation:** a CRL held as an object and replicated by consensus. Note the
  §1.1 dependency — without a clock, CRL freshness cannot be enforced, so this
  is best-effort until the RTC lands.

### 4.2 The bootstrap problem, named

The first node's CA key is generated from a CSPRNG that has just booted, on a
machine that may have very little entropy accumulated. **This is the single
highest-value key in the system and it is generated at the worst possible
moment.**

Mitigations, all three:

- Block CA generation until the pool has exceeded a **higher** threshold than
  normal operation requires.
- Prefer generating it during provisioning, after the node has run for a period
  and accumulated jitter entropy, not in the first second of first boot.
- Allow an operator-supplied seed at provisioning for the paranoid case.

---

## 5. Mutual TLS on the cluster segment

The decision to include this in v1 is the right one, and it is worth being
explicit about why: **the cluster segment is currently the worse exposure.**

`run-cluster.sh` puts every node on a shared multicast L2 segment carrying
consensus traffic in clear. Anything on that segment can read the cluster's
internal state and — more seriously — inject frames. Node identity today is a
kernel command-line argument (`node=<n>`, `boot_params.c`) with no
authentication whatsoever: a machine that claims to be node 3 *is* node 3.

Client certificates fix the identity problem and the confidentiality problem
with the same mechanism. A node presents its cert; the peer verifies it against
the cluster CA and checks the node id in the SAN matches the id it is claiming.

**Ordering note:** do the server-side path first and get it interoperating with
a browser and curl, because that is the one with an independent, unforgiving
reference implementation on the other end. Bugs found against Chrome are found
cheaply. Then do node-to-node, where both ends are ours and a shared
misunderstanding stays invisible.

---

## 6. Things that will go wrong, listed before they do

1. **The private key gets checkpointed in plaintext.** `checkpoint_mgr` and
   `persist.c` write kernel state to NVMe. A node key that lands in a checkpoint
   is a node key on a disk that might be discarded, RMA'd, or backed up to
   somewhere less careful. Decide the storage story *before* generating the
   first key: a dedicated encrypted region, or an explicit exclusion in the
   checkpoint walk with a guard test asserting it.
   → **Settled 2026-08-12**; see "§6.1 settled" at the end of this document.
   The answer turned out to be neither of the two options listed here: the
   region is outside the checkpoint walk *by construction*, so there is no
   exclusion to add. It is still in plaintext, and that cost is stated there
   rather than softened.
2. **Timing side channels through the record layer.** BearSSL's primitives are
   constant-time; glue code is not automatically. Padding checks, error paths
   that return early, and length-dependent branches in the bridge are all live
   risks.
3. **Entropy pool state surviving into a checkpoint.** Restoring a checkpoint
   would restore the DRBG state, and two nodes restored from the same checkpoint
   would produce identical output. The DRBG must reseed on restore, and there
   should be a test that asserts it.
4. **Error messages that leak.** A handshake failure that distinguishes "bad
   certificate" from "unknown CA" to an unauthenticated peer is an oracle. The
   audit log should be detailed; the wire should not.
5. **`-Wframe-larger-than` fights the port.** Expect to hit it. The answer is
   always static/heap contexts, never raising the limit.

---

## 7. Sequencing

| Phase | Work | Budget | Gate |
|---|---|---|---|
| **0** | Entropy subsystem: sources, pool, ChaCha20 DRBG, health tests, fail-closed. `CNTVCT_EL0` for ARM64. Rename `vi_rand32`. | 5-6 weeks | §2.5's five tests green on both arches; a no-hardware-source boot refuses TLS |
| **1** | RTC driver, x86 CMOS + ARM PL031, with a fail-closed "no trusted time" path | 1 week | Clock survives reboot; missing RTC refuses cert validation rather than assuming |
| **2** | BearSSL vendored, built for x86-64 and ARM64, entropy wired, KATs passing | 2-3 weeks | BearSSL's own test suite green in-kernel |
| **3** | TCP bridge + server-side TLS on the HTTP port | 3-4 weeks | Chrome, Firefox and curl complete a handshake and fetch the Navigator |
| **4** | Private CA, node cert issuance, key storage with the §6.1 decision made | 3 weeks | Cluster init generates a root; nodes get certs; keys provably absent from checkpoints |
| **5** | Mutual TLS on the cluster segment | 3 weeks | A node with no cert cannot join; a node claiming another's id is rejected |

**Total: 17-20 weeks — call it 4-5 months of one person's time**, which is
roughly double the 4-8 weeks the roadmap document estimated for "option B".

That estimate was wrong, and specifically it was wrong because it costed the
TLS stack and not its prerequisites. Phases 0 and 1 — entropy and a clock —
are 6-7 weeks before any TLS code is compiled, and they were invisible in the
original figure. Recording that here rather than quietly using the new number.

---

## 8. Assumptions to challenge

1. **That BearSSL is still the right choice.** Its upstream has been quiet for
   some time. That matters less for a vendored, frozen, audited-once dependency
   than for a live one, but if a CVE lands there is no upstream to wait for —
   we would be patching it ourselves. mbedTLS trades a heavier port for a
   maintained upstream, and that trade should be re-examined before Phase 2
   starts, not after.
2. **That jitter entropy is sufficient on ARMv8.0.** It is what Linux relies on
   in the same situation, so the precedent is strong, but the quality is
   hardware-dependent and must be measured on the *actual* target boards, not
   assumed from the design. If a Pi 4 produces poor jitter, the board RNG stops
   being a bonus and becomes required.
3. **That 4-5 months is right.** It is an estimate for work not yet started, by
   someone who has not written a TLS integration in this kernel. Phase 0 will
   inform it; the number should be revised after Phase 0 lands rather than
   defended.
4. **That mutual TLS belongs in v1.** It is the right call on exposure, but it
   adds ~3 weeks to the critical path before the first user sees anything. If
   the first user materialises early, shipping Phase 3 and deferring Phase 5
   is a defensible re-cut.

## Phase 3 decision, 2026-08-11: self-signed first

Dave chose the self-signed route over private-CA-first.

**What it buys:** the shortest path to a browser handshake, and that handshake
exercises the entire stack end to end — `entropy.c` -> EC key generation ->
X.509 writing -> `rtc.c` supplying notBefore/notAfter -> the record layer ->
`sls_tls_bio_send/recv`. Every component Phases 0-2 built gets proven together
rather than one at a time.

**What it costs:** certificate generation happens twice. The private CA in the
later phase does not reuse the self-signed path; it replaces it.

**Phase 3 gate (unchanged):** Chrome, Firefox and curl complete a handshake and
fetch the Navigator.

### API notes measured from the vendored headers, not remembered

Confirmed present in `include/mbedtls/x509_crt.h`:

- `mbedtls_x509write_crt_set_subject_name` (:1033)
- `mbedtls_x509write_crt_set_validity` (:1003)
- `mbedtls_x509write_crt_set_serial_raw` (:986)
- `mbedtls_x509write_crt_set_basic_constraints` (:1090)
- `mbedtls_x509write_crt_set_subject_key_identifier` (:1103)
- `mbedtls_x509write_crt_der` (:1178)

`set_subject_key`, `set_issuer_key` and `set_md_alg` did NOT match a grep for
`int mbedtls_x509write_crt_...`, which suggests they return `void`. That is an
INFERENCE from a negative result, not a measurement — confirm the signatures
before calling them. A negative grep is exactly the kind of evidence that
produced the `gmtime_r` error in Phase 2.

`mbedtls_ecp_gen_key` likewise did not match; only `mbedtls_ecp_gen_keypair_base`
(ecp.h:1201) did. Check which generator this version actually exposes.

#### Both inferences resolved, 2026-08-11 — one held, one did not

Read out of the vendored tree rather than inferred:

- **The `void` inference was right.** `mbedtls_x509write_crt_set_subject_key`,
  `_set_issuer_key` and `_set_md_alg` all return `void`. Nothing to check at
  those call sites, and nothing to handle.
- **The `mbedtls_ecp_gen_key` inference was WRONG.** It is right there at
  `vendor/mbedtls/include/mbedtls/ecp.h:1248`:

  ```c
  int mbedtls_ecp_gen_key(mbedtls_ecp_group_id grp_id, mbedtls_ecp_keypair *key,
                          mbedtls_f_rng_t *f_rng, void *p_rng);
  ```

  So is `mbedtls_ecp_gen_keypair`. The grep that "did not match" was reading a
  population that did not contain the answer — the same failure mode as
  `gmtime_r`, in the same document, two sections apart, and this time the
  section flagged the risk in its own text and then filed the conclusion anyway.

  **The rule this earns:** a negative grep is not a finding, it is a failed
  search. Write the positive form (`grep -n 'mbedtls_ecp_gen' <file>` and read
  every hit) or write nothing. Note also that this one survived a first attempt
  at confirmation — an independent reader of `ecp.h` also reported the symbol
  absent before an exact-string search found it. Two negative results are not a
  measurement either.

`mbedtls_ecp_gen_key` is the one to call: it takes the group id directly, so
there is no separate `mbedtls_ecp_group_load` step and no `G` to pass.

### Three things to settle when writing kernel/tls_cert.c

1. **Validity strings.** `set_validity` takes `YYYYMMDDHHMMSS` text, so the
   generator needs `rtc_civil_from_days()` and a formatter. This is the first
   consumer of the inverse function added in Phase 1.

2. **notBefore skew.** A certificate stamped with the node's exact boot time is
   "not yet valid" for any verifier whose clock is a second behind. Backdate it.

3. **Serial numbers.** `set_serial_raw` takes bytes; they must come from
   `entropy_get()` and the fail-closed path must be honoured — no serial rather
   than a predictable one.

### The three, settled — 2026-08-11

Read out of `vendor/mbedtls/` and `kernel/rtc.h`, `kernel/entropy.h`. Line
numbers are this vendored 3.6.7 tree.

#### 1. Validity strings — exactly 14 characters, and no `Z`

`MBEDTLS_X509_RFC5280_UTC_TIME_LEN` is **15** (`x509_crt.h:140`) and
`set_validity` demands `strlen(s) == LEN - 1`, i.e. **exactly 14**, for both
arguments or it returns `MBEDTLS_ERR_X509_BAD_INPUT_DATA`. It then copies the
14 and writes `'Z'` into index 14 itself:

```c
ctx->not_before[MBEDTLS_X509_RFC5280_UTC_TIME_LEN - 1] = 'Z';
```

So the string we build is `YYYYMMDDhhmmss` with **no trailing `Z`** — mbedTLS
appends it. The field is `char not_before[MBEDTLS_X509_RFC5280_UTC_TIME_LEN + 1]`
(`x509_crt.h:226`), zeroed by `_init`, so it ends up a well-formed
`"YYYYMMDDhhmmssZ"`. Adding the `Z` ourselves makes the argument 15 characters
and the call fails the length test outright.

**UTCTime vs GeneralizedTime is not our problem.** `x509_write_time`
(`x509write_crt.c:393`) picks between them by *reading the first three
characters as text* — `t[0] < '2' || (t[0]=='2' && t[1]=='0' && t[2] < '5')` —
so 2049 encodes as UTCTime (it drops `t+2`, writing the 2-digit year RFC 5280
requires) and 2050 as GeneralizedTime. Nothing to configure and nothing to get
wrong, provided the year is really 4 digits.

**What the formatter actually needs.** `rtc_civil_from_days` is
`void rtc_civil_from_days(int64_t z, int64_t* y, unsigned* m, unsigned* d)`
(`rtc.h:109`) — it yields **the date only**. Time of day is our arithmetic on
the same `uint64_t`: `days = t / 86400`, `sod = t % 86400`, then
`hh = sod/3600`, `mm = sod%3600/60`, `ss = sod%60`. And the year comes back as
`int64_t`, so it needs narrowing before a 4-digit conversion.

**The prediction worth writing down:** this needs zero-padded fixed-width
integer conversion (`%04`/`%02`). `sls_tls_snprintf` was written days ago for
a library that mostly wanted `%s` and `%d`. If it does not implement width and
zero-fill, the string comes out short — and `set_validity`'s exact-length test
catches it immediately and by name. That is the good failure, and it is worth
noticing that it is only good because someone upstream wrote `!= LEN - 1`
instead of `> LEN - 1`. Assert `strlen == 14` at our own call site anyway, so
the message names the formatter rather than the certificate.

**And the prior question underneath it:** `rtc_get_unix()` returns `int` and is
allowed to fail (`rtc.h:59`, and the header's own comment says it "fails on
plausible-looking" input). No trusted time means no defensible `notBefore`, so
§1.1's fail-closed path gets its first real consumer here too: refuse to
generate the certificate and say why. Do not fall back to the boot epoch.

#### 2. notBefore skew — backdate by a day, not by a second

There is no grace period in any verifier; mbedTLS, NSS, BoringSSL and OpenSSL
all compare `notBefore` against now and reject a certificate that is not yet
valid. So the only question is how far back, and "a second behind" is the
smallest of the three risks actually in play:

| Risk | Size |
|---|---|
| Peer clock unsynchronised (no NTP, drifting RTC) | minutes to hours |
| **Our own CMOS RTC read as UTC when the firmware holds local time** | up to ±14 hours |
| Handshake happening one second after issuance | one second |

The middle row is the one that will bite, it is ours rather than the peer's,
and it is invisible in QEMU if the host and guest agree. **Backdate 24 hours.**
On a self-signed certificate that is regenerated anyway it costs nothing, and
it covers a whole-timezone error in `rtc.c` that would otherwise present as an
intermittent, geography-dependent handshake failure.

Keep `notAfter` short for the same reason — the certificate is cheap to
reissue. A long validity buys nothing here and only raises the question of
which browser validity ceiling applies to a certificate that does not chain to
an installed root, which is not a question worth answering experimentally.

**Operational consequence to expect in Phase 3:** if the certificate is
regenerated on every boot, every browser trust exception is invalidated on
every boot. That is going to make the gate below tedious long before it makes
it fail. Persisting the key and certificate in the object catalogue (§3.1) is
what fixes it, and that pulls §6.1's "where does the private key live" decision
forward into Phase 3 rather than Phase 4.

#### 3. Serial numbers — the fail-closed path is worse than "predictable"

`MBEDTLS_X509_RFC5280_MAX_SERIAL_LEN` is **20** (`x509_crt.h:139`), matching
RFC 5280. `set_serial_raw` is a bounds check and a `memcpy` and nothing else:

```c
if (serial_len > MBEDTLS_X509_RFC5280_MAX_SERIAL_LEN) {
    return MBEDTLS_ERR_X509_BAD_INPUT_DATA;
}
ctx->serial_len = serial_len;
memcpy(ctx->serial, serial, serial_len);
```

Two things it does not do, both of which are ours:

- **The sign byte.** `mbedtls_x509write_crt_der` prepends `0x00` when the top
  bit of the first byte is set, which is correct DER — and silently produces a
  **21-octet** INTEGER, one over the RFC 5280 ceiling that `set_serial_raw`
  just finished enforcing. Half of all random 20-byte serials do this.
- **The leading zero.** A first byte that comes back `0x00` is written
  verbatim, giving a non-minimal INTEGER. That is a DER violation with a 1-in-256
  incidence — the sort of defect that reproduces once a fortnight and gets
  blamed on the network.

So:

```c
unsigned char serial[20];
if (entropy_get(serial, sizeof serial) != ENTROPY_OK) {
    /* refuse — do not generate a certificate */
}
serial[0] = (serial[0] & 0x7f) | 0x01;   /* positive, minimal, non-zero */
```

158 bits, against the 64 anyone asks for.

**And the part the original note undersold.** `entropy.h:78` is explicit:
`entropy_get()` returns `ENTROPY_OK` or negative **with `out` UNTOUCHED**. So
ignoring the return does not give a predictable serial. It gives whatever was
in that stack slot — uninitialised kernel stack, published in a certificate, to
every client that connects. That is a disclosure bug, not a weak-serial bug.
Zero-initialising the buffer first only downgrades it to serial `0`, which
RFC 5280 forbids (the serial MUST be a positive integer). **Testing the return
is the only handling that is correct**, and it is worth a mutation test in the
§2.5 style: force `entropy_get()` to fail and assert no certificate is
produced.

#### 4. The one that was not on the list: signing consumes entropy too

`mbedtls_x509write_crt_der` takes `f_rng`/`p_rng` (`x509_crt.h:1178`) because
ECDSA needs a per-signature nonce. A nonce reused or made predictable across two
signatures recovers the private key outright — the PlayStation 3 result, and a
much shorter path to catastrophe than a weak serial.

So the fail-closed path has to be honoured in the `f_rng` **wrapper** as well,
and the wrapper is where it is easiest to get wrong: mbedTLS's convention is
`0` for success, so a wrapper that forwards `entropy_get()`'s return directly
inverts the meaning of every error — `ENTROPY_E_NOT_SEEDED` is `-1`, which is
non-zero and therefore correctly read as failure, but `ENTROPY_OK` is `0` and
so is mbedTLS's success. That happens to line up. It lines up *by coincidence*,
between two enumerations neither of which was written with the other in mind,
and the coincidence should be stated in a comment at the wrapper rather than
relied on silently.

### Still owed from Phase 2

- ~~The **image-end number**~~ — **obtained 2026-08-11, fifth time of asking.**
  512 KiB for all 107 mbedTLS modules, against a 223 MiB image. Section below.
- Collapse the two snprintfs (`sls_tls_snprintf` vs the TCG `<nofmt>` stub).
  Now has a second reason: `sls_tls_snprintf` is about to become load-bearing
  for validity strings, so whichever survives needs width and zero-fill.
- Run the ARM64 entropy diversity gate on hardware with no RNG instruction.

---

## Image-end measurement, 2026-08-11

**`_kernel_image_end` at HEAD (`9060e89`), mbedTLS linked: 223 MiB. mbedTLS's
share of that is 512 KiB — 0.22%. Trimming the module list is housekeeping.**

### The first thing found, before any number

`my_sls_kernel.bin` in the tree was **not the linking build**. It was built at
17:40 UTC, five hours before the mbedTLS commits (`c0bdc0f`..`9060e89`,
22:54–23:21 UTC), and `nm` finds **zero** `mbedtls_*` symbols in it. None of
the 107 `vendor/mbedtls/library/*.x86.o` objects existed on disk at all.

Running the check against it would have produced a clean five-line PASS and a
plausible 222 MiB, and that number would have been the *baseline* answering a
question nobody asked. Four requests for this number were four requests for a
build that did not exist yet — which is worth knowing, because it means the
number was never one command away, and a fifth ask would not have produced it
either. The check's own header says it: *"Run `make` first."*

### The three measurements

| | `_kernel_image_end` | MiB |
|---|---|---|
| Their real pre-mbedTLS build (`x86_64-elf-gcc` 13.2.0 / binutils 2.42) | `0x0deb6000` | 222.711 |
| Control: HEAD tree, mbedTLS objects removed from the link | `0x0defc000` | 222.984 |
| HEAD, all 107 mbedTLS modules linked — *substitute toolchain* | `0x0df7c000` | 223.484 |
| **HEAD, real toolchain — confirmed on hardware, see below** | **`0x0df88000`** | **223.531** |

- **mbedTLS delta: 524,288 B = 512 KiB exactly.** Rows 2→3. One tree, one
  toolchain, one link; the only variable is the presence of the 107 objects.
- Toolchain-and-commit skew: 286,720 B = 280 KiB. Rows 1→2.

Corroborated independently: `size -t` over the 107 objects totals 524,856 B of
text+data+bss, which page-aligns to exactly the observed 0x80000.

Where it went, by section (control → linked):

| Section | Before | After | Δ |
|---|---|---|---|
| `.text` | 770,561 | 1,136,008 | +357 KiB |
| `.rodata` | 1,165,880 | 1,311,912 | +143 KiB |
| `.data` | 31,800 | 31,844 | +44 B |
| `.bss` | 229,746,896 | 229,757,880 | +10.7 KiB |
| `.bootstrap_stack` | 1,048,576 | 1,048,576 | — |

### The caveat, stated rather than discovered later

**This was not built with the project's toolchain.** `x86_64-elf-gcc` lives in
Dave's WSL2 at `~/opt/cross/bin` and is not reachable from where this ran; the
build used the Linux VM's native `gcc 11.4.0` / `ld 2.38` with
`-fno-stack-protector -U_FORTIFY_SOURCE` added to match the cross compiler's
defaults, reusing the genuine cross-toolchain objects for the six NASM sources
and the nineteen TCG objects (nasm and `../qemu` both being absent too).

That gap is the 280 KiB in row 1→2, mixed together with thirteen genuinely
changed source files. Which is exactly why the control link exists: **the
512 KiB is a within-toolchain difference and does not depend on any of it.**
The absolute 223.484 MiB does.

### Confirmed on the real toolchain, and one prediction corrected

Run on `ubuntu-16gb-fsn1-1` — a different machine and a different checkout,
which makes the agreement worth more than a same-box rerun would have been:

```
_kernel_image_end = 0x000000000df88000 (223 MiB)
bootstrap stack   [0x000000000de873c0, 0x000000000df873c0) = 1024 KiB
---- passed=5 failed=0
```

**223 MiB and five `ok:` lines. The headline stands, and the delta stands.**

The substitute toolchain came in **48 KiB low**, and the bootstrap stack moved
by exactly 48 KiB as well (`0x0de873c0` against `0x0de7b3c0`) — a *uniform
shift of the whole image*, not scattered codegen noise. That is the best shape
this error could have taken, because the delta rides through it unchanged:

| | |
|---|---|
| Real HEAD | `0x0df88000` |
| Implied real control (my control + the same 48 KiB) | `0x0df08000` |
| **mbedTLS delta in the real toolchain** | **524,288 B = 512 KiB** |

512 KiB from a second direction. Trimming is still housekeeping.

**What this section got wrong.** It predicted `223.211 MiB (0x0df36000)`. The
real reading is `0x0df88000` — **out by 328 KiB**, and the reasoning was wrong
in a way that is a variant of the mistake this document keeps catching.

The 280 KiB between the control and the old baseline was labelled
"toolchain-and-commit skew" and then subtracted whole to project. Only the
*toolchain* part was skew. The rest was those thirteen changed source files —
`tls_platform.c` and the others — which are real code, present in the real
build too. Subtracting them removed something that was actually there.

It reconciles exactly, which is how the error is confirmed rather than
explained away. Total growth from the old baseline is **840 KiB**: 512 KiB of
mbedTLS and **328 KiB of new kernel code** — and 328 KiB is precisely the
projection error, because that new code is precisely what was subtracted. The
toolchain's own contribution was **48 KiB**, a fifth of what was charged to it.

**The rule this earns:** a difference measured across two variables is not
skew, it is a difference across two variables. Naming it after only one of them
("toolchain-and-commit skew" — the *and* was right there in the label) licensed
subtracting the whole of it. The control link was built specifically to avoid
this and it did its job; the projection was a separate, avoidable arithmetic
laid on top of a sound measurement, and it made a 48 KiB toolchain gap look
like a 280 KiB one.

Reproducing it is a one-liner, and the five `ok:` lines above are now the real
ones — the orphan-section sweep and both stack checks have passed against an
image linked by the toolchain that will actually ship it, which is the only
place that invariant means anything:

```
make my_sls_kernel.bin && tests/kernel_image_end_check.sh
```

### What the number decides, and what it does not

**Decides:** trimming the module list is housekeeping. 512 KiB on a 223 MiB
image is 0.22%, and even a perfect trim — deleting every module the listener
does not reach — recovers some fraction of that 512 KiB. It should not be
allowed to delay Phase 3, and `oid.c`/`x509_csr.c` are worth removing when
convenient rather than as a work item.

**Does not decide, and must not be read as deciding:** §3.2's per-connection
RAM. That is ~32–35 KB of I/O buffer plus ~25 KB of handshake context *per
connection*, allocated at run time, and the image-end number cannot see it.
Ten connections is ~500 KB — comparable to the entire static cost of the
library. Trimming modules does not reduce it by a byte. **The memory question
in this document was always the connection budget, and it is still open.**

**The number nobody asked about.** `.bss` is 229.7 MB — **98% of the image**.
The 512 KiB that took five requests to measure is 0.2% of a figure sitting next
to it that has not been questioned once, and `arch/x86/linker.ld`'s own comment
still describes it as "117 MiB of .bss", roughly half what it now is. Whatever
doubled it did so unremarked. That is the memory question worth a section, and
this document is not the place for it.

---

## Phase 3's gate is a browser, not a test suite

**Gate (unchanged): Chrome, Firefox and curl complete a handshake and fetch the
Navigator.** This section is about why that wording is load-bearing, and what
it does and does not buy.

### Why it is harder to fool than anything asserted from inside

A test suite written alongside an implementation shares the implementation's
misunderstandings. Phase 2 has two worked examples of exactly this, four days
apart and both in this document: `stack_frame_budget_check.sh` reported "109
files scanned, all within budget" while scanning none of mbedTLS, and 31 host
tests passed against an entropy subsystem that the booted kernel never
initialised, because every one of them called `entropy_init()` as setup. In
both cases the test was green, the code was wrong, and the test was green
*because* it was written by the same understanding that wrote the code.

A browser cannot be recruited into that. It was written years ago, by people
who have never seen this kernel, against the RFC rather than against our
reading of it. It has no setup function it can call on our behalf, no
sympathetic default, and no interest in the handshake succeeding.

**But the strength is not "three user agents". It is three independent
implementations of X.509 and TLS 1.3:**

| Client | Stack | Trust store |
|---|---|---|
| Chrome | BoringSSL | Chrome's own bundled root store |
| Firefox | NSS | Firefox's own, independent of the OS |
| curl | OpenSSL or GnuTLS depending on build | System store, or `--cacert` |

Three codebases that share no common ancestor closer than SSLeay, disagreeing
about nothing except by accident. A certificate all three accept is a
certificate that is actually conformant, rather than one that happens to suit
whichever parser we tested against first. That is the property worth having,
and it is why dropping any one of the three costs more than a third of the
gate.

### The trap the gate walks straight into: no SAN, no Chrome

**Chrome has ignored `commonName` entirely since Chrome 58.** A certificate
whose identity lives only in the CN is rejected — the name is simply not read.
The identity has to be in a `subjectAltName`, and for a node reached by IP
address (which is every node in `run-cluster.sh` today) it must be an
`iPAddress` SAN, **not** a `dNSName` containing a dotted quad. Those are
different ASN.1 types and Chrome will not substitute one for the other.

**This is not in the confirmed-API list above.** That list has
`set_subject_name`, `set_validity`, `set_serial_raw`, `set_basic_constraints`,
`set_subject_key_identifier` and `crt_der` — and no SAN setter, which means a
`tls_cert.c` written from that list alone produces a certificate that fails the
gate on its first contact with Chrome, for a reason that looks like a TLS bug
and is not one.

It is present, and it does what is needed:

```c
/* vendor/mbedtls/include/mbedtls/x509_crt.h:244 */
int mbedtls_x509write_crt_set_subject_alternative_name(
        mbedtls_x509write_cert *ctx, const mbedtls_x509_san_list *san_list);
```

with `MBEDTLS_X509_SAN_DNS_NAME = 2` and `MBEDTLS_X509_SAN_IP_ADDRESS = 7`
(`x509.h:129`, `:134`); its own doc comment lists dnsName, URI, IP address,
otherName and DirectoryName as supported. `MBEDTLS_X509_CRT_WRITE_C` and
`MBEDTLS_X509_CREATE_C` are both on in the base config and
`sls_mbedtls_config.h` does not disable them — it only adds
`MBEDTLS_X509_REMOVE_INFO`, which removes pretty-printers, not writers. Worth
confirming at first link rather than at first handshake.

### `curl -k` is not a participant

`curl -k` disables verification. It proves the record layer, the key exchange
and the HTTP response — genuinely useful, and the right first target because
its failures are legible. It proves **nothing whatsoever** about the
certificate: not the validity dates, not the SAN, not the serial encoding, not
the signature.

So the gate needs the certificate exported and pinned:

```
curl --cacert node.pem https://<node>/           # self-signed acts as its own root
```

That path runs real chain validation, and it is the one that will tell us
whether the 14-character validity string and the 20-byte serial actually
encode. **Both forms should be run**, and the log should record which is which,
because `-k` passing while `--cacert` fails is the single most informative
outcome the gate can produce: it localises the fault to the certificate and
away from everything else.

The same distinction applies to the browsers. Clicking through an interstitial
is *not* a pass — it is the browser recording that verification failed and the
human overriding it. A pass means the certificate is installed in that
browser's trust store and the padlock appears without an override. Firefox's
store being separate from the OS store makes that two installations, not one,
which is a feature here rather than an annoyance.

### What the gate cannot see, and this is §0's whole argument

**Every one of these three clients will complete a handshake against a node
whose CSPRNG is broken, and report success.**

That is not a hypothetical: it is the Debian OpenSSL failure exactly, and
Netscape's, and ROCA's. All three shipped correct protocol implementations that
interoperated with everything. A browser validates the protocol and the
certificate encoding. It has no view of whether the private key it just
negotiated against was drawn from 32,767 possibilities.

So the Phase 3 gate and the §2.5 boot-diversity gate are not two tests of the
same thing at different levels. They are the only two tests here, and each is
blind exactly where the other sees:

| | Protocol and encoding correct | Key material unpredictable |
|---|---|---|
| Chrome / Firefox / curl | **yes** | no — cannot see it |
| `entropy_boot_diversity_check.sh` | no — never speaks TLS | **yes** |

Neither substitutes for the other, and passing Phase 3 must not be allowed to
read as "TLS works". It reads as "the protocol and the certificate are
conformant" — which is what it says, and all it says.

### What to capture, so the gate cannot be fooled either

The gate is itself an assertion, and assertions in this project have a record
of being green for the wrong reason. Capture evidence that is hard to fake:

- The negotiated version, cipher suite and key-share group from
  `openssl s_client -connect <node>:443 -tls1_3`, recorded verbatim. TLS 1.3
  with `TLS_AES_128_GCM_SHA256` and x25519 is the expected line; anything else
  means the config did not land the way §3 says it did.
- The certificate as the peer actually sent it, `openssl x509 -noout -text`,
  with the serial, the SAN block and both validity timestamps read back — the
  three things settled above, verified from the wire rather than from the
  generator.
- A hash of the fetched Navigator payload, matched against the same fetch over
  plain HTTP. A handshake that completes and then serves a truncated or empty
  body is a passing handshake and a failing gate.
- **Two consecutive boots, and the serials must differ.** This costs nothing,
  runs in the same harness, and is the one line of the browser gate that would
  notice §0's failure mode.

---

## Phase 3 gate result, 2026-08-12: two of three

**curl and Chrome both complete a handshake and fetch the Navigator. Firefox
is outstanding.** The listener is `net/http.c` driving `kernel/tls_server.c`
over the BIO bridge, on port 8443 (a separate port from 3000 — see below).

### What each client actually verified, which is not the same thing

| | Proved | Did not |
|---|---|---|
| `curl -k` | record layer, key exchange, an HTTP response with a correct `Content-Length` | **nothing about the certificate** — `-k` disables verification entirely |
| `curl --cacert` | signature over the certificate, `notBefore`/`notAfter` against a real wall clock, hostname match | client authentication (Phase 5) |
| Chrome | all of the above, plus a trust decision it made itself, plus the steady state — page assets fetched over separate sequential sessions | anything about entropy |

The distinction between the first two rows is the reason `-k` is worth running
first and worth never mistaking for a pass. It exercises everything *except*
the three things §"The three, settled" was about.

Chrome's pass is the one the gate's wording was chosen for: **no interstitial,
no override.** Clicking Advanced → Proceed would have been Chrome recording
that verification failed and a human overruling it, which is the opposite of
what this gate exists to establish.

### Five things the live runs settled that nothing in this repo could

Each of these was invisible to every test that passes, and each cost one round
trip against a real client to find.

**1. The listener was unreachable, and it was not a TLS problem.** The first
attempt timed out after 133 seconds. `10.0.2.15` is slirp's guest address:
correct inside QEMU, not routable from the host, and `run-cluster.sh` forwarded
only 3000. The failure had nothing to do with the record layer, the
certificate, or the handshake — it was a missing line in the launcher. Worth
recording because "TLS does not work" was the obvious reading and was wrong.

**2. A verifier checks the name the CLIENT used.** The certificate carried one
SAN, `IP Address:10.0.2.15`, because that is the address the node believes it
has. Every client reaches it as `localhost` through the port forward. `curl -k`
passed anyway — it was not checking — and both browsers would have rejected it
with an error that reads as a certificate fault and is really an addressing
one.

**The oracle should have caught this and did not.** It asserted the SAN it
had itself chosen, not the SAN `net/http.c` installs. An independent parser
judging a specimen the test invented is only independent about the encoding.
The harness now builds the same list the kernel does, and the smoke checks
names through `openssl -verify_hostname` / `-verify_ip` — with a negative case,
because an assertion that only ever succeeds cannot show it is able to fail.

**3. `CA:FALSE` was an accurate description and the wrong one.** Windows filed
the certificate under Intermediate Certification Authorities rather than
Trusted Root — its import wizard classifies by type, and a certificate not
asserting `CA:TRUE` is not a root. An intermediate is not a trust anchor, so
Chrome reported `ERR_CERT_AUTHORITY_INVALID`, correctly. A certificate you
deliberately install as an anchor *is* acting as a CA; saying otherwise while
asking a browser to treat it as one is the encoding disagreeing with the
intent. Now `CA:TRUE` with `pathlen 0`, which is a Phase 3 expedient with a
stated expiry: §4's private CA removes it by making the anchor a separate key
that never serves traffic.

**4. The negotiated suite is `TLS_AES_256_GCM_SHA384`.** The amendment's table
lists `TLS_AES_128_GCM_SHA256` and `TLS_CHACHA20_POLY1305_SHA256` under "Suites
we want", verified. That verification was real but it established
*availability*, not *selection* — nothing ever called
`mbedtls_ssl_conf_ciphersuites`, so mbedTLS's default preference won and the
document describes a choice that was never made. Nothing is wrong with the
suite. What is wrong is a table that reads as a decision. Either pin the list
or amend the table; do not leave a reader believing a knob was set.

**5. The session cap has met browser parallelism and has not been measured
against it.** `TLS_SERVER_MAX_SESSIONS` is 2, derived from §3.2's per-connection
estimate against a 256 KiB pool. Chrome opens up to six connections per host.
The Navigator and its assets loaded, so Chrome recovered from whatever was
refused — but the header's own instruction was to raise that number only
against measured pool high-water under real handshakes, and there is now a real
workload to measure it against for the first time. Check the serial log for
`[TLS] conn N refused: no session slot` before touching the constant.

### What two of three still does not prove

**Entropy.** All three clients complete a handshake against a node whose
CSPRNG is broken and report success. That is §0's entire argument, and it is
not weakened by a green padlock — Debian, Netscape and ROCA all shipped
correct protocol implementations that interoperated with everything.

The pairing in "Phase 3's gate is a browser" holds exactly as written: this
gate says the protocol and the certificate are conformant, and
`entropy_boot_diversity_check.sh` says the key material is unpredictable.
Neither substitutes for the other, and passing this one must not be read as
"TLS works".

**Still owed on the gate itself:** Firefox, which has its own trust store and
is therefore a second installation and a genuinely independent verifier — NSS
rather than BoringSSL. And the evidence the gate section asks for and this run
did not capture: the negotiated parameters recorded verbatim, a hash of the
Navigator payload matched against the same fetch over plaintext, and **two
consecutive boots with differing serials**, which is the one line of the
browser gate that would notice §0's failure mode.

---

## §6.1 settled, 2026-08-12: the CA survives a reboot, the leaf does not

Committed as `a60a170`.

This closes the first item in §6 — "decide the storage story *before*
generating the first key" — and it was forced rather than chosen. The gate
above passed in Chrome and then in Firefox, and each pass was followed by a
reboot that invalidated it. **Three re-imports in one afternoon**, by hand,
into two trust stores with separate UIs and separate ways of getting it wrong.
An anchor that changes every boot is not a trust store entry; it is a chore.

### The shape: one long-lived secret, not two

**The CA certificate and CA private key persist. The leaf and its key do not.**
The leaf is regenerated with a fresh key on every boot and signed by the stored
CA.

That asymmetry is the whole design, and each half of it earns its place:

- **Persisting the CA** is the only thing that makes an operator's import
  survive a reboot. Nothing else was ever the problem.
- **Not persisting the leaf** keeps exactly one long-lived secret on disk
  instead of two, bounds the leaf key's exposure to a single uptime, and leaves
  the SAN list following the *build* rather than the *disk* — so changing the
  names a node answers to takes a reboot, not a wipe. The cost is one P-256
  keygen at startup.

Layout: one 4 KiB frame at `PERSIST_TLS_LBA` (7680), after `checkpoint_mgr`'s
entries end at 7672 with the same one-frame gap every other boundary in
`persist.h` uses. 504 sectors still free before `STREAM_DIR_LBA`.

### What this costs, written where it will be read

The CA private key is now on an unencrypted disk at a fixed LBA. **Whoever
reads that frame can mint certificates this node's operators trust, and nothing
in the chain will look wrong to any verifier.** That is a real and permanent
increase in what a stolen disk is worth.

It is not avoidable while the requirement stands: reboot-survivable trust is
not available without a secret that survives reboots. §4.1's "root key never
leaves that node in plaintext" is the *cluster* CA and still stands; this is a
per-node anchor for browser access, and it is a weaker thing deliberately.

What is **not** acceptable is the same key reaching a *second* place. A
checkpoint, a snapshot, a migration stream — each has its own lifetime, its own
copies, and its own audience, and every one of those paths was written by
someone who had no idea a private key would ever pass through it. Nothing about
that failure is visible: the node works, the browsers are happy, and the key is
in a stream file on another host.

### §6.1's guard, and what it does not cover

§6 asked for "an explicit exclusion in the checkpoint walk with a guard test
asserting it". What landed is slightly different and stronger on one axis:
**the region is outside the walk by construction.** It is in no `persist_*`
region and no `persist_*` function knows the LBA, so there is no exclusion to
add — there is nothing to exclude *from*.

`tests/tls_key_containment_check.sh` asserts that in two ways, because neither
is enough alone:

1. **Structural.** `PERSIST_TLS_LBA`, `PERSIST_TLS_MAGIC` and
   `tls_store_load/save/wipe` have exactly one user each. A grep, and a
   weak-looking one — but it is the check that fires when someone adds the
   frame to a checkpoint walk, because they would have to name the LBA or call
   the store to do it. Exclusions in it are *named filenames*, not globs: a
   `tests/` pattern would also excuse a future test that copied the key
   somewhere.
2. **Behavioural.** A host harness with a simulated disk plants a key made of
   bytes nothing else would produce and goes looking for it. It must be in
   **exactly one frame** of the disk — not zero, not two — and in **none** of
   the module's staging buffer, on every path including the early returns.
   `tls_store.h` claims that zeroize on every path; this is what stops it from
   being a claim. 24 assertions.

**What it does not cover, and this matters:** it cannot prove a *running*
kernel's checkpoint is clean. That needs a live node and its real disk image,
and no such check exists. The structural half is a real argument for why a
checkpoint cannot contain the key, and it is not the same as having looked. A
green line here is not "the key has been confirmed absent from a written
checkpoint", and the script's own header says so.

### Three checks in `tls_cert_sign_leaf` that nothing checked before

Each exists because the failure it prevents is *unreadable at the far end* —
the browser reports something true and useless.

**1. `mbedtls_pk_check_pair` on the stored certificate and key.** A torn 4 KiB
write, a half-updated frame, or a key from a different CA otherwise produces
leaves whose signature does not verify. Firefox reports that as
`SEC_ERROR_BAD_SIGNATURE`, which is correct and says nothing whatsoever about
which of the two blobs on disk was wrong.

This is also **why the frame carries no CRC**, which is a decision and not an
omission. A CRC catches a torn write; so does handing both blobs to real
parsers and then proving the key matches the certificate. The pairing check is
*strictly stronger* over the same bytes — it also catches a wrong key and a
stale pairing, neither of which a checksum notices. A third `crc32`
implementation in this tree would have been cost without coverage. What the
parsers cannot catch is a header that disagrees with its payload, so the
**lengths** are checked in `tls_store.c` before any byte reaches a parser.

**2. The finished leaf's `issuer_raw` against the CA's `subject_raw`, byte for
byte.** `ca_dn` is a *string* that mbedTLS re-encodes into DER. Path builders
match encoded bytes, not meaning. The future this is for: **someone edits the
DN constant in a later build while a CA made with the old one is still on
disk** — a one-line change with no visible connection to certificates,
producing a leaf that is internally valid, signed by the right key, and which
no path builder will join to its issuer. `tls_server_init` answers it by
discarding the stored CA, which is the correct repair.

**3. The leaf's window clamped into the issuer's, both ends.** The CA is made
once and the leaf every boot, so without the `notAfter` clamp the leaf's expiry
marches past its issuer's and the chain becomes unverifiable at a moment
nothing in the logs explains. `notBefore` is clamped *up* for the narrower case
of an RTC that has moved backwards — which can put `notBefore` slightly in this
node's future, and that is correct: the verifier's clock is the one being
satisfied.

### Lifetimes, and why the third number is not arbitrary

| | Value | Why |
|---|---|---|
| CA | 5 years | It is what an operator imports by hand, twice. Its expiry is the only thing that makes them do it again. Five and not ten because the key is in plaintext on an unencrypted disk, and an open-ended commitment to a secret in that position is not one worth making. |
| Leaf | 1 year | **About uptime, not key exposure.** The key is fresh every boot; the lifetime only has to outlast the longest run. Nothing re-issues a leaf while the node is running, so a shorter one would expire a certificate underneath a node that is serving happily — no log line at the moment it happens, browser errors afterwards. |
| Renewal margin | = leaf lifetime | Deliberately not a fourth number. It states the actual rule — *replace the CA when it can no longer issue a full-length leaf* — so the two cannot drift apart, and it means a leaf is never silently short-changed by the clamp into a nearly-expired issuer's window. |

The leaf's year is **a bound, not a solution.** In-flight re-issue is the real
fix and is not written.

### Everything that can be wrong converges on one repair

Absent, older format, torn, wrong key, wrong DN, expired: every one of them is
answered by *throw it away and make a new one*. So the cases in
`tls_server_init` differ in **what they log** and not in what they do. A node
that silently serves nothing because its stored CA was unreadable is worse than
one that costs an import — and the log line is what tells the operator which of
the two afternoons they are having.

### Testing, because none of this is visible from a single generation

Everything the oracle asserted before this judged **one** generation. The
property that had to hold is about a **second boot**, and nothing in a
single-shot test can see it.

`tests/tls_cert_oracle.c --persist` makes a CA, moves its clock forward twice,
and signs a leaf at each stop — two reboots, minus the reboots. Then:

- both leaves verify against the **same** anchor, with `openssl verify
  -attime`. The instants come from the oracle itself rather than being
  recomputed in the shell, so the two cannot drift apart from what was actually
  signed;
- a leaf must **not** verify against a CA that did not sign it — without that
  negative, "verify succeeded" could mean the flags were wrong and openssl
  checked nothing;
- the serials and public keys differ, so the fresh-key-per-boot claim is not
  just a comment;
- the over-long leaf lands on the CA's **exact** `notAfter`, and a normal leaf
  does **not** — otherwise the clamp assertion would pass for the wrong reason.

The three refusals — wrong key, wrong DN, expired CA — are asserted in C,
because *"this must fail, with THIS code"* is not something OpenSSL can be
asked.

**41 assertions, up from 22.** Host test 42/42. Containment 29/29.

### Measurement

Measured as a real A/B on the native-gcc control build — `a14c92c` and this
change, same toolchain, same flags, both links complete:

| | Image bytes | `_kernel_image_end` |
|---|---|---|
| `a14c92c` (before) | 2,770,488 | `0x000000000dfc1000` |
| after | 2,778,088 | `0x000000000dfc5000` |
| **delta** | **+7,600** | +16 KiB (page-rounded) |

The 16 KiB on `_kernel_image_end` is page granularity crossing a boundary, not
16 KiB of content; the 4 KiB staging frame in BSS is most of what pushed it.
Nothing here moves the "trim mbedTLS modules" question. Every frame stays
within the 25% advisory and `stack_frame_budget_check` scans 219 files clean.

**A correction, because the first number was wrong.** The commit message for
`a60a170` says +7,016 bytes. It is not right, and the way it was wrong is worth
more than the number: the "before" side was a binary in the control tree that
had been linked with a **stale `net/http.x86.o`** — an object predating a
symbol the current source references. Both sides shared the stale object, so
the comparison looked internally consistent, and it silently excluded the
`/api/health` change and any drift in `http.c`. Recompiling `http.c` is what
exposed it, by failing to link at all.

This is the **third** time in this project that a stale artifact produced a
confident wrong number — after the `my_sls_kernel.bin` that predated the
mbedTLS link, and the mutation run that restored `rtc.c` through a path it did
not own. The pattern each time is the same: the measurement was reproducible,
self-consistent, and about a program that was not the one being asked about.
`stack_frame_budget_check.sh` refuses a binary older than its sources for
exactly this reason, and it is the guard that has caught the most.

The control tree now links a one-line stub for the missing symbol. It is
identical on both sides of the A/B and therefore cancels, and it exists only in
`/tmp/ab` — the repo has no such file.
- CA certificate plus key measured well inside one 4 KiB frame, and the oracle
  asserts that rather than assuming it — a future key type that broke it should
  fail there, not in a store that silently refuses to save.

### Still owed

- **The live-node half of the containment guard.** Boot a node, trigger a
  checkpoint, and grep the written frames for the CA key's bytes. The
  structural argument says it cannot be there; nobody has looked.
- **In-flight leaf re-issue**, so the leaf's lifetime stops being a bet on
  uptime.
- **The ciphersuite decision.** `TLS_AES_256_GCM_SHA384` is confirmed live, but
  the amendment's table names two others as wanted. Pin via
  `conf_ciphersuites` or amend the table — the table currently describes an
  intention, not the build.
- **ARM64 entropy diversity**, still unmet from Phase 0 and still needing
  hardware.
