/*
 * Copyright (c) 2026 Jon Sharp
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Controlled-flood multi-hop mesh over the DECT NR+ PHY broadcast channel.
 */

#include "mesh.h"
#include "mesh_metrics.h"

#include <string.h>
#include <stdio.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/byteorder.h>

LOG_MODULE_REGISTER(mesh, CONFIG_DECT_MESH_LOG_LEVEL);

#define MESH_MAGIC 0x4dU /* 'M' */
#define MESH_PROTO_VERSION 1U
#define MESH_BROADCAST_ID 0xffffU

/** Mesh frame header. Multi-byte fields are little-endian on the wire. */
struct mesh_hdr {
	uint8_t magic;
	uint8_t version;
	uint8_t msg_type;
	uint8_t ttl;
	uint8_t hop_count;
	uint8_t payload_len;
	uint16_t src_id;
	uint16_t dest_id;
	uint16_t msg_id;
} __packed;

#define MESH_HDR_LEN (sizeof(struct mesh_hdr))
#define MESH_MAX_PAYLOAD (MESH_FRAME_MAX - MESH_HDR_LEN)

enum mesh_msg_type {
	MESH_MSG_DATA = 1,
};

struct mesh_rx_frame {
	uint8_t data[MESH_FRAME_MAX];
	uint8_t len;
	int16_t rssi_dbm;
	uint16_t last_hop_id;
};

struct mesh_tx_frame {
	uint8_t data[MESH_FRAME_MAX];
	uint8_t len;
};

struct dedup_entry {
	uint16_t src_id;
	uint16_t msg_id;
	bool valid;
};

struct neighbor {
	uint16_t id;
	int16_t rssi_dbm;
	int64_t last_seen_ms;
	uint32_t rx_count;
	bool valid;
};

K_MSGQ_DEFINE(rx_msgq, sizeof(struct mesh_rx_frame), CONFIG_DECT_MESH_RX_QUEUE_DEPTH, 4);
K_MSGQ_DEFINE(tx_msgq, sizeof(struct mesh_tx_frame), CONFIG_DECT_MESH_TX_QUEUE_DEPTH, 4);

static uint16_t self_id;
static uint16_t tx_seq;
static int64_t next_originate_ms;

/* Ring buffer of recently seen (src_id, msg_id) pairs for loop suppression. */
static struct dedup_entry dedup_cache[CONFIG_DECT_MESH_DEDUP_CACHE_SIZE];
static uint16_t dedup_head;

static struct neighbor neighbors[CONFIG_DECT_MESH_MAX_NEIGHBORS];

/* Inbound-queue overflow counter, incremented from modem context. */
static atomic_t rx_overflow;

/**
 * @brief Test whether a (src, msg) pair was seen before; record it if not.
 *
 * @return true if the pair was already present (a duplicate).
 */
static bool dedup_seen_or_record(uint16_t src_id, uint16_t msg_id)
{
	for (size_t i = 0; i < ARRAY_SIZE(dedup_cache); i++) {
		if (dedup_cache[i].valid && dedup_cache[i].src_id == src_id &&
		    dedup_cache[i].msg_id == msg_id) {
			return true;
		}
	}

	dedup_cache[dedup_head].src_id = src_id;
	dedup_cache[dedup_head].msg_id = msg_id;
	dedup_cache[dedup_head].valid = true;
	dedup_head = (dedup_head + 1) % ARRAY_SIZE(dedup_cache);

	return false;
}

static void neighbor_update(uint16_t id, int16_t rssi_dbm)
{
	int64_t now = k_uptime_get();
	struct neighbor *slot = NULL;
	struct neighbor *oldest = &neighbors[0];

	for (size_t i = 0; i < ARRAY_SIZE(neighbors); i++) {
		if (neighbors[i].valid && neighbors[i].id == id) {
			slot = &neighbors[i];
			break;
		}
		if (!neighbors[i].valid) {
			slot = &neighbors[i];
			break;
		}
		if (neighbors[i].last_seen_ms < oldest->last_seen_ms) {
			oldest = &neighbors[i];
		}
	}

	/* Table full of other neighbors: evict the least recently seen. */
	if (slot == NULL) {
		slot = oldest;
		slot->rx_count = 0;
	}

	if (!slot->valid || slot->id != id) {
		slot->id = id;
		slot->rx_count = 0;
		slot->valid = true;
	}

	slot->rssi_dbm = rssi_dbm;
	slot->last_seen_ms = now;
	slot->rx_count++;
}

