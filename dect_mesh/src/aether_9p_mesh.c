/*
 * Copyright (c) 2026 Æther Authors
 * SPDX-License-Identifier: MIT
 *
 * 9P served over the mesh itself. A second ninep_server instance shares the
 * node's composed namespace (the same union fs the USB/UART path serves), with
 * the mesh's reliable unicast datagrams as the transport: each 9P T-message
 * arrives as one ACKed datagram, each R-message leaves as one. A peer node --
 * or a host driving a peer node's conversation layer -- can mount this node's
 * entire filesystem across the mesh, through relays. This is the 9P-over-Æther
 * milestone: the mesh is not just carrying chat, it is carrying sessions.
 *
 * Transport contract: one datagram == one complete 9P message, so the server's
 * negotiated msize is capped to the mesh payload (get_mtu -> AETHER_MAX_PAYLOAD)
 * and no reassembly is needed on either side.
 *
 * Threading: mesh receive callbacks run on the RX thread, and the reply path
 * (aether_mesh_send_reliable) BLOCKS awaiting the peer's ACK -- which that same
 * RX thread processes. So T-messages are bounced to a dedicated work queue and
 * the server runs there, keeping the RX thread free (the same discipline as
 * 9p4z's session_pool_uart).
 *
 * Service demux: there is no port space on the mesh (yet -- a spec item), so
 * incoming unicast datagrams are recognized as 9P by shape: exact framed length
 * (LE u32 size == datagram length) and a T-message type byte. Anything else is
 * left to the conversation layer untouched.
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/aether_mesh.h>
#include <zephyr/9p/server.h>
#include <zephyr/9p/transport.h>
#include <string.h>

#include "aether_9p.h"

LOG_MODULE_REGISTER(aether_9p_mesh, LOG_LEVEL_INF);

static struct ninep_transport mesh_transport;
static struct ninep_server mesh_server;
static struct net_if *mesh_iface;

/* One T-message in flight at a time (9P is client-driven; our sessions are
 * strictly request-response). A busy drop simply looks like datagram loss to
 * the requester, whose T arrives again via its own retry discipline. */
static uint8_t req_buf[CONFIG_AETHER_MAX_PAYLOAD];
static size_t req_len;
static uint8_t req_peer[6];
static atomic_t req_busy;

static K_THREAD_STACK_DEFINE(mesh9p_stack, 4096);
static struct k_work_q mesh9p_workq;

/* True while a request is being served BY the mesh 9P server. dfu_status checks
 * this: a DFU write arriving over the mesh must NOT quiesce the mesh (the mesh is
 * the transport carrying the image -- quiescing it kills every subsequent Twrite).
 * The wired (uart/USB) DFU path leaves this false, so it still quiesces as before. */
static atomic_t mesh9p_serving;

bool aether_9p_mesh_serving(void)
{
	return atomic_get(&mesh9p_serving) != 0;
}

static void mesh9p_work_fn(struct k_work *work)
{
	ARG_UNUSED(work);

	if (mesh_transport.recv_cb) {
		/* The server processes the T and replies synchronously through
		 * ops->send (a blocking reliable unicast) on this work thread. */
		atomic_set(&mesh9p_serving, 1);
		mesh_transport.recv_cb(&mesh_transport, req_buf, req_len,
				       mesh_transport.user_data);
		atomic_set(&mesh9p_serving, 0);
	}
	atomic_clear(&req_busy);
}
static K_WORK_DEFINE(mesh9p_work, mesh9p_work_fn);

/* Does this unicast datagram look like a framed 9P T-message? */
static bool looks_like_9p_t(const uint8_t *data, size_t len)
{
	if (len < 7 || len > sizeof(req_buf)) {
		return false;
	}
	uint32_t sz = data[0] | (data[1] << 8) | (data[2] << 16) |
		      ((uint32_t)data[3] << 24);
	if (sz != len) {
		return false;
	}
	/* T-message types are even, Tversion(100)..Twstat(126). */
	return data[4] >= 100 && data[4] <= 126 && (data[4] & 1) == 0;
}

