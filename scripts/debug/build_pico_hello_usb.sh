#!/usr/bin/env bash
# Builds pico-examples' hello_usb for one pico-sdk version, the way the "Test Pi Pico SDK" workflow does, into ~/pico/pico-examples/build/hello_world/usb/hello_usb.uf2.
#   scripts/debug/build_pico_hello_usb.sh 1.2.0      then:  rp2040py micropython --image ~/pico/pico-examples/build/hello_world/usb/hello_usb.uf2 --expect-text "Hello, world!"
# Needs cmake and an ARM toolchain (apt: gcc-arm-none-eabi libnewlib-arm-none-eabi build-essential). The old TinyUSB of SDK 1.2.0-1.4.0 drives the USB controller differently from the new
# one, so it found what MicroPython / CircuitPython / Kaluma did not (record 0098, USB).
set -euo pipefail
SDK_VERSION=${1:?pico-sdk version, e.g. 1.2.0}
mkdir -p ~/pico && cd ~/pico
rm -rf pico-sdk pico-examples
git clone -q https://github.com/raspberrypi/pico-sdk.git --depth 1 --branch "$SDK_VERSION"
(cd pico-sdk && git submodule update -q --init)
git clone -q https://github.com/raspberrypi/pico-examples.git --depth 1 --branch "sdk-$SDK_VERSION"
cd pico-examples && mkdir build && cd build
PICO_SDK_PATH=~/pico/pico-sdk cmake .. > /dev/null
make -C hello_world/usb -j"$(nproc)"
ls -l hello_world/usb/hello_usb.uf2
