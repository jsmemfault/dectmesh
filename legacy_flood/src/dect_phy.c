/*
 * Copyright (c) 2026 Jon Sharp
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * DECT NR+ PHY wrapper. The init/configure/activate handshake and the operation
 * cadence follow the nRF Connect SDK "DECT NR+ PHY hello" sample, generalized
 * into a reusable transmit/receive interface for the mesh layer.
 */

#include "dect_phy.h"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/drivers/hwinfo.h>
#include <nrf_modem_dect_phy.h>
#include <modem/nrf_modem_lib.h>

LOG_MODULE_REGISTER(dect_phy, CONFIG_DECT_MESH_LOG_LEVEL);

BUILD_ASSERT(CONFIG_DECT_MESH_CARRIER,
	     "DECT NR+ carrier must be set per local regulations "
	     "(see overlay-eu.conf / overlay-us.conf)");

#define DECT_PHY_TX_HANDLE 0
#define DECT_PHY_RX_HANDLE 1

/* PHY header type 1. Due to endianness the field order differs from the spec. */
struct phy_ctrl_field_common {
	uint32_t packet_length : 4;
	uint32_t packet_length_type : 1;
	uint32_t header_format : 3;
	uint32_t short_network_id : 8;
	uint32_t transmitter_id_hi : 8;
	uint32_t transmitter_id_lo : 8;
	uint32_t df_mcs : 3;
	uint32_t reserved : 1;
	uint32_t transmit_power : 4;
	uint32_t pad : 24;
};

/* Synchronizes blocking calls with their completion event. */
static K_SEM_DEFINE(operation_sem, 0, 1);
static K_SEM_DEFINE(deinit_sem, 0, 1);

static uint16_t phy_device_id;
static uint64_t modem_time;
static bool init_failed;
static dect_phy_rx_cb_t rx_cb;

/* Transmitter ID from the most recent control-channel header, paired with the
 * data-channel payload that follows it. Both are accessed only from the modem
 * event-handler context, so no additional locking is required.
 */
static uint16_t last_pcc_tx_id;

static const struct nrf_modem_dect_phy_config_params config_params = {
	.band_group_index =
		((CONFIG_DECT_MESH_CARRIER >= 525 && CONFIG_DECT_MESH_CARRIER <= 551)) ? 1 : 0,
	.harq_rx_process_count = 4,
	.harq_rx_expiry_time_us = 5000000,
};

static void on_init(const struct nrf_modem_dect_phy_init_event *evt)
{
	if (evt->err) {
		LOG_ERR("Init failed, err %d", evt->err);
		init_failed = true;
		return;
	}

	k_sem_give(&operation_sem);
}

static void on_configure(const struct nrf_modem_dect_phy_configure_event *evt)
{
	if (evt->err) {
		LOG_ERR("Configure failed, err %d", evt->err);
		init_failed = true;
		return;
	}

	k_sem_give(&operation_sem);
}

static void on_activate(const struct nrf_modem_dect_phy_activate_event *evt)
{
	if (evt->err) {
		LOG_ERR("Activate failed, err %d", evt->err);
		init_failed = true;
		return;
	}

	k_sem_give(&operation_sem);
}

static void on_op_complete(const struct nrf_modem_dect_phy_op_complete_event *evt)
{
	LOG_DBG("op_complete time %" PRIu64 " status %d", modem_time, evt->err);
	k_sem_give(&operation_sem);
}

static void on_cancel(const struct nrf_modem_dect_phy_cancel_event *evt)
{
	LOG_DBG("cancel status %d", evt->err);
	k_sem_give(&operation_sem);
}

/* Control-channel reception: remember who is transmitting this frame. */
static void on_pcc(const struct nrf_modem_dect_phy_pcc_event *evt)
{
	last_pcc_tx_id = (uint16_t)(evt->hdr.hdr_type_1.transmitter_id_hi << 8 |
				    evt->hdr.hdr_type_1.transmitter_id_lo);
}

/* Data-channel reception: deliver the payload to the application. */
static void on_pdc(const struct nrf_modem_dect_phy_pdc_event *evt)
{
	/* RSSI-2 is in dBm with 0.5 dBm resolution (Q14.1). */
	int16_t rssi_dbm = evt->rssi_2 / 2;

	if (rx_cb != NULL && evt->data != NULL && evt->len > 0) {
		rx_cb((const uint8_t *)evt->data, evt->len, rssi_dbm, last_pcc_tx_id);
	}
}

static void dect_phy_event_handler(const struct nrf_modem_dect_phy_event *evt)
{
	modem_time = evt->time;

	switch (evt->id) {
	case NRF_MODEM_DECT_PHY_EVT_INIT:
		on_init(&evt->init);
		break;
	case NRF_MODEM_DECT_PHY_EVT_CONFIGURE:
		on_configure(&evt->configure);
		break;
	case NRF_MODEM_DECT_PHY_EVT_ACTIVATE:
		on_activate(&evt->activate);
		break;
	case NRF_MODEM_DECT_PHY_EVT_COMPLETED:
		on_op_complete(&evt->op_complete);
		break;
	case NRF_MODEM_DECT_PHY_EVT_CANCELED:
		on_cancel(&evt->cancel);
		break;
	case NRF_MODEM_DECT_PHY_EVT_PCC:
		on_pcc(&evt->pcc);
		break;
	case NRF_MODEM_DECT_PHY_EVT_PDC:
		on_pdc(&evt->pdc);
		break;
	default:
		/* Remaining events (RSSI, capability, time, errors) are unused
		 * by the mesh and are intentionally ignored.
		 */
		break;
	}
}

