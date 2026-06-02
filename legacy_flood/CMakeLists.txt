#
# Copyright (c) 2026 Jon Sharp
#
# SPDX-License-Identifier: Apache-2.0
#

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(dect_nrplus_mesh)

# Memfault picks up memfault_platform_config.h and
# memfault_metrics_heartbeat_config.def from this directory.
zephyr_include_directories(config)

target_sources(app PRIVATE
	src/main.c
	src/dect_phy.c
	src/mesh.c
	src/mesh_metrics.c
)
