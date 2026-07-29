/*
 * boot_params.h — kernel command line parsing, and boot-time node identity.
 *
 * ─── Why this exists ──────────────────────────────────────────────────────
 * Until now the only way to give a node its cluster identity was to type
 * `cluster init <n>` at the serial shell. On a networked boot that is not
 * merely inconvenient, it is impossible: kernel.c enters http_server_run()
 * when a NIC is present and never returns, so sls_shell_loop() is never
 * reached and no prompt is ever printed. A node with an e1000 therefore had
 * no way to be told who it was. See docs/AeroSLS-N-Node-Launcher-Plan-v0.1.md
 * §0.
 *
 * The fix is for a node to know its own identity before anything asks:
 *   multiboot2 /boot/my_sls_kernel.bin node=3
 *
 * ─── Why the command line, and not the MAC address ───────────────────────
 * Deriving the id from the NIC's MAC was the tempting option -- the launcher
 * already assigns one MAC per node, and it needs no per-node boot media. It
 * does not work, for an ordering reason that is worth recording so nobody
 * re-proposes it:
 *
 *   partition_init()  (kernel.c, early)  stamps PARTITION_SYSTEM's owner with
 *                                        cluster_local_node_id()
 *   e1000_init()      (kernel.c, late)   is the first point net_my_mac exists
 *
 * Identity must be settled BEFORE partition_init(), or partition 0 is stamped
 * as owned by node 0 while the node believes it is node 3 -- a split that
 * every ownership check downstream would then read wrongly. The multiboot2
 * info pointer is a parameter of kernel_main(), available before anything
 * runs, so the command line can be read early enough. The MAC cannot.
 *
 * The cost is that each node needs boot media carrying its own `node=`, since
 * one ISO cannot say two different things. That is cheap (grub-mkrescue takes
 * a second or so) and it is also how real hardware would do it.
 *
 * QEMU's `-append` is NOT an alternative: it requires `-kernel`, whose x86
 * loader implements Multiboot v1 only, and arch/x86/boot.asm declares a
 * Multiboot2 header (0xe85250d6) with no v1 header alongside it.
 *
 * ─── Design note: the parser is separated from the tag walk ──────────────
 * boot_params_find_uint() is pure string handling over a caller-supplied
 * buffer, so it is host-testable without a multiboot2 environment. Only
 * boot_params_scan_mb2() touches the tag structures.
 */
#ifndef BOOT_PARAMS_H
#define BOOT_PARAMS_H

#include <stdint.h>

/* Longest command line retained. GRUB permits more; a longer line is
 * truncated rather than rejected, because losing a trailing parameter is a
 * better failure than refusing to boot. */
#define BOOT_CMDLINE_MAX 256

/*
 * Find `key=<unsigned decimal>` in `cmdline` and write the value to *out.
 *
 * Returns 1 on a clean parse, 0 otherwise (key absent, no '=', no digits,
 * non-digit trailing characters, or a value that overflows uint32_t).
 * *out is only written on success.
 *
 * Matching is whole-token: the key must start at the beginning of the
 * string or immediately follow a space, and must be followed by '='. So
 * "node=3" matches key "node", but "subnode=3" and "nodeid=3" do not --
 * without that rule a future "node_role=..." parameter would be read as a
 * malformed "node".
 *
 * Trailing garbage is rejected rather than ignored: "node=3x" returns 0.
 * A parameter the operator typed wrong should be reported, not silently
 * rounded down to something plausible.
 */
int boot_params_find_uint(const char* cmdline, const char* key, uint32_t* out);

/*
 * Find `key=<token>` in `cmdline` and copy the token to `out`.
 *
 * Same whole-token matching and same refusal-over-guessing rules as
 * boot_params_find_uint(): the key must start the string or follow a space
 * and be followed by '=', and the value runs to the next space or the end.
 * Returns 1 on success with `out` NUL-terminated, 0 otherwise (key absent,
 * no '=', empty value, or a value that will not fit).
 *
 * A value too long is a FAILURE, not a truncation. `nic0=clus` silently
 * accepted as `nic0=cluster` would put DSPP on the wrong wire, and a
 * truncated role name that still matched something is exactly the class of
 * bug the uint parser already refuses.
 */
int boot_params_find_str(const char* cmdline, const char* key,
                         char* out, int cap);

/*
 * Walk the multiboot2 tag list at `mb2_phys` for the boot command line
 * (tag type 1) and copy it into an internal buffer.
 *
 * `mb2_magic` is checked first; on a bad magic nothing is stored. Safe to
 * call with a null/garbage pointer only insofar as the magic check catches
 * it -- that is the same trust this kernel already places in GRUB's handoff
 * everywhere else.
 */
void boot_params_scan_mb2(uint32_t mb2_magic, uint32_t mb2_phys);

/* The command line captured by boot_params_scan_mb2(), or "" if there was
 * none. Never NULL. */
const char* boot_params_cmdline(void);

/*
 * Resolve and apply this boot's cluster identity from the command line.
 *
 * Reads `node=<n>`; if present and in [1, CLUSTER_NODE_MAX], calls
 * cluster_init(n) and returns n. Returns 0 when no `node=` was given, which
 * leaves the node standalone -- the long-standing default, and correct for
 * every single-instance deployment.
 *
 * An out-of-range or malformed value returns 0 AND logs loudly. It does not
 * fall back to a "reasonable" id: two nodes silently agreeing they are both
 * node 1 is a worse outcome than one node staying standalone, because the
 * first is a split-brain that looks healthy.
 *
 * Must be called before partition_init() -- see the header comment above.
 */
uint32_t boot_params_apply_node_identity(void);

#endif /* BOOT_PARAMS_H */
