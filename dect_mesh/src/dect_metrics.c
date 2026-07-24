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
#include "aether_net.h"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/aether_mesh.h>
#include <zephyr/net/heymac.h>
#include <zephyr/net/honr.h>
#include <zephyr/net/net_if.h>
#include <stdio.h>

/* DECT PHY driver (aephyr) — live modem telemetry: LBT, tx_ok, temp, voltage. */
#include <dect_phy.h>
#include <nrf_modem_dect_phy.h> /* NRF_MODEM_DECT_PHY_TEMP_NOT_MEASURED sentinel */

#if defined(CONFIG_MEMFAULT)
#include <memfault/metrics/metrics.h>
#include <memfault/core/trace_event.h>
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

#if defined(CONFIG_AETHER_ROUTING_HONR)
		MEMFAULT_METRIC_SET_UNSIGNED(dect_is_root, honr_is_root(c->honr_addr) ? 1U : 0U);
#endif
		/* Link health from the HeyMac L2 context (last-frame RSSI/SNR + error
		 * counters) -- available today, no aephyr changes. */
		struct heymac_context *hc = c->iface ? net_if_l2_data(c->iface) : NULL;

		if (hc) {
			MEMFAULT_METRIC_SET_SIGNED(dect_rssi, hc->last_rx_rssi);
			MEMFAULT_METRIC_SET_SIGNED(dect_snr, hc->last_rx_snr);
			MEMFAULT_METRIC_SET_UNSIGNED(dect_tx_err, hc->tx_errors);
			MEMFAULT_METRIC_SET_UNSIGNED(dect_rx_err, hc->rx_errors);
		}
		/* RF regime (build config) -- fleet filter axes. */
		MEMFAULT_METRIC_SET_UNSIGNED(dect_carrier, CONFIG_AETHER_DECT_CARRIER);
		MEMFAULT_METRIC_SET_UNSIGNED(dect_tx_power, CONFIG_AETHER_DECT_TX_POWER);

		/* Phase 2c: live DECT NR+ PHY telemetry, straight from the modem. LBT
		 * deferrals + tx/rx outcomes (RF characterization), and the modem's own
		 * temperature + supply voltage (thermal/battery health in the field). */
		struct dect_phy_stats ps;

		dect_phy_get_stats(&ps);
		MEMFAULT_METRIC_SET_UNSIGNED(dect_tx_ok, ps.tx_ok);
		MEMFAULT_METRIC_SET_UNSIGNED(dect_lbt_busy, ps.tx_busy);
		MEMFAULT_METRIC_SET_UNSIGNED(dect_rx_pcc, ps.rx_pcc);
		MEMFAULT_METRIC_SET_UNSIGNED(dect_rx_frames, ps.rx_frames);
		if (ps.last_temp_c != NRF_MODEM_DECT_PHY_TEMP_NOT_MEASURED) {
			MEMFAULT_METRIC_SET_SIGNED(dect_temp, ps.last_temp_c);
		}
		if (ps.last_voltage_mv != 0) {
			MEMFAULT_METRIC_SET_UNSIGNED(dect_voltage, ps.last_voltage_mv);
		}

		/* Phase 2: self-organization + ARQ reliability (aephyr counters). */
		MEMFAULT_METRIC_SET_UNSIGNED(dect_reelections, c->reelections);
		MEMFAULT_METRIC_SET_UNSIGNED(dect_orphans, c->orphans);
		MEMFAULT_METRIC_SET_UNSIGNED(dect_arq_retx, c->arq_retx);
		MEMFAULT_METRIC_SET_UNSIGNED(dect_arq_failed, c->arq_failed);

		/* Datagram service (dect_mesh counters): volume + rxq back-pressure. */
		uint32_t dtx = 0, drx = 0, drop = 0;

		aether_net_get_stats(&dtx, &drx, &drop);
		MEMFAULT_METRIC_SET_UNSIGNED(dect_data_tx, dtx);
		MEMFAULT_METRIC_SET_UNSIGNED(dect_data_rx, drx);
		MEMFAULT_METRIC_SET_UNSIGNED(dect_rxq_drops, drop);

		/* Trace events on the discrete moments: a counter that stepped since the
		 * last poll marks a self-heal / orphan / delivery-failure on the timeline. */
		static uint32_t prev_reel, prev_orph, prev_arqf;

		if (c->reelections > prev_reel) {
			MEMFAULT_TRACE_EVENT(Dect_Reelection);
		}
		if (c->orphans > prev_orph) {
			MEMFAULT_TRACE_EVENT(Dect_Orphan);
		}
		if (c->arq_failed > prev_arqf) {
			MEMFAULT_TRACE_EVENT(Dect_ArqFailed);
		}
		prev_reel = c->reelections;
		prev_orph = c->orphans;
		prev_arqf = c->arq_failed;
	}
	/* High-resolution cadence for the roaming field test: sample every 15 s so a
	 * 15 s heartbeat never reports a stale snapshot (see CONFIG_MEMFAULT_METRICS_
	 * HEARTBEAT_INTERVAL_SECS in prj.conf). */
	k_work_reschedule(k_work_delayable_from_work(work), K_SECONDS(15));
}
static K_WORK_DELAYABLE_DEFINE(metrics_work, metrics_collect);
#endif /* CONFIG_MEMFAULT */

void dect_metrics_init(void)
{
#if defined(CONFIG_MEMFAULT) && defined(CONFIG_MEMFAULT_NCS_DEVICE_ID_RUNTIME)
	char id[24];
	int len;

	/* Identity = the CGA (node_eui): the self-certifying address IS the device's
	 * name in the fleet, so a reviewer filters Memfault by cryptographic identity.
	 * Fall back to the hardware node id if the mesh isn't up yet. */
	if (g_mesh_ctx) {
		const uint8_t *e = g_mesh_ctx->node_eui;

		len = snprintf(id, sizeof(id), "dect-%02x%02x%02x%02x%02x%02x",
			       e[0], e[1], e[2], e[3], e[4], e[5]);
	} else {
		len = snprintf(id, sizeof(id), "dect-mesh-%08x", aether_honr_node_id());
	}
	if (len > 0 && len < (int)sizeof(id) &&
	    memfault_ncs_device_id_set(id, len) == 0) {
		LOG_INF("Memfault device ID (CGA): %s", id);
	}
#endif
#if defined(CONFIG_MEMFAULT)
	k_work_reschedule(&metrics_work, K_SECONDS(5));
#endif
}
