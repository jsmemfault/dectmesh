/*
 * Copyright (c) 2026 Jon Sharp
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef DECT_PHY_H_
#define DECT_PHY_H_

#include <stdint.h>
#include <stddef.h>

/**
 * @file
 * @brief Thin wrapper around the DECT NR+ PHY modem API.
 *
 * Hides the init/configure/activate handshake and the event-handler plumbing,
 * and exposes a simple blocking transmit plus a continuous receive window. Each
 * decoded data frame is delivered to a registered callback. The wrapper is
 * single-radio and half-duplex: transmit while no receive window is open, then
 * open a receive window to collect inbound frames.
 */

/**
 * @brief Callback invoked for every successfully decoded DECT NR+ data frame.
 *
 * Called from the modem event-handler context. Keep the work short and
 * non-blocking; queue the frame for processing on an application thread.
 *
 * @param data        Pointer to the received payload (valid only for the call).
 * @param len         Payload length in bytes.
 * @param rssi_dbm    Received signal strength in dBm.
 * @param last_hop_id PHY transmitter ID of the immediate sender (last hop).
 */
typedef void (*dect_phy_rx_cb_t)(const uint8_t *data, size_t len, int16_t rssi_dbm,
				 uint16_t last_hop_id);

/**
 * @brief Initialize the modem and bring up the DECT NR+ PHY.
 *
 * Runs the full init, configure and activate handshake, blocking until each
 * step completes.
 *
 * @param device_id Output: the PHY device ID derived from the hardware ID.
 *
 * @retval 0 on success, negative errno otherwise.
 */
int dect_phy_init(uint16_t *device_id);

/**
 * @brief Register the receive callback.
 *
 * @param cb Callback to invoke per decoded frame, or NULL to disable.
 */
void dect_phy_set_rx_cb(dect_phy_rx_cb_t cb);

/**
 * @brief Transmit one DECT NR+ data frame and block until the operation ends.
 *
 * @param data Payload to send.
 * @param len  Payload length in bytes.
 *
 * @retval 0 on success, negative errno otherwise.
 */
int dect_phy_tx(const uint8_t *data, size_t len);

/**
 * @brief Open a continuous receive window and block until it ends.
 *
 * Decoded frames are delivered to the registered receive callback while the
 * window is open.
 *
 * @param duration_seconds Receive window duration in seconds.
 *
 * @retval 0 on success, negative errno otherwise.
 */
int dect_phy_rx_window(uint32_t duration_seconds);

#endif /* DECT_PHY_H_ */
