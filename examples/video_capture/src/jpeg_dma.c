//*****************************************************************************
//
// SPDX-FileCopyrightText: Copyright (c) 2026, UCSC Rocket Team
//
// SPDX-License-Identifier: BSD-3-Clause
//
//*****************************************************************************

//*****************************************************************************
//
//! @file jpeg_dma.c
//!
//! @brief Encode NV12 frames with the STM32N6 JPEG codec over HPDMA.
//!
//! @addtogroup video_capture
//! @{
//!
//! Zephyr's JPEG driver moves every byte into and out of the codec's FIFOs by
//! interrupt, at 16 bytes per interrupt. At this resolution and frame rate that
//! is over 4 million interrupts per second, or under 200 CPU cycles for each,
//! so this file drives the codec over DMA instead.
//!
//! The codec takes its input as 4:2:0 MCUs: 16x16 pixels stored as four 8x8
//! luma blocks and one 8x8 block for each chroma plane, so 384 bytes. Nothing
//! in the hardware can make these from an NV12 frame, so the CPU converts one
//! row of MCUs at a time into one of two strip buffers in internal RAM. DMA
//! feeds the codec from one strip while the CPU fills the other.
//!
//! The JPEG leaves the codec in a single DMA transfer per frame, which the DMA
//! controller repeats block by block with no gaps. This is why the STM32Cube
//! HAL is only used here for the codec's tables and the DMA channels, and not
//! for HAL_JPEG_Encode_DMA(). That function has the application hand over the
//! output in chunks of up to 64 kB, restarting the DMA between them. Setting the
//! codec's output DMA enable again for a restart makes it ask for a burst even
//! if it has too little ready, putting 32 or more bytes of junk in the middle of
//! the JPEG. And a chunk that ends along with the frame makes the encode hang.
//
//*****************************************************************************

//
// Standard C library
//
#include <errno.h>                  // Error codes
#include <string.h>                 // memcpy()

//
// Zephyr kernel and drivers
//
#include <zephyr/kernel.h>          // Semaphores, timeouts and interrupts
#include <zephyr/cache.h>           // Data cache flush and invalidate
#include <zephyr/drivers/clock_control.h>                // Peripheral clocks
#include <zephyr/drivers/clock_control/stm32_clock_control.h> // STM32 clock specifiers
#include <zephyr/drivers/reset.h>   // Peripheral reset
#include <zephyr/irq.h>             // IRQ_CONNECT()
#include <zephyr/logging/log.h>     // Logging over the RTT console

//
// STM32Cube HAL
//
#include <soc.h>                    // HAL JPEG and DMA drivers

#include "jpeg_dma.h"

//*****************************************************************************
//
// Constants.
//
//*****************************************************************************

#define JPEG_NODE               DT_NODELABEL(jpeg)
#define HPDMA_NODE              DT_NODELABEL(hpdma1)
#define JPEG_IRQN               DT_IRQN(JPEG_NODE)

//
// Only HPDMA channels 12 to 15 can repeat a block.
//
#define DMA_IN_CHANNEL          HPDMA1_Channel0
#define DMA_IN_IRQN             DT_IRQ_BY_IDX(HPDMA_NODE, 0, irq)
#define DMA_OUT_CHANNEL         HPDMA1_Channel12
#define DMA_OUT_IRQN            DT_IRQ_BY_IDX(HPDMA_NODE, 12, irq)

#define MCU_SIZE                16U                     // Pixels per MCU side
#define BLOCK_SIZE              8U                      // Pixels per block side
#define BLOCK_BYTES             (BLOCK_SIZE * BLOCK_SIZE)
#define MCU_LUMA_BYTES          (4U * BLOCK_BYTES)
#define MCU_BYTES               (6U * BLOCK_BYTES)

#define FRAME_WIDTH             CONFIG_VIDEO_CAPTURE_WIDTH
#define FRAME_HEIGHT            CONFIG_VIDEO_CAPTURE_HEIGHT
#define MCU_COLUMNS             (FRAME_WIDTH / MCU_SIZE)
#define MCU_ROWS                (FRAME_HEIGHT / MCU_SIZE)
#define STRIP_BYTES             (MCU_COLUMNS * MCU_BYTES)
#define STRIP_COUNT             2U

//
// Words per DMA burst. The codec's FIFOs want 8: with bursts of 4, it stops
// asking for data a few words into the frame. For the same reason, the length
// of every DMA block must be a whole number of bursts.
//
#define DMA_BURST_WORDS         8U
#define DMA_BURST_BYTES         (DMA_BURST_WORDS * sizeof(uint32_t))

