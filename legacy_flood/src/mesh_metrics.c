/*
 * Copyright (c) 2026 Jon Sharp
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Memfault device identity and custom mesh metrics. All Memfault calls are
 * guarded so the mesh builds with or without CONFIG_MEMFAULT.
 */

#include "mesh_metrics.h"

#include <stdio.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#if defined(CONFIG_MEMFAULT)
#include <memfault/metrics/metrics.h>
#include <memfault_ncs.h>
#endif

LOG_MODULE_REGISTER(mesh_metrics, CONFIG_DECT_MESH_LOG_LEVEL);

/* Highest hop count observed so far, reported as a gauge. */
static uint8_t max_hops_seen;

void mesh_metrics_set_device_id(uint16_t node_id)
{
#if defined(CONFIG_MEMFAULT) && defined(CONFIG_MEMFAULT_NCS_DEVICE_ID_RUNTIME)
	char device_id[16];
	int len;
	int err;

	len = snprintf(device_id, sizeof(device_id), "dect-mesh-%04x", node_id);
	if (len <= 0 || len >= (int)sizeof(device_id)) {
		LOG_ERR("Failed to format device ID");
		return;
	}

	err = memfault_ncs_device_id_set(device_id, len);
	if (err) {
		LOG_ERR("memfault_ncs_device_id_set failed, err %d", err);
		return;
	}

	LOG_INF("Memfault device ID: %s", device_id);
#else
	ARG_UNUSED(node_id);
#endif
}

void mesh_metrics_record_originated(void)
{
#if defined(CONFIG_MEMFAULT)
	MEMFAULT_METRIC_ADD(dect_originated, 1);
#endif
}

void mesh_metrics_record_relayed(void)
{
#if defined(CONFIG_MEMFAULT)
	MEMFAULT_METRIC_ADD(dect_relayed, 1);
#endif
}

void mesh_metrics_record_delivered(void)
{
#if defined(CONFIG_MEMFAULT)
	MEMFAULT_METRIC_ADD(dect_delivered, 1);
#endif
}

void mesh_metrics_record_dup_dropped(void)
{
#if defined(CONFIG_MEMFAULT)
	MEMFAULT_METRIC_ADD(dect_dups_dropped, 1);
#endif
}

void mesh_metrics_record_tx_failure(void)
{
#if defined(CONFIG_MEMFAULT)
	MEMFAULT_METRIC_ADD(dect_tx_failures, 1);
#endif
}

void mesh_metrics_record_rx(int16_t rssi_dbm, uint8_t hops)
{
	if (hops > max_hops_seen) {
		max_hops_seen = hops;
	}

#if defined(CONFIG_MEMFAULT)
	MEMFAULT_METRIC_SET_SIGNED(dect_rx_rssi_dbm, rssi_dbm);
	MEMFAULT_METRIC_SET_UNSIGNED(dect_max_hops, max_hops_seen);
#else
	ARG_UNUSED(rssi_dbm);
#endif
}

void mesh_metrics_set_neighbor_count(uint8_t count)
{
#if defined(CONFIG_MEMFAULT)
	MEMFAULT_METRIC_SET_UNSIGNED(dect_neighbors, count);
#else
	ARG_UNUSED(count);
#endif
}
