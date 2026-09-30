#!/usr/bin/env bash
# Compile the product firmware: firmware/nat_router (vendored esp32_nat_router base).
#
# The product firmware is ESP-IDF, not Arduino. The base is vendored upstream
# source (esp32_nat_router 2.4.17, commit 2fe7a4c) and must compile unmodified
# before any NomadLink feature is added on top of it — otherwise "the base works"
# is an unfalsifiable claim. See firmware/nat_router/README.md and AGENTS.md.
#
# This target exists because of a real gap, not for tidiness. The previous product
# sketch (firmware/nomadlink) was Arduino; build-baseline.sh loops
# firmware/baseline/* only, and for a long while nothing compiled the product at
# all. A product tree with no compile gate rots silently. See AGENTS.md.
#
# Requires ESP-IDF 5.5.x. It does NOT build on 6.x: main requires component
# `json`, which moved into the component manager in IDF 6
# ("Failed to resolve component 'json' required by component 'main': unknown
# name"). Do not "fix" that by editing the component list — that diverges from the
# vendored base. Point IDF_PATH at a 5.5.x install instead.
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
SKETCH_DIR="$ROOT/firmware/nat_router"
BUILD_DIR="${NAT_ROUTER_BUILD_DIR:-$SKETCH_DIR/build_esp32s3}"

# The reference targets 5.5.x. A 6.x default on PATH would fail with the `json`
# component error above, which reads like a code problem but is a toolchain one.
IDF_EXPORT="${NAT_ROUTER_IDF_EXPORT:-$HOME/esp/esp-idf-5.5.4/export.sh}"

if [[ ! -d "$SKETCH_DIR" ]]; then
  echo "SKIP: no product firmware at $SKETCH_DIR"
  exit 0
fi

if [[ ! -f "$IDF_EXPORT" ]]; then
  echo "SKIP: ESP-IDF 5.5.x export.sh not found at $IDF_EXPORT"
  echo "      (set NAT_ROUTER_IDF_EXPORT; the base does not build on IDF 6.x)"
  exit 0
fi

# This machine's system python cannot verify TLS without an explicit CA bundle,
# which breaks IDF's tool downloads. Harmless when already installed.
if [[ -z "${SSL_CERT_FILE:-}" && -f "/Library/Frameworks/Python.framework/Versions/3.13/lib/python3.13/site-packages/certifi/cacert.pem" ]]; then
  export SSL_CERT_FILE="/Library/Frameworks/Python.framework/Versions/3.13/lib/python3.13/site-packages/certifi/cacert.pem"
fi

# shellcheck disable=SC1090
if ! source "$IDF_EXPORT" >/dev/null 2>&1; then
  echo "SKIP: could not source $IDF_EXPORT (is ESP-IDF 5.5.x installed there?)"
  exit 0
fi

if ! command -v idf.py >/dev/null 2>&1; then
  echo "SKIP: idf.py not on PATH after sourcing $IDF_EXPORT"
  exit 0
fi

# idf.py --version prints "ESP-IDF v5.5.4", not a bare number.
idf_ver="$(idf.py --version 2>/dev/null | grep -oE 'v?[0-9]+\.[0-9]+(\.[0-9]+)?' | head -1)"
idf_ver="${idf_ver#v}"
case "$idf_ver" in
  5.*) ;;
  *) echo "SKIP: ESP-IDF '${idf_ver:-unknown}', but the vendored base requires 5.5.x"
     exit 0 ;;
esac

# idf.py must run from the project directory holding CMakeLists.txt, not from
# wherever make invoked this script. Running it from the repo root fails with
# "CMakeLists.txt not found in project directory <root>" — and a log-pattern gate
# did not catch that string, so the gate reported success having built nothing.
cd "$SKETCH_DIR" || exit 1

# idf.py writes sdkconfig to the PROJECT ROOT, not the build dir. Guarding on
# "$BUILD_DIR/sdkconfig" made this re-run `set-target` on every invocation, and
# set-target full-cleans — so the build was wiped and then never re-run. Guard on
# the app binary instead.
app="$BUILD_DIR/nomadlink.bin"
sdkcfg="$SKETCH_DIR/sdkconfig"

if [[ ! -f "$sdkcfg" || ! -f "$app" ]]; then
  idf.py -B "$BUILD_DIR" -DIDF_TARGET=esp32s3 set-target esp32s3 \
    > "$BUILD_DIR.set-target.log" 2>&1
  if [[ $? -ne 0 ]]; then
    echo "  x nat_router set-target FAILED (ESP-IDF $idf_ver)"
    tail -20 "$BUILD_DIR.set-target.log" | sed 's/^/      /'
    exit 1
  fi
fi

# Trust the exit code, not a grep over the log. A log-pattern gate silently passed
# a build that produced no binary at all.
idf.py -B "$BUILD_DIR" build > "$BUILD_DIR.build.log" 2>&1
rc=$?

if [[ $rc -ne 0 ]]; then
  echo "  x nat_router FAILED (ESP-IDF $idf_ver, exit $rc)"
  grep -B2 -A6 -E "error:|Error [0-9]|FAILED|not found" "$BUILD_DIR.build.log" \
    | head -40 | sed 's/^/      /'
  exit 1
fi

if [[ ! -f "$app" ]]; then
  echo "  x nat_router reported success but produced no $app"
  echo "      A clean build is required; 'ok' with no artifact is not a pass."
  exit 1
fi

bytes="$(wc -c < "$app" | tr -d ' ')"
size="$(grep -oE 'nomadlink\.bin binary size 0x[0-9a-f]+' "$BUILD_DIR.build.log" | tail -1 | grep -oE '0x[0-9a-f]+')"
echo "  o nat_router ok (ESP-IDF $idf_ver, app $size / $bytes bytes)"
exit 0
# --- Staged-payload freshness check -------------------------------------------
# The installer serves docs/project/firmware/*.bin. If those are older than the
# sources they came from, the page flashes a stale image while the repository reads
# as changed: the SSID stayed ESP32_NAT_Router after being renamed in the source,
# because the payload was upstream's prebuilt binary. Detect that, don't trust it.
DEST="$ROOT/docs/project/firmware"
staged="$DEST/nomadlink.bin"
if [[ -f "$staged" ]]; then
  newest_src="$(find "$SKETCH_DIR/main" "$SKETCH_DIR/include" "$SKETCH_DIR/components" \
                  "$SKETCH_DIR/sdkconfig.defaults" "$SKETCH_DIR/sdkconfig.defaults.esp32s3" \
                  -type f -newer "$staged" 2>/dev/null | head -1)"
  if [[ -n "$newest_src" ]]; then
    echo "     ^ WARNING: the staged payload is older than ${newest_src#$ROOT/}"
    echo "       Run 'make stage-firmware'. Until then the web installer flashes the OLD image."
  fi
fi
