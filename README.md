# polyphemus-firmware

Flight firmware for the UCSC Rocketry gimballed camera payload, "Polyphemus".

## First-time setup
Do not `git clone` this repository! Doing so will make things break.

This setup will create the Zephyr workspace (/payload-ws/) in whichever directory you run the following commands. It is recommended that this workspace be your home folder, or some other folder that you dedicate to this work.

### MacOS/Linux
```sh
# 1. Create a Python virtual environment where the workspace will go.
python3 -m venv ~/payload-ws/.venv # Will store your workspace in your home/user folder
source payload-ws/.venv/bin/activate
pip install west

# 2. Create the workspace, with this repo as the manifest
west init -m https://github.com/UCSCRocketry/polyphemus-firmware --mr main payload-ws
cd payload-ws

# 3. Fetch Zephyr and modules
west update

# 4. Install python dependencies
west packages pip --install

# 5. Register zephyr install
west zephyr-export
```

### Windows (CMD)
```sh
# 1. Create a Python virtual environment where the workspace will go.
cd %HOMEPATH% # Will store your workspace in your home/user folder
py -3.12 -m venv payload-ws\.venv
payload-ws\.venv\Scripts\activate.bat
pip install west

# 2. Create the workspace, with this repo as the manifest
west init -m https://github.com/UCSCRocketry/polyphemus-firmware --mr main payload-ws
cd payload-ws

# 3. Fetch Zephyr and modules
west update

# 4. Install python dependencies
cmd /c zephyr\scripts\utils\west-packages-pip-install.cmd

# 5. Register zephyr install
west zephyr-export
```

### Windows (Powershell)
```sh
# 1. Create a Python virtual environment where the workspace will go.
cd $Env:HOMEPATH # Will store your workspace in your home/user folder
py -3.12 -m venv payload-ws\.venv
Set-ExecutionPolicy -ExecutionPolicy RemoteSigned -Scope CurrentUser
payload-ws\.venv\Scripts\Activate.ps1
pip install west

# 2. Create the workspace, with this repo as the manifest
west init -m https://github.com/UCSCRocketry/polyphemus-firmware --mr main payload-ws
cd payload-ws

# 3. Fetch Zephyr and modules
west update

# 4. Install python dependencies
python -m pip install @((west packages pip) -split ' ')

# 5. Register zephyr install
west zephyr-export
```

You may need to install the Zephyr SDK if you haven't done so already. To do so, follow [this section](https://docs.zephyrproject.org/latest/develop/getting_started/index.html#install-the-zephyr-sdk) in Zephyr's official getting started guide.

## Building Hello World

Within the examples folder in our polyphemus-firmware are several examples. Each one is essentially a program we can flash on a board. We will start with the hello_world example:
```sh
cd polyphemus-firmware/examples/hello_world
```

As these examples may have support for multiple boards, we must configure the compiler to build for the right board. Within the `hello_world` example folder there is a file called `CMakeLists.txt`. There you can set your board by uncommenting the correct `set(BOARD ...)`. Make sure only one of these is uncommented at the end, for example if you have an F411 board your `CMakeLists.txt` file should look like this:
```sh
# SPDX-License-Identifier: Apache-2.0

# set(BOARD nucleo_n657x0_q)
set(BOARD nucleo_f411re)
# set(BOARD nucleo_f446re)
# set(BOARD nucleo_l073rz)

cmake_minimum_required(VERSION 3.28.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(hello_world)

target_sources(app PRIVATE src/main.c)
```

Ensure that you save the file, then you can build using this command:
```sh
west build -p always
```

`-p always` just means to do a pristine ("clean") build every time we build.

If it is successful, you should see there is now a `zephyr.elf` file within `hello_world/build/zephyr`. This is your binary, containing the application. To run it on the board, you must flash it to the board first in the following section.

## Flashing a binary onto the Nucleo

First, you must ensure you have the correct board selected in the `scripts/jlink.ini` file within `polyphemus-firmware`. For example, if you have an F411 board your `jlink.ini` file should look like this (`;` is the comment character here):
```sh
# J-Link settings for jlink.py

[target]
; device = STM32N657X0
; device = STM32F446RE
device = STM32F411RE
; device = STM32L073RZ
interface = SWD
speed = 4000
```

Make sure your board is connected to your computer properly. Now from any example's directory, you may run:

```sh
python3 ../../scripts/jlink.py load
```

It loads `build/zephyr/zephyr.elf` unless given another ELF.
