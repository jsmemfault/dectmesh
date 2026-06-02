/*
 * Copyright (c) 2026 Jon Sharp
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * DECT NR+ mesh node: bring up the PHY, then run the transmit/receive cadence
 * that drives the controlled-flood mesh. Each loop iteration drains any pending
 * inbound frames and originations, transmits queued outbound frames, then opens
 * a receive window to collect the next batch.
 */

#include "dect_phy.h"
#include "mesh.h"
#include "mesh_metrics.h"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(main, CONFIG_DECT_MESH_LOG_LEVEL);

int main(void)
{
	int err;
	uint16_t phy_device_id;
	uint16_t node_id;
	uint8_t frame[MESH_FRAME_MAX];
	size_t len;

	LOG_INF("DECT NR+ mesh node starting");

	err = dect_phy_init(&phy_device_id);
	if (err) {
		LOG_ERR("DECT NR+ PHY init failed, err %d", err);
		return err;
	}

	/* A configured node ID overrides the hardware-derived PHY ID. */
	node_id = (CONFIG_DECT_MESH_NODE_ID != 0) ? CONFIG_DECT_MESH_NODE_ID : phy_device_id;

	mesh_metrics_set_device_id(node_id);
	mesh_init(node_id);
	dect_phy_set_rx_cb(mesh_rx_enqueue);

	while (1) {
		/* Process the previous window's inbound frames and originate. */
		mesh_process();

		/* Transmit everything the mesh queued (originations and relays). */
		while ((len = mesh_tx_dequeue(frame, sizeof(frame))) > 0) {
			err = dect_phy_tx(frame, len);
			if (err) {
				LOG_ERR("Transmit failed, err %d", err);
				mesh_metrics_record_tx_failure();
			}
		}

		/* Listen for inbound frames for the configured window. */
		err = dect_phy_rx_window(CONFIG_DECT_MESH_RX_WINDOW_SECONDS);
		if (err) {
			LOG_ERR("Receive window failed, err %d", err);
			return err;
		}
	}

	return 0;
}
