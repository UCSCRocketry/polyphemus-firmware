# polyphemus-firmware

Flight firmware for the UCSC Rocketry gimballed camera payload, "Polyphemus".

## First-time setup
### MacOS/Linux
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

### Windows
```sh
# 1. Create a Python virtual environment where the workspace will go.
py -3.12 -m venv payload-ws\.venv
payload-ws\.venv\Scripts\activate.bat
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
cd payload-firmware/examples/still_capture
west build -p always
```

## Loading onto the Nucleo

`scripts/jlink.py` loads a built image into the Nucleo over a J-Link on its
MIPI20 connector (CN1) and starts it. From an example's directory:

```sh
python3 ../../scripts/jlink.py load
```

It loads `build/zephyr/zephyr.elf` unless given another ELF.
`jlink.py read ADDRESS LENGTH OUTPUT` saves target memory to a file. The J-Link
device name, interface and speed are in `scripts/jlink.ini`; pass `--config` to
use another file.
