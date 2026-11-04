# video_capture

Record video from the B-CAMS-IMX to the microSD card on the STM32N6570-DK as MJPEG. By default this is 1440x1088 at 30 frames per second for 30 seconds, starting as soon as the image runs.

Each frame goes from the camera through the DCMIPP into PSRAM as NV12. The VENC peripheral encodes it as a JPEG, also in PSRAM, where it waits to be written to the card. The hardware does nearly all of this, which leaves the CPU almost free. The Nucleo is not supported, as it has neither the PSRAM nor the card slot.

## Building and running

Ensure the B-CAMS-IMX is plugged into the board's CSI camera connector and a FAT32 formatted card is in the microSD slot. You must have a J-Link debugger connected to the board.

Run from this directory:

```console
$ west build -p always
$ python3 ../../scripts/jlink.py load
```

Watch the recording with `JLinkRTTClient`. Wait for the final line before removing the card or the power, as the file is only complete once it has been closed:

```console
Video capture example!
Recording 30 s to /SD:/VID00031.MJP
Capturing NV12 1440x1088 from a 2572x1944 crop at 30 fps, 2350080 bytes per frame
...
Recorded /SD:/VID00031.MJP: 901 frames, 57083904 bytes in 30020 ms, 30.01 fps
Dropped 0 frames: 0 missed by the camera, 0 with no JPEG buffer free, 0 too large, 0 failed to encode
Encoding took 20043 us per frame on average, 20104 us at worst
Writing took 311390 us at worst, with up to 27 of 64 buffers queued
Done. It is now safe to remove the SD card.
```

The driver warning `stm32_sdmmc: Failed to configure pins for sleep (-2)` is harmless.

Rarely, `video_imx335: Unable to initialize the sensor` appears at boot and the recording fails. Load the image again.

Attaching a debugger stalls the CPU briefly. A recording survives this, usually with nothing lost. If the stall lands at a bad moment, the camera has to be restarted, which loses about 10 frames and is reported in the final log. A debugger attaching over and over keeps costing frames, but the recording still finishes.

A debugger connecting through J-Link Commander, which includes `scripts/jlink.py read`, can also cut the VENC off from memory, by clearing its attributes in the RIF. The VENC gives no sign of this. It goes on reporting success while writing nothing, and without a check, one connection ruined the rest of the recording in 2 of 8 tries. The example checks before and after each frame and restores the attributes, which costs at most that frame. `The VENC lost its access to memory` in the log means that this happened. None of the recordings with `JLinkRTTLogger` connected throughout has shown it.

## Playing a recording

The file is raw MJPEG: JPEG images one after another, with no container or timing. Tell the player the frame rate:

```console
$ ffplay -f mjpeg -framerate 30 VID00001.MJP
$ ffmpeg -f mjpeg -framerate 30 -i VID00001.MJP -c copy VID00001.avi
```

Every dropped frame makes the rest of the video play slightly early, so check the final log when timing matters. Each frame is padded to a multiple of 512 bytes, which players skip.

The picture is green and dark, as nothing corrects the raw sensor image for white balance, black level or gamma. `CONFIG_VIDEO_CAPTURE_EXPOSURE` and `CONFIG_VIDEO_CAPTURE_GAIN` set the sensor's exposure and gain. The sensor driver defaults to the longest exposure, which is a whole frame time, and no gain.

## The SD card

The example never formats the card or overwrites a file. Each recording goes to the next unused `VIDnnnnn.MJP`, and a card that will not mount is an error. Recordings stop before FAT32's 4 GB file limit.

Write speed depends far more on how much is written at once than on the bus, because of the time the card takes to set up each write. FatFs splits every write at cluster boundaries, so the format's cluster size sets the limit. Measured with a 32 GB card:

| Write size | Speed |
| --- | --- |
| 16 kB | 2.3 to 3.2 MB/s |
| 32 kB | 3.9 to 4.4 MB/s |
| 64 kB | 5.0 to 7.8 MB/s |
| 128 kB | 6.2 to 10.1 MB/s |
| 256 kB | 7.7 to 10.7 MB/s |
| 512 kB | 8.4 to 10.7 MB/s |
| 1 MB | 11.0 to 14.4 MB/s |

