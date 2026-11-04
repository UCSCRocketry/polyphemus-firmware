#******************************************************************************
#
# SPDX-FileCopyrightText: Copyright (c) 2026, UCSC Rocket Team
#
# SPDX-License-Identifier: BSD-3-Clause
#
#******************************************************************************

#
# The JPEG half of the STM32N6 VENC encoder library. The common half, the headers
# and the EWL functions that reach the hardware all come from Zephyr's VENC
# support, so that must be enabled too.
#
if(NOT CONFIG_VIDEO_STM32_VENC)
  message(FATAL_ERROR "vc8000nanoe_jpeg needs CONFIG_VIDEO_STM32_VENC")
endif()

set(VC8000NANOE_DIR ${ZEPHYR_HAL_STM32_MODULE_DIR}/lib/vc8000nanoe)

add_library(vc8000nanoe_jpeg STATIC
  source/jpeg/EncJpeg.c
  source/jpeg/EncJpegCodeFrame.c
  source/jpeg/EncJpegInit.c
  source/jpeg/EncJpegPutBits.c
  source/jpeg/JpegEncApi.c
)

#
# The same definitions as hal_stm32 builds the common half with.
#
target_compile_definitions(vc8000nanoe_jpeg PRIVATE H1_INTERNAL H1_HAVE_ENCDEBUG_H)

target_include_directories(vc8000nanoe_jpeg
  PUBLIC ${VC8000NANOE_DIR}/inc
  PRIVATE source/jpeg ${VC8000NANOE_DIR}/source/common
)

target_link_libraries(vc8000nanoe_jpeg PUBLIC zephyr_interface)
