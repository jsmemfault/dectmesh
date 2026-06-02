/*
 * Copyright (c) 2026 Jon Sharp
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef MESH_H_
#define MESH_H_

#include <stdint.h>
#include <stddef.h>

/**
 * @file
 * @brief Application-layer multi-hop relay over the DECT NR+ PHY.
 *
 * Implements a controlled-flood mesh on top of the broadcast PHY: every node is
 * a peer that re-transmits frames it has not seen before, bounded by a
 * time-to-live (TTL) hop count and a duplicate-suppression cache. A neighbor
 * table is maintained from the link quality of directly received frames.
 *
 * The mesh owns two queues. Inbound frames are enqueued from the PHY receive
 * callback (modem context) and processed later on the application thread;
 * outbound frames (originations and relays) are produced on the application
 * thread and drained by the caller for transmission. All mesh state is touched
 * only from the application thread, so it needs no locking.
 */

/** Maximum on-air frame size, in bytes (PHY payload budget). */
#define MESH_FRAME_MAX 32U

/**
 * @brief Initialize the mesh with this node's address.
 *
 * @param node_id Address used as the originator ID and to recognize frames
 *                addressed to, or originated by, this node.
 */
void mesh_init(uint16_t node_id);

/**
 * @brief Enqueue a raw frame received from the PHY.
 *
 * Safe to call from the PHY receive callback context. Does not process the
 * frame; @ref mesh_process performs the mesh logic on the application thread.
 *
 * @param data        Received payload.
 * @param len         Payload length in bytes.
 * @param rssi_dbm    Link RSSI in dBm.
 * @param last_hop_id PHY transmitter ID of the immediate sender.
 */
void mesh_rx_enqueue(const uint8_t *data, size_t len, int16_t rssi_dbm, uint16_t last_hop_id);

/**
 * @brief Run one mesh service pass on the application thread.
 *
 * Drains and processes queued inbound frames (delivering local messages and
 * scheduling relays), originates a new message if the origination interval has
 * elapsed, and refreshes the neighbor-count metric.
 */
void mesh_process(void);

/**
 * @brief Dequeue the next outbound frame for transmission.
 *
 * @param out_buf      Destination buffer.
 * @param out_buf_size Size of @p out_buf in bytes.
 *
 * @return Frame length in bytes, or 0 if no frame is pending.
 */
size_t mesh_tx_dequeue(uint8_t *out_buf, size_t out_buf_size);

#endif /* MESH_H_ */
