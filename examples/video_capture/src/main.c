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
//! @brief Record MJPEG video from the camera to the microSD card.
//!
//! @addtogroup payload_examples Payload Examples
//
//! @defgroup video_capture Video Capture Example
//! @ingroup payload_examples
//! @{
//!
//! Purpose: This example records video from the "zephyr,camera" device to the
//! microSD card as soon as it starts. Each NV12 frame is encoded as a JPEG in
//! the main thread, then waits in PSRAM for a lower priority thread to write it
//! to a new file on the card.
//!
//! This example requires 4096 bytes of main stack.<br>
//! The stack size is set by CONFIG_MAIN_STACK_SIZE in prj.conf.<br>
//!
//! @section video_capture_features Key Features
//!
//! 1. @b Camera @b Capture: Streams NV12 frames through the Zephyr video API,
//!    cropping the sensor image so that scaling it does not squeeze the picture
//!
//! 2. @b JPEG @b Encoding: Encodes each frame over DMA with the hardware JPEG
//!    codec, which is the job of jpeg_dma.c
//!
//! 3. @b Write @b Buffering: Holds encoded frames in PSRAM, so that recording
//!    carries on through the write stalls of the SD card
//!
//! 4. @b Dropped @b Frames: Counts each frame that is lost and carries on,
//!    rather than ending the recording
//!
//! 5. @b Safe @b Storage: Writes each recording to the next unused file name,
//!    and never formats the card
//!
//! @section video_capture_usage Usage
//!
//! 1. Plug the B-CAMS-IMX into the CSI camera connector, a FAT formatted card
//!    into the microSD slot, and a J-Link into the board
//! 2. Build with west build -p always
//! 3. Load and start the image with scripts/jlink.py load
//! 4. Monitor the RTT log for the recording's progress and final statistics
//! 5. Play the VIDnnnnn.MJP file from the card on a computer. See the README.
//!
//! @section video_capture_configuration Configuration
//!
//! - @b CONFIG_VIDEO_CAPTURE_WIDTH, @b CONFIG_VIDEO_CAPTURE_HEIGHT: Video size
//!   in pixels (default: 1440x1088)
//! - @b CONFIG_VIDEO_CAPTURE_FPS: Frames per second (default: 30)
//! - @b CONFIG_VIDEO_CAPTURE_DURATION_S: Length of the recording (default: 30)
//! - @b CONFIG_VIDEO_CAPTURE_JPEG_QUALITY: JPEG quality (default: 50)
//! - @b CONFIG_VIDEO_CAPTURE_NUM_BUFS: Capture buffers (default: 4)
//! - @b CONFIG_VIDEO_CAPTURE_NUM_JPEG_BUFS: Encoded frames that can wait for the
//!   card (default: 64)
//! - @b CONFIG_VIDEO_CAPTURE_JPEG_BUF_SIZE: Largest encoded frame (default: 256 kB)
//! - @b CONFIG_VIDEO_CAPTURE_CAMERA_POWER_UP_MS: Wait at boot for the camera
//!   module to power up (default: 100)
//! - @b CONFIG_VIDEO_CAPTURE_WARMUP_FRAMES: Frames discarded before recording
//!   (default: 8)
//! - @b CONFIG_VIDEO_CAPTURE_EXPOSURE, @b CONFIG_VIDEO_CAPTURE_GAIN: Sensor
//!   overrides (default: -1, keep the driver's value)
//
//*****************************************************************************

//
// Standard C library
//
#include <stdio.h>                  // snprintf() for the file name
#include <string.h>                 // memset() for the padding

//
// Zephyr kernel and drivers
//
#include <zephyr/kernel.h>          // Threads, message queues and timeouts
#include <zephyr/init.h>            // SYS_INIT() for the hardware set-up at boot
#include <zephyr/cache.h>           // Data cache flush and invalidate
#include <zephyr/drivers/video.h>   // Video capture API
#include <zephyr/video/video.h>     // Video buffers and compose format
#include <zephyr/fs/fs.h>           // File system API
#include <zephyr/logging/log.h>     // Logging over the RTT console