//
// The output DMA repeats a block of this size to cover the whole buffer.
//
#define OUT_BLOCK_BYTES         JPEG_DMA_OUT_MULTIPLE
#define MAX_OUT_BLOCKS          2048U

//
// The interrupts share a priority, since their handlers share state.
//
#define IRQ_PRIORITY            2

//
// Far longer than any healthy step of the encoder takes.
//
#define STEP_TIMEOUT            K_MSEC(500)

BUILD_ASSERT(FRAME_WIDTH % MCU_SIZE == 0 && FRAME_HEIGHT % MCU_SIZE == 0,
             "The JPEG encoder works in 16x16 blocks");

BUILD_ASSERT(STRIP_BYTES < 0x10000U, "One DMA block is limited to 64 kB");

BUILD_ASSERT(STRIP_BYTES % DMA_BURST_BYTES == 0 && OUT_BLOCK_BYTES % DMA_BURST_BYTES == 0,
             "Every DMA block must be a whole number of bursts");

//*****************************************************************************
//
// Globals.
//
//*****************************************************************************

LOG_MODULE_REGISTER(jpeg_dma);

static uint32_t jpeg_quality;
static JPEG_HandleTypeDef jpeg_handle;
static DMA_HandleTypeDef dma_in;
static DMA_HandleTypeDef dma_out;

//
// The strips the CPU fills and DMA drains. Internal RAM is much faster for the
// CPU to write than the PSRAM.
//
static uint8_t strips[STRIP_COUNT][STRIP_BYTES] __aligned(64);

K_SEM_DEFINE(strip_free, 0, STRIP_COUNT);
K_SEM_DEFINE(encode_done, 0, 1);

//
// State of the encode in progress, shared with the interrupt handlers.
//
static struct
{
    uint8_t *out;               // Where the JPEG is going
    size_t out_size;            // Size of out, in bytes
    size_t out_len;             // Size of the finished JPEG, in bytes
    uint8_t *queued_strip;      // Filled strip waiting for the DMA, or NULL
    bool last_strip_queued;     // No more strips are coming
    bool input_paused;          // DMA ran out of strips, and is waiting for one
    int error;                  // Negative errno once the encode has failed
} encode;

//*****************************************************************************
//
// Convert one row of MCUs from NV12 into a strip.
//
// This walks the frame a line at a time rather than an MCU at a time, since
// reading the PSRAM sequentially lets the CPU's prefetcher help.
//
//*****************************************************************************
static void
convert_row(const uint8_t *frame, uint32_t row, uint8_t *strip)
{
    const uint8_t *luma = frame + (row * MCU_SIZE * FRAME_WIDTH);
    const uint8_t *chroma = frame + (FRAME_WIDTH * FRAME_HEIGHT) +
                            (row * BLOCK_SIZE * FRAME_WIDTH);

    //
    // Each line of luma is split between the two blocks it crosses in every MCU.
    //
    for ( uint32_t line = 0; line < MCU_SIZE; line++ )
    {
        const uint8_t *src = luma + (line * FRAME_WIDTH);
        uint8_t *dst = strip + ((line / BLOCK_SIZE) * 2U * BLOCK_BYTES) +
                       ((line % BLOCK_SIZE) * BLOCK_SIZE);

        for ( uint32_t mcu = 0; mcu < MCU_COLUMNS; mcu++ )
        {
            memcpy(dst, src, BLOCK_SIZE);
            memcpy(dst + BLOCK_BYTES, src + BLOCK_SIZE, BLOCK_SIZE);
            src += MCU_SIZE;
            dst += MCU_BYTES;
        }
    }

    //
    // Each line of chroma has one interleaved CbCr pair per two luma pixels.
    //
    for ( uint32_t line = 0; line < BLOCK_SIZE; line++ )
    {
        const uint8_t *src = chroma + (line * FRAME_WIDTH);
        uint8_t *dst = strip + MCU_LUMA_BYTES + (line * BLOCK_SIZE);

        for ( uint32_t mcu = 0; mcu < MCU_COLUMNS; mcu++ )
        {
            for ( uint32_t i = 0; i < BLOCK_SIZE; i++ )
            {
                dst[i] = src[2U * i];
                dst[BLOCK_BYTES + i] = src[(2U * i) + 1U];
            }
            src += MCU_SIZE;
            dst += MCU_BYTES;
        }
    }

    sys_cache_data_flush_range(strip, STRIP_BYTES);
}

