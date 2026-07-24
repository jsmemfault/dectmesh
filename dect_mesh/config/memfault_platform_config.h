/*
 * Copyright (c) 2026 Jon Sharp
 * SPDX-License-Identifier: MIT
 *
 * Memfault platform overrides for the DECT NR+ mesh. The heartbeat interval is
 * set via Kconfig (CONFIG_MEMFAULT_METRICS_HEARTBEAT_INTERVAL_SECS); add any
 * non-Kconfig compile-time overrides below.
 */

#ifndef MEMFAULT_PLATFORM_CONFIG_H_
#define MEMFAULT_PLATFORM_CONFIG_H_

/* User-defined trace-event reasons (see config/memfault_trace_reason_user_config.def).
 * config/ is on the include path (zephyr_include_directories(config)). */
#define MEMFAULT_TRACE_REASON_USER_DEFS_FILE "memfault_trace_reason_user_config.def"

#endif /* MEMFAULT_PLATFORM_CONFIG_H_ */
