# AeroSLS Multi-NIC — Plan v0.1

**Goal.** Let one kernel drive more than one network interface, so a cluster node can hold its DSPP segment *and* a host-facing management NIC at the same time — giving every node a URL.

**Status.** **Complete.** All six phases built. Every node has a URL and the dashboard can switch between them. Two of §6's open questions are already answered — see below. Every claim in §1 was read out of the tree.

---

## 0. The finding that shapes the whole design

The obvious way to read "multi-NIC" is "we need a routing table". **We do not**, and the reason is worth establishing before anything else, because it changes the cost by an order of magnitude.

The two traffic classes this kernel emits are already **completely disjoint by protocol**:

| | references to the other class |
| --- | --- |
| `net/dspp.c` (cluster traffic) | **0** mentions of IP, `net_my_ip`, or `ipv4_*` |
| `net/ipv4.c`, `arp.c`, `udp.c`, `tcp.c` | **0** mentions of `ETHERTYPE_DSPP` |

DSPP is pure L2: `ETHERTYPE_DSPP` (0x88B5), broadcast destination, node ids in its own header. It has never used an IP address and has no reason to. Everything IP-shaped — HTTP, ARP, TCP, UDP, DHCP — never speaks DSPP.

Consensus is not a third case: `net/consensus.c` transmits through `dspp_transmit_raw()` (7 call sites), so it rides the cluster interface with everything else DSPP.

**So "which interface should this frame leave by" is not a routing question — it is already answered by which file you are in.** A frame built in `dspp.c` goes out the cluster NIC; a frame built in `ipv4.c` goes out the management NIC. No route lookup, no longest-prefix match, no per-interface gateway.

That is the difference between a week and an afternoon, and it is only true because of how the protocols happen to be split today. §5 records what would break it.

---

## 1. What is actually global (verified inventory)

### The driver — `net/e1000.c`, 7 items

```c
static struct E1000TxDesc tx_ring[E1000_RING_SIZE];
static struct E1000RxDesc rx_ring[E1000_RING_SIZE];
static uint8_t            rx_bufs[E1000_RING_SIZE][E1000_RX_BUF_SIZE];
static uint16_t           tx_tail;
static uint16_t           rx_tail;
static uint8_t            e1000_pci_slot;
uint64_t                  e1000_mmio_base;      /* non-static; read by kernel.c */
```

All seven are now fields of `struct E1000Nic`, held in `e1000_nics[E1000_MAX_NICS]`. Phase 1 uses exactly one, reached through a `nic0()` accessor rather than by indexing — so Phase 2 changes the accessor, not the ~20 sites below it.

Measured rather than guessed: 128 × 2048 B of buffers + 4 KiB of descriptors = **~260 KiB per interface**, 0.2% of a 1 GiB node. Not a memory decision worth agonising over; `E1000_MAX_NICS` is 2 because that is the known need, not because 4 would be costly.

`e1000_mmio_base` stays exported for now — `kernel.c`'s PCI scan writes it and tests it as "did we find a NIC". Nothing reads it for register access any more; Phase 3 replaces that scan with an enumeration loop and the global goes away.

### The call sites — far fewer than expected

| Function | External call sites | What changes |
| -------- | ------------------- | ------------ |
| `e1000_transmit_packet()` | **4** — `arp.c`, `dspp.c`, `ipv4.c`, `udp.c` | each gains an interface argument |
| `e1000_poll_rx()` | **1** — `kernel/net_event.c` | loops over interfaces |
| `e1000_init()` | 1 real — `kernel/kernel.c` | called per NIC found |

Four transmit sites. That is the entire fan-in.

### Identity — `net/net.c`

```c
IPv4Addr net_my_ip, net_gw_ip, net_subnet_mask;
MACAddr  net_my_mac;
```

One set of four, today shared by everything. The MAC must become per-NIC (each has its own from EEPROM). The IP triple only needs to exist on the management interface — the cluster interface has no IP and wants none.

### Two things that are *not* interface-aware and would need thought

- **`arp_table[]` (`net/arp.c`)** is keyed by IP alone, with no interface column. Harmless while only one interface carries IP; a correctness bug the moment two do.
- **The self-echo guard** (`net_rx_dispatch()`) compares against the single `net_my_mac`. It must compare against *the receiving interface's* MAC, or a frame from NIC A gets dropped on NIC B for matching the wrong address.

### Boot — `kernel/kernel.c`

~~The PCI scan finds an e1000 and `break`s.~~ **Done** — it now collects every match up to `E1000_MAX_NICS`, calls `e1000_init()` per card, then assigns roles once the count is known.

---

## 2. Design: roles, not routes

Each interface is initialised with a **role**, and the role decides who transmits on it.

