//*****************************************************************************
//
// SPDX-FileCopyrightText: Copyright (c) 2026, UCSC Rocket Team
//
// SPDX-License-Identifier: BSD-3-Clause
//
//*****************************************************************************

//*****************************************************************************
//
//! @file jpeg_venc.h
//!
//! @brief Encode NV12 frames as JPEGs with the STM32N6 VENC peripheral.
//!
//! @addtogroup video_capture
//! @{
//
//*****************************************************************************

#ifndef JPEG_VENC_H
#define JPEG_VENC_H

#include <stddef.h>
#include <stdint.h>

//*****************************************************************************
//
//! @brief Bring up the JPEG encoder.
//!
//! The picture size comes from CONFIG_VIDEO_CAPTURE_WIDTH and
//! CONFIG_VIDEO_CAPTURE_HEIGHT.
//!
//! @param quality JPEG quality, from 1 to 100.
//!
//! @return 0 on success, or a negative errno otherwise.
//
//*****************************************************************************
int jpeg_venc_init(uint32_t quality);

//*****************************************************************************
//
//! @brief Encode one NV12 frame, and wait for it to finish.
//!
//! The VENC reads the frame and writes the JPEG directly, so neither can be in
//! cached memory.
//!
//! @param frame The NV12 frame, with a luma pitch of the frame width.
//! @param out Where to write the JPEG. Must be 8 byte aligned.
//! @param out_size Size of out, in bytes. The VENC stops a little short of the
//!        end, so a JPEG cannot quite fill it.
//! @param[out] out_len Size of the JPEG, in bytes.
//!
//! @return 0 on success, -ENOSPC if the JPEG did not fit, or another negative
//!         errno otherwise. The encoder is ready for another frame in all
//!         cases, unless it could not be restarted after an error.
//
//*****************************************************************************
int jpeg_venc_encode(const uint8_t *frame, uint8_t *out, size_t out_size, size_t *out_len);

#endif // JPEG_VENC_H

//*****************************************************************************
//
// End Doxygen group.
//! @}
//
//*****************************************************************************
