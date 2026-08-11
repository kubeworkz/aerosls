# AeroSLS roadmap — 2026 H2, v0.1

**Window:** August 2026 → February 2027
**Capacity:** 2 people
**Target:** edge / ARM64 boxes
**Definition of done:** one real user running a real workload in production

---

## 0. The budget, stated first, because it decides everything else

Two people, six months, is about **12 person-months**. Subtract operations,
bug-fixing, the deploy host, and the weeks that go missing — call it **8 to 9
person-months of feature work**.

The seven items on the list are, conservatively, 30+ person-months. So the
roadmap's main job is not sequencing. It is refusal, with reasons.

Everything below is measured against one question: **does this get one real
user into production, and keep them there?** Items that do not are deferred,
not because they are bad but because they are next year's.

---

## 1. The lens: what actually made IBM i survive

The mini-IBM-i mindset is right, and it is worth being precise about *which*
part of IBM i is the durable one, because the roadmap's priorities fall out of
it directly.

IBM i has outlived every contemporary not because of LPARs, not because of its
integrated HTTP server, and not because of any feature on this list. It
survived on three properties:

| IBM i | AeroSLS today | State |
|---|---|---|
| Single-level store — one address space, objects not files | SLS, object catalog, capability tags | **Built** |
| TIMI — programs are hardware-independent, re-translated per machine | SIMI, with x86 / ARM64 / RISC-V backends + interpreter | **Built** |
| Integrated DB — DB2 is not a product *on* the OS, it *is* the OS | sql_parser → sql_exec → query_engine → index_mgr, in-kernel | **Built** |
| **It does not lose your data, and 1990s applications still run** | — | **This is the gap** |

The first three are done, which is a genuinely unusual position to be in. The
fourth is the one customers actually buy, and it is the one with nothing built
against it.

Read that way, the seven items sort themselves. Two of them serve the fourth
property. Four of them are commodity cloud infrastructure imported from a
different architecture, where they belong. One of them is a marketing question
with a trap in it.

---

## 2. What is missing from the list, and it is the biggest thing

**There is no TLS anywhere in this codebase.**

```bash
grep -rln "TLS\|tls_\|SSL\|X509\|chacha\|aes_" net/*.c kernel/*.c
#   (no output)
```

A bare-metal kernel serving business data over plaintext HTTP cannot be put in
front of a real user in production. Not because of a policy, but because the
first competent person who looks at it will stop the deployment — and on an
edge box there is no sidecar to hide behind. The node *is* the machine.

This is a fork in the road and it needs deciding early, because the two answers
cost wildly different amounts:

**Option A — terminate TLS off-box.** A small proxy in front (the deploy host
already does this with nginx/Cloudflare). Cost: near zero. Price: the edge box
is no longer self-contained, which is most of the pitch, and the link from
proxy to node is plaintext on whatever LAN the box sits on.

**Option B — TLS in the kernel.** A minimal TLS 1.3 stack: X25519, AES-GCM or
ChaCha20-Poly1305, one cipher suite, no renegotiation, no session resumption.
This is a well-bounded 4-8 week job for one person and there are auditable
reference implementations to work from. Price: it is cryptographic code in a
kernel with no memory protection between subsystems, which is the highest-risk
code this project would have written.

**Recommendation: A now, B when a user requires it.** Not because B is wrong —
long-term it is clearly right for an edge appliance — but because six months
with one user is not the window to write your first TLS stack. Ship A, document
the limitation honestly in the deployment guide, and let the first user's
security review decide when B gets funded.

*This should be item 8 on the list, ahead of at least four of the existing
seven.*

---

## 3. The seven items, ranked against "one real user"

### DO — these three fill the window

---

#### #6 Rigorous DB test harness → **first, and by a wide margin**

The least exciting item on the list and the one that decides whether there is a
second user.

Edge boxes lose power. That is not a hypothetical failure mode, it is the
defining property of the deployment target — no UPS, no datacentre, a domestic
breaker. If a power cut corrupts the database, the user is gone and so is
everyone they talk to. No feature elsewhere on this list compensates for that.