```c
typedef enum {
    NIC_ROLE_NONE = 0,
    NIC_ROLE_MGMT,      /* IP: HTTP, ARP, TCP, UDP, DHCP */
    NIC_ROLE_CLUSTER,   /* DSPP only: L2 broadcast, no IP */
} NicRole;
```

- `dspp.c` transmits on `nic_by_role(NIC_ROLE_CLUSTER)`
- `arp.c` / `ipv4.c` / `udp.c` transmit on `nic_by_role(NIC_ROLE_MGMT)`
- RX dispatch passes the receiving interface down, so the self-echo guard and any future per-interface logic have it

**Single-NIC boots must not change behaviour.** With one interface, it takes both roles, every `nic_by_role()` returns it, and the code path is what exists today. That is the property to hold onto: this is additive, and a one-NIC node — which is every node that exists right now — must behave identically.

### Assigning roles

The kernel cannot tell from PCI config space which segment a NIC is on. Two candidate sources, and this is the main open question (§6):

- **By enumeration order**, documented: first NIC found is MGMT, second is CLUSTER. Zero new machinery; fragile against PCI ordering changes.
- **From the kernel command line**, e.g. `nic0=mgmt nic1=cluster`. `kernel/boot_params.c` already parses `node=<n>` and its parser is general — this is one more key. Explicit, testable, and the launcher already generates a per-node command line, so it costs nothing at the call site.

**Resolved: the command line, with enumeration order as the fallback.** `e1000_assign_roles(count, cmdline)` reads `nicN=`; if none is given it defers to `e1000_assign_roles_by_order()`. It was cheap exactly as predicted — `boot_params.c` needed one new function (`boot_params_find_str()`), and the launcher already generates a per-node command line.

---

## 3. Phases

| Phase | Work | Gate |
| ----- | ---- | ---- |
| **1** | ✅ **DONE** — `struct E1000Nic` in `net/e1000.c`; all 7 globals are now fields; one instance, `nic0()`, no behaviour change | Compiles clean under `-Wall -Wextra`; whole-image link clean; 80/80 suite green |
| **2** | ✅ **DONE** — `NicRole` bitmask; `e1000_transmit(role, ...)` at all 4 TX sites; `net_rx_dispatch(..., ifindex)`; per-interface self-echo guard via `e1000_nic_mac()` | 27 checks in `net_self_echo_host_test.c` (links the real driver), 5/5 mutations caught; link clean 97/97; 80/80 suite |
| **3** | ✅ **DONE** — PCI scan enumerates instead of breaking; `e1000_init(idx, …, roles)`; per-NIC EEPROM MAC; `e1000_mmio_base` retired | 38 checks, 4/4 mutations caught; link clean 97/97; 80/80 suite |
| **4** | ✅ **DONE** — `boot_params_find_str()`; `e1000_assign_roles(count, cmdline)` reading `nicN=mgmt\|cluster\|both\|none`, enumeration order only as fallback | 57 + 61 checks, 5/5 mutations caught; link clean; 80/80 suite |
| **5** | ✅ **DONE** — management NIC on `-netdev user` with per-node `hostfwd`, PCI slots pinned, `nicN=` in each node's grub.cfg, REST ports in the pre-flight | 88 harness checks, 4/4 mutations caught |
| **6** | ✅ **DONE** — `src/lib/nodeTargets.ts` with a real refusal for unplaceable ids; launcher emits the `AEROSLS_NODES` string | 25 frontend checks; typecheck clean; 89 harness checks |

Phase 1 is worth doing alone even if the rest stalls — it is a refactor with a green suite as its own gate, and it makes every later phase small.

### What Phase 6 turned up

**The frontend was not, in fact, "just an environment variable".** The plan said Phase 6 needed "no frontend work beyond setting the environment variable". That was wrong, and the reason is the sharpest bug of the whole effort: `server.ts`'s proxy router read

```ts
// A request to an unknown node id is refused rather than silently
// served by the default target -- answering for the wrong machine is
// worse than answering "I do not know where that node is".
return NODE_TARGETS.get(id) || DEFAULT_KERNEL;      // <- a FALLBACK
```

The comment describes a refusal. The code is a fallback. With `AEROSLS_NODES` unset, `/node/4/api/cluster` was proxied to `localhost:3001` — node 1 — and the panel rendered **node 1's roster, memory and workload counts under the heading "node 4"**. No error, no warning. In a monitoring UI that is the worst shape a bug can take, and the comment above it had predicted it exactly.

The decision now lives in `resolveNodeTarget()`, a pure function with a test, and the express wiring refuses through a guard mounted ahead of the proxy.