//*****************************************************************************
//
// Record why the encode failed, and wake the thread waiting on it.
//
//*****************************************************************************
static void
fail_encode(int error)
{
    if ( encode.error == 0 )
    {
        encode.error = error;
    }
    k_sem_give(&strip_free);
    k_sem_give(&encode_done);
}

//*****************************************************************************
//
// Start the input DMA on a strip.
//
// The codec only asks for more input when its DMA enable goes from 0 to 1.
//
//*****************************************************************************
static int
start_strip(uint8_t *strip)
{
    JPEG_TypeDef *codec = jpeg_handle.Instance;

    codec->CR &= ~JPEG_CR_IDMAEN;

    if ( HAL_DMA_Start_IT(&dma_in, (uint32_t)strip, (uint32_t)&codec->DIR,
                          STRIP_BYTES) != HAL_OK )
    {
        return -EIO;
    }

    codec->CR |= JPEG_CR_IDMAEN;

    return 0;
}

//*****************************************************************************
//
// DMA callback: the input DMA has finished a strip, so give it the next one.
//
//*****************************************************************************
static void
dma_in_done(DMA_HandleTypeDef *hdma)
{
    ARG_UNUSED(hdma);

    k_sem_give(&strip_free);

    if ( encode.queued_strip != NULL )
    {
        if ( start_strip(encode.queued_strip) < 0 )
        {
            fail_encode(-EIO);
        }
        encode.queued_strip = NULL;
    }
    else if ( !encode.last_strip_queued )
    {
        //
        // The CPU has not finished the next strip. queue_strip() starts the
        // transfer when it has.
        //
        encode.input_paused = true;
    }
}

//*****************************************************************************
//
// DMA callback: the output DMA has filled the whole output buffer.
//
// If the codec has finished as well then the JPEG fits, and its interrupt
// handler takes care of it. Otherwise, there is more to come and no room for it.
//
//*****************************************************************************
static void
dma_out_done(DMA_HandleTypeDef *hdma)
{
    ARG_UNUSED(hdma);

    if ( (jpeg_handle.Instance->SR & JPEG_SR_EOCF) == 0U )
    {
        fail_encode(-ENOSPC);
    }
}

//*****************************************************************************
//
// DMA callback: a DMA channel failed.
//
//*****************************************************************************
static void
dma_failed(DMA_HandleTypeDef *hdma)
{
    ARG_UNUSED(hdma);

    fail_encode(-EIO);
}

//*****************************************************************************
//
// Stop the output DMA at the end of a frame, leaving it ready for the next.
//
// Returns how many bytes it wrote to memory.
//
//*****************************************************************************
static size_t
stop_output_dma(void)
{
    DMA_Channel_TypeDef *channel = dma_out.Instance;
    size_t written;

    //
    // The channel suspends when it finishes the burst it is on. It is already
    // idle if it has just filled the buffer, and so does not suspend at all.
    //
    channel->CCR |= DMA_CCR_SUSP;
    while ( (channel->CSR & (DMA_CSR_SUSPF | DMA_CSR_IDLEF)) == 0U )
    {
    }

    written = channel->CDAR - (uint32_t)encode.out;

    __HAL_DMA_DISABLE_IT(&dma_out, DMA_IT_TC | DMA_IT_DTE | DMA_IT_ULE | DMA_IT_USE | DMA_IT_TO);
    channel->CCR |= DMA_CCR_RESET;
    __HAL_DMA_CLEAR_FLAG(&dma_out, DMA_FLAG_TC | DMA_FLAG_HT | DMA_FLAG_DTE | DMA_FLAG_ULE |
                         DMA_FLAG_USE | DMA_FLAG_SUSP | DMA_FLAG_TO);

    dma_out.State = HAL_DMA_STATE_READY;
    __HAL_UNLOCK(&dma_out);

    return written;
}

//*****************************************************************************
//
// Interrupt handler for the codec, which reaches the end of the frame.
//
//*****************************************************************************
static void
jpeg_isr(const void *arg)
{
    JPEG_TypeDef *codec = jpeg_handle.Instance;
    size_t length;

    ARG_UNUSED(arg);

    if ( (codec->SR & JPEG_SR_EOCF) == 0U )
    {
        return;
    }

    codec->CR &= ~(JPEG_CR_EOCIE | JPEG_CR_IDMAEN | JPEG_CR_ODMAEN);
    codec->CONFR0 &= ~JPEG_CONFR0_START;
    codec->CFR |= JPEG_CFR_CEOCF;

    length = stop_output_dma();

    //
    // The rest of the JPEG is in the codec's FIFO, as too little to make up a
    // burst for the DMA.
    //
    while ( (codec->SR & JPEG_SR_OFNEF) != 0U )
    {
        uint32_t word = codec->DOR;

        if ( length + sizeof(word) > encode.out_size )
        {
            fail_encode(-ENOSPC);
            return;
        }

        memcpy(encode.out + length, &word, sizeof(word));
        length += sizeof(word);
    }

    encode.out_len = length;
    k_sem_give(&encode_done);
}