There is a foundation to build on: `tests/persist_crash_consistency_host_test.c`
exists, and the project's mutation-testing discipline is already stronger than
most commercial databases'. What is missing is adversarial:

- **Fault injection at the storage boundary** — torn writes at sector
  granularity, a flush that reports success and drops, power loss between WAL
  append and data page write, and the same again during checkpoint restore.
- **Property-based testing of the SQL path** — generate schemas and statement
  sequences, compare against a reference model, shrink failures. The
  parser/exec/index stack is ~4 files of hand-written code and has never met an
  input it did not expect.
- **Linearizability checking on the cluster** — Jepsen-shaped, not necessarily
  Jepsen. Generate concurrent histories against the consensus layer, partition
  the multicast segment, verify the history is linearizable. Consensus code
  that has not been partition-tested is consensus code that has not been
  tested.

**And run all of it on ARM64, not only x86-64.**

That last point is the one I would push hardest on, because it is
non-obvious and it bites late. **ARM64 has a substantially weaker memory model
than x86-64.** On x86 you get total-store-order almost for free: stores are not
reordered with other stores, loads are not reordered with other loads. On
AArch64 both can be reordered, and a missing barrier that is *invisible* on x86
becomes a real, intermittent, once-a-week corruption on ARM.

Anywhere this kernel has a lock-free ring, a publish-then-set-flag pattern, a
double-checked initialisation, or a WAL sequence counter read without a
barrier, x86 has been silently forgiving it. The ARM64 port is new. Those bugs
are almost certainly present and have not yet had the chance to show
themselves. Finding them with a stress harness is cheap; finding them in the
user's data is not.

**Budget: 2.5 months, one person, front-loaded.**
**Gate:** 10,000 injected crashes across x86-64 and ARM64 with zero
inconsistent recoveries, and a linearizability run that survives partition.

---

#### #2 Nodes/LPARs as language environments → **second, but drastically narrowed**

This is the adoption path. Nobody adopts an operating system to run nothing on
it, so this is what converts the architecture into a user.

But "full Node/Rust/Python environments" is not a six-month item for one
person; it is not a six-month item for five. Each of those runtimes assumes a
POSIX substrate — dynamic linking, threads, mmap with W^X, epoll, a filesystem,
signals. That surface is bigger than everything built so far.

**The IBM i answer is already the right one: this is PASE.**

IBM i does not run AIX applications by reimplementing AIX. It runs them in a
compatibility environment beside native ILE programs, and has for thirty years.
The equivalent here is a **Linux syscall ABI shim on the ARM64 kernel, for
statically linked binaries only**:

- No dynamic linker, no `.so` loading, no `dlopen`. Static musl or nothing.
- A subset of the syscall surface, chosen by what the target binary actually
  calls — measured with `strace`, not guessed. 131 `SYS_` handlers already
  exist -- `case SYS_` arms in `syscall_dispatch.c`, i.e. implemented and
  reachable, not merely `#define`d -- which is a real head start.
- Native execution on ARM64. Not translated.

Sequence, easiest to hardest, and the ordering is not negotiable:

1. **Static Rust or Go** — no runtime to speak of, no dynamic linking, small
   syscall footprint. This is the proof the shim works.
2. **Static CPython** — a bigger surface and worth it only if the first user
   needs Python. Note the catch: without `dlopen` there are no binary C
   extensions, which removes most of why people choose Python. Scope this
   honestly or not at all.
3. **Node — explicitly out of scope this window.** V8 needs a JIT, which needs
   W^X page flipping, which needs threads, which needs a scheduler contract
   this kernel does not have. It is the hardest of the three by a wide margin
   and should be the last, not the flagship.