**A discriminated union was the wrong shape here.** `{ ok: true; target } | { ok: false; reason }` does not narrow in this project, because its `tsconfig.json` has no `strict` — boolean-literal discriminants get widened. A flat `{ target: string | null; reason }` carries the same information and needs no narrowing. Worth knowing before reaching for that pattern again in this repo.

**The launcher emits the address book rather than describing it.** A hand-typed `AEROSLS_NODES` with one wrong port shows another node's data under the wrong heading — the same failure by a different route. `run-cluster.sh` now prints the exact string, and the harness checks its ports match the `hostfwd` ports it actually assigned.

### What Phase 5 turned up

**PCI slot order is not `-device` order, and the whole scheme rests on it.** The kernel is told `nic0=mgmt nic1=cluster`, where "nic0" means the first e1000 the PCI scan finds — the lowest slot. QEMU makes no promise that the order of `-device` arguments matches slot assignment. Both cards therefore carry an explicit `addr=`. Without it a bus reordering would put DSPP on the NAT and HTTP on the cluster wire: **neither would error**, and nothing would reach anything.

**Two MAC prefixes, not one.** `…:AE:51:0i` for cluster, `…:AE:52:0i` for management. Sharing a prefix would eventually collide across a node's two cards, and the self-echo guard keys on exactly that — a collision means dropping the peer's real traffic as "our own". The harness asserts all 6 interfaces in a 3-node cluster are distinct.

**A backtick-comment idiom that happened to be safe.** The first version interleaved `` `# ...` `` comments inside the `printf` building the argv. Those are command substitutions producing empty output, which — unquoted — vanish entirely rather than becoming empty arguments. It worked, but the argv is the one thing that must be unambiguous, and quoting one of them would have silently inserted an empty argument QEMU rejects. Rebuilt as an array with ordinary comments.

**REST ports are exclusive; the segment port is not.** The pre-flight now checks both console and REST ports but still deliberately skips the multicast port, which every node binds by design. The REST check also names the collision an operator will actually hit: `make x86-run` forwards host 3001, which is node 1's port.

### What Phase 4 turned up

**Naming one interface names them all.** An explicit `nicN=` for *any* card switches the whole assignment to command-line mode, so an unnamed card gets **no** role rather than quietly inheriting the enumeration-order convention. Half-explicit configuration is the ambiguity the command line exists to remove; letting it through would mean an operator who named one interface silently got a default for the other.

**A typo must not resolve to something plausible.** `nic0=mgnt` leaves that card with no role and logs. Falling through to management would put DSPP on the wrong wire with nothing to show for it, and a mutation doing exactly that was caught.

**Refusing an over-long value is load-bearing, and the first test could not see it.** `boot_params_find_str()` refuses rather than truncates, because `nic0=clusterX` clipped to `cluster` is a *valid role name* and would silently misroute. The mutation survived at first: the role test's 16-byte buffer meant truncation never landed on a valid name. Moving the check into the parser's own test with an 8-byte cap — where `clusterX` clips to exactly `cluster` — made it observable. **A property is only tested at the buffer size where it can go wrong.**

**Reversed roles in the test, deliberately.** The command-line case asserts `nic0=cluster nic1=mgmt` — the opposite of the fallback — so it cannot pass by coincidence if the command line were ignored.

### What Phase 3 turned up

**The whole-image link earned its keep again.** Retiring `e1000_mmio_base` compiled fine everywhere — and the link found `kernel/net_event.c` still gating its poll on it, plus a stale `extern` in the header. Neither is reachable by compiling a file in isolation.

**Only the management interface may publish `net_my_mac`.** That global is what ARP answers with and what the IP path stamps on every frame. With two cards each reading its EEPROM into it, whichever initialised last would decide the node's IP identity — so a cluster-only card could end up announcing itself as the node. The gate is one line, and it was **untestable where it first sat**: inside `e1000_init()`, behind MMIO. A mutation deleting it survived the entire suite. Moving the rule into `e1000_nic_set_mac()` — which is also its more natural home, since that is where an address becomes known — made it reachable, and the mutation is now caught.

**A half-reset makes tests pass for the wrong reason.** `e1000_nics_reset_for_test()` originally cleared only `present`. Stale MACs survived it, so a later `bind()` with no address silently inherited the previous test's identity, and a mutation deleting `set_mac()`'s store passed. Two separate mutations survived on that one weakness. The reset now clears the whole entry.

**Roles are assigned after the scan, not during it.** A single card has to hold both, and that is not knowable mid-loop. `e1000_assign_roles_by_order()` runs once the count is in — and the one-card case is the path every existing node takes, so it is asserted rather than assumed.

### What Phase 2 turned up