//*****************************************************************************
//
// Interrupt handlers for the DMA channels.
//
//*****************************************************************************
static void
dma_in_isr(const void *arg)
{
    ARG_UNUSED(arg);

    HAL_DMA_IRQHandler(&dma_in);
}

static void
dma_out_isr(const void *arg)
{
    ARG_UNUSED(arg);

    HAL_DMA_IRQHandler(&dma_out);
}

//*****************************************************************************
//
// Set up one DMA channel between memory and a FIFO of the codec.
//
// Port 0 of the HPDMA is its AXI master, which reaches the RAM and PSRAM, and
// port 1 is its AHB master, which reaches the codec.
//
//*****************************************************************************
static int
dma_channel_init(DMA_HandleTypeDef *hdma, DMA_Channel_TypeDef *channel, uint32_t request,
                 bool to_codec)
{
    const DMA_IsolationConfigTypeDef isolation = {
        .CidFiltering = DMA_ISOLATION_ON,
        .StaticCid = DMA_CHANNEL_STATIC_CID_1,
    };

    hdma->Instance = channel;
    hdma->Init.Request = request;
    hdma->Init.BlkHWRequest = DMA_BREQ_SINGLE_BURST;
    hdma->Init.Direction = to_codec ? DMA_MEMORY_TO_PERIPH : DMA_PERIPH_TO_MEMORY;
    hdma->Init.SrcInc = to_codec ? DMA_SINC_INCREMENTED : DMA_SINC_FIXED;
    hdma->Init.DestInc = to_codec ? DMA_DINC_FIXED : DMA_DINC_INCREMENTED;
    hdma->Init.SrcDataWidth = DMA_SRC_DATAWIDTH_WORD;
    hdma->Init.DestDataWidth = DMA_DEST_DATAWIDTH_WORD;
    hdma->Init.Priority = DMA_HIGH_PRIORITY;
    hdma->Init.SrcBurstLength = DMA_BURST_WORDS;
    hdma->Init.DestBurstLength = DMA_BURST_WORDS;
    hdma->Init.TransferAllocatedPort = to_codec ?
                                       (DMA_SRC_ALLOCATED_PORT0 | DMA_DEST_ALLOCATED_PORT1) :
                                       (DMA_SRC_ALLOCATED_PORT1 | DMA_DEST_ALLOCATED_PORT0);
    hdma->Init.TransferEventMode = to_codec ? DMA_TCEM_BLOCK_TRANSFER :
                                   DMA_TCEM_REPEATED_BLOCK_TRANSFER;
    hdma->Init.Mode = DMA_NORMAL;

    if ( HAL_DMA_Init(hdma) != HAL_OK )
    {
        return -EIO;
    }

    hdma->XferCpltCallback = to_codec ? dma_in_done : dma_out_done;
    hdma->XferErrorCallback = dma_failed;

    //
    // This example runs in the secure state, as do the codec and the memory.
    //
    if ( HAL_DMA_ConfigChannelAttributes(hdma, DMA_CHANNEL_PRIV | DMA_CHANNEL_SEC |
                                         DMA_CHANNEL_SRC_SEC | DMA_CHANNEL_DEST_SEC) != HAL_OK )
    {
        return -EIO;
    }

    //
    // Zephyr gives the bus masters in the secure state CID 1, and the memory
    // only accepts accesses from it. The HPDMA has its own setting for this, per
    // channel. Without it, transfers stall or silently fail to move any data.
    //
    if ( HAL_DMA_SetIsolationAttributes(hdma, &isolation) != HAL_OK )
    {
        return -EIO;
    }

    return 0;
}

