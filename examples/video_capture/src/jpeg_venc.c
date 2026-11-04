//*****************************************************************************
//
// SPDX-FileCopyrightText: Copyright (c) 2026, UCSC Rocket Team
//
// SPDX-License-Identifier: BSD-3-Clause
//
//*****************************************************************************

//*****************************************************************************
//
//! @file jpeg_venc.c
//!
//! @brief Encode NV12 frames as JPEGs with the STM32N6 VENC peripheral.
//!
//! @addtogroup video_capture
//! @{
//!
//! The VENC reads the frame straight from memory and writes the JPEG straight
//! to memory, so an encode costs the CPU almost nothing. The separate JPEG codec
//! cannot do this: it needs its input rearranged into MCU blocks first, which
//! would take about 45% of the CPU at this resolution and frame rate.
//!
//! The encoder is ST's library in third_party/vc8000nanoe_jpeg, which reaches
//! the hardware through the EWL functions in Zephyr's VENC driver. Zephyr itself
//! only supports H.264 on the VENC, which leaves three things to do here:
//!
//! 1. Turn on the APB5 bus. The driver only turns on the VENC's own clock, so
//!    without this its registers read as 0 and the library decides that the
//!    hardware has no JPEG encoder.
//!
//! 2. Keep the scheduler running during an encode. The driver's interrupt is a
//!    direct ISR that does not reschedule, so a waiting thread only wakes at the
//!    next ordinary interrupt, which could be a whole frame later. A short
//!    periodic timer provides those interrupts.
//!
//! 3. Avoid CONFIG_ASSERT, as the driver's EWLInit() asserts that its client is
//!    the H.264 encoder.
//
//*****************************************************************************

//
// Standard C library
//
#include <errno.h>                  // Error codes

//
// Zephyr kernel and drivers
//
#include <zephyr/kernel.h>          // Timers
#include <zephyr/device.h>          // Device readiness
#include <zephyr/logging/log.h>     // Logging over the RTT console
#include <zephyr/sys/util.h>        // CLAMP()

//
// STM32Cube HAL
//
#include <soc.h>                    // RCC registers and the RIF

//
// ST's encoder library
//
#include <jpegencapi.h>             // JPEG encoder API

#include "jpeg_venc.h"

//*****************************************************************************
//
// Constants.
//
//*****************************************************************************

#define VENC_NODE               DT_NODELABEL(venc)

#define FRAME_WIDTH             CONFIG_VIDEO_CAPTURE_WIDTH
#define FRAME_HEIGHT            CONFIG_VIDEO_CAPTURE_HEIGHT

#define QUANT_TABLE_SIZE        64U

//
// The VENC's security attributes in the RIF, as set at boot.
//
#define VENC_RIF_ATTRIBUTES     (RIF_ATTRIBUTE_SEC | RIF_ATTRIBUTE_PRIV)

BUILD_ASSERT(FRAME_WIDTH % 16 == 0 && FRAME_HEIGHT % 16 == 0,
             "The JPEG encoder works in 16x16 blocks");

BUILD_ASSERT(!IS_ENABLED(CONFIG_ASSERT),
             "Zephyr's VENC driver asserts that it is only used for H.264");

//
// The example quantization tables in ISO/IEC 10918-1, annexes K.1 and K.2. The
// encoder takes them in this raster order, although jpegencapi.h says zigzag.
//
static const uint8_t base_quant_luma[QUANT_TABLE_SIZE] = {
    16, 11, 10, 16,  24,  40,  51,  61,
    12, 12, 14, 19,  26,  58,  60,  55,
    14, 13, 16, 24,  40,  57,  69,  56,
    14, 17, 22, 29,  51,  87,  80,  62,
    18, 22, 37, 56,  68, 109, 103,  77,
    24, 35, 55, 64,  81, 104, 113,  92,
    49, 64, 78, 87, 103, 121, 120, 101,
    72, 92, 95, 98, 112, 100, 103,  99,
};

static const uint8_t base_quant_chroma[QUANT_TABLE_SIZE] = {
    17, 18, 24, 47, 99, 99, 99, 99,
    18, 21, 26, 66, 99, 99, 99, 99,
    24, 26, 56, 99, 99, 99, 99, 99,
    47, 66, 99, 99, 99, 99, 99, 99,
    99, 99, 99, 99, 99, 99, 99, 99,
    99, 99, 99, 99, 99, 99, 99, 99,
    99, 99, 99, 99, 99, 99, 99, 99,
    99, 99, 99, 99, 99, 99, 99, 99,
};

//*****************************************************************************
//
// Globals.
//
//*****************************************************************************

LOG_MODULE_REGISTER(jpeg_venc);

static JpegEncInst encoder;
static uint8_t quant_luma[QUANT_TABLE_SIZE];
static uint8_t quant_chroma[QUANT_TABLE_SIZE];

//
// Does nothing itself. Its interrupt gives the scheduler a chance to run.
//
K_TIMER_DEFINE(scheduler_nudge, NULL, NULL);

