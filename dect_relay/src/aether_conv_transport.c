/*
 * aether_conv_transport -- see aether_conv_transport.h and doc/MESH_REMOTE_MOUNT.md.
 *
 * Layering (bottom to top):
 *   carrier mesh_client (UART) -> 9151 /net/aether conversation <conv>
 *      ctl_fid  : net/aether/clone   (held open; `connect <peer>` written here)
 *      data_fid : net/aether/<conv>/data  (one framed 9P msg written, reply read)
 *   this transport: send(T) -> pump writes data_fid, reads reply (retry+tag-match)
 *   nested ninep_client: Tversion/attach/walk/... to the REMOTE node's 9P server
 */
#include "aether_conv_transport.h"

#include <zephyr/logging/log.h>
#include <zephyr/9p/protocol.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

LOG_MODULE_REGISTER(aether_conv_tp, LOG_LEVEL_INF);

/* The pump runs the CARRIER ninep_client write+read AND the nested client's
 * recv_cb decode (recv_cb fires on this thread) -- the same class of work the
 * relay's proxy work queue gets 16384 for. 2048 overflowed the MPU stack guard
 * on the first /net/mesh access (fault -> reboot); 8192 gives comfortable
 * headroom for one carrier round-trip + the nested R decode. */
#define PUMP_STACK 8192
#define TX_ATTEMPTS 8   /* resend a lost T this many times (do_bridge used 4 on USB) */
#define RX_POLLS    6   /* reads per attempt while awaiting the reply datagram */
#define RX_POLL_MS  30  /* gap before re-reading an empty data fid (mesh round-trip) */

static K_THREAD_STACK_DEFINE(pump_stack, PUMP_STACK);

static inline uint16_t tag16(const uint8_t *m) { return (uint16_t)(m[5] | (m[6] << 8)); }

/* ---- ninep_transport ops ------------------------------------------------ */

static int aether_tx_send(struct ninep_transport *tr, const uint8_t *buf, size_t len)
{
	struct aether_conv_transport *t = CONTAINER_OF(tr, struct aether_conv_transport, transport);

	if (len < 7 || len > sizeof(t->txbuf)) {
		return -EMSGSIZE;
	}
	/* Copy: the nested client's tx_buf is only valid for this call; the pump
	 * runs asynchronously. The nested client is request-response (one T in
	 * flight -- the mesh 9P server is one-T-in-flight), so txbuf is never
	 * overwritten mid-pump. */
	memcpy(t->txbuf, buf, len);
	t->txlen = len;
	k_sem_give(&t->tx_sem);
	return (int)len;
}

static int aether_tx_start(struct ninep_transport *tr) { ARG_UNUSED(tr); return 0; }
static int aether_tx_stop(struct ninep_transport *tr)  { ARG_UNUSED(tr); return 0; }
static int aether_tx_mtu(struct ninep_transport *tr)   { ARG_UNUSED(tr); return AETHER_CONV_MTU; }

static const struct ninep_transport_ops aether_tx_ops = {
	.send = aether_tx_send,
	.start = aether_tx_start,
	.stop = aether_tx_stop,
	.get_mtu = aether_tx_mtu,
};

/* ---- the pump: do_bridge's inner loop, over the carrier client ---------- */

static void pump_fn(void *a, void *b, void *c)
{
	struct aether_conv_transport *t = a;
	ARG_UNUSED(b); ARG_UNUSED(c);

	for (;;) {
		k_sem_take(&t->tx_sem, K_FOREVER);

		k_mutex_lock(&t->lock, K_FOREVER);
		bool conn = t->connected;
		uint32_t dfid = t->data_fid;
		k_mutex_unlock(&t->lock);

		int rn = -1;

		if (!conn) {
			LOG_WRN("send with no conversation; nested client will time out");
			continue;
		}

		uint16_t want = tag16(t->txbuf);

		for (int attempt = 0; attempt < TX_ATTEMPTS && rn <= 0; attempt++) {
			int w = ninep_client_write(t->carrier, dfid, 0, t->txbuf, t->txlen);
			if (w < 0) {
				LOG_DBG("carrier write failed: %d (attempt %d)", w, attempt);
				continue;
			}
			/* Await the reply datagram: the 9151 surfaces it as readable data on
			 * the same data fid once it returns over the mesh. Poll a few times
			 * (round-trip latency), tag-matching to discard stale/duplicate R's
			 * on the untagged datagram stream. */
			for (int poll = 0; poll < RX_POLLS; poll++) {
				int r = ninep_client_read(t->carrier, dfid, 0, t->rb, sizeof(t->rb));
				if (r >= 7 && tag16(t->rb) == want) { rn = r; break; }
				if (r >= 7) {
					LOG_DBG("discard stale R tag=%u (want %u)", tag16(t->rb), want);
					continue;   /* another reply may be queued behind it */
				}
				k_msleep(RX_POLL_MS);   /* nothing yet -- let the round-trip land */
			}
		}

		if (rn > 0) {
			t->transport.recv_cb(&t->transport, t->rb, (size_t)rn,
					     t->transport.user_data);
		} else {
			LOG_WRN("mesh round-trip failed after retries (T type=%u tag=%u)",
				t->txbuf[4], want);
			/* No recv_cb -> nested client times out on this tag and reports it. */
		}
	}
}

