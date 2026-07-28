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

/* A small RING of pending T-messages instead of one-in-flight. The 9P server is
 * processed serially on the workq (it has per-session fid state), but queuing a
 * few requests lets a STREAMING requester (OTA) keep several writes in flight
 * without the peer dropping the 2nd..Nth as "busy" -- the enabler for a windowed
 * DFU transfer. Each incoming datagram is already ACKed at the mesh layer, so a
 * ring slot is a commitment to process it; the requester's window keeps the ring
 * from overflowing (WINDOW <= MESH9P_RING). */
#define MESH9P_RING 8
struct mesh9p_req {
	uint8_t buf[CONFIG_AETHER_MAX_PAYLOAD];
	size_t  len;
	uint8_t peer[6];
};
static struct mesh9p_req ring[MESH9P_RING];
static uint8_t ring_head;          /* consumer: next to process */
static uint8_t ring_tail;          /* producer: next to fill */
static atomic_t ring_count;
static struct k_spinlock ring_lock;
static uint8_t cur_peer[6];        /* peer of the request being served (mesh9p_send) */

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

	/* Drain the ring in FIFO order. New arrivals during processing bump
	 * ring_count and re-submit the work, so nothing is left behind. */
	while (atomic_get(&ring_count) > 0) {
		struct mesh9p_req *r = &ring[ring_head];

		memcpy(cur_peer, r->peer, 6);
		if (mesh_transport.recv_cb) {
			/* Server processes the T and replies via ops->send. */
			atomic_set(&mesh9p_serving, 1);
			mesh_transport.recv_cb(&mesh_transport, r->buf, r->len,
					       mesh_transport.user_data);
			atomic_set(&mesh9p_serving, 0);
		}
		k_spinlock_key_t k = k_spin_lock(&ring_lock);

		ring_head = (ring_head + 1) % MESH9P_RING;
		k_spin_unlock(&ring_lock, k);
		atomic_dec(&ring_count);
	}
}
static K_WORK_DEFINE(mesh9p_work, mesh9p_work_fn);

/* Does this unicast datagram look like a framed 9P T-message? */
static bool looks_like_9p_t(const uint8_t *data, size_t len)
{
	if (len < 7 || len > CONFIG_AETHER_MAX_PAYLOAD) {
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

/* Transport flow-control gate (runs on the RX thread, before dedup/ACK): accept a
 * 9P T-message only if the request ring has room. A refusal is not ACKed, so the
 * sender's ARQ retransmits -- lossless backpressure. Non-9P DATA always accepted. */
static bool mesh9p_ack_gate(const uint8_t *payload, size_t len)
{
	if (!looks_like_9p_t(payload, len)) {
		return true;
	}
	return atomic_get(&ring_count) < MESH9P_RING;
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
	k_spinlock_key_t k = k_spin_lock(&ring_lock);

	if (atomic_get(&ring_count) >= MESH9P_RING) {
		/* Should be rare now (the ack-gate refuses before ACK), but keep the
		 * guard: if it fires, the gate + this node disagree momentarily -- drop. */
		k_spin_unlock(&ring_lock, k);
		LOG_WRN("9P/mesh ring full, dropping T type=%u from %02x:%02x",
			data[4], src[4], src[5]);
		return;
	}
	struct mesh9p_req *r = &ring[ring_tail];

	ring_tail = (ring_tail + 1) % MESH9P_RING;
	k_spin_unlock(&ring_lock, k);

	memcpy(r->buf, data, len);
	r->len = len;
	memcpy(r->peer, src, 6);
	atomic_inc(&ring_count);
	k_work_submit_to_queue(&mesh9p_workq, &mesh9p_work);
}

static int mesh9p_send(struct ninep_transport *transport, const uint8_t *buf,
		       size_t len)
{
	ARG_UNUSED(transport);

	/* Rwrite (type 119) is the DFU stream's per-chunk reply. We SUPPRESS it: the
	 * requester (windowed pusher) never waits for it, delivery is guaranteed by the
	 * Twrite's own transport ARQ (exactly-once) and integrity by MCUboot's signature.
	 * Crucially, actually transmitting a reply per chunk HOGS this node's half-duplex
	 * radio -- while it TXes the Rwrite it cannot RX+ACK the next incoming Twrite, so
	 * the sender's send_reliable retries ~5x (~2 s/chunk, measured). Not sending the
	 * reply frees the radio to receive the stream at full rate. Report success to the
	 * server so its state stays consistent. Non-write R-messages (Rread/Rversion/...,
	 * where a lost reply aborts the op) stay reliable. */
	if (len > 4 && buf[4] == 119) {
		return (int)len;
	}
	int ret = aether_mesh_send_reliable(mesh_iface, cur_peer, buf, len, 3);

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
	return CONFIG_AETHER_MAX_PAYLOAD;
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

	sc.max_message_size = CONFIG_AETHER_MAX_PAYLOAD;

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

	/* Backpressure: refuse a 9P T-message at the transport (no ACK -> sender
	 * retransmits) when our request ring is full, instead of accepting it (ACKed)
	 * and then dropping it as "ring full" -- which silently loses a DFU chunk. */
	aether_mesh_set_ack_gate(mesh9p_ack_gate);

	LOG_INF("9P server up over the mesh (msize <= %u, reliable unicast)",
		(unsigned int)CONFIG_AETHER_MAX_PAYLOAD);
	return 0;
}