//*****************************************************************************
//
// Reset the codec, and set it and its DMA channels up for encoding.
//
//*****************************************************************************
static int
codec_start(void)
{
    const struct reset_dt_spec reset = RESET_DT_SPEC_GET(JPEG_NODE);
    JPEG_ConfTypeDef config = {
        .ColorSpace = JPEG_YCBCR_COLORSPACE,
        .ChromaSubsampling = JPEG_420_SUBSAMPLING,
        .ImageWidth = FRAME_WIDTH,
        .ImageHeight = FRAME_HEIGHT,
        .ImageQuality = jpeg_quality,
    };
    int ret;

    ret = reset_line_toggle_dt(&reset);
    if ( ret < 0 )
    {
        return ret;
    }

    ret = dma_channel_init(&dma_in, DMA_IN_CHANNEL, HPDMA1_REQUEST_JPEG_RX, true);
    if ( ret < 0 )
    {
        return ret;
    }

    ret = dma_channel_init(&dma_out, DMA_OUT_CHANNEL, HPDMA1_REQUEST_JPEG_TX, false);
    if ( ret < 0 )
    {
        return ret;
    }

    jpeg_handle.Instance = JPEG;

    if ( HAL_JPEG_Init(&jpeg_handle) != HAL_OK )
    {
        return -EIO;
    }

    if ( HAL_JPEG_ConfigEncoding(&jpeg_handle, &config) != HAL_OK )
    {
        return -EIO;
    }

    return 0;
}

//*****************************************************************************
//
// Put the codec and its DMA channels back in order after a failed encode.
//
// A failed encode can stop at any point, so start again from a reset. Masking
// the interrupts keeps the handlers from acting on a half-reset state.
//
//*****************************************************************************
static int
codec_recover(void)
{
    int ret;

    irq_disable(JPEG_IRQN);
    irq_disable(DMA_IN_IRQN);
    irq_disable(DMA_OUT_IRQN);

    HAL_DMA_DeInit(&dma_in);
    HAL_DMA_DeInit(&dma_out);
    HAL_JPEG_DeInit(&jpeg_handle);

    ret = codec_start();

    NVIC_ClearPendingIRQ(JPEG_IRQN);
    NVIC_ClearPendingIRQ(DMA_IN_IRQN);
    NVIC_ClearPendingIRQ(DMA_OUT_IRQN);

    irq_enable(JPEG_IRQN);
    irq_enable(DMA_IN_IRQN);
    irq_enable(DMA_OUT_IRQN);

    if ( ret < 0 )
    {
        LOG_ERR("Could not restart the encoder (err %d)", ret);
    }

    return ret;
}

//*****************************************************************************
//
// Bring up the JPEG codec and its two DMA channels.
//
//*****************************************************************************
int
jpeg_dma_init(uint32_t quality)
{
    static const struct stm32_pclken jpeg_clock[] = STM32_DT_CLOCKS(JPEG_NODE);
    static const struct stm32_pclken hpdma_clock[] = STM32_DT_CLOCKS(HPDMA_NODE);
    const struct device *const rcc = DEVICE_DT_GET(STM32_CLOCK_CONTROL_NODE);
    int ret;

    jpeg_quality = quality;

    ret = clock_control_on(rcc, (clock_control_subsys_t)&jpeg_clock[0]);
    if ( ret < 0 )
    {
        return ret;
    }

    ret = clock_control_on(rcc, (clock_control_subsys_t)&hpdma_clock[0]);
    if ( ret < 0 )
    {
        return ret;
    }

    ret = codec_start();
    if ( ret < 0 )
    {
        return ret;
    }

    IRQ_CONNECT(JPEG_IRQN, IRQ_PRIORITY, jpeg_isr, NULL, 0);
    IRQ_CONNECT(DMA_IN_IRQN, IRQ_PRIORITY, dma_in_isr, NULL, 0);
    IRQ_CONNECT(DMA_OUT_IRQN, IRQ_PRIORITY, dma_out_isr, NULL, 0);
    irq_enable(JPEG_IRQN);
    irq_enable(DMA_IN_IRQN);
    irq_enable(DMA_OUT_IRQN);

    return 0;
}

