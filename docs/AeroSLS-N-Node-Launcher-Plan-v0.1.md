# AeroSLS N-Node Launcher — Plan v0.1

**Goal.** Replace `run-two-nodes.sh` with `run-cluster.sh`, taking a node count and a per-node size, detecting the host's capacity, and refusing or right-sizing rather than thrashing the machine.

**Status.** **Complete.** All five phases built, and §0's blocker fully closed — a networked node self-identifies at boot and has a working console. Every constraint in §1 was read out of the tree, with file and line, rather than assumed.

---

## 0. The blocker: these nodes have no way in — **CLOSED**

> **Resolved.** Both halves are done. Nodes self-identify at boot (0a,
> `kernel/boot_params.c`), and the HTTP loop now presents the serial console
> between sweeps (0c, `kernel/console.c` + `net/http.c`), so a clustered node
> has a prompt for the first time. `tests/console_feed_host_test.c`, 23
> checks, 7/7 mutations caught.
>
> 0c turned out far cheaper than this section estimated. The hard part was
> assumed to be plumbing the shell into the HTTP loop — but `POST
> /api/shell/exec` already ran `sls_shell_execute()` from that very loop, so
> the dispatch, the session and the output capture all existed. The only
> missing piece was a line editor that can be fed one byte at a time, since
> `read_line()` owns the CPU until ENTER.
>
> One deliberate choice: the polled console shares `serial_session` with the
> blocking loop rather than creating its own. That session carries uid, gid
> and `tx_id` — a second one would silently be a second identity with its own
> open transaction.
>
> The original analysis follows, since the reasoning about *why* each route
> was or was not available is still what governs the design.

## 0 (original analysis). The blocker: these nodes have no way in

This has to come first, because building a launcher for N undrivable nodes would be wasted work.

`kernel.c`'s final dispatch:

```c
if (e1000_mmio_base) {
    http_server_run();  // does not return — serves REST API on port 3000
}
// ── Shell (does not return) ─────────────────────────────────────────────
sls_shell_loop();
```

**The serial shell is only reached when there is no NIC.** Every node `run-two-nodes.sh` launches has an e1000 — that is the entire point of the script — so the BSP enters `http_server_run()` and never returns. `sls_shell_loop()` is dead code on any networked boot.

Cross that with the other two facts already established:

| Way in | Status |
| ------ | ------ |
| QEMU graphics window | No PS/2 driver anywhere; `read_line()` polls `inb(SERIAL_COM1_BASE)` (`read_line()` in `kernel/kernel_io.c`). A window renders VGA and accepts nothing. |
| Serial console | ~~No prompt, because `sls_shell_loop()` is never called.~~ **Fixed** — `http_server_run()` polls `serial_console_poll()` each sweep and runs the same dispatch against the same session. |
| HTTP / `aeroslsctl` | No host port forward, and can't have one: `net/e1000.c` keeps one global tx/rx ring pair and a single `e1000_pci_slot`, so a second NIC sits dead on the bus. |

**So the two-node walkthrough has never been executable**, and the telnet console added in the previous change shows boot output but never a prompt. That correction belongs in `COMMANDS.md` and the roadmap addendum regardless of whether the rest of this plan proceeds.

It also reframes the goal. Typing `cluster init <id>` into eight consoles was never the design we wanted; **nodes should self-identify at boot.** Options, cheapest first:

- **0a. Boot-time node id (recommended).** No multiboot cmdline reader exists (`grep -rn cmdline kernel/` → nothing), so add one: read the multiboot2 cmdline tag, parse `node=<n>`, call `cluster_init()` during boot. GRUB's `grub.cfg` already exists and can carry per-node `-append`-style args; QEMU's `-append` needs `-kernel` instead of `-cdrom`, which is a separate decision. Removes interactive setup entirely and is what an N-node launcher wants anyway.
- **0b. Derive the id from the MAC.** The launcher already assigns `52:54:00:12:34:0N`. Zero new plumbing, but implicit and awkward to override.
- **0c. Multiplex the serial console into the HTTP loop.** The loop already yields through `net_event_hlt_wait()` (`kernel/net_event.h`), so adding "UART has data" as a wake source is plausible, and would restore an interactive shell on networked boots. More invasive; useful independent of this plan.

