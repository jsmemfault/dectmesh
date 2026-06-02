/*
 * Copyright (c) 2026 Jon Sharp
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Memfault platform configuration overrides for the DECT NR+ mesh.
 */

#ifndef MEMFAULT_PLATFORM_CONFIG_H_
#define MEMFAULT_PLATFORM_CONFIG_H_

/* Memfault platform overrides for the DECT NR+ mesh.
 *
 * The heartbeat interval is set through Kconfig in prj.conf
 * (CONFIG_MEMFAULT_METRICS_HEARTBEAT_INTERVAL_SECS); defining the legacy
 * MEMFAULT_METRICS_HEARTBEAT_INTERVAL_SECS macro here is deprecated in NCS
 * v3.3.0. Add any non-Kconfig Memfault compile-time overrides below.
 */

#endif /* MEMFAULT_PLATFORM_CONFIG_H_ */