**Note the tension with QEMU-SLS, and resolve it deliberately.** There are now
two routes to running foreign binaries: translate them (QEMU-SLS, works
cross-ISA, slow, Step 6.4 unfinished) or run them natively against a shimmed
ABI (fast, ARM64-only, needs the shim). These are not competitors — IBM i has
exactly this split — but they must be *positioned*, or they will compete for
attention by accident. The native shim serves the user. QEMU-SLS serves the
x86-binary-on-ARM story, which is a differentiator but not a month-6 blocker.

**Budget: 2 months, one person.**
**Gate:** a static binary doing the first user's actual work, running natively
on an ARM64 node, surviving a checkpoint/restore cycle.

---

#### #7 Marketing → **yes, but not the version described**

Splitting this in two, because the item as written covers two very different
things and one of them is dangerous.

**Workload simulation — do it.** Generating synthetic load shaped like a
plausible business (order volumes, query mixes, concurrent users) to size the
system and find breaking points is straightforward engineering and directly
useful. No objection.

**Simulated companies as marketing evidence — don't.**

This project's single most valuable asset is not the kernel. It is the
verification culture around it: figures withdrawn when the CV came back at 28%,
a defect severity revised *upward* in writing, "**That was wrong.**" in bold in
a document that nobody outside would ever have checked. That record is why the
technical claims are believable, and it is much rarer than a database in a
kernel.

Fictional customers spend that asset. Even carefully labelled, the label is the
first thing lost when a screenshot is reposted — and the moment one prospect
works out that Acme Logistics does not exist, every real number in every real
document becomes something to be checked rather than believed. The downside is
uncapped and the upside is a slide.

**The alternative is better and cheaper: dogfood it.** Gridworkz is a real
business with real operations — billing, monitoring, logs, internal tooling.
Move one of them onto AeroSLS and publish what happens, including what breaks.
"We run our own billing on it, here is the uptime, here is the incident where
it lost a node" is worth more than ten invented logos, and it finds the bugs
before the user does. It also makes the first-user conversation honest: you are
not asking them to be first.

And publish the verification documents as-is. `docs/` is 65 files of exactly
the kind of engineering writing that makes technical buyers trust a small
vendor. That is the marketing. It already exists.

**Budget: continuous, ~0.5 person-months of write-up.**

---

### DEFER — with the reason, not just the verdict

---

#### #5 SSO → **no. But the question underneath it is a yes.**

SSO is enterprise procurement machinery. It matters when a security team with a
checklist is between you and a signature. One user on an edge box is not that,
and SAML/OIDC built speculatively will be built to the wrong spec — every buyer
wants their own IdP quirks.

What the "etc." in the question is actually pointing at, and what *is* needed:

- **Transport security** (§2 — the real gap)
- **Credential hygiene** — API tokens that can be rotated and revoked without a
  reboot
- **Audit trail** — `kernel/security_audit.c` exists; make sure it records
  enough to answer "who changed this row" after the fact

Do those three. Do SSO when a named prospect makes it a condition, and build it
against their IdP.

---

#### #4 Auto-scaling nodes → **no, and this is the clearest one**

Auto-scaling is a cloud-economics feature: it exists because cloud capacity is
elastic and metered by the second. **An edge deployment is one to ten boxes
that are already bought and already powered on.** There is nothing to scale
into. The economics that justify the feature are absent from the chosen target.

This is worth flagging as a category, because three of the four deferrals share
it: auto-scaling, SSO, and a general-purpose application server are all
cloud-platform features. They are on the list because that is what modern
infrastructure roadmaps contain — but the architecture being built is
deliberately not that, and importing its roadmap dilutes the thing that makes
it interesting.

The primitives are already there and demo well — `checkpoint_mgr`,
`checkpoint_delta`, `pte_migrate`, `simi_ctx_migrate`, and the reconcile
loop in `workload.c` / `workload_ctx.c`.
Showing a live workload migrating between nodes is a strong demo and costs a
day. Productising cross-host pool placement is months and, this window, serves
nobody.

---

#### #3 Web server / application server in the kernel → **mostly no, one narrow yes**

