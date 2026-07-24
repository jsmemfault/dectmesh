/*
 * cga.h -- Cryptographically Generated Address: ownership-proof primitives.
 *
 * The node identity node_eui = SHA256(pubkey)[:6] is bound to a persistent
 * P-256 keypair (see cga.c). These expose the two halves a verifier needs to
 * confirm the node owns its address, without the private key ever leaving cga.c.
 */
#ifndef AETHER_CGA_H
#define AETHER_CGA_H

#include <stdint.h>
#include <stddef.h>
#include <errno.h>
#include <zephyr/kernel.h>

#if defined(CONFIG_AETHER_CGA)

/* Copy the node's 64-byte P-256 public key (raw X || Y). -EAGAIN before the
 * identity has been derived at boot. */
int cga_get_pubkey(uint8_t pub[64]);

/* ECDSA-P256/SHA256 sign `msg` with the node's CGA private key into sig[64]
 * (raw R || S). A fresh per-signature nonce is drawn internally. -EAGAIN if the
 * key is not ready, -EIO on RNG/sign failure. */
int cga_sign(const uint8_t *msg, size_t len, uint8_t sig[64]);

#else  /* no CGA: the ownership proof is unavailable, callers get -ENOTSUP */

static inline int cga_get_pubkey(uint8_t pub[64]) { ARG_UNUSED(pub); return -ENOTSUP; }
static inline int cga_sign(const uint8_t *msg, size_t len, uint8_t sig[64])
{
	ARG_UNUSED(msg); ARG_UNUSED(len); ARG_UNUSED(sig); return -ENOTSUP;
}

#endif

#endif /* AETHER_CGA_H */
