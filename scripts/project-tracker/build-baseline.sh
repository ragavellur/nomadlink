#!/usr/bin/env bash
# Compile every hardware regression sketch.
#
# A baseline sketch that stops compiling is a broken gate: the hardware
# regression suite is how we detect that a firmware change silently broke a
# proven capability.
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
ARC="${ARDUINO_CLI:-/Applications/Arduino IDE.app/Contents/Resources/app/lib/backend/resources/arduino-cli}"
FQBN="esp32:esp32:esp32s3:UploadSpeed=921600,USBMode=hwcdc,CDCOnBoot=cdc,MSCOnBoot=default,DFUOnBoot=default,UploadMode=default,CPUFreq=240,FlashMode=dio,FlashSize=16M,PartitionScheme=default_8MB,DebugLevel=none,PSRAM=enabled,LoopCore=1,EventsCore=1,EraseFlash=none,JTAGAdapter=default,ZigbeeMode=default"

export ARDUINO_DATA_DIR="${ARDUINO_DATA_DIR:-$HOME/Library/Arduino15}"
export ARDUINO_SKETCHBOOK_DIR="${ARDUINO_SKETCHBOOK_DIR:-$HOME/Documents/Arduino}"

if [[ ! -x "$ARC" ]]; then
  echo "SKIP: arduino-cli not found at $ARC (set ARDUINO_CLI to override)"
  exit 0
fi

fail=0
for dir in "$ROOT"/firmware/baseline/*/; do
  name="$(basename "$dir")"
  ino="$(find "$dir" -maxdepth 1 -name '*.ino' | head -1)"
  [[ -z "$ino" ]] && continue
  # Provide a template secrets.h if the sketch includes one; it stays gitignored.
  if grep -q '#include "secrets.h"' "$ino" && [[ ! -f "$dir/secrets.h" ]]; then
    [[ -f "$dir/secrets.h.example" ]] && cp "$dir/secrets.h.example" "$dir/secrets.h"
  fi
  out="$("$ARC" compile --fqbn "$FQBN" --build-path /tmp/nomadlink-build-$name "$dir" 2>&1)"
  if echo "$out" | grep -q "error:"; then
    echo "  x $name FAILED"
    echo "$out" | grep "error:" | head -5 | sed 's/^/      /'
    fail=1
  else
    echo "  o $name ok ($(echo "$out" | grep -o 'Sketch uses [0-9]* bytes' | head -1))"
  fi
done
exit $fail
