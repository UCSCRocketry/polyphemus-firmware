# vc8000nanoe_jpeg

The JPEG half of ST's encoder library for the STM32N6 VENC peripheral. Unlike the separate JPEG codec, which needs its input already arranged in MCU blocks, ST's API for the VENC takes an NV12 frame directly from memory. `hal_stm32` carries the rest of this library, but leaves out the JPEG half because Zephyr's VENC driver only does H.264.

## Origin

| | |
| --- | --- |
| Repository | [STM32CubeN6](https://github.com/STMicroelectronics/STM32CubeN6) |
| Path | `Middlewares/Third_Party/VideoEncoder/source/jpeg` |
| Tag | `v1.2.0` |
| Commit | `85170d2c6eaa438622ed879caa4d7bcc6cc3920d` |

This tag is the version of the common half in `hal_stm32`, which this code compiles against. These files are the same in `v1.4.1`, the newest release when they were copied.

## License

BSD-3-Clause, as stated in `LICENSE.md` and in each source file. `st_readme.txt` is ST's change log for the library, and records that VeriSilicon's original is dual licensed as GPL-2.0 or BSD-3-Clause.

## Local changes

One, in `source/jpeg/JpegEncApi.c`. `hal_stm32` changed `enccommon.h` to always include its `encdebug.h`, which defines `APITRACE`, so this file's own definition became a redefinition. This is the same fix `hal_stm32` made in `H264EncApi.c`:

```diff
 #ifdef JPEGENC_TRACE
 #define APITRACE(str) JpegEnc_Trace(str)
 #else
+#ifndef APITRACE
 #define APITRACE(str)
+#endif
 #endif
```

## Using it

From an application's `CMakeLists.txt`, after `find_package(Zephyr)`:

```cmake
add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/../../third_party/vc8000nanoe_jpeg vc8000nanoe_jpeg)
target_link_libraries(app PRIVATE vc8000nanoe_jpeg)
```

It needs all of this:

- **Zephyr's VENC driver.** Set `CONFIG_VIDEO_ENCODER_H264=y`, with `&venc { status = "okay"; };` in the overlay. The driver has the `EWL` functions that reach the hardware, and enabling it builds the common half. The H.264 encoder gets built too.
- **The APB5 bus enabled** before the first call. The driver turns on VENC's own clock but not this bus, so VENC's registers read as 0 and `JpegEncInit()` returns `JPEGENC_INVALID_ARGUMENT`, because it reads that as having no JPEG support:

  ```c
  WRITE_REG(RCC->BUSENSR, RCC_BUSENSR_APB5ENS);
  ```

- **`JpegEncSetPictureSize()`** after `JpegEncInit()`. Without it, `JpegEncEncode()` divides by zero.
- **`CONFIG_ASSERT` off.** The driver's `EWLInit()` asserts that its client is the H.264 encoder.
- **Uncached output buffers**, or a cache flush between the CPU writing the JPEG header and the hardware starting, which there is no hook for. `JpegEncEncode()` does both, and the hardware then writes the rest of the image right beside the header.
- **Something to wake the waiting thread.** The driver's interrupt is a direct ISR that does not reschedule, so the thread waiting in `JpegEncEncode()` only wakes at the next ordinary interrupt. A short periodic timer during the encode does the job.
- **A check on the VENC's RIF attributes.** A debugger connecting can clear them. The hardware then cannot reach memory, but the library still reports success, and resetting the VENC does not help. Restoring them does.
- **Room for the driver's heap.** It holds the encoder's state, which the hardware does not read. On the STM32N6570-DK it goes in PSRAM by default, at 7 MB. 64 kB in internal RAM is enough (`CONFIG_VIDEO_STM32_VENC_HEAP_ZEPHYR_REGION=n`, `CONFIG_VIDEO_STM32_VENC_HEAP_SIZE`).

`JpegEncCfg.qTableLuma` and `qTableChroma` are in raster order, although `jpegencapi.h` says zigzag, and the library rounds values over 32 to what the hardware can use. A frame cannot quite fill the output buffer.

## Status

`examples/video_capture` uses this, and has recorded 1440x1088 at 30 frames per second on the STM32N6570-DK.
