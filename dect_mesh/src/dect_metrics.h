/*
 * Copyright (c) 2026 Jon Sharp
 * SPDX-License-Identifier: MIT
 *
 * Memfault device identity + mesh heartbeat metrics. No-ops without CONFIG_MEMFAULT.
 */

#ifndef DECT_METRICS_H_
#define DECT_METRICS_H_

/** @brief Set the Memfault runtime device ID from this node's identity. */
void dect_metrics_init(void);

#endif /* DECT_METRICS_H_ */