These were raw writes to the card, 24 MB at each size, over four runs. Shorter tests vary far more. Through FatFs on that card's 16 kB clusters, the speed was 2.8 to 3.0 MB/s, and individual writes stalled for up to 1.2 s. A card formatted with 64 kB clusters should approach the 64 kB row, though that has not been measured. On macOS this erases the card:

```console
$ sudo newfs_msdos -F 32 -c 128 /dev/rdiskNsM
```

The bus is 4 bits wide at 48 MHz, using SDMMC2's internal DMA. This is faster than the 25 MHz rated for the default speed mode that the driver leaves the card in. It has worked with the card tested here, and all data read back correctly. If a card gives CRC errors, set `clk-div = <1>` in the board overlay for 24 MHz, which limits the bus to 12 MB/s.

## Data rate

At JPEG quality 50, indoors at night, with the sensor gain raising the signal and its noise:

| Sensor gain | Mean brightness | Frame size | Data rate | Frames dropped on the test card |
| --- | --- | --- | --- | --- |
| 0 dB | 18 of 255 | 28 kB | 0.8 MB/s | 0 of 901 |
| 18 dB | 57 of 255 | 63 kB | 1.9 MB/s | 0 of 901 in every run |
| 30 dB | 158 of 255 | 98 kB | 2.9 MB/s | 13 and 53 of 901 |

So on a card with 16 kB clusters, a bright and detailed scene will outrun the card at this quality. A detailed scene in daylight has not been measured. Forcing the sensor to 60 dB, which fills each frame with noise, gave 235 kB frames, and the recording kept going with frames dropped. It kept up 4.5 MB/s for the first 12 s with a third of its frames dropped, but over 24 minutes it settled at 3.0 MB/s with 58% dropped.

The frame rate is not limited by the encoder, which takes about 20 ms per frame at this size whatever the quality or content.

## CPU

At 1.9 MB/s, the CPU is about 45% busy. Nearly all of that is the SD driver polling the card for the end of each write, at a lower priority than everything else, so it gets out of the way of any other work. Leaving that out, the example uses under 2% of the CPU.

## Dropped frames

A recording carries on through anything short of a failed write, and the final log says what was lost:

- **Missed by the camera**: the camera had no free buffer in `CONFIG_VIDEO_CAPTURE_NUM_BUFS`, or was being restarted.
- **No JPEG buffer free**: the card fell behind and all `CONFIG_VIDEO_CAPTURE_NUM_JPEG_BUFS` JPEG buffers were waiting for it. Lower the quality, use a faster card, or format it with larger clusters.
- **Too large**: the frame did not fit in `CONFIG_VIDEO_CAPTURE_JPEG_BUF_SIZE`.
- **Failed to encode**: the encoder had an error. It restarts itself and carries on with the next frame.

## Memory

The video buffer pool takes 26.4 MB of the 32 MB PSRAM, for 4 capture buffers at 2.35 MB and 64 JPEG buffers at 256 kB. The JPEG buffers cover a little over 2 seconds of card stalls at 30 frames per second. Changing the size or number of buffers can also mean changing `CONFIG_VIDEO_BUFFER_POOL_HEAP_SIZE` and `CONFIG_VIDEO_BUFFER_POOL_NUM_MAX`.

The PSRAM is uncached, because the hardware does nearly all of the reading and writing there. The CPU only writes each JPEG's header and padding.

JPEG works in 16x16 blocks, so the width and height must be multiples of 16, and 1080 lines is not possible. Other sizes can be set with `CONFIG_VIDEO_CAPTURE_WIDTH` and `CONFIG_VIDEO_CAPTURE_HEIGHT`, and the sensor image is cropped to match their aspect ratio. Larger sizes take the encoder longer, and it must stay under the 33 ms between frames.

## Why the VENC?

The chip has two JPEG encoders. The JPEG codec has no way to read a frame from memory, and needs its input rearranged into blocks first, which takes about 45% of the CPU at this resolution and frame rate. The VENC takes NV12 directly. Compared on the same frames, the two come out at the same image quality. At quality 50 the sizes match, and at quality 90 the VENC's are up to 4.5% larger.

Zephyr's VENC driver only supports H.264, so `src/jpeg_venc.c` uses ST's encoder library in `third_party/vc8000nanoe_jpeg`, with the driver providing the hardware access. The comments there cover what that takes.
