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
//! @brief Capture one still from the camera and leave it in RAM for the host.
//!
//! @addtogroup payload_examples Payload Examples
//
//! @defgroup still_capture Still Capture Example
//! @ingroup payload_examples
//! @{
//!
//! Purpose: This example captures a single still from the "zephyr,camera"
//! device, encodes it as JPEG with the "zephyr,videoenc" device, and leaves
//! it in RAM, described by still_capture_result, for download.py to read
//! out over SWD.
//!
//! This example requires 4096 bytes of main stack.<br>
//! The stack size is set by CONFIG_MAIN_STACK_SIZE in prj.conf.<br>
//!
//! @section still_capture_features Key Features
//!
//! 1. @b Camera @b Capture: Captures an NV12 frame through the Zephyr video
//!    API.
//!
//! 2. @b Sensor @b Warm-up: Discards the first frames after the stream
//!    starts, which are unusable while the sensor settles
//!
//! 3. @b JPEG @b Encoding: Encodes the frame with the hardware JPEG encoder,
//!    so the picture comes off ready to view
//!
//! 4. @b SWD @b Readout: Publishes the picture through a descriptor the host
//!    finds by name, with a CRC32 to check the bytes it reads
//!
//! @section still_capture_usage Usage
//!
//! 1. Plug the B-CAMS-IMX into the CSI camera connector and a J-Link into the
//!    board
//! 2. Build with west build -p always
//! 3. Load and start the image with scripts/jlink.py load
//! 4. Run download.py to retrieve and save the picture
//! 5. Monitor the RTT log for the capture details or the cause of a failure
//!
//! @section still_capture_configuration Configuration
//!
//! - @b CONFIG_STILL_CAPTURE_WIDTH, @b CONFIG_STILL_CAPTURE_HEIGHT: Picture
//!   size in pixels (default: 1920x1440 on the STM32N6570-DK, 640x480 on the
//!   NUCLEO-N657X0-Q)
//! - @b CONFIG_STILL_CAPTURE_NUM_BUFS: Capture buffers (default: 2)
//! - @b CONFIG_STILL_CAPTURE_CAMERA_POWER_UP_MS: Wait at boot for the camera
//!   module to power up (default: 100)
//! - @b CONFIG_STILL_CAPTURE_WARMUP_FRAMES: Frames discarded before the
//!   picture is kept (default: 8)
//! - @b CONFIG_STILL_CAPTURE_EXPOSURE, @b CONFIG_STILL_CAPTURE_GAIN: Sensor
//!   overrides (default: -1, keep the driver's value)
//! - @b CONFIG_STILL_CAPTURE_JPEG_QUALITY: JPEG quality (default: 100)
//
//*****************************************************************************

//
// Zephyr kernel and drivers
//
#include <zephyr/kernel.h>          // Kernel services and timeouts
#include <zephyr/init.h>            // SYS_INIT() for the camera power-up wait
#include <zephyr/cache.h>           // Data cache flush and invalidate
#include <zephyr/drivers/video.h>   // Video capture and encoder API
#include <zephyr/logging/log.h>     // Logging over the RTT console

//
// Zephyr utilities
//
#include <zephyr/sys/crc.h>         // crc32_ieee() for the host's check

//*****************************************************************************
//
// Build-time checks.
//
//*****************************************************************************

#ifdef CONFIG_DCACHE
BUILD_ASSERT(CONFIG_VIDEO_BUFFER_POOL_ALIGN % CONFIG_DCACHE_LINE_SIZE == 0,
             "Video buffers are invalidated whole, so they must not share a cache line");
#endif

BUILD_ASSERT(CONFIG_STILL_CAPTURE_NUM_BUFS + 1 <= CONFIG_VIDEO_BUFFER_POOL_NUM_MAX,
             "The video buffer pool needs a slot per capture buffer plus one for the JPEG");

BUILD_ASSERT(CONFIG_STILL_CAPTURE_WIDTH % 16 == 0 && CONFIG_STILL_CAPTURE_HEIGHT % 16 == 0,
             "The JPEG encoder works in 16x16 blocks");

//*****************************************************************************
//
// Globals.
//
//*****************************************************************************