static uint8_t neighbor_active_count(void)
{
	int64_t now = k_uptime_get();
	int64_t timeout_ms = (int64_t)CONFIG_DECT_MESH_NEIGHBOR_TIMEOUT_SECONDS * MSEC_PER_SEC;
	uint8_t count = 0;

	for (size_t i = 0; i < ARRAY_SIZE(neighbors); i++) {
		if (neighbors[i].valid && (now - neighbors[i].last_seen_ms) <= timeout_ms) {
			count++;
		}
	}

	return count;
}

static int tx_enqueue(const struct mesh_hdr *hdr, const uint8_t *payload, uint8_t payload_len)
{
	struct mesh_tx_frame frame;

	if (MESH_HDR_LEN + payload_len > MESH_FRAME_MAX) {
		return -EMSGSIZE;
	}

	memcpy(frame.data, hdr, MESH_HDR_LEN);
	if (payload_len > 0) {
		memcpy(frame.data + MESH_HDR_LEN, payload, payload_len);
	}
	frame.len = MESH_HDR_LEN + payload_len;

	if (k_msgq_put(&tx_msgq, &frame, K_NO_WAIT) != 0) {
		LOG_WRN("TX queue full, dropping frame");
		return -ENOMEM;
	}

	return 0;
}

static int mesh_send(uint16_t dest_id, const uint8_t *payload, uint8_t payload_len)
{
	struct mesh_hdr hdr = {
		.magic = MESH_MAGIC,
		.version = MESH_PROTO_VERSION,
		.msg_type = MESH_MSG_DATA,
		.ttl = CONFIG_DECT_MESH_DEFAULT_TTL,
		.hop_count = 0,
		.payload_len = payload_len,
		.src_id = sys_cpu_to_le16(self_id),
		.dest_id = sys_cpu_to_le16(dest_id),
		.msg_id = sys_cpu_to_le16(++tx_seq),
	};

	/* Suppress relaying our own frame if it floods back to us. */
	(void)dedup_seen_or_record(self_id, tx_seq);

	LOG_INF("Originate msg_id %u -> 0x%04x (ttl %u): \"%.*s\"", tx_seq, dest_id,
		CONFIG_DECT_MESH_DEFAULT_TTL, payload_len, payload);

	mesh_metrics_record_originated();

	return tx_enqueue(&hdr, payload, payload_len);
}

static void relay_frame(const struct mesh_hdr *in_hdr, const uint8_t *payload)
{
	struct mesh_hdr hdr = *in_hdr;

	hdr.ttl = in_hdr->ttl - 1;
	hdr.hop_count = in_hdr->hop_count + 1;

	if (tx_enqueue(&hdr, payload, in_hdr->payload_len) == 0) {
		LOG_DBG("Relay msg_id %u (ttl %u, hop %u)", sys_le16_to_cpu(in_hdr->msg_id),
			hdr.ttl, hdr.hop_count);
		mesh_metrics_record_relayed();
	}
}

static void process_frame(const struct mesh_rx_frame *frame)
{
	const struct mesh_hdr *hdr;
	const uint8_t *payload;
	uint16_t src_id;
	uint16_t dest_id;
	uint16_t msg_id;
	bool for_me;
	bool is_broadcast;

	if (frame->len < MESH_HDR_LEN) {
		LOG_DBG("Runt frame (%u bytes), dropping", frame->len);
		return;
	}

	hdr = (const struct mesh_hdr *)frame->data;
	if (hdr->magic != MESH_MAGIC || hdr->version != MESH_PROTO_VERSION) {
		LOG_DBG("Foreign frame (magic 0x%02x ver %u), dropping", hdr->magic, hdr->version);
		return;
	}

	if (MESH_HDR_LEN + hdr->payload_len > frame->len) {
		LOG_DBG("Truncated payload, dropping");
		return;
	}

	payload = frame->data + MESH_HDR_LEN;
	src_id = sys_le16_to_cpu(hdr->src_id);
	dest_id = sys_le16_to_cpu(hdr->dest_id);
	msg_id = sys_le16_to_cpu(hdr->msg_id);

	/* The immediate sender is a one-hop neighbor; record link quality. */
	neighbor_update(frame->last_hop_id, frame->rssi_dbm);
	mesh_metrics_record_rx(frame->rssi_dbm, hdr->hop_count + 1);

	/* Our own frame looped back. */
	if (src_id == self_id) {
		return;
	}

	/* Already relayed/delivered this message: suppress the loop. */
	if (dedup_seen_or_record(src_id, msg_id)) {
		LOG_DBG("Duplicate msg_id %u from 0x%04x, dropping", msg_id, src_id);
		mesh_metrics_record_dup_dropped();
		return;
	}

	is_broadcast = (dest_id == MESH_BROADCAST_ID);
	for_me = (dest_id == self_id);

	if (for_me || is_broadcast) {
		LOG_INF("Deliver msg_id %u from 0x%04x (%u hops, RSSI %d dBm): \"%.*s\"", msg_id,
			src_id, hdr->hop_count + 1, frame->rssi_dbm, hdr->payload_len, payload);
		mesh_metrics_record_delivered();
	}

	/* Relay onwards unless it was a unicast addressed to us, while TTL allows. */
	if (!for_me && hdr->ttl > 1) {
		relay_frame(hdr, payload);
	}
}

