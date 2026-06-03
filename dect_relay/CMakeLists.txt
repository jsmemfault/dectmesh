# SPDX-License-Identifier: MIT
#
# DECT mesh BLE relay (Thingy:91 X nRF5340): L2CAP CoC <-> inter-chip UART.
# Consumes 9P4Z as a Zephyr module (9P session pools, sysfs, DFU):
#   west build -b thingy91x/nrf5340/cpuapp . --sysbuild -- \
#     -DZEPHYR_EXTRA_MODULES=/Users/jrsharp/src/9p4z

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(dect_relay)

target_sources(app PRIVATE src/main.c)
