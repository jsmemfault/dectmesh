/*
 * cga.c -- Cryptographically Generated Address for the Aether node identity.
 *
 * The durable node_eui was random-per-boot, so a node's Aether address changed
 * on every reset. Here it becomes a CGA: an address bound to a P-256 keypair the
 * node OWNS, with
 *
 *     node_eui = SHA256(public_key)[:6]  (locally-administered unicast bits set)
 *
 * Stage 1 (this file): the private key is a 32-byte scalar seed, generated once
 * on first boot from the CSPRNG and persisted in app-managed settings/NVS ("NVS
 * management of the key for now"). Each boot the public key is derived from the
 * seed (pubkey = seed*G) to recompute the address.
 *
 * We call the Oberon ocrypto P-256 primitive directly rather than going through
 * the PSA keystore: psa_import_key of a raw scalar returned NOT_SUPPORTED (-134)
 * from the PSA policy layer on this build, and the raw primitive is the same
 * math with none of that indirection. ocrypto_ecdsa_p256_sign() (same seed) is
 * ready for the Stage-2 ownership proof.
 *
 * Future state (TF-M): move the seed into the secure enclave (PSA persistent key
 * in ITS) so it never lives in app flash. The address derivation is unchanged.
 *
 * Implements the weak aether_node_identity_override() hook from aephyr.
 */
#include <zephyr/kernel.h>
#include <zephyr/net/aether_mesh.h>
#include <zephyr/settings/settings.h>
#include <psa/crypto.h>            /* psa_generate_random() for the seed */
#include <ocrypto_ecdsa_p256.h>   /* pubkey = seed*G, no PSA keystore */
#include <ocrypto_sha256.h>
#include <string.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(cga, LOG_LEVEL_INF);

#define CGA_SEED_LEN 32   /* P-256 private scalar */

static uint8_t cga_seed[CGA_SEED_LEN];
static bool cga_have_seed;

/* settings loader for "cga/seed" */
static int cga_seed_set(const char *name, size_t len,
			settings_read_cb read_cb, void *cb_arg)
{
	const char *next;

	if (!settings_name_steq(name, "seed", &next) || next) {
		return -ENOENT;
	}
	if (len != CGA_SEED_LEN) {
		return -EINVAL;
	}
	ssize_t n = read_cb(cb_arg, cga_seed, CGA_SEED_LEN);

	if (n == CGA_SEED_LEN) {
		cga_have_seed = true;
	}
	return (n < 0) ? (int)n : 0;
}
SETTINGS_STATIC_HANDLER_DEFINE(cga, "cga", NULL, cga_seed_set, NULL, NULL);

/* Draw a fresh seed that is a valid P-256 scalar (the ocrypto-recommended
 * do/while), leaving the derived public key in pub[64]. */
static int cga_new_seed(uint8_t pub[64])
{
	for (int tries = 0; tries < 8; tries++) {
		if (psa_generate_random(cga_seed, CGA_SEED_LEN) != PSA_SUCCESS) {
			return -EIO;
		}
		if (ocrypto_ecdsa_p256_public_key(pub, cga_seed) == 0) {
			return 0;   /* valid scalar */
		}
	}
	return -EIO;   /* astronomically unlikely */
}

/* Establish the identity seed and derive its public key into pub[64]:
 * load the persisted seed (self-healing if it is missing or invalid), else
 * generate + persist a fresh one on first boot. */
static int cga_identity_pubkey(uint8_t pub[64])
{
	int err = settings_subsys_init();

	if (err) {
		LOG_ERR("settings init: %d", err);
		return err;
	}
	(void)settings_load_subtree("cga");

	if (cga_have_seed && ocrypto_ecdsa_p256_public_key(pub, cga_seed) == 0) {
		return 0;   /* persisted, valid -> stable address across reboots */
	}
	if (cga_have_seed) {
		LOG_WRN("persisted CGA seed invalid -- regenerating");
	}

	if (cga_new_seed(pub) < 0) {
		LOG_ERR("CSPRNG seed failed");
		return -EIO;
	}
	err = settings_save_one("cga/seed", cga_seed, CGA_SEED_LEN);
	if (err) {
		LOG_ERR("persist seed: %d", err);
		return err;
	}
	cga_have_seed = true;
	LOG_INF("CGA identity seed generated + persisted (first boot)");
	return 0;
}

/* aephyr hook: node_eui = SHA256(pubkey)[:6], locally-administered unicast. */
int aether_node_identity_override(uint8_t out[6])
{
	uint8_t pub[64];   /* P-256 public key, raw X || Y */
	uint8_t hash[32];

	if (psa_crypto_init() != PSA_SUCCESS) {
		LOG_WRN("PSA init failed -- falling back to non-persistent identity");
		return -EIO;
	}
	if (cga_identity_pubkey(pub) < 0) {
		return -EIO;
	}
	ocrypto_sha256(hash, pub, sizeof(pub));

	memcpy(out, hash, 6);
	out[0] = (uint8_t)((out[0] | 0x02) & 0xFE);   /* locally-administered unicast */
	LOG_INF("CGA node identity %02x:%02x:%02x:%02x:%02x:%02x (persistent)",
		out[0], out[1], out[2], out[3], out[4], out[5]);
	return 0;
}