/* ---- conversation setup (lifted from tools/aether_conv.c do_bridge) ------ */

void aether_conv_transport_disconnect(struct aether_conv_transport *t)
{
	k_mutex_lock(&t->lock, K_FOREVER);
	if (t->connected) {
		if (t->data_fid != NINEP_NOFID) (void)ninep_client_clunk(t->carrier, t->data_fid);
		if (t->ctl_fid != NINEP_NOFID)  (void)ninep_client_clunk(t->carrier, t->ctl_fid);
	}
	t->data_fid = NINEP_NOFID;
	t->ctl_fid = NINEP_NOFID;
	t->conv = -1;
	t->connected = false;
	k_mutex_unlock(&t->lock);
}

int aether_conv_transport_connect(struct aether_conv_transport *t,
				  uint32_t carrier_root, const char *peer)
{
	int ret;

	k_mutex_lock(&t->lock, K_FOREVER);
	if (t->connected && strcmp(t->peer, peer) == 0) {
		k_mutex_unlock(&t->lock);
		return 0;   /* already connected to this peer */
	}
	k_mutex_unlock(&t->lock);

	aether_conv_transport_disconnect(t);   /* drop any prior conversation */

	uint32_t ctl_fid = NINEP_NOFID, data_fid = NINEP_NOFID;

	/* walk net/aether/clone, open, read the allocated conversation number */
	ret = ninep_client_walk(t->carrier, carrier_root, &ctl_fid, "net/aether/clone");
	if (ret < 0) { LOG_WRN("walk clone: %d", ret); goto fail; }
	ret = ninep_client_open(t->carrier, ctl_fid, NINEP_ORDWR);
	if (ret < 0) { LOG_WRN("open clone: %d", ret); goto fail; }

	uint8_t nb[16];
	int n = ninep_client_read(t->carrier, ctl_fid, 0, nb, sizeof(nb) - 1);
	if (n <= 0) { LOG_WRN("read conv#: %d", n); ret = -EIO; goto fail; }
	nb[n] = 0;
	int conv = atoi((char *)nb);

	/* walk net/aether/<conv>/data, open read-write */
	char path[48];
	snprintf(path, sizeof(path), "net/aether/%d/data", conv);
	ret = ninep_client_walk(t->carrier, carrier_root, &data_fid, path);
	if (ret < 0) { LOG_WRN("walk data: %d", ret); goto fail; }
	ret = ninep_client_open(t->carrier, data_fid, NINEP_ORDWR);
	if (ret < 0) { LOG_WRN("open data: %d", ret); goto fail; }

	/* connect the conversation to the target peer (written to the ctl fid) */
	char cmd[40];
	int l = snprintf(cmd, sizeof(cmd), "connect %s", peer);
	ret = ninep_client_write(t->carrier, ctl_fid, 0, (const uint8_t *)cmd, l);
	if (ret < 0) { LOG_WRN("ctl connect: %d", ret); goto fail; }

	k_mutex_lock(&t->lock, K_FOREVER);
	t->ctl_fid = ctl_fid;
	t->data_fid = data_fid;
	t->conv = conv;
	strncpy(t->peer, peer, sizeof(t->peer) - 1);
	t->peer[sizeof(t->peer) - 1] = 0;
	t->connected = true;
	k_mutex_unlock(&t->lock);

	LOG_INF("conversation %d connected to %s (ctl fid %u, data fid %u)",
		conv, peer, ctl_fid, data_fid);
	return 0;

fail:
	if (data_fid != NINEP_NOFID) (void)ninep_client_clunk(t->carrier, data_fid);
	if (ctl_fid != NINEP_NOFID)  (void)ninep_client_clunk(t->carrier, ctl_fid);
	return ret;
}

int aether_conv_transport_init(struct aether_conv_transport *t,
			       struct ninep_client *carrier)
{
	memset(t, 0, sizeof(*t));
	t->carrier = carrier;
	t->conv = -1;
	t->ctl_fid = NINEP_NOFID;
	t->data_fid = NINEP_NOFID;
	t->transport.ops = &aether_tx_ops;
	t->transport.priv_data = NULL;

	k_mutex_init(&t->lock);
	k_sem_init(&t->tx_sem, 0, 1);

	k_thread_create(&t->pump_thread, pump_stack, K_THREAD_STACK_SIZEOF(pump_stack),
			pump_fn, t, NULL, NULL, K_PRIO_PREEMPT(7), 0, K_NO_WAIT);
	k_thread_name_set(&t->pump_thread, "aether_tp_pump");
	return 0;
}
