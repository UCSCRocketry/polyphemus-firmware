# video_capture

Record video from the B-CAMS-IMX to the microSD card on the STM32N6570-DK as MJPEG. By default this is 1440x1088 at 30 frames per second for 30 seconds, starting as soon as the image runs.

Each frame goes from the camera through the DCMIPP into PSRAM as NV12, then through the STM32N6 hardware JPEG codec over DMA, and waits in PSRAM to be written to the card. The Nucleo is not supported, as it has neither the PSRAM nor the card slot.

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
Recording 30 s to /SD:/VID00016.MJP
Capturing NV12 1440x1088 from a 2572x1944 crop at 30 fps, 2350080 bytes per frame
...
 29 s: 871 encoded, 870 written, 1344 kB/s, 0 of 64 buffers queued, dropped 0
Recorded /SD:/VID00016.MJP: 901 frames, 38772736 bytes in 30016 ms, 30.01 fps
Dropped 0 frames: 0 missed by the camera, 0 with no JPEG buffer free, 0 too large, 0 failed to encode
Encoding took 16883 us per frame on average, 16942 us at worst
Writing took 384309 us at worst, with up to 31 of 64 buffers queued
Done. It is now safe to remove the SD card.
```

The driver warning `stm32_sdmmc: Failed to configure pins for sleep (-2)` is harmless.

Rarely, `video_imx335: Unable to initialize the sensor` appears at boot and the recording fails. Load the image again.

## Playing a recording

The file is raw MJPEG: JPEG images one after another, with no container or timing. Tell the player the frame rate:

```console
$ ffplay -f mjpeg -framerate 30 VID00001.MJP
$ ffmpeg -f mjpeg -framerate 30 -i VID00001.MJP -c copy VID00001.avi
```

Every dropped frame makes the rest of the video play slightly early, so check the final log when timing matters. Each frame is padded to a multiple of 512 bytes, which players skip.

The picture is green and dark, as nothing corrects the raw sensor image for white balance, black level or gamma. `CONFIG_VIDEO_CAPTURE_EXPOSURE` and `CONFIG_VIDEO_CAPTURE_GAIN` set the sensor's exposure and gain. The sensor driver defaults to the longest exposure, which is a whole frame time.

## The SD card

The example never formats the card or overwrites a file. Each recording goes to the next unused `VIDnnnnn.MJP`, and a card that will not mount is an error. Recordings stop before FAT32's 4 GB file limit.

Write speed depends far more on how much is written at once than on the bus, because of the time the card takes to set up each write. FatFs splits every write at cluster boundaries, so the format's cluster size sets the limit. Measured with a 32 GB card:

| Write size | Speed |
| --- | --- |
| 16 kB | 3.9 MB/s |
| 32 kB | 7.1 MB/s |
| 64 kB | 10.6 MB/s |
| 256 kB | 17.0 MB/s |
| 1 MB | 20.0 MB/s |

These were raw writes to the card. Through FatFs on that card's 16 kB clusters, the speed was around 3 MB/s, and individual writes stalled for up to 843 ms. A card formatted with 64 kB clusters should approach the 64 kB row, though that has not been measured. On macOS this erases the card:

```console
$ sudo newfs_msdos -F 32 -c 128 /dev/rdiskNsM
```

The bus is 4 bits wide at 48 MHz, using SDMMC2's internal DMA. This is faster than the 25 MHz rated for the default speed mode that the driver leaves the card in. It has worked with the card tested here, and all data read back correctly. If a card gives CRC errors, set `clk-div = <1>` in the board overlay for 24 MHz, which limits the bus to 12 MB/s.

## Data rate

The encoder takes about 17 ms per frame at this size whatever the JPEG quality, so what limits the frame rate is the card. Measured indoors at the default exposure:

| JPEG quality | Scene | Frame size | Data rate | Result on the test card |
| --- | --- | --- | --- | --- |
| 50 | Dim and plain | 30 kB | 1 MB/s | No frames dropped |
| 50 | Brighter | 43 kB | 1.3 MB/s | No frames dropped |
| 90 | Dim and plain | 86 kB | 2.6 MB/s | No frames dropped |
| 90 | Brighter | 133 kB | 4 MB/s | 115 of 601 frames dropped |

A detailed scene in daylight will make larger frames than any of these, and has not been measured. At quality 100, the frames of even the dim scene were over 512 kB, too large for the default 256 kB JPEG buffers.

## Dropped frames

A recording carries on through anything short of a failed write, and the final log says what was lost:

- **Missed by the camera**: the encoder fell behind and all `CONFIG_VIDEO_CAPTURE_NUM_BUFS` capture buffers were full.
- **No JPEG buffer free**: the card fell behind and all `CONFIG_VIDEO_CAPTURE_NUM_JPEG_BUFS` JPEG buffers were waiting for it. Lower the quality, use a faster card, or format it with larger clusters.
- **Too large**: the frame did not fit in `CONFIG_VIDEO_CAPTURE_JPEG_BUF_SIZE`.
- **Failed to encode**: the encoder had an error. It resets itself and carries on with the next frame.

## Memory

The video buffer pool takes 26.4 MB of the 32 MB PSRAM, for 4 capture buffers at 2.35 MB and 64 JPEG buffers at 256 kB. The JPEG buffers cover a little over 2 seconds of card stalls at 30 frames per second. Changing the size or number of buffers can also mean changing `CONFIG_VIDEO_BUFFER_POOL_HEAP_SIZE` and `CONFIG_VIDEO_BUFFER_POOL_NUM_MAX`.

The JPEG codec needs its input as 16x16 blocks, so the width and height must be multiples of 16, and 1080 lines is not possible. Other sizes up to the sensor's 2592x1944 can be set with `CONFIG_VIDEO_CAPTURE_WIDTH` and `CONFIG_VIDEO_CAPTURE_HEIGHT`, and the sensor image is cropped to match their aspect ratio.

## Why not Zephyr's JPEG driver?

It moves data through the codec's FIFOs by interrupt, 16 bytes at a time, which at this rate is over 4 million interrupts per second, or under 200 CPU cycles for each. `src/jpeg_dma.c` uses the codec over HPDMA instead. This builds without `CONFIG_VIDEO_ENCODER_JPEG`, so that the driver and its HAL callbacks stay out of the image.

The STM32Cube HAL has a function for this, `HAL_JPEG_Encode_DMA()`, but it is not used here. Restarting its output DMA between chunks can put junk in the middle of the JPEG, and a chunk that ends along with the frame makes the encode hang. So `src/jpeg_dma.c` starts and ends each frame itself, with one DMA transfer for the whole JPEG. The comments there explain this, along with what the HPDMA and the codec need that is not obvious from the HAL: a compartment ID on each channel, 8 word bursts, and setting the input DMA enable again for each strip.