//
// FatFs
//
#include <ff.h>                     // FATFS, the volume state for fs_mount()

//
// STM32Cube HAL
//
#include <soc.h>                    // HAL RIF driver

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
// Constants.
//
//*****************************************************************************

//
// Time between frames, in ms.
//
#define FRAME_PERIOD_MS         (1000U / CONFIG_VIDEO_CAPTURE_FPS)

//
// FatFs passes whole sectors straight to the SD driver and copies anything else
// through a small buffer. Each frame is padded to a whole number of sectors so
// that it always takes the fast path, and the SD card's DMA, which needs 4 byte
// aligned addresses, always sees an aligned one.
//
#define SECTOR_BYTES            512U

BUILD_ASSERT(CONFIG_VIDEO_CAPTURE_JPEG_BUF_SIZE % SECTOR_BYTES == 0,
             "JPEG buffers must have room for the padding at the end of a frame");

BUILD_ASSERT(CONFIG_VIDEO_CAPTURE_JPEG_BUF_SIZE % JPEG_DMA_OUT_MULTIPLE == 0,
             "The encoder can only use JPEG buffers of a whole number of its DMA blocks");

//
// Players look for the start of the next image, and skip anything before it.
// 0xFF is also the fill byte that JPEG allows between segments.
//
#define PADDING_BYTE            0xFFU

//
// The largest file FAT32 allows.
//
#define MAX_FILE_BYTES          0xFFFFFFFFU

//
// Mount point, which FatFs requires to match the devicetree's "disk-name".
//
#define DISK_NAME               "SD"
#define MOUNT_POINT             "/" DISK_NAME ":"
#define MAX_FILE_NUMBER         99999U

//
// The writer spends most of its time polling the card for the end of a write,
// so it must run at a lower priority than the main thread.
//
#define WRITER_STACK_SIZE       4096
#define WRITER_PRIORITY         K_PRIO_PREEMPT(5)

//*****************************************************************************
//
// Globals.
//
//*****************************************************************************

LOG_MODULE_REGISTER(video_capture);

//
// The camera device.
//
static const struct device *const camera_dev = DEVICE_DT_GET(DT_CHOSEN(zephyr_camera));

//
// The file system and the recording.
//
static FATFS fat_fs;
static struct fs_mount_t mount = {
    .type = FS_FATFS,
    .fs_data = &fat_fs,
    .storage_dev = (void *) DISK_NAME,
    .mnt_point = MOUNT_POINT,
    .flags = FS_MOUNT_FLAG_NO_FORMAT,
};
static struct fs_file_t file;
static char file_name[sizeof(MOUNT_POINT "/VID00000.MJP")];

//
// JPEG buffers go around between these two queues. A NULL in the write queue
// tells the writer that the recording is over, hence the extra slot.
//
K_MSGQ_DEFINE(free_jpegs, sizeof(struct video_buffer *), CONFIG_VIDEO_CAPTURE_NUM_JPEG_BUFS,
              sizeof(void *));
K_MSGQ_DEFINE(write_queue, sizeof(struct video_buffer *), CONFIG_VIDEO_CAPTURE_NUM_JPEG_BUFS + 1,
              sizeof(void *));

//
// The writer thread.
//
K_THREAD_STACK_DEFINE(writer_stack, WRITER_STACK_SIZE);
static struct k_thread writer;

//
// Statistics. The main thread owns the first group and the writer the second.
//
static struct
{
    uint32_t encoded;           // Frames encoded and queued for the card
    uint32_t queued_bytes;      // Bytes queued for the card
    uint32_t missed;            // Frames the camera lost with no capture buffer
    uint32_t no_buffer;         // Frames dropped with no JPEG buffer free
    uint32_t too_large;         // Frames dropped for not fitting a JPEG buffer
    uint32_t failed;            // Frames dropped because the encoder failed
    uint64_t encode_total_us;   // Time spent encoding the frames
    uint32_t encode_worst_us;   // The longest time taken to encode a frame
    uint32_t queue_peak;        // The most frames that were waiting for the card

