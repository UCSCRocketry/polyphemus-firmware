# SPDX-License-Identifier: Apache-2.0

# set(BOARD nucleo_n657x0_q)
set(BOARD stm32n6570_dk/stm32n657xx/sb)
# set(BOARD nucleo_f411re)
# set(BOARD nucleo_f446re)
# set(BOARD nucleo_l073rz)

cmake_minimum_required(VERSION 3.28.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(hello_world)

target_sources(app PRIVATE src/main.c)
