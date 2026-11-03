//*****************************************************************************
//
// SPDX-FileCopyrightText: Copyright (c) 2026, UCSC Rocket Team
//
// SPDX-License-Identifier: BSD-3-Clause
//
//*****************************************************************************

//*****************************************************************************
//
//! @file main.c
//!
//! @brief Capture video from the camera and encode it as MJPEG.
//!
//! @addtogroup payload_examples Payload Examples
//
//! @defgroup video_capture Video Capture Example
//! @ingroup payload_examples
//! @{
//!
//! Purpose: This example streams NV12 frames from the "zephyr,camera" device
//! and encodes each one as a JPEG, reporting the frame rate and data rate over
//! RTT.
//!
//! This example requires 4096 bytes of main stack.<br>
//! The stack size is set by CONFIG_MAIN_STACK_SIZE in prj.conf.<br>
//
//*****************************************************************************

//
// Zephyr kernel and drivers
//
#include <zephyr/kernel.h>          // Kernel services and timeouts
#include <zephyr/init.h>            // SYS_INIT() for the camera power-up wait
#include <zephyr/cache.h>           // Data cache flush and invalidate
#include <zephyr/drivers/video.h>   // Video capture API
#include <zephyr/video/video.h>     // Video buffers and compose format
#include <zephyr/logging/log.h>     // Logging over the RTT console

#include "jpeg_dma.h"

//*****************************************************************************
//
// Build-time checks.
//
//*****************************************************************************

#ifdef CONFIG_DCACHE
BUILD_ASSERT(CONFIG_VIDEO_BUFFER_POOL_ALIGN % CONFIG_DCACHE_LINE_SIZE == 0,
             "Video buffers are invalidated whole, so they must not share a cache line");
#endif

BUILD_ASSERT(CONFIG_VIDEO_CAPTURE_NUM_BUFS + CONFIG_VIDEO_CAPTURE_NUM_JPEG_BUFS <=
             CONFIG_VIDEO_BUFFER_POOL_NUM_MAX,
             "The video buffer pool needs a slot per capture buffer and per JPEG buffer");

//*****************************************************************************
//
// Globals.
//
//*****************************************************************************

LOG_MODULE_REGISTER(video_capture);

static const struct device *const camera_dev = DEVICE_DT_GET(DT_CHOSEN(zephyr_camera));

//
// Time between frames, in ms.
//
#define FRAME_PERIOD_MS         (1000U / CONFIG_VIDEO_CAPTURE_FPS)

//*****************************************************************************
//
// Extra delay for the camera module to power up.
//
//*****************************************************************************
#define CAMERA_POWER_UP_INIT_PRIORITY   59

BUILD_ASSERT(CONFIG_GPIO_HOGS_INIT_PRIORITY < CAMERA_POWER_UP_INIT_PRIORITY &&
             CAMERA_POWER_UP_INIT_PRIORITY < CONFIG_VIDEO_INIT_PRIORITY,
             "The camera power-up wait must run after the GPIO hogs and before the sensor driver");

static int
camera_power_up_wait(void)
{
    k_msleep(CONFIG_VIDEO_CAPTURE_CAMERA_POWER_UP_MS);
    return 0;
}

SYS_INIT(camera_power_up_wait, POST_KERNEL, CAMERA_POWER_UP_INIT_PRIORITY);

//*****************************************************************************
//
// Set one control on a device
//
//*****************************************************************************
static void
set_ctrl(const struct device *dev, uint32_t id, int32_t val, const char *name)
{
    struct video_control ctrl = {.id = id, .val = val};
    int ret;

    ret = video_set_ctrl(dev, &ctrl);
    if ( ret < 0 )
    {
        LOG_WRN("Unable to set %s to %d (err %d)", name, val, ret);
    }
}