    volatile uint32_t written;          // Frames written to the card
    volatile uint32_t written_bytes;    // Bytes written to the card
    volatile uint32_t write_worst_us;   // The longest time taken to write a frame
    volatile int write_error;           // Negative errno once a write has failed
} stats;

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
// Let the SD card's internal DMA reach memory.
//
// Zephyr gives the secure bus masters a compartment ID at boot, and the memory
// only accepts accesses from it. Its list does not include SDMMC2.
//
//*****************************************************************************
static int
sdmmc_rif_config(void)
{
    const RIMC_MasterConfig_t master = {
        .MasterCID = RIF_CID_1,
        .SecPriv = RIF_ATTRIBUTE_SEC | RIF_ATTRIBUTE_PRIV,
    };

    HAL_RIF_RIMC_ConfigMasterAttributes(RIF_MASTER_INDEX_SDMMC2, &master);
    HAL_RIF_RISC_SetSlaveSecureAttributes(RIF_RISC_PERIPH_INDEX_SDMMC2,
                                          RIF_ATTRIBUTE_SEC | RIF_ATTRIBUTE_PRIV);
    return 0;
}

SYS_INIT(sdmmc_rif_config, PRE_KERNEL_1, 0);

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
// Mount the card, and create the next unused file on it.
//
//*****************************************************************************
static int
open_recording(void)
{
    struct fs_dirent entry;
    int ret;

    ret = fs_mount(&mount);
    if ( ret < 0 )
    {
        LOG_ERR("Unable to mount the SD card. Is one inserted, and FAT formatted? (err %d)", ret);
        return ret;
    }

    for ( uint32_t number = 1; number <= MAX_FILE_NUMBER; number++ )
    {
        snprintf(file_name, sizeof(file_name), MOUNT_POINT "/VID%05u.MJP", number);

        ret = fs_stat(file_name, &entry);
        if ( ret == -ENOENT )
        {
            fs_file_t_init(&file);
            ret = fs_open(&file, file_name, FS_O_CREATE | FS_O_WRITE);
            if ( ret < 0 )
            {
                LOG_ERR("Unable to create %s (err %d)", file_name, ret);
            }
            return ret;
        }

        if ( ret < 0 )
        {
            LOG_ERR("Unable to check for %s (err %d)", file_name, ret);
            return ret;
        }
    }

    LOG_ERR("Every file name is already taken");
    return -EEXIST;
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

    if ( !device_is_ready(camera_dev) )
    {
        LOG_ERR("%s is not ready", camera_dev->name);
        return -ENODEV;
    }

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

    LOG_INF("Capturing NV12 %ux%u from a %ux%u crop at %u fps, %u bytes per frame", fmt->width,
            fmt->height, crop_width, crop_height, frame_interval.denominator, fmt->size);

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
// Allocate the JPEG buffers and the capture buffers, and give the latter to the
// camera.
//
//*****************************************************************************
static int
allocate_buffers(const struct video_format *fmt)
{
    struct video_buffer *vbuf;
    int ret;

    for ( unsigned int i = 0; i < CONFIG_VIDEO_CAPTURE_NUM_JPEG_BUFS; i++ )
    {
        vbuf = alloc_buffer(CONFIG_VIDEO_CAPTURE_JPEG_BUF_SIZE);
        if ( vbuf == NULL )
        {
            return -ENOMEM;
        }

        k_msgq_put(&free_jpegs, &vbuf, K_NO_WAIT);
    }

    for ( unsigned int i = 0; i < CONFIG_VIDEO_CAPTURE_NUM_BUFS; i++ )
    {
        vbuf = alloc_buffer(fmt->size);
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

    return 0;
}

//*****************************************************************************
//
// Writer thread: move encoded frames from the queue to the card.
//
// If a write fails, this carries on taking frames from the queue without
// writing them, so that the main thread can shut down normally.
//
//*****************************************************************************
static void
writer_thread(void *arg1, void *arg2, void *arg3)
{
    struct video_buffer *jpeg;

    ARG_UNUSED(arg1);
    ARG_UNUSED(arg2);
    ARG_UNUSED(arg3);

    while ( true )
    {
        k_msgq_get(&write_queue, &jpeg, K_FOREVER);
        if ( jpeg == NULL )
        {
            return;
        }

        if ( stats.write_error == 0 )
        {
            uint32_t start = k_cycle_get_32();
            ssize_t written = fs_write(&file, jpeg->buffer, jpeg->bytesused);
            uint32_t elapsed_us = k_cyc_to_us_floor32(k_cycle_get_32() - start);

            if ( written == jpeg->bytesused )
            {
                stats.written++;
                stats.written_bytes += written;
                stats.write_worst_us = MAX(stats.write_worst_us, elapsed_us);
            }
            else
            {
                stats.write_error = (written < 0) ? (int)written : -ENOSPC;
            }
        }

        k_msgq_put(&free_jpegs, &jpeg, K_NO_WAIT);
    }
}

//*****************************************************************************
//
// Encode one frame, and queue it to be written to the card.
//
// A frame that cannot be kept for any reason is counted and dropped.
//
//*****************************************************************************
static void
record_frame(struct video_buffer *frame)
{
    struct video_buffer *jpeg;
    size_t length;
    size_t padded;
    uint32_t start;
    uint32_t elapsed_us;
    int ret;

    if ( k_msgq_get(&free_jpegs, &jpeg, K_NO_WAIT) != 0 )
    {
        stats.no_buffer++;
        return;
    }

    start = k_cycle_get_32();
    sys_cache_data_invd_range(frame->buffer, frame->size);
    ret = jpeg_dma_encode(frame->buffer, jpeg->buffer, jpeg->size, &length);
    elapsed_us = k_cyc_to_us_floor32(k_cycle_get_32() - start);

    if ( ret < 0 )
    {
        if ( ret == -ENOSPC )
        {
            stats.too_large++;
        }
        else
        {
            stats.failed++;
            LOG_WRN("Unable to encode a frame (err %d)", ret);
        }

        k_msgq_put(&free_jpegs, &jpeg, K_NO_WAIT);
        return;
    }

    padded = ROUND_UP(length, SECTOR_BYTES);
    memset(jpeg->buffer + length, PADDING_BYTE, padded - length);

    //
    // The SD card's DMA reads this from memory, not from the cache. Nothing is
    // left in the cache, so that it cannot go stale for the next frame in this
    // buffer.
    //
    sys_cache_data_flush_and_invd_range(jpeg->buffer, padded);

    jpeg->bytesused = padded;

    stats.encoded++;
    stats.queued_bytes += padded;
    stats.encode_total_us += elapsed_us;
    stats.encode_worst_us = MAX(stats.encode_worst_us, elapsed_us);

    k_msgq_put(&write_queue, &jpeg, K_NO_WAIT);
    stats.queue_peak = MAX(stats.queue_peak, k_msgq_num_used_get(&write_queue));
}

//*****************************************************************************
//
// Report the progress of the recording over the last second.
//
//*****************************************************************************
static void
report_progress(uint32_t seconds)
{
    static uint32_t last_written_bytes;
    uint32_t written_bytes = stats.written_bytes;

    LOG_INF("%3u s: %u encoded, %u written, %u kB/s, %u of %u buffers queued, dropped %u",
            seconds, stats.encoded, stats.written, (written_bytes - last_written_bytes) / 1000U,
            k_msgq_num_used_get(&write_queue), CONFIG_VIDEO_CAPTURE_NUM_JPEG_BUFS,
            stats.missed + stats.no_buffer + stats.too_large + stats.failed);

    last_written_bytes = written_bytes;
}

//*****************************************************************************
//
// Stream from the camera, discard the warm-up frames, and record the rest until
// the configured time is up, the file is full, or the card fails.
//
//*****************************************************************************
static int
record(int64_t *elapsed_ms)
{
    const int64_t duration_ms = CONFIG_VIDEO_CAPTURE_DURATION_S * 1000LL;
    struct video_buffer *frame;
    uint32_t last_timestamp = 0;
    uint32_t last_report_s = 0;
    int64_t start = 0;
    int ret;

    ret = video_stream_start(camera_dev, VIDEO_BUF_TYPE_OUTPUT);
    if ( ret < 0 )
    {
        LOG_ERR("Unable to start the camera (err %d)", ret);
        return ret;
    }

    for ( uint32_t count = 0; ; count++ )
    {
        ret = video_dequeue(camera_dev, &frame, K_SECONDS(2));
        if ( ret < 0 )
        {
            LOG_ERR("No frame after %u frames (err %d)", count, ret);
            return ret;
        }

        if ( count >= CONFIG_VIDEO_CAPTURE_WARMUP_FRAMES )
        {
            if ( count == CONFIG_VIDEO_CAPTURE_WARMUP_FRAMES )
            {
                start = k_uptime_get();
            }
            else
            {
                //
                // The camera loses frames without a word when it has no free
                // capture buffer, which shows up as a gap in the timestamps.
                //
                uint32_t gap = (frame->timestamp - last_timestamp + (FRAME_PERIOD_MS / 2U)) /
                               FRAME_PERIOD_MS;

                stats.missed += (gap > 1U) ? (gap - 1U) : 0U;
            }

            record_frame(frame);
        }

        last_timestamp = frame->timestamp;

        ret = video_enqueue(camera_dev, frame);
        if ( ret < 0 )
        {
            LOG_ERR("Unable to recycle a capture buffer (err %d)", ret);
            return ret;
        }

        if ( start == 0 )
        {
            continue;
        }

        *elapsed_ms = k_uptime_get() - start;

        if ( stats.write_error != 0 )
        {
            LOG_ERR("Stopping, as the card failed a write (err %d)", stats.write_error);
            return stats.write_error;
        }

        if ( *elapsed_ms >= duration_ms )
        {
            return 0;
        }

        //
        // Leave room for what can still be queued as the recording stops.
        //
        if ( stats.queued_bytes > MAX_FILE_BYTES - (2U * CONFIG_VIDEO_CAPTURE_JPEG_BUF_SIZE) )
        {
            LOG_WRN("Stopping, as the file is at the FAT32 size limit");
            return 0;
        }

        if ( (uint32_t)(*elapsed_ms / 1000) > last_report_s )
        {
            last_report_s = (uint32_t)(*elapsed_ms / 1000);
            report_progress(last_report_s);
        }
    }
}

//*****************************************************************************
//
// Report how the recording went.
//
//*****************************************************************************
static void
report_summary(int64_t elapsed_ms)
{
    uint32_t centi_fps = 0;

    if ( elapsed_ms > 0 )
    {
        centi_fps = (uint32_t)(stats.written * 100000ULL / (uint64_t)elapsed_ms);
    }

    LOG_INF("Recorded %s: %u frames, %u bytes in %u ms, %u.%02u fps", file_name, stats.written,
            stats.written_bytes, (uint32_t)elapsed_ms, centi_fps / 100U, centi_fps % 100U);

    LOG_INF("Dropped %u frames: %u missed by the camera, %u with no JPEG buffer free, "
            "%u too large, %u failed to encode",
            stats.missed + stats.no_buffer + stats.too_large + stats.failed, stats.missed,
            stats.no_buffer, stats.too_large, stats.failed);

    if ( stats.encoded > 0U )
    {
        LOG_INF("Encoding took %u us per frame on average, %u us at worst",
                (uint32_t)(stats.encode_total_us / stats.encoded), stats.encode_worst_us);
    }

    LOG_INF("Writing took %u us at worst, with up to %u of %u buffers queued",
            stats.write_worst_us, stats.queue_peak, CONFIG_VIDEO_CAPTURE_NUM_JPEG_BUFS);
}

//*****************************************************************************
//
// Main.
//
//*****************************************************************************
int
main(void)
{
    struct video_format fmt;            // Format of the captured frames
    struct video_buffer *end = NULL;    // Tells the writer the recording is over
    int64_t elapsed_ms = 0;             // Length of the recording
    int ret;                            // Status return for API calls
    int close_ret;                      // Status return for closing the file

    LOG_INF("Video capture example!");

    //
    // ========================================================================
    // STEP 1: Open the Recording
    // ========================================================================
    //
    // Mount the card and create the next unused file. Nothing else is worth
    // doing if this fails.
    //
    ret = open_recording();
    if ( ret < 0 )
    {
        goto err;
    }

    LOG_INF("Recording %u s to %s", CONFIG_VIDEO_CAPTURE_DURATION_S, file_name);

    //
    // ========================================================================
    // STEP 2: Prepare the Camera and Encoder
    // ========================================================================
    //
    ret = configure_camera(&fmt);
    if ( ret < 0 )
    {
        goto err_close;
    }

    ret = jpeg_dma_init(CONFIG_VIDEO_CAPTURE_JPEG_QUALITY);
    if ( ret < 0 )
    {
        LOG_ERR("Unable to start the JPEG encoder (err %d)", ret);
        goto err_close;
    }

    ret = allocate_buffers(&fmt);
    if ( ret < 0 )
    {
        goto err_close;
    }

    //
    // ========================================================================
    // STEP 3: Record
    // ========================================================================
    //
    // The main thread encodes frames, and the writer thread saves them.
    //
    k_thread_create(&writer, writer_stack, K_THREAD_STACK_SIZEOF(writer_stack), writer_thread,
                    NULL, NULL, NULL, WRITER_PRIORITY, 0, K_NO_WAIT);
    k_thread_name_set(&writer, "writer");

    ret = record(&elapsed_ms);

    //
    // ========================================================================
    // STEP 4: Finish the Recording
    // ========================================================================
    //
    // Stop the camera, wait for the frames still in the queue to reach the card,
    // and close the file. The file is only complete once this has finished.
    //
    if ( video_stream_stop(camera_dev, VIDEO_BUF_TYPE_OUTPUT) < 0 )
    {
        LOG_WRN("Unable to stop the camera");
    }

    k_msgq_put(&write_queue, &end, K_FOREVER);
    k_thread_join(&writer, K_FOREVER);

    report_summary(elapsed_ms);

err_close:

    close_ret = fs_close(&file);
    if ( close_ret < 0 )
    {
        LOG_ERR("Unable to close %s (err %d)", file_name, close_ret);
        ret = (ret < 0) ? ret : close_ret;
    }

    //
    // A recording that never got going leaves nothing worth keeping.
    //
    if ( stats.written == 0U )
    {
        close_ret = fs_unlink(file_name);
        if ( close_ret < 0 )
        {
            LOG_WRN("Unable to remove the empty %s (err %d)", file_name, close_ret);
        }
    }

    close_ret = fs_unmount(&mount);
    if ( close_ret < 0 )
    {
        LOG_ERR("Unable to unmount the SD card (err %d)", close_ret);
        ret = (ret < 0) ? ret : close_ret;
    }

    if ( ret < 0 )
    {
        goto err;
    }

    LOG_INF("Done. It is now safe to remove the SD card.");
    return 0;

err:

    //
    // Every failure above has already logged its cause.
    //
    LOG_ERR("The recording did not complete (err %d)", ret);
    return ret;
}

//*****************************************************************************
//
// End Doxygen group.
//! @}
//
//*****************************************************************************
