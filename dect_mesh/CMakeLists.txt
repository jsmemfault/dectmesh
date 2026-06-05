# SPDX-License-Identifier: MIT
#
# Æther/HONR mesh demo application over the DECT NR+ PHY.
# Consumes the Æther/HONR mesh stack (aephyr) and 9P4Z as Zephyr modules:
#   west build -b thingy91x/nrf9151/ns . -- \
#     -DZEPHYR_EXTRA_MODULES="/Users/jrsharp/src/aephyr;/Users/jrsharp/src/9p4z"

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(dect_mesh)

# Memfault picks up memfault_platform_config.h and the heartbeat metric defs
# from this directory.
zephyr_include_directories(config)

target_sources(app PRIVATE src/main.c)
target_sources_ifdef(CONFIG_NINEP app PRIVATE src/aether_9p.c)
target_sources_ifdef(CONFIG_NINEP app PRIVATE src/aether_net.c)
target_sources_ifdef(CONFIG_MEMFAULT app PRIVATE src/dect_metrics.c)
