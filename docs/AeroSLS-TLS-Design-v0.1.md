# AeroSLS TLS — design and sequencing, v0.1

**Status:** Phases 0, 1 and 2 implemented — mbedTLS 3.6.7 now links into the
x86-64 kernel image. Phase 3 (a listener) is next and nothing calls the library
yet. See "Phase 2 result" below. Phase 2 library choice REVERSED — see
the amendment immediately below before reading §3.
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

- **Trim the module list.** Everything is linked because nothing was called
  yet and no closure could be measured. `oid.c` and `x509_csr.c` demonstrated
  the cost. Once a listener exists the real closure is measurable and this
  becomes a real task with the image-end number as its instrument.
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

### Three things to settle when writing kernel/tls_cert.c

1. **Validity strings.** `set_validity` takes `YYYYMMDDHHMMSS` text, so the
   generator needs `rtc_civil_from_days()` and a formatter. This is the first
   consumer of the inverse function added in Phase 1.

2. **notBefore skew.** A certificate stamped with the node's exact boot time is
   "not yet valid" for any verifier whose clock is a second behind. Backdate it.

3. **Serial numbers.** `set_serial_raw` takes bytes; they must come from
   `entropy_get()` and the fail-closed path must be honoured — no serial rather
   than a predictable one.

### Still owed from Phase 2

- The **image-end number** from `tests/kernel_image_end_check.sh` on the linking
  build. Baseline was 223 MiB pre-mbedTLS. Requested four times, still not
  obtained; it is the instrument for deciding whether trimming the module list
  is housekeeping or the next real task.
- Collapse the two snprintfs (`sls_tls_snprintf` vs the TCG `<nofmt>` stub).
- Run the ARM64 entropy diversity gate on hardware with no RNG instruction.