`net/http.c` already exists and serves the Navigator. Building a general
application server on top of it is commodity work that every platform has, and
if the user's application is a static binary that binds a socket (see #2), they
do not need one.

The narrow version is genuinely differentiated and worth keeping on the shelf:
**an HTTP route as a first-class object, dispatching to a program object,
reading the database through the single-level store with no marshalling
anywhere.** No serialisation, no connection pool, no ORM — the handler
addresses the row directly. That is a thing no commodity stack can do and it
follows straight from the architecture.

It is also a *product* decision disguised as an engineering one, and it should
be made after the first user, when there is evidence about what they actually
build. Note it, do not build it.

---

#### #1 Native applications / a language → **no, and this is the seductive one**

The item most likely to consume the entire six months and produce nothing a
user can run.

Designing a language is a career. Designing one that is *worth adopting* over
Rust, Go and Python — for a platform with no users yet — is not a resourcing
question, it is a category error. RPG did not win on language design; it won
because it was already there when the applications were written.

And the framing may be off by one layer: **SIMI is already the TIMI analog.**
The machine-independent program representation exists, with three backends and
an interpreter. What does not exist is a *source* language targeting it — and
the honest answer to "which language" is that the first user answers it, not
the roadmap.

If some native-program capability is wanted for the story, the cheap version is
a **C-to-SIMI path producing one small, real program object**, enough to prove
the pipeline end to end. That is weeks, not months, and it can wait until the
DB harness and the ABI shim have landed.

---

## 4. The sequence

```
Month  1      2      3      4      5      6
       ├──────┴──────┴──┐
       │  #6 DB harness │  crash injection, property tests,
       │  + ARM64 memory│  linearizability, ARM64 barrier audit
       │    model audit │
       └────────────────┘
              ├──────┴──────┴──────┐
              │  #2 Linux ABI shim │  static Rust/Go first,
              │  (PASE equivalent) │  CPython only if needed
              └────────────────────┘
                     ├──┐
                     │TLS│  option A (proxy), documented honestly
                     └──┘
                            ├──────┴──────┴──────┐
                            │  First user deploy │  install, upgrade,
                            │  + ops             │  backup/restore, monitoring
                            └────────────────────┘
       ├──────────────────────────────────────────┤
       │  #7 dogfood + publish the docs (continuous)│
       └──────────────────────────────────────────┘
```

Two people map onto this cleanly: one holds the kernel/DB line through months
1-3 and the ops work later; the other takes the ABI shim from month 2 and the
user-facing deployment from month 4.

---

## 5. The thing this roadmap depends on that is not engineering

**There is no named first user.**

Every item above is sequenced against "one real user in production", and the
single largest risk is that the user is hypothetical for five of the six
months. A roadmap toward an imaginary user produces a system that fits an
imaginary user — and the #2 decision in particular (which language, which
syscalls, which binary) is *unanswerable* without one. `strace` on their actual
workload is the specification.

**This should be started in week 1, in parallel, before any of the engineering
below.** It is also the strongest argument for §3's dogfooding: if Gridworkz
runs something real on it, the first user exists on day one and is you.

---

## 6. Assumptions in this document that should be challenged

Stated so they can be argued with rather than inherited:

1. **That edge/ARM64 is settled.** Much of the ranking turns on it — auto-scaling
   is deferred *because* of it. If the target moves to cloud VMs, #4 moves up
   and the ARM64 memory-model audit becomes less urgent.
2. **That one user is worth more than a broad demo.** A different funding
   position would invert this and favour breadth.
3. **That the ARM64 memory-model bugs are real.** This is inference from the
   port's newness and the architecture's ordering rules, not from an observed
   failure. It is cheap to test and expensive to be wrong about, which is why
   it is ranked where it is — but nobody has seen one yet, and this document
   should not be quoted as though someone had.
4. **That TLS option A is acceptable to the first user.** Unknown until there is
   a first user. If it is not, §2's option B moves into the window and
   something else leaves it.