//*****************************************************************************
//
// Scale a base quantization table to a JPEG quality.
//
//*****************************************************************************
static void
scale_quant_table(const uint8_t *base, uint8_t *table, uint32_t quality)
{
    uint32_t scale = (quality < 50U) ? (5000U / quality) : (200U - (2U * quality));

    for ( uint32_t i = 0; i < QUANT_TABLE_SIZE; i++ )
    {
        uint32_t value = ((base[i] * scale) + 50U) / 100U;

        table[i] = (uint8_t)CLAMP(value, 1U, 255U);
    }
}

//*****************************************************************************
//
// Create the encoder instance.
//
//*****************************************************************************
static int
encoder_start(void)
{
    const JpegEncCfg config = {
        .inputWidth = FRAME_WIDTH,
        .inputHeight = FRAME_HEIGHT,
        .codingWidth = FRAME_WIDTH,
        .codingHeight = FRAME_HEIGHT,
        .qTableLuma = quant_luma,
        .qTableChroma = quant_chroma,
        .frameType = JPEGENC_YUV420_SEMIPLANAR,
        .rotation = JPEGENC_ROTATE_0,
        .codingType = JPEGENC_WHOLE_FRAME,
        .codingMode = JPEGENC_420_MODE,
        .unitsType = JPEGENC_NO_UNITS,
        .markerType = JPEGENC_SINGLE_MARKER,
        .xDensity = 1,
        .yDensity = 1,
    };
    JpegEncRet ret;

    ret = JpegEncInit(&config, &encoder);
    if ( ret != JPEGENC_OK )
    {
        LOG_ERR("JpegEncInit() returned %d", ret);
        return -EIO;
    }

    ret = JpegEncSetPictureSize(encoder, &config);
    if ( ret != JPEGENC_OK )
    {
        LOG_ERR("JpegEncSetPictureSize() returned %d", ret);
        JpegEncRelease(encoder);
        encoder = NULL;
        return -EIO;
    }

    return 0;
}

//*****************************************************************************
//
// Bring up the JPEG encoder.
//
//*****************************************************************************
int
jpeg_venc_init(uint32_t quality)
{
    if ( !device_is_ready(DEVICE_DT_GET(VENC_NODE)) )
    {
        LOG_ERR("The VENC is not ready");
        return -ENODEV;
    }

    WRITE_REG(RCC->BUSENSR, RCC_BUSENSR_APB5ENS);

    scale_quant_table(base_quant_luma, quant_luma, quality);
    scale_quant_table(base_quant_chroma, quant_chroma, quality);

    return encoder_start();
}

//*****************************************************************************
//
// Check the VENC's security attributes in the RIF, and restore them if they were
// lost, returning whether they were intact.
//
// While a debugger connects, these can be cleared. The VENC then carries on as if
// nothing were wrong, but cannot reach the PSRAM, so it reads a blank frame and writes
// nothing, leaving whatever was already in the output buffer. It reports success, and
// neither resetting it nor starting the encoder again helps.
//
//*****************************************************************************
static bool
venc_access_intact(void)
{
    if ( HAL_RIF_RISC_GetSlaveSecureAttributes(RIF_RISC_PERIPH_INDEX_VENC) == VENC_RIF_ATTRIBUTES )
    {
        return true;
    }

    LOG_WRN("The VENC lost its access to memory, and it has been restored");
    HAL_RIF_RISC_SetSlaveSecureAttributes(RIF_RISC_PERIPH_INDEX_VENC, VENC_RIF_ATTRIBUTES);

    return false;
}

//*****************************************************************************
//
// Encode one NV12 frame, and wait for it to finish.
//
//*****************************************************************************
int
jpeg_venc_encode(const uint8_t *frame, uint8_t *out, size_t out_size, size_t *out_len)
{
    const uint8_t *chroma = frame + (FRAME_WIDTH * FRAME_HEIGHT);
    const JpegEncIn input = {
        .frameHeader = 1,
        .busLum = (size_t)frame,
        .busCb = (size_t)chroma,
        .busCr = (size_t)chroma,
        .pLum = frame,
        .pCb = chroma,
        .pCr = chroma,
        .pOutBuf = out,
        .busOutBuf = (size_t)out,
        .outBufSize = (u32)out_size,
    };
    JpegEncOut output = {0};
    JpegEncRet ret;

    (void)venc_access_intact();

    k_timer_start(&scheduler_nudge, K_MSEC(1), K_MSEC(1));
    ret = JpegEncEncode(encoder, &input, &output, NULL, NULL);
    k_timer_stop(&scheduler_nudge);

    //
    // Nothing is wrong with the encoder if this frame was lost, so there is no need
    // to start it again.
    //
    if ( !venc_access_intact() )
    {
        return -EIO;
    }

    if ( ret == JPEGENC_FRAME_READY )
    {
        *out_len = output.jfifSize;
        return 0;
    }

    if ( ret == JPEGENC_OUTPUT_BUFFER_OVERFLOW )
    {
        return -ENOSPC;
    }

    //
    // Start again with a new instance, to leave nothing of this failure behind.
    // If that fails, there is no instance, and the library rejects the next frame
    // for that reason, which brings this back here to try again.
    //
    LOG_ERR("JpegEncEncode() returned %d", ret);
    JpegEncRelease(encoder);
    encoder = NULL;
    if ( encoder_start() < 0 )
    {
        LOG_ERR("Could not restart the encoder");
    }

    return -EIO;
}

//*****************************************************************************
//
// End Doxygen group.
//! @}
//
//*****************************************************************************
