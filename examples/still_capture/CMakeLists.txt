#******************************************************************************
#
# SPDX-FileCopyrightText: Copyright (c) 2026, UCSC Rocket Team
#
# SPDX-License-Identifier: BSD-3-Clause
#
#******************************************************************************

# set(BOARD nucleo_n657x0_q)
set(BOARD stm32n6570_dk/stm32n657xx/sb)
set(SHIELD st_b_cams_imx_mb1854)

cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(still_capture)

target_sources(app PRIVATE src/main.c)
