#!/usr/bin/env bash
# Compile the product firmware sketch (firmware/nomadlink).
#
# This target exists because of a real gap, not for tidiness. build-baseline.sh
# loops firmware/baseline/* only, so firmware/nomadlink/nomadlink.ino — 1300
# lines of SoftAP + PPP + NAPT + console — was outside the commit gate and could
# rot silently. The project's own rule is that a sketch which stops compiling is
# a broken gate; a product sketch with no gate at all is worse. See AGENTS.md.
#
# The same FQNB as the baseline: PartitionScheme MUST be app3M_fat9M_16MB. A
# default_8MB build compiles fine and then does not boot, which a compile gate
# cannot catch — only flashing can.
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
ARC="${ARDUINO_CLI:-/Applications/Arduino IDE.app/Contents/Resources/app/lib/backend/resources/arduino-cli}"
FQBN="esp32:esp32:esp32s3:UploadSpeed=921600,USBMode=hwcdc,CDCOnBoot=cdc,MSCOnBoot=default,DFUOnBoot=default,UploadMode=default,CPUFreq=240,FlashMode=dio,FlashSize=16M,PartitionScheme=app3M_fat9M_16MB,DebugLevel=none,PSRAM=enabled,LoopCore=1,EventsCore=1,EraseFlash=none,JTAGAdapter=default,ZigbeeMode=default"
SKETCH_DIR="$ROOT/firmware/nomadlink"

export ARDUINO_DATA_DIR="${ARDUINO_DATA_DIR:-$HOME/Library/Arduino15}"
export ARDUINO_SKETCHBOOK_DIR="${ARDUINO_SKETCHBOOK_DIR:-$HOME/Documents/Arduino}"

if [[ ! -x "$ARC" ]]; then
  echo "SKIP: arduino-cli not found at $ARC (set ARDUINO_CLI to override)"
  exit 0
fi

if [[ ! -d "$SKETCH_DIR" ]]; then
  echo "SKIP: no product sketch at $SKETCH_DIR"
  exit 0
fi

out="$("$ARC" compile --fqbn "$FQBN" --build-path /tmp/nomadlink-build-product "$SKETCH_DIR" 2>&1)"
if echo "$out" | grep -q "error:"; then
  echo "  x nomadlink FAILED"
  echo "$out" | grep -A2 "error:" | head -20 | sed 's/^/      /'
  exit 1
fi

echo "  o nomadlink ok ($(echo "$out" | grep -o 'Sketch uses [0-9]* bytes' | head -1))"
exit 0