static void mesh9p_recv_cb(struct net_if *iface, const uint8_t src[6],
			   const uint8_t *data, size_t len, bool broadcast,
			   void *user_data)
{
	ARG_UNUSED(iface);
	ARG_UNUSED(user_data);

	if (broadcast || !looks_like_9p_t(data, len)) {
		return;
	}
	if (atomic_test_and_set_bit(&req_busy, 0)) {
		LOG_WRN("9P/mesh busy, dropping T type=%u from %02x:%02x",
			data[4], src[4], src[5]);
		return;
	}
	memcpy(req_buf, data, len);
	req_len = len;
	memcpy(req_peer, src, 6);
	k_work_submit_to_queue(&mesh9p_workq, &mesh9p_work);
}

static int mesh9p_send(struct ninep_transport *transport, const uint8_t *buf,
		       size_t len)
{
	ARG_UNUSED(transport);

	/* Reply reliably, but with a BOUNDED retry budget: req_busy is held for the
	 * whole duration of this send, and while it is held every incoming T is dropped
	 * as "busy". A long reply-ARQ stall (e.g. the requester's ACK is slow) therefore
	 * blocks the NEXT request for seconds -- fatal to a streaming session (OTA) where
	 * the requester's retransmit keeps hitting a busy peer. A short budget frees
	 * req_busy fast; the requester's own retry covers the rare genuinely-lost R. */
	int ret = aether_mesh_send_reliable(mesh_iface, req_peer, buf, len, 3);

	return ret == 0 ? (int)len : ret;
}

static int mesh9p_start(struct ninep_transport *transport)
{
	ARG_UNUSED(transport);
	return 0;
}

static int mesh9p_stop(struct ninep_transport *transport)
{
	ARG_UNUSED(transport);
	return 0;
}

static int mesh9p_get_mtu(struct ninep_transport *transport)
{
	ARG_UNUSED(transport);
	return sizeof(req_buf);
}

static const struct ninep_transport_ops mesh9p_ops = {
	.send = mesh9p_send,
	.start = mesh9p_start,
	.stop = mesh9p_stop,
	.get_mtu = mesh9p_get_mtu,
};

int aether_9p_mesh_init(struct net_if *iface,
			const struct ninep_server_config *base_sc)
{
	int ret;

	mesh_iface = iface;

	/* Same namespace as the UART/USB server; smaller message ceiling (one
	 * datagram per message; get_mtu clamps the negotiated msize anyway). */
	struct ninep_server_config sc = *base_sc;

	sc.max_message_size = sizeof(req_buf);

	k_work_queue_start(&mesh9p_workq, mesh9p_stack,
			   K_THREAD_STACK_SIZEOF(mesh9p_stack),
			   K_PRIO_PREEMPT(9), NULL);

	/* (ninep_transport_init is declared but unimplemented in 9p4z; fill the
	 * struct directly, as each concrete transport does. The server installs
	 * its own recv_cb at ninep_server_init.) */
	mesh_transport.ops = &mesh9p_ops;
	mesh_transport.recv_cb = NULL;
	mesh_transport.user_data = NULL;
	mesh_transport.priv_data = NULL;

	ret = ninep_server_init(&mesh_server, &sc, &mesh_transport);
	if (ret < 0) {
		LOG_ERR("mesh 9P server init: %d", ret);
		return ret;
	}
	ret = ninep_server_start(&mesh_server);
	if (ret < 0) {
		LOG_ERR("mesh 9P server start: %d", ret);
		return ret;
	}

	ret = aether_mesh_register_recv_callback(iface, mesh9p_recv_cb, NULL);
	if (ret < 0) {
		LOG_ERR("mesh 9P recv callback: %d", ret);
		return ret;
	}

	LOG_INF("9P server up over the mesh (msize <= %u, reliable unicast)",
		(unsigned int)sizeof(req_buf));
	return 0;
}