static void maybe_originate(void)
{
	uint8_t payload[MESH_MAX_PAYLOAD];
	int len;

	if (CONFIG_DECT_MESH_ORIGINATE_INTERVAL_SECONDS == 0) {
		return;
	}

	if (k_uptime_get() < next_originate_ms) {
		return;
	}

	next_originate_ms =
		k_uptime_get() + (int64_t)CONFIG_DECT_MESH_ORIGINATE_INTERVAL_SECONDS * MSEC_PER_SEC;

	len = snprintf((char *)payload, sizeof(payload), "node %04x #%u", self_id, tx_seq + 1);
	if (len < 0 || len > (int)sizeof(payload)) {
		return;
	}

	(void)mesh_send(MESH_BROADCAST_ID, payload, (uint8_t)len);
}

void mesh_rx_enqueue(const uint8_t *data, size_t len, int16_t rssi_dbm, uint16_t last_hop_id)
{
	struct mesh_rx_frame frame;

	if (len > MESH_FRAME_MAX) {
		len = MESH_FRAME_MAX;
	}

	memcpy(frame.data, data, len);
	frame.len = (uint8_t)len;
	frame.rssi_dbm = rssi_dbm;
	frame.last_hop_id = last_hop_id;

	if (k_msgq_put(&rx_msgq, &frame, K_NO_WAIT) != 0) {
		atomic_inc(&rx_overflow);
	}
}

void mesh_process(void)
{
	struct mesh_rx_frame frame;
	atomic_val_t dropped;

	while (k_msgq_get(&rx_msgq, &frame, K_NO_WAIT) == 0) {
		process_frame(&frame);
	}

	dropped = atomic_clear(&rx_overflow);
	if (dropped > 0) {
		LOG_WRN("Dropped %ld inbound frames (RX queue overflow)", (long)dropped);
	}

	maybe_originate();
	mesh_metrics_set_neighbor_count(neighbor_active_count());
}

size_t mesh_tx_dequeue(uint8_t *out_buf, size_t out_buf_size)
{
	struct mesh_tx_frame frame;

	if (k_msgq_get(&tx_msgq, &frame, K_NO_WAIT) != 0) {
		return 0;
	}

	if (frame.len > out_buf_size) {
		LOG_ERR("Outbound frame (%u) exceeds buffer (%zu)", frame.len, out_buf_size);
		return 0;
	}

	memcpy(out_buf, frame.data, frame.len);

	return frame.len;
}

void mesh_init(uint16_t node_id)
{
	self_id = node_id;
	tx_seq = 0;
	dedup_head = 0;
	next_originate_ms = 0;
	atomic_clear(&rx_overflow);
	memset(dedup_cache, 0, sizeof(dedup_cache));
	memset(neighbors, 0, sizeof(neighbors));

	BUILD_ASSERT(MESH_HDR_LEN < MESH_FRAME_MAX, "Mesh header exceeds frame budget");

	LOG_INF("Mesh node 0x%04x up (TTL %u, frame max %u, payload max %u)", node_id,
		CONFIG_DECT_MESH_DEFAULT_TTL, MESH_FRAME_MAX, (unsigned int)MESH_MAX_PAYLOAD);
}
