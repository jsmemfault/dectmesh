/*
 * aether_conv_transport -- a 9p4z ninep_transport that carries a NESTED 9P
 * client's messages over the DECT mesh, by tunneling them through a held
 * /net/aether conversation on the 9151 (reached via the relay's EXISTING
 * mesh_client over the inter-chip UART).
 *
 * This is the on-device home of tools/aether_conv --bridge: the 9151 does the
 * radio + HONR hop-routing; this transport only frames one 9P message per
 * reliable datagram and applies do_bridge's retry + tag-match. Feed it to a
 * second ninep_client + remote_fs so the relay can re-export a REMOTE node's
 * filesystem (aether!<addr>) to L2CAP/USB clients (e.g. a macOS 9pfuse mount).
 *
 * See doc/MESH_REMOTE_MOUNT.md.
 */
#ifndef AETHER_CONV_TRANSPORT_H_
#define AETHER_CONV_TRANSPORT_H_

#include <zephyr/kernel.h>
#include <zephyr/9p/transport.h>
#include <zephyr/9p/client.h>

/* Max single 9P message == one mesh datagram. Kept <= the 9151's AETHER_MAX_PAYLOAD
 * so every tunneled T/R fits one reliable unicast with no reassembly. */
#define AETHER_CONV_MTU 448

struct aether_conv_transport {
	struct ninep_transport transport;   /* embedded; client_init fills recv_cb/user_data */
	struct ninep_client *carrier;       /* OUTER client to the 9151 (UART) */

	/* Held /net/aether conversation on the 9151 (carrier fids). */
	struct k_mutex lock;                /* guards the conversation state below */
	uint32_t ctl_fid;                   /* net/aether/clone, held open == the ctl fid */
	uint32_t data_fid;                  /* net/aether/<conv>/data */
	int conv;                           /* conversation number (-1 = none) */
	bool connected;
	char peer[24];                      /* current target addr string */

	/* TX pump: send() hands a framed T here; the pump does the carrier
	 * write/read (retry + tag-match) and delivers the R via recv_cb -- off the
	 * caller's stack so the nested client is already waiting when recv_cb fires
	 * (same reason the UART transport defers RX to a workqueue). */
	struct k_sem tx_sem;
	uint8_t txbuf[AETHER_CONV_MTU + 16];
	size_t txlen;
	uint8_t rb[AETHER_CONV_MTU + 16];
	int rn;                             /* pump result: reply length, or <=0 on failure */
	struct k_thread pump_thread;
};

/*
 * Initialize the transport and start its pump thread. @carrier is the relay's
 * already-inited mesh_client (must be attached before connect()). After this,
 * pass &t->transport to ninep_client_init() for the nested client.
 */
int aether_conv_transport_init(struct aether_conv_transport *t,
			       struct ninep_client *carrier);

/*
 * (Re)open a /net/aether conversation on the 9151 and `connect` it to @peer.
 * @carrier_root is the live 9151 root fid (from mesh_ensure_attached). Idempotent
 * for the same peer; switching peers drops the old conversation first.
 */
int aether_conv_transport_connect(struct aether_conv_transport *t,
				  uint32_t carrier_root, const char *peer);

/* Clunk the conversation fids and mark disconnected (next connect() reopens). */
void aether_conv_transport_disconnect(struct aether_conv_transport *t);

#endif /* AETHER_CONV_TRANSPORT_H_ */
