//*****************************************************************************
//
// SPDX-FileCopyrightText: Copyright (c) 2026, UCSC Rocket Team
//
// SPDX-License-Identifier: BSD-3-Clause
//
//*****************************************************************************

//*****************************************************************************
//
//! @file jpeg_dma.h
//!
//! @brief Encode NV12 frames with the STM32N6 JPEG codec over HPDMA.
//!
//! @addtogroup video_capture
//! @{
//
//*****************************************************************************

#ifndef JPEG_DMA_H
#define JPEG_DMA_H

#include <stddef.h>
#include <stdint.h>

//
// The size of an output buffer must be a multiple of this.
//
#define JPEG_DMA_OUT_MULTIPLE   4096U

//*****************************************************************************
//
//! @brief Bring up the JPEG codec and its two DMA channels.
//!
//! The picture size comes from CONFIG_VIDEO_CAPTURE_WIDTH and
//! CONFIG_VIDEO_CAPTURE_HEIGHT.
//!
//! @param quality JPEG quality, from 1 to 100.
//!
//! @return 0 on success, or a negative errno otherwise.
//
//*****************************************************************************
int jpeg_dma_init(uint32_t quality);

//*****************************************************************************
//
//! @brief Encode one NV12 frame, and wait for it to finish.
//!
//! The caller must already have invalidated the frame from the data cache.
//! On return, the part of out that was written may be dirty in the data cache,
//! and the caller is responsible for flushing it.
//!
//! @param frame The NV12 frame, with a luma pitch of the frame width.
//! @param out Where to write the JPEG. Must be 4 byte aligned.
//! @param out_size Size of out, in bytes. Any part beyond the last multiple of
//!        JPEG_DMA_OUT_MULTIPLE goes unused.
//! @param[out] out_len Size of the JPEG, in bytes. This is a multiple of 4, so
//!             up to 3 bytes of padding can follow the end of the image.
//!
//! @return 0 on success, -ENOSPC if the JPEG did not fit, or another negative
//!         errno otherwise. After a failure, the encoder has been reset and is
//!         ready for another frame.
//
//*****************************************************************************
int jpeg_dma_encode(const uint8_t *frame, uint8_t *out, size_t out_size, size_t *out_len);

#endif // JPEG_DMA_H

//*****************************************************************************
//
// End Doxygen group.
//! @}
//
//*****************************************************************************
