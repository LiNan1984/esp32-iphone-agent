#!/usr/bin/env bash
# Build and flash the board (needs arduino-cli and the esp32 core 3.3.x).
# Usage: hardware/flash.sh [port] [--debug] [--sta] [--build-only]
#   --debug       log commands and the AirPlay handshake on USB serial
#   --sta         test build: join the Wi-Fi in hid_bridge/secrets.h instead of opening a hotspot
#   --build-only  compile only (prints flash/RAM usage)
#   --ap-tune     A/B build: hotspot on a 20 MHz channel with 300 TU beacons (more airtime for BLE)
#   --p4 / --s3   target chip; --s3 is the default and keeps the original behaviour
# BLE host: h2zero NimBLE-Arduino (arduino-cli lib install "NimBLE-Arduino"); its buffer sizes live in
# hid_bridge/nimble_cfg.h, force-included into every C/C++ file below.
# A running bridge is a TinyUSB device, so it is first kicked into the ROM
# bootloader with a 1200-baud touch; a blank board is flashed directly.
#
# Targets: --s3 (default, the original XIAO ESP32S3 board) or --p4 (ESP32-P4).
# The P4 port lives in docs/superpowers/specs/2026-11-07-p4-port-design.md; the
# short version is that P4 has no radio of its own and needs an ESP32-C6 over
# SDIO, so --p4 alone does not give a working board.
set -euo pipefail
cd "$(dirname "$0")"
TARGET="${TARGET:-s3}"
ports() { for p in /dev/cu.usbmodem* /dev/ttyACM*; do [[ -e $p ]] && echo "$p"; done; return 0; }
PORT="" DEFS="" BUILD_ONLY=0
for a in "$@"; do
  case $a in
    --debug) DEFS+=" -DDEBUG_SERIAL=1" ;;
    --sta) [[ -f hid_bridge/secrets.h ]] || { echo "--sta needs hid_bridge/secrets.h (see secrets.example.h)"; exit 1; }
           DEFS+=" -DSTA_FROM_SECRETS=1" ;;
    --build-only) BUILD_ONLY=1 ;;
    --ap-tune) DEFS+=" -DAP_TUNE=1" ;;
    --crash-test) DEFS+=" -DOTA_CRASH_TEST=1" ;;  # build only, then push it with ota_push.py
    --small-gop) DEFS+=" -DGOP_CAP_KB=256" ;;  # test build: the GOP fills (freezes) within minutes
    --p4) TARGET=p4 ;;
    --s3) TARGET=s3 ;;
    *) PORT=$a ;;
  esac
done

# DebugLevel=info is load-bearing, not cosmetics: without it arduino-cli builds
# with the log level at ERROR and every log_i()/Serial.printf() is dropped at
# COMPILE time, so a running firmware looks silent. Both targets need it.
case $TARGET in
  s3) FQBN="esp32:esp32:esp32s3:USBMode=default,CDCOnBoot=cdc,FlashSize=8M,PSRAM=opi,PartitionScheme=default_8MB,DebugLevel=info" ;;
  p4)
    # ChipVariant=prev3 is required for silicon below rev v3.0: it selects the
    # esp32p4_es variant (360 MHz, different memory layout and prebuilt libs).
    # PSRAM=enabled, not opi — P4 has 16-line HEX PSRAM and no opi/qspi choice.
    # app13M_data7M_32MB keeps the same shape as the S3 default_8MB table:
    # otadata + two ota app slots + coredump.
    FQBN="esp32:esp32:esp32p4:USBMode=default,CDCOnBoot=cdc,FlashSize=32M,PSRAM=enabled,ChipVariant=prev3,FlashMode=qio,FlashFreq=80,PartitionScheme=app13M_data7M_32MB,DebugLevel=info" ;;
  *) echo "unknown target '$TARGET' (want s3 or p4)"; exit 1 ;;
esac
echo "target: $TARGET"
echo "fqbn:   $FQBN"
[[ $PWD != *" "* ]] || { echo "the -include path below cannot contain spaces: $PWD"; exit 1; }
DEFS+=" -include $PWD/hid_bridge/nimble_cfg.h"
FLAGS=(--build-property "compiler.cpp.extra_flags=$DEFS" --build-property "compiler.c.extra_flags=$DEFS")

arduino-cli compile -b "$FQBN" "${FLAGS[@]}" --output-dir build hid_bridge
(( BUILD_ONLY )) && exit 0
BEFORE=$(ports)
PORT=${PORT:-$(head -n 1 <<<"$BEFORE")}
[[ -n $PORT ]] || { echo "no board found (looked for /dev/cu.usbmodem* and /dev/ttyACM*)"; exit 1; }
stty -f "$PORT" 1200 2>/dev/null || stty -F "$PORT" 1200 2>/dev/null || true
# The ROM bootloader may come back under a new name. Follow the port that appeared after the
# reset rather than the first one listed, so other connected boards are never picked up.
new_ports() { comm -13 <(sort <<<"$BEFORE") <(ports | sort); }
for _ in $(seq 20); do sleep 0.5; [[ -n $(new_ports) ]] && break; done
sleep 1  # let the ROM bootloader's serial come up
NEW=$(new_ports)
case $(grep -c . <<<"$NEW" || true) in
  0) [[ -e $PORT ]] || { echo "$PORT is gone after the reset and no new port appeared"; exit 1; } ;;
  1) PORT=$NEW ;;
  *) echo "several ports appeared after the reset, not guessing:" $NEW; exit 1 ;;
esac
arduino-cli upload -b "$FQBN" -p "$PORT" --input-dir build hid_bridge
