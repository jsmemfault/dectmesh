/*
 * Copyright (c) 2026 Jon Sharp
 * SPDX-License-Identifier: MIT
 *
 * Memfault integration for the Æther HONR mesh over the DECT NR+ PHY: a stable per-node
 * device ID, and a heartbeat collector that snapshots the live mesh topology
 * and traffic counters into Memfault metrics for the fleet. All Memfault calls
 * are guarded so the app builds with or without CONFIG_MEMFAULT.
 */

#include "dect_metrics.h"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/aether_mesh.h>
#include <zephyr/net/honr.h>
#include <stdio.h>

#if defined(CONFIG_MEMFAULT)
#include <memfault/metrics/metrics.h>
#include <memfault_ncs.h>
#endif

LOG_MODULE_REGISTER(dect_metrics, LOG_LEVEL_INF);

/* Mesh context, owned by aether_mesh.c. */
extern struct aether_mesh_ctx *g_mesh_ctx;

#if defined(CONFIG_MEMFAULT)
/* Snapshot the live mesh topology + counters into Memfault metrics, then
 * re-arm. Values are reported at the next heartbeat. NCS already owns
 * memfault_metrics_heartbeat_collect_data(), so we set metrics on our own
 * cadence rather than overriding that hook.
 */
static void metrics_collect(struct k_work *work)
{
	struct aether_mesh_ctx *c = g_mesh_ctx;
	unsigned int nbr = 0, rt = 0;

	if (c) {
		for (int i = 0; i < CONFIG_AETHER_MAX_NEIGHBORS; i++) {
			if (c->neighbors[i].active) {
				nbr++;
			}
		}
		for (int i = 0; i < CONFIG_AETHER_MAX_ROUTES; i++) {
			if (c->routes[i].active) {
				rt++;
			}
		}
		MEMFAULT_METRIC_SET_UNSIGNED(dect_neighbors, nbr);
		MEMFAULT_METRIC_SET_UNSIGNED(dect_routes, rt);
#if defined(CONFIG_AETHER_ROUTING_HONR)
		MEMFAULT_METRIC_SET_UNSIGNED(dect_honr_rank, honr_rank(c->honr_addr));
		MEMFAULT_METRIC_SET_UNSIGNED(dect_honr_joined, c->honr_joined ? 1U : 0U);
#endif
		MEMFAULT_METRIC_SET_UNSIGNED(dect_hello_sent, c->hello_sent);
		MEMFAULT_METRIC_SET_UNSIGNED(dect_hello_recv, c->hello_received);
		MEMFAULT_METRIC_SET_UNSIGNED(dect_forwarded, c->packets_forwarded);
		MEMFAULT_METRIC_SET_UNSIGNED(dect_dropped, c->packets_dropped);
	}
	k_work_reschedule(k_work_delayable_from_work(work), K_SECONDS(30));
}
static K_WORK_DELAYABLE_DEFINE(metrics_work, metrics_collect);
#endif /* CONFIG_MEMFAULT */

void dect_metrics_init(void)
{
#if defined(CONFIG_MEMFAULT) && defined(CONFIG_MEMFAULT_NCS_DEVICE_ID_RUNTIME)
	char id[24];
	int len = snprintf(id, sizeof(id), "dect-mesh-%08x", aether_honr_node_id());

	if (len > 0 && len < (int)sizeof(id) &&
	    memfault_ncs_device_id_set(id, len) == 0) {
		LOG_INF("Memfault device ID: %s", id);
	}
#endif
#if defined(CONFIG_MEMFAULT)
	k_work_reschedule(&metrics_work, K_SECONDS(5));
#endif
}
