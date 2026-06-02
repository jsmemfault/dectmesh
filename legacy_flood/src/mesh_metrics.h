/*
 * Copyright (c) 2026 Jon Sharp
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef MESH_METRICS_H_
#define MESH_METRICS_H_

#include <stdint.h>

/**
 * @file
 * @brief Memfault integration for the DECT NR+ mesh.
 *
 * Sets the Memfault device identity from the mesh node ID and records custom
 * mesh metrics into the Memfault heartbeat. When @c CONFIG_MEMFAULT is
 * disabled, every entry point compiles to a no-op so the mesh builds without
 * the Memfault SDK.
 */

/**
 * @brief Set the Memfault device serial to "dect-mesh-<node_id>".
 *
 * @param node_id Mesh node address.
 */
void mesh_metrics_set_device_id(uint16_t node_id);

/** Record that this node originated a new message. */
void mesh_metrics_record_originated(void);

/** Record that this node relayed a message onward. */
void mesh_metrics_record_relayed(void);

/** Record that a message was delivered to this node. */
void mesh_metrics_record_delivered(void);

/** Record that a duplicate frame was suppressed. */
void mesh_metrics_record_dup_dropped(void);

/** Record a failed PHY transmission. */
void mesh_metrics_record_tx_failure(void);

/**
 * @brief Record a received frame's link quality and observed hop count.
 *
 * @param rssi_dbm Link RSSI in dBm.
 * @param hops     Hops traversed by the frame so far.
 */
void mesh_metrics_record_rx(int16_t rssi_dbm, uint8_t hops);

/**
 * @brief Update the current active-neighbor count gauge.
 *
 * @param count Number of neighbors seen within the inactivity timeout.
 */
void mesh_metrics_set_neighbor_count(uint8_t count);

#endif /* MESH_METRICS_H_ */