//*****************************************************************************
//
// Ask the camera for NV12 at the configured size, frame rate and exposure.
//
// The sensor image is cropped to the aspect ratio of the video first, so that
// scaling it does not squeeze the picture.
//
//*****************************************************************************
static int
configure_camera(struct video_format *fmt)
{
    const uint32_t sensor_width = CONFIG_VIDEO_STM32_DCMIPP_SENSOR_WIDTH;
    const uint32_t sensor_height = CONFIG_VIDEO_STM32_DCMIPP_SENSOR_HEIGHT;
    uint32_t crop_width = sensor_width;
    uint32_t crop_height = sensor_height;
    struct video_selection crop;
    struct video_frmival frame_interval = {.numerator = 1,
                                           .denominator = CONFIG_VIDEO_CAPTURE_FPS};
    int ret;

    if ( sensor_width * CONFIG_VIDEO_CAPTURE_HEIGHT > sensor_height * CONFIG_VIDEO_CAPTURE_WIDTH )
    {
        crop_width = ROUND_DOWN(sensor_height * CONFIG_VIDEO_CAPTURE_WIDTH /
                                CONFIG_VIDEO_CAPTURE_HEIGHT, 2);
    }
    else
    {
        crop_height = ROUND_DOWN(sensor_width * CONFIG_VIDEO_CAPTURE_HEIGHT /
                                 CONFIG_VIDEO_CAPTURE_WIDTH, 2);
    }

    crop = (struct video_selection) {
        .type = VIDEO_BUF_TYPE_OUTPUT,
        .target = VIDEO_SEL_TGT_CROP,
        .rect = {
            .left = (sensor_width - crop_width) / 2U,
            .top = (sensor_height - crop_height) / 2U,
            .width = crop_width,
            .height = crop_height,
        },
    };

    ret = video_set_selection(camera_dev, &crop);
    if ( ret < 0 )
    {
        LOG_ERR("Unable to crop the sensor image to %ux%u (err %d)", crop_width, crop_height, ret);
        return ret;
    }

    *fmt = (struct video_format) {
        .type = VIDEO_BUF_TYPE_OUTPUT,
        .pixelformat = VIDEO_PIX_FMT_NV12,
        .width = CONFIG_VIDEO_CAPTURE_WIDTH,
        .height = CONFIG_VIDEO_CAPTURE_HEIGHT,
    };

    ret = video_set_compose_format(camera_dev, fmt);
    if ( ret < 0 )
    {
        LOG_ERR("Unable to set compose format NV12 %ux%u (err %d)", CONFIG_VIDEO_CAPTURE_WIDTH,
                CONFIG_VIDEO_CAPTURE_HEIGHT, ret);
        return ret;
    }

    ret = video_set_frmival(camera_dev, &frame_interval);
    if ( ret < 0 )
    {
        LOG_ERR("Unable to set the frame rate to %u fps (err %d)", CONFIG_VIDEO_CAPTURE_FPS, ret);
        return ret;
    }

    if ( CONFIG_VIDEO_CAPTURE_EXPOSURE >= 0 )
    {
        set_ctrl(camera_dev, VIDEO_CID_EXPOSURE, CONFIG_VIDEO_CAPTURE_EXPOSURE, "exposure");
    }

    if ( CONFIG_VIDEO_CAPTURE_GAIN >= 0 )
    {
        set_ctrl(camera_dev, VIDEO_CID_ANALOGUE_GAIN, CONFIG_VIDEO_CAPTURE_GAIN, "gain");
    }

    LOG_INF("Capturing NV12 %ux%u from a %ux%u crop at %u/%u s per frame, %u bytes per frame",
            fmt->width, fmt->height, crop_width, crop_height, frame_interval.numerator,
            frame_interval.denominator, fmt->size);

    return 0;
}

//*****************************************************************************
//
// Allocate one buffer from the video buffer pool.
//
//*****************************************************************************
static struct video_buffer *
alloc_buffer(size_t size)
{
    struct video_buffer *vbuf;

    vbuf = video_buffer_aligned_alloc(ROUND_UP(size, CONFIG_VIDEO_BUFFER_POOL_ALIGN),
                                      CONFIG_VIDEO_BUFFER_POOL_ALIGN, K_NO_WAIT);
    if ( vbuf == NULL )
    {
        LOG_ERR("Out of video buffer pool: %u bytes needed, "
                "raise CONFIG_VIDEO_BUFFER_POOL_HEAP_SIZE", (unsigned int)size);
    }

    return vbuf;
}