**0a is the recommendation, with 0c as a follow-on** because an interactive console on a networked node is worth having on its own merits.

---

## 1. Hard constraints (all verified in-tree)

| # | Constraint | Source | Consequence |
| - | ---------- | ------ | ----------- |
| 1 | `#define CLUSTER_NODE_MAX 8` | `net/consensus.h:273` | **N ≤ 8**, full stop. Raising it means a roster resize and a persist-layout review. |
| 2 | DSPP is L2 broadcast to `ff:ff:ff:ff:ff:ff` with self-filtering | `net/dspp.c:116`, `:379` | ✅ Done — every node now joins one `-netdev socket,mcast=` segment. The old `listen=`/`connect=` pair was point-to-point and capped the cluster at exactly two. |
| 3 | ~~Exactly one AP is started~~ → **`-smp 1` is now supported** | `kernel/smp.{c,h}` | Was `-smp 2` minimum. The AP wait is now bounded and the BSP drives the service loop when there is no AP. |
| 4 | ~~The AP loop "never idles at all"~~ → **resolved by running single-core** | `kernel/net_event.h:29-30` | On `-smp 1` the spinning half does not exist, and the BSP's loop already `hlt`-waits. **An idle node now costs ~0 CPU.** Sizing is memory-bound. |
| 5 | Kernel image ends at `0x7781000` ≈ 119.5 MiB | linker `_kernel_image_end` | RAM floor is well above a toy VM. |
| 6 | Frame bitmap spans a fixed 4 GiB; `frame_pool_limit_ram()` clamps to real RAM | `kernel/frame_pool.c:78` | Under-4 GiB nodes are supported *by design*, not by accident. |
| 7 | Every node falls back to the same static IP `10.0.2.15` | `include/config.h:17`, `net/dhcp.h:8` | Harmless for DSPP (pure L2), but there is **no IP-level inter-node traffic** and no DHCP server on the segment. Do not build anything on node-to-node IP. |

### The one that mattered most — now fixed

Constraint 4 was the surprise, and it dominated everything: `net_event.h` credits the BSP's `hlt`-wait with taking that core from ~100% to <1%, then says plainly that Core 1's loop "never idles at all." A *completely idle* node consumed a full host core, making density CPU-bound at roughly `cores - 1`.

**Single-CPU operation removes it.** With `-smp 1` there is no AP loop to spin; the BSP runs the same work from its own idle points and `hlt`-waits between sweeps. Two things had to change (`kernel/smp.{c,h}`):

1. `boot_application_processors()` waited on `ap_bootstrap_lock` with an **unbounded spin**, so `-smp 1` did not merely degrade — the kernel hung at boot, silently, before any subsystem started. The wait is now bounded by `AP_BOOT_TIMEOUT_TICKS`.
2. `smp_uniprocessor_tick()` drives `flush_daemon_tick()` + `microkernel_service_poll()` from the BSP, called from the HTTP sweep and the shell's input poll. It is a no-op whenever an AP is online, checked live rather than latched, so the two can never both drive it.

Running `reconcile_tick()` on the BSP is *safer*, not merely acceptable: it is documented as queueing persist work rather than calling `persist_*()` directly precisely because it normally runs on the AP. Same core for producer and consumer means the SPSC ring degenerates to a plain queue and the race cannot occur.

Verified by `tests/smp_uniprocessor_host_test.c` (16 checks, 5/5 mutations caught), which links the real `smp.c`.

### What Phase 2 turned up

Moving to a shared segment introduced an input class the stack had never seen. QEMU forces `IP_MULTICAST_LOOP` on for `socket,mcast=` sockets — deliberately, so several instances on one host can hear each other — which means **every node also receives its own transmissions**. Point-to-point mode never did that.

DSPP already survived it: the migrate families filter on `node_dest_id`, the service family on `node_source_id`. ARP did not, and ARP was the real exposure — every node compiles in the same static IP (`10.0.2.15`), because DHCP times out on a segment with no server. A node hearing its own gratuitous ARP is hearing its own address claimed from elsewhere on the wire.

