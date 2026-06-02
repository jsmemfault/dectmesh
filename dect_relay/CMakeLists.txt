# SPDX-License-Identifier: MIT
#
# DECT mesh BLE relay (Thingy:91 X nRF5340): L2CAP CoC <-> inter-chip UART.
#   west build -b thingy91x/nrf5340/cpuapp . --sysbuild

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(dect_relay)

target_sources(app PRIVATE src/main.c)