//*****************************************************************************
//
// Main.
//
//*****************************************************************************
int
main(void)
{
    struct video_format fmt;
    struct video_buffer *jpeg_buf;
    struct video_buffer *frame;
    int ret;

    LOG_INF("Video capture example!");

    if ( !device_is_ready(camera_dev) )
    {
        LOG_ERR("%s is not ready", camera_dev->name);
        return -ENODEV;
    }

    ret = configure_camera(&fmt);
    if ( ret < 0 )
    {
        return ret;
    }

    ret = jpeg_dma_init(CONFIG_VIDEO_CAPTURE_JPEG_QUALITY);
    if ( ret < 0 )
    {
        LOG_ERR("Unable to start the JPEG encoder (err %d)", ret);
        return ret;
    }

    jpeg_buf = alloc_buffer(CONFIG_VIDEO_CAPTURE_JPEG_BUF_SIZE);
    if ( jpeg_buf == NULL )
    {
        return -ENOMEM;
    }

    for ( unsigned int i = 0; i < CONFIG_VIDEO_CAPTURE_NUM_BUFS; i++ )
    {
        struct video_buffer *vbuf = alloc_buffer(fmt.size);

        if ( vbuf == NULL )
        {
            return -ENOMEM;
        }

        vbuf->type = VIDEO_BUF_TYPE_OUTPUT;
        ret = video_enqueue(camera_dev, vbuf);
        if ( ret < 0 )
        {
            LOG_ERR("Unable to enqueue a capture buffer (err %d)", ret);
            return ret;
        }
    }

    ret = video_stream_start(camera_dev, VIDEO_BUF_TYPE_OUTPUT);
    if ( ret < 0 )
    {
        LOG_ERR("Unable to start the camera (err %d)", ret);
        return ret;
    }

    uint32_t encoded = 0;
    uint32_t missed = 0;
    uint32_t failed = 0;
    uint32_t bytes = 0;
    uint64_t encode_total_us = 0;
    uint32_t encode_worst_us = 0;
    uint32_t last_timestamp = 0;
    size_t len = 0;
    int64_t start = 0;

    for ( uint32_t count = 0; ; count++ )
    {
        ret = video_dequeue(camera_dev, &frame, K_SECONDS(2));
        if ( ret < 0 )
        {
            LOG_ERR("No frame after %u frames (err %d)", count, ret);
            return ret;
        }

        if ( count == CONFIG_VIDEO_CAPTURE_WARMUP_FRAMES )
        {
            start = k_uptime_get();
        }
        else if ( count > CONFIG_VIDEO_CAPTURE_WARMUP_FRAMES )
        {
            uint32_t gap = frame->timestamp - last_timestamp;

            missed += (gap + (FRAME_PERIOD_MS / 2U)) / FRAME_PERIOD_MS - 1U;
        }
        last_timestamp = frame->timestamp;

        if ( count >= CONFIG_VIDEO_CAPTURE_WARMUP_FRAMES )
        {
            uint32_t t0 = k_cycle_get_32();

            sys_cache_data_invd_range(frame->buffer, frame->size);
            ret = jpeg_dma_encode(frame->buffer, jpeg_buf->buffer, jpeg_buf->size, &len);

            uint32_t us = k_cyc_to_us_floor32(k_cycle_get_32() - t0);

            if ( ret < 0 )
            {
                failed++;
                LOG_WRN("Encode failed (err %d)", ret);
            }
            else
            {
                encoded++;
                bytes += len;
                encode_total_us += us;
                encode_worst_us = MAX(encode_worst_us, us);
                sys_cache_data_flush_range(jpeg_buf->buffer, len);
            }

            if ( (encoded + failed) % CONFIG_VIDEO_CAPTURE_FPS == 0U && encoded > 0U )
            {
                LOG_INF("%u encoded, %u missed, %u failed; %u ms; avg encode %u us, worst %u us; "
                        "last JPEG %u bytes at %p; avg %u bytes",
                        encoded, missed, failed, (uint32_t)(k_uptime_get() - start),
                        (uint32_t)(encode_total_us / encoded), encode_worst_us, (uint32_t)len,
                        jpeg_buf->buffer, bytes / encoded);
            }

            if ( k_uptime_get() - start >= CONFIG_VIDEO_CAPTURE_DURATION_S * 1000LL )
            {
                break;
            }
        }

        ret = video_enqueue(camera_dev, frame);
        if ( ret < 0 )
        {
            LOG_ERR("Unable to recycle a capture buffer (err %d)", ret);
            return ret;
        }
    }

    ret = video_stream_stop(camera_dev, VIDEO_BUF_TYPE_OUTPUT);
    if ( ret < 0 )
    {
        LOG_WRN("Unable to stop the camera (err %d)", ret);
    }

    LOG_INF("Stopped: %u encoded, %u missed, %u failed, last JPEG %u bytes at %p", encoded, missed,
            failed, (uint32_t)len, jpeg_buf->buffer);

    return 0;
}

//*****************************************************************************
//
// End Doxygen group.
//! @}
//
//*****************************************************************************
