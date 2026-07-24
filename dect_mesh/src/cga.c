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
 * management of the key for now"). Each boot the seed is imported as a VOLATILE
 * PSA keypair (no persistent-key backend / TF-M ITS needed on this minimal-TF-M
 * build) purely to derive the public key -> address. The keypair carries
 * SIGN_HASH usage so Stage 2 can add an ownership-proof signature over the same
 * identity without changing the key.
 *
 * Future state (TF-M): move the seed into the secure enclave (PSA persistent key
 * in ITS) so it never lives in app flash. The address derivation is unchanged.
 *
 * Implements the weak aether_node_identity_override() hook from aephyr.
 */
#include <zephyr/kernel.h>
#include <zephyr/net/aether_mesh.h>
#include <zephyr/settings/settings.h>
#include <psa/crypto.h>
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

/* Load the persisted identity seed, or generate + persist one on first boot. */
static int cga_load_seed(void)
{
	int err = settings_subsys_init();

	if (err) {
		LOG_ERR("settings init: %d", err);
		return err;
	}
	(void)settings_load_subtree("cga");
	if (cga_have_seed) {
		return 0;
	}

	/* First boot: draw a fresh CSPRNG seed and persist it. */
	if (psa_generate_random(cga_seed, CGA_SEED_LEN) != PSA_SUCCESS) {
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

/* pubkey = seed*G, via a transient PSA keypair imported from the seed scalar. */
static int cga_pubkey(uint8_t *pub, size_t pub_size, size_t *publen)
{
	psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;

	psa_set_key_type(&attr, PSA_KEY_TYPE_ECC_KEY_PAIR(PSA_ECC_FAMILY_SECP_R1));
	psa_set_key_bits(&attr, 256);
	psa_set_key_usage_flags(&attr, PSA_KEY_USAGE_SIGN_HASH);   /* for Stage 2 */
	psa_set_key_algorithm(&attr, PSA_ALG_ECDSA(PSA_ALG_SHA_256));

	psa_key_id_t key;
	psa_status_t st = psa_import_key(&attr, cga_seed, CGA_SEED_LEN, &key);

	if (st != PSA_SUCCESS) {
		LOG_ERR("import seed: %d", (int)st);
		return -EIO;
	}
	/* Exporting the PUBLIC half of a keypair needs no EXPORT usage flag. */
	st = psa_export_public_key(key, pub, pub_size, publen);
	psa_destroy_key(key);
	if (st != PSA_SUCCESS) {
		LOG_ERR("export pubkey: %d", (int)st);
		return -EIO;
	}
	return 0;
}

/* aephyr hook: node_eui = SHA256(pubkey)[:6], locally-administered unicast. */
int aether_node_identity_override(uint8_t out[6])
{
	uint8_t pub[65];   /* P-256 uncompressed public key: 04 || X || Y = 65 bytes */
	uint8_t hash[32];
	size_t publen = 0, hlen = 0;

	if (psa_crypto_init() != PSA_SUCCESS) {
		LOG_WRN("PSA init failed -- falling back to non-persistent identity");
		return -EIO;
	}
	if (cga_load_seed() < 0) {
		return -EIO;
	}
	if (cga_pubkey(pub, sizeof(pub), &publen) < 0) {
		return -EIO;
	}
	if (psa_hash_compute(PSA_ALG_SHA_256, pub, publen,
			     hash, sizeof(hash), &hlen) != PSA_SUCCESS || hlen < 6) {
		LOG_ERR("SHA256(pubkey) failed");
		return -EIO;
	}

	memcpy(out, hash, 6);
	out[0] = (uint8_t)((out[0] | 0x02) & 0xFE);   /* locally-administered unicast */
	LOG_INF("CGA node identity %02x:%02x:%02x:%02x:%02x:%02x (persistent)",
		out[0], out[1], out[2], out[3], out[4], out[5]);
	return 0;
}
