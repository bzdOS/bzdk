# Copyright (c) 2026 bzdOS project
# SPDX-License-Identifier: Apache-2.0
#
# Minimal standalone heartbeat app for the bpi_m64_hv board (see
# ../boards/bzdos/bpi_m64_hv/). Deliberately does not use k_sleep()/any
# kernel timeout API: this board runs CONFIG_SYS_CLOCK_EXISTS=n for its
# first bring-up (see that board's Kconfig.defconfig for why), so all
# pacing here is k_busy_wait(), which works without a system tick.

cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(bpi_m64_hv_heartbeat)

target_sources(app PRIVATE src/main.c)