void dect_phy_set_rx_cb(dect_phy_rx_cb_t cb)
{
	rx_cb = cb;
}

int dect_phy_init(uint16_t *device_id)
{
	int err;

	err = nrf_modem_lib_init();
	if (err) {
		LOG_ERR("nrf_modem_lib_init failed, err %d", err);
		return err;
	}

	err = nrf_modem_dect_phy_event_handler_set(dect_phy_event_handler);
	if (err) {
		LOG_ERR("nrf_modem_dect_phy_event_handler_set failed, err %d", err);
		return err;
	}

	err = nrf_modem_dect_phy_init();
	if (err) {
		LOG_ERR("nrf_modem_dect_phy_init failed, err %d", err);
		return err;
	}

	k_sem_take(&operation_sem, K_FOREVER);
	if (init_failed) {
		return -EIO;
	}

	err = nrf_modem_dect_phy_configure(&config_params);
	if (err) {
		LOG_ERR("nrf_modem_dect_phy_configure failed, err %d", err);
		return err;
	}

	k_sem_take(&operation_sem, K_FOREVER);
	if (init_failed) {
		return -EIO;
	}

	err = nrf_modem_dect_phy_activate(NRF_MODEM_DECT_PHY_RADIO_MODE_LOW_LATENCY);
	if (err) {
		LOG_ERR("nrf_modem_dect_phy_activate failed, err %d", err);
		return err;
	}

	k_sem_take(&operation_sem, K_FOREVER);
	if (init_failed) {
		return -EIO;
	}

	/* Derive the PHY device ID from the hardware ID. */
	hwinfo_get_device_id((void *)&phy_device_id, sizeof(phy_device_id));

	if (device_id != NULL) {
		*device_id = phy_device_id;
	}

	LOG_INF("DECT NR+ PHY ready, device ID %u, carrier %d", phy_device_id,
		CONFIG_DECT_MESH_CARRIER);

	return 0;
}

int dect_phy_tx(const uint8_t *data, size_t len)
{
	int err;

	struct phy_ctrl_field_common header = {
		.header_format = 0x0,
		.packet_length_type = 0x0,
		.packet_length = 0x01,
		.short_network_id = (CONFIG_DECT_MESH_NETWORK_ID & 0xff),
		.transmitter_id_hi = (phy_device_id >> 8),
		.transmitter_id_lo = (phy_device_id & 0xff),
		.transmit_power = CONFIG_DECT_MESH_TX_POWER,
		.reserved = 0,
		.df_mcs = CONFIG_DECT_MESH_MCS,
	};

	struct nrf_modem_dect_phy_tx_params tx_op_params = {
		.start_time = 0,
		.handle = DECT_PHY_TX_HANDLE,
		.network_id = CONFIG_DECT_MESH_NETWORK_ID,
		.phy_type = 0,
		.lbt_rssi_threshold_max = 0,
		.carrier = CONFIG_DECT_MESH_CARRIER,
		.lbt_period = NRF_MODEM_DECT_LBT_PERIOD_MAX,
		.phy_header = (union nrf_modem_dect_phy_hdr *)&header,
		.data = (void *)data,
		.data_size = len,
	};

	err = nrf_modem_dect_phy_tx(&tx_op_params);
	if (err) {
		LOG_ERR("nrf_modem_dect_phy_tx failed, err %d", err);
		return err;
	}

	/* Wait for the transmit operation to complete. */
	k_sem_take(&operation_sem, K_FOREVER);

	return 0;
}

int dect_phy_rx_window(uint32_t duration_seconds)
{
	int err;

	struct nrf_modem_dect_phy_rx_params rx_op_params = {
		.start_time = 0,
		.handle = DECT_PHY_RX_HANDLE,
		.network_id = CONFIG_DECT_MESH_NETWORK_ID,
		.mode = NRF_MODEM_DECT_PHY_RX_MODE_CONTINUOUS,
		.rssi_interval = NRF_MODEM_DECT_PHY_RSSI_INTERVAL_OFF,
		.link_id = NRF_MODEM_DECT_PHY_LINK_UNSPECIFIED,
		.rssi_level = -60,
		.carrier = CONFIG_DECT_MESH_CARRIER,
		.duration = (uint64_t)duration_seconds * MSEC_PER_SEC *
			    NRF_MODEM_DECT_MODEM_TIME_TICK_RATE_KHZ,
		.filter.short_network_id = CONFIG_DECT_MESH_NETWORK_ID & 0xff,
		.filter.is_short_network_id_used = 1,
		/* Receive from any transmitter (broadcast flooding). */
		.filter.receiver_identity = 0,
	};

	err = nrf_modem_dect_phy_rx(&rx_op_params);
	if (err) {
		LOG_ERR("nrf_modem_dect_phy_rx failed, err %d", err);
		return err;
	}

	/* Wait for the receive window to elapse. */
	k_sem_take(&operation_sem, K_FOREVER);

	return 0;
}