//
// Log module for this example.
//
LOG_MODULE_REGISTER(still_capture);

//
// Magic for capture result
//
#define STILL_CAPTURE_MAGIC     0x4349505aU

//
// Picture descriptor.
//
volatile struct still_capture_result
{
    uint32_t magic;     // STILL_CAPTURE_MAGIC once the rest is valid
    uint32_t width;     // Width in pixels
    uint32_t height;    // Height in pixels
    uint32_t length;    // Size of the JPEG in bytes
    uint32_t crc32;     // crc32_ieee() of the JPEG
    uint32_t address;   // Address of the JPEG in target RAM
} still_capture_result;

//
// The camera and the JPEG encoder devices
//
static const struct device *const camera_dev =
    DEVICE_DT_GET(DT_CHOSEN(zephyr_camera));
static const struct device *const encoder_dev =
    DEVICE_DT_GET(DT_CHOSEN(zephyr_videoenc));

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
    k_msleep(CONFIG_STILL_CAPTURE_CAMERA_POWER_UP_MS);
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
// Capture one frame from the camera.
//
// Throws away the first frames, which are unusable while the sensor settles,
// and keeps the next. On success *out is the kept frame, invalidated from the
// data cache so the CPU reads what the DCMIPP wrote.
//
// Returns 0 on success, or a negative errno otherwise.
//
//*****************************************************************************
static int
capture(const struct video_format *fmt, struct video_buffer **out)
{
    struct video_buffer *vbuf;
    int ret;

    //
    // Allocate and enqueue buffers to the camera
    //
    for ( unsigned int i = 0; i < CONFIG_STILL_CAPTURE_NUM_BUFS; i++ )
    {
        vbuf = video_buffer_aligned_alloc(ROUND_UP(fmt->size, CONFIG_VIDEO_BUFFER_POOL_ALIGN),
                                          CONFIG_VIDEO_BUFFER_POOL_ALIGN, K_NO_WAIT);
        if ( vbuf == NULL )
        {
            LOG_ERR("Out of video buffer pool: %u bytes x %u needed, "
                    "raise CONFIG_VIDEO_BUFFER_POOL_HEAP_SIZE",
                    fmt->size, CONFIG_STILL_CAPTURE_NUM_BUFS);
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

    //
    // Start streaming.
    //
    ret = video_stream_start(camera_dev, VIDEO_BUF_TYPE_OUTPUT);
    if ( ret < 0 )
    {
        LOG_ERR("Unable to start the camera (err %d)", ret);
        return ret;
    }

    //
    // Recycle each frame until the warm-up frames are gone, and keep the one
    // after them.
    //
    for ( unsigned int i = 0; ; i++ )
    {
        ret = video_dequeue(camera_dev, &vbuf, K_SECONDS(2));
        if ( ret < 0 )
        {
            LOG_ERR("No frame after %u warm-up frames (err %d)", i, ret);
            return ret;
        }

        if ( i == CONFIG_STILL_CAPTURE_WARMUP_FRAMES )
        {
            break;
        }

        ret = video_enqueue(camera_dev, vbuf);
        if ( ret < 0 )
        {
            LOG_ERR("Unable to recycle a capture buffer (err %d)", ret);
            return ret;
        }
    }

    //
    // The kept frame is ours now, but the other buffers are still being filled.
    //
    ret = video_stream_stop(camera_dev, VIDEO_BUF_TYPE_OUTPUT);
    if ( ret < 0 )
    {
        LOG_WRN("Unable to stop the camera (err %d)", ret);
    }

    //
    // Invalidate any portion of the vbuf in cache
    //
    sys_cache_data_invd_range(vbuf->buffer, vbuf->size);

    *out = vbuf;
    return 0;
}

//*****************************************************************************
//
// Replace the frame in *vbuf, described by fmt, with its JPEG encoding.
//
//*****************************************************************************
static int
encode(struct video_format *fmt, struct video_buffer **vbuf)
{
    struct video_buffer *in_buf = *vbuf;
    struct video_buffer *out_buf;
    int ret;

    if ( !device_is_ready(encoder_dev) )
    {
        LOG_ERR("%s is not ready", encoder_dev->name);
        return -ENODEV;
    }

    //
    // Tell the encoder what it is given, then what to produce.
    //
    fmt->type = VIDEO_BUF_TYPE_INPUT;
    ret = video_set_format(encoder_dev, fmt);
    if ( ret < 0 )
    {
        LOG_ERR("Could not set encoder input format %ux%u (err %d)", fmt->width, fmt->height,
                ret);
        return ret;
    }

    fmt->type = VIDEO_BUF_TYPE_OUTPUT;
    fmt->pixelformat = VIDEO_PIX_FMT_JPEG;
    ret = video_set_format(encoder_dev, fmt);
    if ( ret < 0 )
    {
        LOG_ERR("Could not set encoder output format (err %d)", ret);
        return ret;
    }

    set_ctrl(encoder_dev, VIDEO_CID_JPEG_COMPRESSION_QUALITY,
             CONFIG_STILL_CAPTURE_JPEG_QUALITY, "JPEG quality");

    //
    // Allocate the JPEG buffer.
    //
    out_buf = video_buffer_aligned_alloc(ROUND_UP(fmt->size, CONFIG_VIDEO_BUFFER_POOL_ALIGN),
                                         CONFIG_VIDEO_BUFFER_POOL_ALIGN, K_NO_WAIT);
    if ( out_buf == NULL )
    {
        LOG_ERR("Out of video buffer pool: %u bytes needed for the JPEG, "
                "raise CONFIG_VIDEO_BUFFER_POOL_HEAP_SIZE", fmt->size);
        return -ENOMEM;
    }

    //
    // Start both sides of the encoder.
    //
    ret = video_stream_start(encoder_dev, VIDEO_BUF_TYPE_INPUT);
    if ( ret < 0 )
    {
        LOG_ERR("Unable to start the encoder input (err %d)", ret);
        return ret;
    }

    ret = video_stream_start(encoder_dev, VIDEO_BUF_TYPE_OUTPUT);
    if ( ret < 0 )
    {
        LOG_ERR("Unable to start the encoder output (err %d)", ret);
        return ret;
    }

    //
    // Queue the empty buffer as output, then the frame buffer as input, which
    // starts the encode.
    //
    out_buf->type = VIDEO_BUF_TYPE_OUTPUT;
    ret = video_enqueue(encoder_dev, out_buf);
    if ( ret < 0 )
    {
        LOG_ERR("Unable to enqueue the output buffer (err %d)", ret);
        return ret;
    }

    in_buf->type = VIDEO_BUF_TYPE_INPUT;
    ret = video_enqueue(encoder_dev, in_buf);
    if ( ret < 0 )
    {
        LOG_ERR("Unable to enqueue the frame buffer for encoding (err %d)", ret);
        return ret;
    }

    //
    // Wait for the encoder to hand back the frame and the JPEG.
    //
    ret = video_dequeue(encoder_dev, &in_buf, K_SECONDS(5));
    if ( ret < 0 )
    {
        LOG_ERR("Encoder did not release the input frame buffer (err %d)", ret);
        return ret;
    }

    ret = video_dequeue(encoder_dev, &out_buf, K_SECONDS(5));
    if ( ret < 0 )
    {
        LOG_ERR("Encoder did not release the output JPEG buffer (err %d)", ret);
        return ret;
    }

    ret = video_stream_stop(encoder_dev, VIDEO_BUF_TYPE_OUTPUT);
    if ( ret < 0 )
    {
        LOG_WRN("Unable to stop the encoder output (err %d)", ret);
    }

    ret = video_stream_stop(encoder_dev, VIDEO_BUF_TYPE_INPUT);
    if ( ret < 0 )
    {
        LOG_WRN("Unable to stop the encoder input (err %d)", ret);
    }

    //
    // The driver writes the JPEG with the CPU, so flush it out of the data
    // cache to RAM, where the debugger reads it.
    //
    sys_cache_data_flush_range(out_buf->buffer, out_buf->size);

    *vbuf = out_buf;
    return 0;
}

//*****************************************************************************
//
// Fills in still_capture_result and flushes it out to RAM, where the debugger
// will read it. encode() has already flushed the picture itself.
//
//*****************************************************************************
static void
publish(const struct video_format *fmt, const struct video_buffer *vbuf)
{
    uint32_t crc;
    crc = crc32_ieee(vbuf->buffer, vbuf->bytesused);

    still_capture_result.width = fmt->width;
    still_capture_result.height = fmt->height;
    still_capture_result.address = (uint32_t) (uintptr_t) vbuf->buffer;
    still_capture_result.length = vbuf->bytesused;
    still_capture_result.crc32 = crc;
    still_capture_result.magic = STILL_CAPTURE_MAGIC;

    sys_cache_data_flush_range((void *) &still_capture_result, sizeof(still_capture_result));

    LOG_INF("Picture ready: JPEG %ux%u, %u bytes at 0x%08x, crc32 %08x", fmt->width,
            fmt->height, vbuf->bytesused, still_capture_result.address, crc);
}

//*****************************************************************************
//
// Main.
//
//*****************************************************************************
int
main(void)
{
    struct video_format fmt;    // Format of the picture, as captured then encoded
    struct video_buffer *vbuf;  // Buffer holding the picture
    int ret;                    // Status return for video API calls

    LOG_INF("Still capture example!");

    //
    // ========================================================================
    // STEP 1: Select the Capture Format
    // ========================================================================
    //
    // Check that the camera is ready and ask for NV12 at the configured
    // width and height.
    //

    if ( !device_is_ready(camera_dev) )
    {
        LOG_ERR("%s is not ready", camera_dev->name);
        ret = -ENODEV;
        goto err;
    }

    fmt = (struct video_format) {
        .type = VIDEO_BUF_TYPE_OUTPUT,
        .pixelformat = VIDEO_PIX_FMT_NV12,
        .width = CONFIG_STILL_CAPTURE_WIDTH,
        .height = CONFIG_STILL_CAPTURE_HEIGHT,
    };

    ret = video_set_compose_format(camera_dev, &fmt);
    if ( ret < 0 )
    {
        LOG_ERR("Unable to set compose format NV12 %ux%u (err %d)", CONFIG_STILL_CAPTURE_WIDTH,
                CONFIG_STILL_CAPTURE_HEIGHT, ret);
        goto err;
    }

    LOG_INF("Capturing NV12 %ux%u, %u bytes per frame", fmt.width, fmt.height, fmt.size);

    //
    // ========================================================================
    // STEP 2: Apply the Sensor Controls
    // ========================================================================
    //
    // Apply any exposure and gain overrides from Kconfig.
    //
    if ( CONFIG_STILL_CAPTURE_EXPOSURE >= 0 )
    {
        set_ctrl(camera_dev, VIDEO_CID_EXPOSURE, CONFIG_STILL_CAPTURE_EXPOSURE, "exposure");
    }

    if ( CONFIG_STILL_CAPTURE_GAIN >= 0 )
    {
        set_ctrl(camera_dev, VIDEO_CID_ANALOGUE_GAIN, CONFIG_STILL_CAPTURE_GAIN, "gain");
    }

    //
    // ========================================================================
    // STEP 3: Capture the Frame
    // ========================================================================
    //
    // Start the stream, discard the warm-up frames, and keep the next one.
    //
    ret = capture(&fmt, &vbuf);
    if ( ret < 0 )
    {
        goto err;
    }

    //
    // ========================================================================
    // STEP 4: Encode the Frame as JPEG
    // ========================================================================
    //
    ret = encode(&fmt, &vbuf);
    if ( ret < 0 )
    {
        goto err;
    }

    //
    // ========================================================================
    // STEP 5: Publish the Picture
    // ========================================================================
    //
    // Fill in still_capture_result for download.py, which polls it until
    // the magic word appears.
    //
    publish(&fmt, vbuf);

    //
    // Nothing reuses the buffer after main returns, so the picture stays put
    // for the host.
    //
    return 0;

err:

    //
    // Every failure above has already logged its cause.
    //
    LOG_ERR("No picture taken (err %d)", ret);
    return ret;
}

//*****************************************************************************
//
// End Doxygen group.
//! @}
//
//*****************************************************************************
