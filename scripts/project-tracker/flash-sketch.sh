#!/usr/bin/env bash
# Flash one sketch's APPLICATION partition, leaving bootloader/partitions intact.
#
# 0x10000 is the app partition and ONLY that. The bootloader and partition
# table on this board are known good; overwriting them bricks a board that
# otherwise boots. Do not "fix" this by uploading a full image.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
SKETCH="${1:?usage: flash-sketch.sh <sketch-dir>}"
# Resolve the sketch in either location, so the product sketch is flashable with
# the same one-line procedure as a baseline regression sketch.
SRC="$ROOT/firmware/baseline/$SKETCH"
[ -d "$SRC" ] || SRC="$ROOT/firmware/$SKETCH"
[ -d "$SRC" ] || { echo "no such sketch: firmware/baseline/$SKETCH or firmware/$SKETCH"; exit 1; }
ARC="${ARDUINO_CLI:-/Applications/Arduino IDE.app/Contents/Resources/app/lib/backend/resources/arduino-cli}"
FQBN="esp32:esp32:esp32s3:UploadSpeed=921600,USBMode=hwcdc,CDCOnBoot=cdc,MSCOnBoot=default,DFUOnBoot=default,UploadMode=default,CPUFreq=240,FlashMode=dio,FlashSize=16M,PartitionScheme=app3M_fat9M_16MB,DebugLevel=none,PSRAM=enabled,LoopCore=1,EventsCore=1,EraseFlash=none,JTAGAdapter=default,ZigbeeMode=default"
PORT="${FLASH_PORT:-/dev/cu.wchusbserial58750034621}"
BUILD="/tmp/nomadlink-flash-$SKETCH"
[ -f "$SRC/secrets.h" ] || { [ -f "$SRC/secrets.h.example" ] && cp "$SRC/secrets.h.example" "$SRC/secrets.h"; }
"$ARC" compile --fqbn "$FQBN" --build-path "$BUILD" "$SRC"
python3 -m esptool --chip esp32s3 -p "$PORT" -b 921600 write_flash 0x10000 "$BUILD/$SKETCH.ino.bin"
echo "flashed $SKETCH (app partition only). Console: ${CONSOLE_PORT:-/dev/cu.usbmodem58750034621}"
