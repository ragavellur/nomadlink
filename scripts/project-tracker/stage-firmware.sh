#!/usr/bin/env bash
# Build the product firmware and stage it into docs/project/firmware/ so the web
# installer serves what we actually compiled.
#
# WHY THIS EXISTS: the installer page was initially pointed at upstream's PREBUILT
# binaries. That looked fine — the page rendered, the manifest resolved, every part
# returned 200 — while every source change had zero effect on what users flashed.
# The AP SSID still read ESP32_NAT_Router after it had been renamed in the source.
# A correct page serving a stale artifact is the exact failure this project already
# records for *.bin in .gitignore. One command now owns the path from source to the
# bytes a user flashes, so source and payload cannot drift apart silently.
#
# The manifest (docs/project/manifest_nomadlink_esp32s3.json) states the offsets;
# these offsets come from upstream's build and are asserted against it below, because
# a wrong offset produces an image that flashes cleanly and then does not boot.
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
SKETCH_DIR="$ROOT/firmware/nat_router"
BUILD_DIR="${NAT_ROUTER_BUILD_DIR:-$SKETCH_DIR/build_esp32s3}"
DEST="$ROOT/docs/project/firmware"

IDF_EXPORT="${NAT_ROUTER_IDF_EXPORT:-$HOME/esp/esp-idf-5.5.4/export.sh}"

if [[ ! -f "$IDF_EXPORT" ]]; then
  echo "ERROR: ESP-IDF 5.5.x export.sh not found at $IDF_EXPORT"
  echo "       The vendored base does not build on IDF 6.x. Set NAT_ROUTER_IDF_EXPORT."
  exit 1
fi
if [[ -z "${SSL_CERT_FILE:-}" && -f "/Library/Frameworks/Python.framework/Versions/3.13/lib/python3.13/site-packages/certifi/cacert.pem" ]]; then
  export SSL_CERT_FILE="/Library/Frameworks/Python.framework/Versions/3.13/lib/python3.13/site-packages/certifi/cacert.pem"
fi

# shellcheck disable=SC1090
source "$IDF_EXPORT" >/dev/null 2>&1 || { echo "ERROR: could not source $IDF_EXPORT"; exit 1; }
command -v idf.py >/dev/null 2>&1 || { echo "ERROR: idf.py not on PATH"; exit 1; }

idf_ver="$(idf.py --version 2>/dev/null | grep -oE 'v?[0-9]+\.[0-9]+(\.[0-9]+)?' | head -1)"
idf_ver="${idf_ver#v}"
case "$idf_ver" in
  5.*) ;;
  *) echo "ERROR: ESP-IDF '${idf_ver:-unknown}' but the vendored base requires 5.5.x"; exit 1 ;;
esac

cd "$SKETCH_DIR" || exit 1

if [[ ! -f "$SKETCH_DIR/sdkconfig" || ! -f "$BUILD_DIR/nomadlink.bin" ]]; then
  idf.py -B "$BUILD_DIR" -DIDF_TARGET=esp32s3 set-target esp32s3 > "$BUILD_DIR.set-target.log" 2>&1 \
    || { echo "ERROR: set-target failed"; tail -20 "$BUILD_DIR.set-target.log"; exit 1; }
fi

echo "Building product firmware (ESP-IDF $idf_ver)..."
idf.py -B "$BUILD_DIR" build > "$BUILD_DIR.build.log" 2>&1
rc=$?
if [[ $rc -ne 0 ]]; then
  echo "ERROR: build failed (exit $rc)"
  grep -B2 -A6 -E "error:|Error [0-9]|FAILED" "$BUILD_DIR.build.log" | head -40
  exit 1
fi

app="$BUILD_DIR/nomadlink.bin"
[[ -f "$app" ]] || { echo "ERROR: build reported success but produced no $app"; exit 1; }

# Confirm the flash offsets the manifest promises match what IDF actually produced.
# These are the addresses printed by idf.py itself; if they drift, the manifest must
# be updated in the same commit or the image will flash cleanly and then not boot.
# flash_args lists one "0xADDR file" pair per line, in build order (not flash order).
flash_args="$BUILD_DIR/flash_args"
[[ -f "$flash_args" ]] || { echo "ERROR: $flash_args missing — cannot verify offsets"; exit 1; }

check_offset() { # $1=basename, $2=expected offset
  # flash_args paths are build-relative (bootloader/bootloader.bin), so match on the
  # basename rather than the whole path.
  local got
  got="$(awk -v f="$1" '{n=$2; sub(/.*\//, "", n); if (n == f) {print $1; exit}}' "$flash_args")"
  if [[ "$got" != "$2" ]]; then
    echo "ERROR: $1 is at '$got' in flash_args, but the manifest declares '$2'."
    echo "       Update docs/project/manifest_nomadlink_esp32s3.json and this check together."
    exit 1
  fi
}

check_offset bootloader.bin              0x0
check_offset partition-table.bin        0x8000
check_offset ota_data_initial.bin       0xf000
check_offset nomadlink.bin              0x20000

mkdir -p "$DEST" || exit 1
cp "$BUILD_DIR/bootloader/bootloader.bin"                  "$DEST/bootloader.bin"          || exit 1
cp "$BUILD_DIR/partition_table/partition-table.bin"        "$DEST/partition-table.bin"    || exit 1
cp "$BUILD_DIR/ota_data_initial.bin"                       "$DEST/ota_data_initial.bin"   || exit 1
cp "$app"                                                   "$DEST/nomadlink.bin"          || exit 1

echo
echo "Staged to docs/project/firmware/ (offsets verified: 0x0 0x8000 0xf000 0x20000):"
for f in bootloader.bin partition-table.bin ota_data_initial.bin nomadlink.bin; do
  printf '  %-24s %10s bytes\n' "$f" "$(wc -c < "$DEST/$f" | tr -d ' ')"
done
echo
echo "Now update docs/project/firmware.json flash_parts sizes and verification, then 'make render'."