//*****************************************************************************
//
// Start the codec and the DMA on a new frame, whose first strip is filled.
//
//*****************************************************************************
static int
start_frame(uint8_t *out, uint32_t out_blocks)
{
    JPEG_TypeDef *codec = jpeg_handle.Instance;
    const DMA_RepeatBlockConfTypeDef repeat = {
        .RepeatCount = out_blocks,
    };
    int ret;

    //
    // Ready the codec, as HAL_JPEG_Encode_DMA() would.
    //
    codec->CONFR1 &= ~JPEG_CONFR1_DE;
    codec->CONFR0 &= ~JPEG_CONFR0_START;
    codec->CR &= ~(JPEG_CR_IFTIE | JPEG_CR_IFNFIE | JPEG_CR_OFTIE | JPEG_CR_OFNEIE |
                   JPEG_CR_EOCIE | JPEG_CR_HPDIE | JPEG_CR_IDMAEN | JPEG_CR_ODMAEN);
    codec->CR |= JPEG_CR_IFF | JPEG_CR_OFF;
    codec->CFR |= JPEG_CFR_CEOCF | JPEG_CFR_CHPDF;
    codec->CONFR0 |= JPEG_CONFR0_START;
    codec->CR |= JPEG_CR_EOCIE;

    if ( HAL_DMAEx_ConfigRepeatBlock(&dma_out, &repeat) != HAL_OK ||
         HAL_DMA_Start_IT(&dma_out, (uint32_t)&codec->DOR, (uint32_t)out,
                          OUT_BLOCK_BYTES) != HAL_OK )
    {
        return -EIO;
    }

    ret = start_strip(strips[0]);
    if ( ret < 0 )
    {
        return ret;
    }

    codec->CR |= JPEG_CR_ODMAEN;

    return 0;
}

//*****************************************************************************
//
// Give a filled strip to the DMA, starting it again if it is waiting for one.
//
//*****************************************************************************
static int
queue_strip(uint8_t *strip, bool last)
{
    unsigned int key = irq_lock();
    int ret = 0;

    encode.last_strip_queued = last;

    if ( encode.input_paused )
    {
        encode.input_paused = false;
        ret = start_strip(strip);
    }
    else
    {
        encode.queued_strip = strip;
    }

    irq_unlock(key);

    return ret;
}

//*****************************************************************************
//
// Feed the rest of the frame to the codec, a strip at a time.
//
//*****************************************************************************
static int
feed_codec(const uint8_t *frame)
{
    uint32_t strip_index = 1;
    int ret;

    for ( uint32_t row = 1; row < MCU_ROWS; row++ )
    {
        ret = k_sem_take(&strip_free, STEP_TIMEOUT);
        if ( ret < 0 )
        {
            return ret;
        }

        if ( encode.error != 0 )
        {
            return encode.error;
        }

        convert_row(frame, row, strips[strip_index]);

        ret = queue_strip(strips[strip_index], row == MCU_ROWS - 1U);
        if ( ret < 0 )
        {
            return ret;
        }

        strip_index = (strip_index + 1U) % STRIP_COUNT;
    }

    return 0;
}

//*****************************************************************************
//
// Encode one NV12 frame, and wait for it to finish.
//
//*****************************************************************************
int
jpeg_dma_encode(const uint8_t *frame, uint8_t *out, size_t out_size, size_t *out_len)
{
    uint32_t out_blocks = out_size / OUT_BLOCK_BYTES;
    int ret;

    if ( out_blocks == 0U || out_blocks > MAX_OUT_BLOCKS )
    {
        return -EINVAL;
    }

    encode.out = out;
    encode.out_size = out_blocks * OUT_BLOCK_BYTES;
    encode.out_len = 0;
    encode.queued_strip = NULL;
    encode.last_strip_queued = (MCU_ROWS == 1U);
    encode.input_paused = false;
    encode.error = 0;

    k_sem_reset(&encode_done);
    k_sem_reset(&strip_free);

    //
    // The only part of out the CPU writes is the few bytes left in the codec's
    // FIFO at the end of the frame. A stale line in the cache there would
    // overwrite the data the DMA had put beside them.
    //
    sys_cache_data_invd_range(out, encode.out_size);

    //
    // Start on the first row, with the second strip free for the next.
    //
    convert_row(frame, 0, strips[0]);
    k_sem_give(&strip_free);

    ret = start_frame(out, out_blocks);
    if ( ret < 0 )
    {
        LOG_ERR("Could not start the encoder (err %d)", ret);
        goto abort;
    }

    ret = feed_codec(frame);
    if ( ret < 0 )
    {
        goto abort;
    }

    ret = k_sem_take(&encode_done, STEP_TIMEOUT);
    if ( ret < 0 )
    {
        goto abort;
    }

    if ( encode.error != 0 )
    {
        ret = encode.error;
        goto abort;
    }

    *out_len = encode.out_len;
    return 0;

abort:

    //
    // Leave the codec ready for the next frame. The cause of this failure is
    // still the error to report, even if that does not work.
    //
    (void) codec_recover();
    return ret;
}

//*****************************************************************************
//
// End Doxygen group.
//! @}
//
//*****************************************************************************