The fix is one guard in `net_rx_dispatch()`, at the Ethernet layer, before the ethertype demux: drop frames carrying our own source MAC, and count them. Teaching each protocol the same lesson separately would have left the next one to learn it the hard way. It is skipped while `net_my_mac` is still all-zero — before `e1000_init()`, "our MAC" is not yet a fact, and comparing against zero would be matching on ignorance.

A second consequence, easy to miss: the launcher's port pre-flight used to include the segment port. With mcast that port is bound by **every** node on purpose — the shared bind *is* the segment — so checking it would refuse a launch that is working exactly as designed. It now checks only the console ports, and the harness pins that.

---

## 2. Sizing model

Detect at runtime, on Linux:

| Resource | Source |
| -------- | ------ |
| Cores | `nproc` |
| Available RAM | `MemAvailable` in `/proc/meminfo` (not `MemTotal` — the host is using some) |
| Free disk | `df -PB1 <repo>` |
| KVM | `[ -w /dev/kvm ]` — without it QEMU is TCG-emulated and perhaps 10× slower; the launcher should say so loudly |

Then:

```
N_cpu   = max(1, cores - 1)                  # constraint 4: ~1 core per node, 1 left for the host
N_ram   = (MemAvailable - host_reserve) / per_node_ram
N_disk  = (free_disk - margin) / per_node_disk_actual
N_max   = min(8, N_cpu, N_ram, N_disk)       # constraint 1
```

`per_node_disk_actual` is not the 10 G virtual size — `qemu-img create -f raw` makes a **sparse** file, so real consumption starts near zero and grows. Budget on observed growth, not virtual size, and say which is being reported.

### Worked example — the target server (16 GB, headless)

Assume 4 cores, ~15 GB available, 109 GB free (from the `xorriso` output in the reported run):

| Bound | Before uniprocessor support | Now (`-smp 1`) |
| ----- | --------------------------- | -------------- |
| `N_cpu` | 4 − 1 = **3** ← binding | idle nodes `hlt`; no longer binding at this scale |
| `N_ram` @ 1 GiB/node | (15 − 2) / 1 = 13 | (15 − 2) / 1 = 13 |
| `N_disk` | plenty | plenty |
| `CLUSTER_NODE_MAX` | 8 | **8** ← now binding |

**Answer: 3 → 8 nodes**, and the binding constraint is now the roster cap rather than the host. Whether 8 × 1 GiB is comfortable on a 16 GB box under real load is a separate question from whether it boots; the launcher should still reserve host headroom and report which bound it hit.

`N_cpu` does not disappear — a *busy* node still uses CPU. It stops being the constraint for idle or lightly-loaded clusters, which is what this launcher is for.

### Defaults

| Setting | Default | Floor | Reasoning |
| ------- | ------- | ----- | --------- |
| RAM/node | 1 GiB | 512 MiB hard-refuse below 256 MiB | Image is ~120 MiB; below ~256 MiB the pool has almost nothing above it |
| vCPU/node | **1** | 1 | Single-CPU is supported; a second core buys a spinning AP loop, not throughput |
| Disk/node | 10 G sparse | — | Unchanged from today |

---

## 3. Proposed interface

```bash
./run-cluster.sh                      # auto-size, print the reasoning, ask to confirm
./run-cluster.sh --nodes 3
./run-cluster.sh --nodes 5 --ram 2G --force   # over capacity: refuse unless --force
./run-cluster.sh --nodes auto --dry-run       # print the plan and the qemu argv, launch nothing
./run-cluster.sh --stop                       # tear down a previous run
```

- **Auto-size prints its arithmetic.** Which bound was binding, and what to change to raise it. A number with no reasoning invites the "why only 3?" question the model above exists to answer.
- **Explicit N over capacity is refused, not silently clamped**, with `--force` to override. Silent clamping means asking for 8 and getting 3 without noticing.
- **`--dry-run` prints the full QEMU argv.** Everything learned in the last two rounds argues for being able to inspect the command without running it.
- **Port allocation:** consoles at `CON_BASE + i` (default 12341…), the multicast group fixed. Probe every port before building the ISO — the existing script already learned that lesson.
- **Liveness:** the `assert_alive` probe applies per node; a partial cluster reports which nodes came up and which didn't, and tears the rest down rather than leaving orphans.