**The self-echo guard was already wrong for two interfaces, and the test could not have said so.** It compared against one global MAC, so a frame legitimately sent by the peer NIC would be dropped on the other for "matching us". Fixing it meant `net_rx_dispatch()` taking the receiving `ifindex` and asking `e1000_nic_mac()` — and it meant the test linking the **real** driver instead of stubbing the lookup, because a stub cannot model two identities. Scenario 7 is the case that only became expressible then: **dropped on A, delivered on B**.

**A rename beat a compatible signature.** `e1000_transmit_packet()` → `e1000_transmit(role, ...)`. Keeping the old name and picking an interface internally would have been less churn and exactly the kind of default that is wrong without anyone noticing. The rename forced all four call sites to be visited, and the whole-image link found every one — including four host tests whose stubs still carried the old name.

**No route means no fallback.** When no interface holds the requested role the frame is dropped and counted (`e1000_tx_no_route`), never sent out "whichever exists". Sending cluster traffic from a management NIC would leak DSPP onto the wrong segment; sending IP out the cluster NIC goes nowhere. A mutation making the lookup return 0 instead of -1 was caught.

**Test-only reset, named as such.** `e1000_nics_reset_for_test()` exists because production never unbinds — interfaces are found once at boot — but a test that cannot return to "nothing bound" cannot check that the lookup *fails* rather than defaulting to interface 0.

---

## 4. What this unlocks

**Per-node URLs.** `run-cluster.sh` gives node *i* `hostfwd=tcp::(3000+i)-:3000`, so node 4 is `http://localhost:3004`, and `aeroslsctl --host localhost:3004` works against it.

**The frontend is already waiting for this.** `slsos-sim/server.ts` has a `nodeProxy` reading `AEROSLS_NODES="1=http://localhost:3001,2=..."`, and `apiFetch.ts` has `setSelectedNode()` / `routeForNode()` routing every call through `/node/<id>/api/...`. That routing was built for a cluster it could not yet reach. Multi-NIC is the missing half — no frontend work beyond setting the environment variable.

**Beyond this immediate need:** a separate storage or management network, a bridged/tap deployment on real hardware where the cluster link is a physical second NIC, and any future per-interface policy. The role enum is the seam all of those hang off.

---

## 5. Risks, and what would invalidate §0

**The protocol disjointness is a property of today's code, not a law.** Anything that puts IP on the cluster segment — node-to-node HTTP, a DHCP server for the cluster, IP-based DSPP framing — turns the interface choice back into a routing decision, and then §2 is not enough. Worth writing the role assignment so that a real route table could replace it later without touching the four call sites.

**`arp_table[]` has no interface column.** Safe while one interface carries IP. Adding IP to a second one requires that column, or an ARP reply learned on one segment will be used to address a peer on the other.

**Every node may still hold `10.0.2.15`.** Multi-NIC does not hand out distinct addresses; it removes the risk, because each management NIC sits on its own isolated slirp NAT where the duplicate never meets a peer. Nodes are told apart from the host by `hostfwd` port. Worth stating so it does not read as "addressing solved".

**Memory — measured, and not a concern.** An earlier draft of this section called `rx_bufs` "not small", which was a hedge rather than a number. Measured: `E1000_RING_SIZE` 128 × `E1000_RX_BUF_SIZE` 2048 = **256 KiB** of buffers, plus 4 KiB of descriptors, so **~260 KiB per NIC**. A second interface costs 0.2% of a 1 GiB node and 0.2% of the 120 MiB image. Even four NICs is ~1 MiB. This does not constrain the design, and open question 2 is therefore not a memory trade-off — it is only about how much unused array to carry.

---

## 6. Open questions

1. ~~**Role assignment: enumeration order or command line?**~~ **Resolved in Phase 4 — command line, with enumeration order as the fallback.**
2. **How many interfaces should the array hold?** Two covers the known need. Four costs `rx_bufs` × 2 more for cases nobody has asked for.
3. ~~**Should the management NIC get DHCP back?**~~ **Not a question — DHCP is already built and already runs.** `net/dhcp.c` is a complete 230-line client (DISCOVER → OFFER → REQUEST → ACK, ~5 s timeout, updates `net_my_ip`/`net_gw_ip`/`net_subnet_mask`), and `kernel.c` already calls `dhcp_start()` at boot. It falls back to the compiled-in `10.0.2.15` only because it times out — the cluster's `-netdev socket,mcast=` segment has no DHCP server on it.

   A management NIC on `-netdev user` puts it on a slirp network that *does* run one, so the lease happens with **no new code**. Nothing to implement; the interface just has to exist.

   One honest detail: slirp hands the first guest `10.0.2.15` regardless, so nodes may still all hold that address — but each is on its own isolated slirp instance where it never meets a peer, and from the host they are distinguished by `hostfwd` port, not by guest IP. That is a genuine fix for the collision risk, not cosmetic, but it is not "every node gets a distinct address".
