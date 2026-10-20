# still_capture

Take one still from the B-CAMS-IMX through the DCMIPP, encode it with the STM32N6 hardware JPEG encoder, and read it back over SWD. Made for the N657x0 boards.

```c
struct still_capture_result {
        uint32_t magic;    /* "ZPIC" once the rest is valid */
        uint32_t width;
        uint32_t height;
        uint32_t length;
        uint32_t crc32;
        uint32_t address; /* address of photo bytes */
};
```

`scripts/jlink.py load` loads the image into the N6's RAM and starts it. `download.py` copies the picture out of target RAM into a file over SWD.

## Building and running

Ensure the B-CAMS-IMX is plugged into the board's CSI connector (CN6). You must have a J-link debugger on hand and it should be connected to the Nucleo board's MIPI20 connector (CN1).

Run from this directory:

```console
$ west build -p always
$ python3 ../../scripts/jlink.py load
$ python3 download.py
```

By default, the build files and output photo are located within this directory. On some systems you may skip putting the `python3`.

## Memory

Because the Nucleo has no external RAM large enough, the video buffer pool is configured to live in the whole of AXISRAM3 through AXISRAM6, which is a 4x448 kB memory bank that is formed as a contiguous 1792 kB linker region. The default 640x480 capture uses around 1.5 MB of this memory, spread over two capture buffers to store the raw NV12 image data, and then one JPEG buffer to store the output JPEG. Higher resolutions are not possible due to limited space.

The STM32N6 DK board has an onboard 256 Mb PSRAM device which is used in this example as the video/image buffer.

`CONFIG_STILL_CAPTURE_NUM_BUFS`, which is by default 2, configures the number of raw buffers used in the DCMIPP.