---

## 4. Phasing

| Phase | Work | Gate |
| ----- | ---- | ---- |
| **1** | ✅ **DONE** — `kernel/boot_params.{c,h}`: multiboot2 cmdline tag reader + `node=<n>` → `cluster_init()`, called before `partition_init()` | `tests/boot_params_host_test.c`, 49 checks, 7/7 mutations caught. Whole-image link clean. 77/77 suite green |
| **2** | ✅ **DONE** — `-netdev socket,mcast=239.192.152.40:12340` on every node; self-echo guard in `net_rx_dispatch()` | `tests/net_self_echo_host_test.c` 14 checks, 5/5 mutations caught; harness 24 checks. Link clean, 79/79 suite green |
| **3** | ✅ **DONE** — `run-cluster.sh`: N nodes, per-node ISO carrying `node=<i>`, port allocation, one-pass liveness, `--stop`, `--dry-run` | `tests/run_cluster_harness.sh` 43 checks, 5/5 mutations caught |
| **4** | ✅ **DONE** — capacity detection, `--nodes auto`, per-resource bounds with the binding one named | Harness scenarios 8/8b/9, 22 checks, faked `/proc/meminfo`; 65 total, mutations caught |
| **5** | ✅ **DONE** — `run-two-nodes.sh` and its harness removed, their unique checks ported into `run_cluster_harness.sh` first; every reference repointed | 75 harness checks; doc pass verified against the code |
| **6** *(optional)* | §0c multiplex the serial console into the HTTP loop | Interactive shell on a networked node |

Phases 3 and 4 are testable **without a multi-node machine at all**, using the stub-QEMU approach: the sizing arithmetic and the argv construction are pure functions of detected inputs, and faking those inputs is trivial. That matters, since the sandbox has no QEMU.

### What closing §0 turned up

**A file that was never in the build.** `kernel/boot_params.c` shipped in Phase 1 without being added to `X86_C_SRC`, so `make x86-iso` would have failed on an undefined `boot_params_scan_mb2()`. Everything that was supposed to catch that looked elsewhere: it compiled clean standalone, its host test passed 49 checks, and the whole-image link check passed — because that check **globs the tree** for `.c` files rather than reading the Makefile. It happily linked a file the real build would never compile.

The durable fix is `tests/makefile_sources_check.sh`, whose only job is comparing those two lists, with an explicit exclusion table so "we meant to leave that out" has to say why. It was verified by removing the entry again and watching it fail. The whole-image link check now also takes its file list *from the Makefile* rather than from a glob.

### What Phase 5 turned up

**Retiring a script must not retire its tests.** `run_two_nodes_harness.sh` held three checks with no equivalent in the cluster harness: the `AEROSLS_DISPLAY` override, the shared-segment port that must *not* be treated as a clash, and the gtk-failure path — the actual failure originally reported from the server. Those moved first, as scenario 10, and only then were the two files removed. Deleting them together would have quietly dropped coverage of the one bug this whole effort started from.

**Line-number citations rot, and they rotted here.** `kernel/kernel.c:372` and `kernel/kernel_io.c:187` were both wrong by the end of the session — the files had grown by 28 and 18 lines under my own edits. Both now cite the function instead, which does not move. A sweep re-verified all 26 remaining `file:line` citations across the docs; the rest still resolve.

**A status line that contradicted itself.** The plan's header simultaneously claimed "only Phase 5 remains" and "§0's blocker is now half-closed", the second being leftover text from two edits earlier. Worth noting because it is the failure mode of incremental doc updates: new text lands, old text survives beside it, and both look authored.

### What Phase 4 turned up

**CPU is advisory, and saying so is the point.** RAM, disk and the roster cap are hard bounds; cores are not, because after uniprocessor support an idle node halts and costs almost nothing. Folding cores into the hard minimum would have reproduced the old "3 nodes" answer that was only ever true when the AP spun. The launcher prints the busy-node figure separately and says it is not a limit — the real risk is *N busy at once*, not N existing.

