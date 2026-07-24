/*
 * aether_verify -- independently verify a DECTstrous node's CGA ownership proof.
 *
 * Stage 2 of the Cryptographically Generated Address work. A node's durable
 * identity is node_eui = SHA256(pubkey)[:6] (see dect_mesh/src/cga.c). This tool
 * confirms, with NO trust in the node, that the node genuinely owns that address:
 *
 *   1. address binding : SHA256(pubkey)[:6] (locally-administered) == node_eui
 *   2. key possession   : the P-256 signature over our challenge verifies
 *                         against pubkey (ECDSA-P256 / SHA-256)
 *
 * Both must hold: (1) binds the address to the key, (2) proves the node holds
 * the private key -- over a fresh, caller-chosen challenge, so it is not a replay.
 *
 * Inputs are what the 9P side (p9do) already fetched from the node:
 *   aether_verify <addr aa:bb:cc:dd:ee:ff> <challenge> <pubkey-hex> <sig-hex>
 * where <challenge> is the exact byte string written to net/aether/prove, and
 * pubkey-hex (128) / sig-hex (128) are read back from net/aether/prove.
 *
 * Build: cc aether_verify.c -I$(OPENSSL)/include -L$(OPENSSL)/lib -lcrypto
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <strings.h>
#include <openssl/evp.h>
#include <openssl/sha.h>
#include <openssl/ecdsa.h>
#include <openssl/bn.h>
#include <openssl/param_build.h>
#include <openssl/core_names.h>

static int hex2bin(const char *h, uint8_t *out, int want)
{
	int n = (int)strlen(h);

	if (n != want * 2) {
		return -1;
	}
	for (int i = 0; i < want; i++) {
		unsigned int b;

		if (sscanf(h + 2 * i, "%2x", &b) != 1) {
			return -1;
		}
		out[i] = (uint8_t)b;
	}
	return want;
}

int main(int argc, char **argv)
{
	if (argc != 5) {
		fprintf(stderr,
			"usage: %s <addr aa:bb:..> <challenge> <pubkey-hex(128)> <sig-hex(128)>\n",
			argv[0]);
		return 2;
	}
	const char *addr_s = argv[1];
	const char *chal = argv[2];
	uint8_t pub[64], sig[64];

	if (hex2bin(argv[3], pub, 64) != 64) {
		fprintf(stderr, "bad pubkey hex (want 128 chars)\n");
		return 2;
	}
	if (hex2bin(argv[4], sig, 64) != 64) {
		fprintf(stderr, "bad signature hex (want 128 chars)\n");
		return 2;
	}

	/* --- check 1: address binding, SHA256(pubkey)[:6], locally-administered --- */
	uint8_t h[32], a[6];

	SHA256(pub, 64, h);
	memcpy(a, h, 6);
	a[0] = (uint8_t)((a[0] | 0x02) & 0xFE);   /* must match cga.c */

	char acalc[24];

	snprintf(acalc, sizeof(acalc), "%02x:%02x:%02x:%02x:%02x:%02x",
		 a[0], a[1], a[2], a[3], a[4], a[5]);
	int addr_ok = (strcasecmp(acalc, addr_s) == 0);

	/* --- check 2: ECDSA-P256/SHA256 verify over the challenge --- */
	uint8_t point[65];

	point[0] = 0x04;                          /* uncompressed X || Y */
	memcpy(point + 1, pub, 64);

	EVP_PKEY *pkey = NULL;
	EVP_PKEY_CTX *pctx = EVP_PKEY_CTX_new_from_name(NULL, "EC", NULL);
	OSSL_PARAM params[] = {
		OSSL_PARAM_utf8_string(OSSL_PKEY_PARAM_GROUP_NAME, "prime256v1", 0),
		OSSL_PARAM_octet_string(OSSL_PKEY_PARAM_PUB_KEY, point, sizeof(point)),
		OSSL_PARAM_END
	};

	if (!pctx || EVP_PKEY_fromdata_init(pctx) != 1 ||
	    EVP_PKEY_fromdata(pctx, &pkey, EVP_PKEY_PUBLIC_KEY, params) != 1 || !pkey) {
		fprintf(stderr, "failed to build EC public key\n");
		return 2;
	}
	EVP_PKEY_CTX_free(pctx);

	/* raw r || s -> DER-encoded ECDSA_SIG (what EVP_DigestVerify expects) */
	ECDSA_SIG *es = ECDSA_SIG_new();
	BIGNUM *r = BN_bin2bn(sig, 32, NULL);
	BIGNUM *s = BN_bin2bn(sig + 32, 32, NULL);

	ECDSA_SIG_set0(es, r, s);
	uint8_t der[80];
	uint8_t *pder = der;
	int derlen = i2d_ECDSA_SIG(es, &pder);

	int sig_ok = 0;
	EVP_MD_CTX *md = EVP_MD_CTX_new();

	if (derlen > 0 &&
	    EVP_DigestVerifyInit(md, NULL, EVP_sha256(), NULL, pkey) == 1 &&
	    EVP_DigestVerify(md, der, derlen,
			     (const uint8_t *)chal, strlen(chal)) == 1) {
		sig_ok = 1;
	}
	EVP_MD_CTX_free(md);
	ECDSA_SIG_free(es);
	EVP_PKEY_free(pkey);

	printf("  address binding  SHA256(pubkey)[:6]  : %-8s (claimed %s, computed %s)\n",
	       addr_ok ? "MATCH" : "MISMATCH", addr_s, acalc);
	printf("  key possession   ECDSA-P256 over nonce: %s\n",
	       sig_ok ? "VALID" : "INVALID");

	if (addr_ok && sig_ok) {
		printf("\n  ✓ PROOF VALID -- node cryptographically owns CGA %s\n", addr_s);
		return 0;
	}
	printf("\n  ✗ PROOF FAILED\n");
	return 1;
}
