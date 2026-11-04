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

- **`CONFIG_ASSERT` off.** The driver's `EWLInit()` asserts that its client is the H.264 encoder.
- **Room in the PSRAM.** On the STM32N6570-DK, enabling the driver puts its heap there, 7 MB by default (`CONFIG_VIDEO_STM32_VENC_HEAP_SIZE`). 1 MB was enough for the test below.

## Status

`JpegEncInit()` and `JpegEncRelease()` work on the STM32N6570-DK for 1440x1088 NV12. **No frame has been encoded with this yet**, and nothing in this repository uses it.