**An absolute limit must not be reported as a host limit.** The first version checked capacity before the roster range, so `--nodes 9` came back as "exceeds this host's capacity of 8" — inviting someone to go find a bigger machine for a number no machine can serve. The range check now runs first and says so explicitly. Harness scenario 2 caught it.

**Overrides that short-circuit detection also short-circuit its tests.** The harness sets `AEROSLS_HOST_MEM_MB`, so `detect_mem_mb()` never ran, and a mutation swapping `MemAvailable` for `MemTotal` survived the entire suite — the one line whose whole job is that distinction. `/proc/meminfo` is now behind `AEROSLS_MEMINFO` so a fixture can be pointed at it, and scenario 8b exercises the reading rather than the arithmetic over it. Worth generalising: an override added for testability can remove the thing it was meant to test.

### What Phase 3 turned up

Two things worth recording, both about *waiting*.

**The per-node settle was serialised for a reason that no longer existed.** `run-two-nodes.sh` waited after each launch because its point-to-point netdev required the listener to be bound before the other side connected — QEMU's connect side does not retry. A multicast segment has no such ordering: every node joins independently. Carrying the pattern forward would have cost `N × 2s` to learn nothing, so the launcher now starts everything and settles once. It also reports **every** failed node rather than the first, because when several die they usually die of the same cause and seeing one of five sends you to the wrong node.

**The node cap is read from `net/consensus.h`, not hardcoded.** `cluster_init()` refuses an id above `CLUSTER_NODE_MAX` and the node stays STANDALONE — so a launcher with its own stale copy of the number would happily start nodes that boot, look fine, and never join. The harness proves the coupling by raising the cap in a scratch copy of the header and watching the launcher's limit move with it.

The harness also caught a bug in itself worth noting, because it is a trap for any shell test: a stub that runs `sleep 300` without `exec` leaves an orphaned `sleep` holding the caller's captured stdout pipe when the wrapper is killed, hanging the command substitution for the full 300 seconds. Real QEMU is a single killable process; `exec` makes the stub behave the same way.

---

## 5. Deliberately out of scope

- **Raising `CLUSTER_NODE_MAX` past 8.** Roster resize plus persist-layout review; no reason to until 8 is demonstrably tight.
- **Node-to-node IP.** Constraint 7 — every node shares one static IP and there is no DHCP server on the segment. DSPP is L2 and does not care.
- **HTTP / `aeroslsctl` against cluster nodes.** Constraint: the single-NIC driver. Would need multi-NIC support in `net/e1000.c`, which is a real project and unrelated to launching.
- **Cross-node scheduling.** Still nothing chooses which node a workload lands on. Unchanged by this plan and worth naming so the launcher isn't mistaken for progress on it.

---

## 6. Open questions

1. ~~**`-kernel` vs `-cdrom`.**~~ **Resolved: per-node ISO.** `arch/x86/boot.asm` declares only a Multiboot2 header (`0xe85250d6`) with no v1 header alongside it, and QEMU's x86 `-kernel` loader implements Multiboot v1 only — so `-append` cannot be used at all. Each node needs boot media carrying its own `node=`. `grub-mkrescue` takes about a second and the ISO is ~14 MB, so eight of them is a non-issue.

   Deriving the id from the MAC was the obvious way to avoid per-node media, and it does not work: `partition_init()` stamps `PARTITION_SYSTEM`'s owner from `cluster_local_node_id()` early in boot, while `net_my_mac` does not exist until `e1000_init()` far below it. Identity has to be settled first, and the command line is available before anything runs. `tests/boot_params_host_test.c` scenario 6 pins that ordering so it cannot be quietly reversed.
2. ~~**Is one core per node acceptable?**~~ **Resolved by supporting `-smp 1`** rather than by making the AP idle. The spinning loop is simply not started, and the BSP — which already `hlt`-waits — picks up its work. Giving the AP a `hlt`-yield remains worthwhile for multi-core deployments that genuinely want the second core, but it is no longer on the path to node density.
3. **Sparse-disk accounting.** Should the launcher refuse based on virtual size (safe, pessimistic) or observed growth (accurate, can over-commit)?

Question 2 is the highest-value one. If the AP can be made to idle, the practical node count on the target server goes from **3 to roughly 13**, and this stops being a CPU-bound problem.
