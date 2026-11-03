# polyphemus-firmware

Flight firmware for the UCSC Rocketry gimballed camera payload, "Polyphemus".

## First-time setup

```sh
# 1. Create a Python virtual environment where the workspace will go.
python3 -m venv payload-ws/.venv
source payload-ws/.venv/bin/activate
pip install west

# 2. Create the workspace, with this repo as the manifest
west init -m https://github.com/UCSCRocketry/polyphemus-firmware --mr main payload-ws
cd payload-ws

# 3. Fetch Zephyr and modules
west update

# 4. Register zephyr install
west zephyr-export

# 5. Install python dependencies
pip install -r zephyr/scripts/requirements.txt
```

## Building an example

Each example builds and runs from its own directory; see its README for details.

```sh
cd polyphemus-firmware/examples/still_capture
west build -p always
```

## Loading onto a board

`scripts/jlink.py` loads a built image into the board's RAM over a J-Link and
starts it. On the Nucleo, the J-Link goes on the MIPI20 connector (CN1). The
STM32N6570-DK examples build for `stm32n6570_dk/stm32n657xx/sb`, which runs from
RAM in the same way. From an example's directory:

```sh
python3 ../../scripts/jlink.py load
```

It loads `build/zephyr/zephyr.elf` unless given another ELF.
`jlink.py read ADDRESS LENGTH OUTPUT` saves target memory to a file. The J-Link
device name, interface and speed are in `scripts/jlink.ini`; pass `--config` to
use another file.
