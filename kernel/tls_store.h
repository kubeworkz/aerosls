#ifndef SLS_TLS_STORE_H
#define SLS_TLS_STORE_H

/* tls_store.h — the CA certificate and CA private key, across reboots.
 *
 * ─── Why only the CA ───────────────────────────────────────────────────────
 * A browser trusts an anchor it has been shown once. Before this, every boot
 * generated a new anchor, so every boot invalidated that trust and cost a
 * manual import in each of Chrome's and Firefox's stores -- which are separate
 * stores, with separate UIs, and separate ways of getting it wrong. Three of
 * those in one afternoon is what made this necessary rather than nice.
 *
 * The leaf is deliberately NOT stored. It is regenerated, with a fresh key, on
 * every boot and signed by the stored CA. That costs one P-256 keygen at
 * startup and buys three things:
 *
 *   - exactly one long-lived secret on disk instead of two;
 *   - a leaf key whose exposure window is a single uptime;
 *   - a SAN list that follows the build rather than the disk, so changing the
 *     names a node answers to takes a reboot and not a wipe.
 *
 * ─── What this costs, stated plainly ───────────────────────────────────────
 * The CA private key is now written to NVMe in plaintext, at a fixed LBA, on a
 * disk with no encryption. Whoever reads that frame can mint certificates this
 * node's operators trust, for as long as the CA is trusted, and nothing in the
 * chain will look wrong to any verifier. That is a real and permanent increase
 * in what a stolen disk is worth, and it is the deal §6.1 has to make:
 * reboot-survivable trust is not available without a secret that survives
 * reboots.
 *
 * What is NOT acceptable is the same key silently reaching a SECOND place -- a
 * checkpoint, a snapshot, a migration stream -- because each one has its own
 * lifetime, its own copies, and its own audience. This region is therefore
 * outside checkpoint_mgr's walk by construction (it is in no persist_* region
 * and no persist_* function knows the LBA), and tests/tls_key_containment_
 * check.sh is what turns that from an intention into a fact that fails the
 * build when it stops being true.
 */

#include <stdint.h>
#include <stddef.h>

#define TLS_STORE_OK           0
#define TLS_STORE_E_BADARG   (-1)
#define TLS_STORE_E_NO_NVME  (-2)  /* no I/O queue; nothing to load or save */
#define TLS_STORE_E_IO       (-3)  /* the NVMe command itself failed */
#define TLS_STORE_E_EMPTY    (-4)  /* no store on disk (magic absent) */
#define TLS_STORE_E_FORMAT   (-5)  /* magic present, contents not usable */

/* Version of the frame layout. Bumping this makes every older store read as
 * TLS_STORE_E_FORMAT, which regenerates the CA -- correct, and costs one
 * re-import, rather than a mis-parse that produces a plausible wrong key. */
#define TLS_STORE_VERSION      1u

/* Header bytes before the payload. Fixed constants rather than sizeof() on a
 * struct, because this frame is an on-disk format and its offsets must not
 * depend on a compiler's padding decisions. */
#define TLS_STORE_HDR_BYTES    32u
#define TLS_STORE_FRAME_BYTES  4096u
#define TLS_STORE_MAX_PAYLOAD  (TLS_STORE_FRAME_BYTES - TLS_STORE_HDR_BYTES)

/* Load the stored CA. Returns TLS_STORE_E_EMPTY when there is nothing stored,
 * which is the ordinary first-boot case and not an error.
 *
 * On success `ca_der` and `ca_key_der` are filled and both lengths set.
 * `ca_key_der` receives PRIVATE KEY MATERIAL: the caller must zeroize it once
 * mbedTLS has parsed it, and must not log it, hash it into anything
 * exportable, or copy it anywhere that is itself written.
 *
 * This function does NOT check that the certificate and key are a pair --
 * tls_cert_sign_leaf() does, with mbedtls_pk_check_pair(), because it is the
 * one with mbedTLS to hand. A store that loads cleanly here can still be
 * rejected there, and that split is deliberate: it keeps this file free of the
 * vendored tree, so the LBA layout and the crypto can be reviewed apart. */
int tls_store_load(unsigned char *ca_der, size_t ca_size, size_t *ca_len,
                   unsigned char *ca_key_der, size_t ck_size, size_t *ck_len);

/* Write the CA and its key. Zeroizes its own staging frame before returning on
 * every path, so the plaintext key does not sit in a 4 KiB static buffer for
 * the rest of the uptime -- exactly the sort of second copy the header above
 * says must not exist. */
int tls_store_save(const unsigned char *ca_der, size_t ca_len,
                   const unsigned char *ca_key_der, size_t ck_len);

/* Overwrite the frame with zeroes. Used when a stored CA is rejected: an
 * unusable CA key is still a key, and leaving it readable buys nothing. */
int tls_store_wipe(void);

/* Diagnostics for /api/health. `loaded` is 1 when this boot's CA came off disk
 * rather than being generated, which is the single fact an operator needs to
 * know whether their import is still good. `unix_written` is when the store was
 * last written -- informational only; it is a number this node wrote about
 * itself and no decision is made from it. */
void tls_store_stats(int *loaded, uint64_t *unix_written);

#ifdef TLS_STORE_TEST_HOOKS
/* Test-only view of the staging frame. See kernel/tls_store.c. */
const unsigned char *tls_store_test_frame(size_t *len);
#endif

#endif /* SLS_TLS_STORE_H */
