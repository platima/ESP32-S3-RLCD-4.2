#!/usr/bin/env bash
# Builds and runs the host-side logic tests.  Run inside Linux or WSL.
#
# Needs the ArduinoJson headers (v7).  By default they are looked for in the
# Arduino sketchbook; set ARDUINOJSON_SRC to point at ArduinoJson/src otherwise.
set -euo pipefail
cd "$(dirname "$0")"

# AddressSanitizer binaries can spin forever at startup on recent Linux/WSL2
# kernels with high ASLR entropy; run them with ASLR off where that is allowed.
run() {
  if setarch "$(uname -m)" -R true 2>/dev/null; then
    setarch "$(uname -m)" -R "$@"
  else
    "$@"
  fi
}

AJ="${ARDUINOJSON_SRC:-$HOME/Arduino/libraries/ArduinoJson/src}"
if [ ! -f "$AJ/ArduinoJson.h" ]; then
  echo "ArduinoJson not found at $AJ - set ARDUINOJSON_SRC to its src directory" >&2
  exit 2
fi

FLAGS="-std=c++17 -O1 -g -Wall -Wextra -Wno-unused-parameter -Wno-maybe-uninitialized -fsanitize=address,undefined"

mkdir -p build
g++ $FLAGS -isystem "$AJ" -I../.. \
    test_logic.cpp ../../weather.cpp ../../spotify_parse.cpp ../../link_page.cpp \
    -o build/test_logic
g++ $FLAGS -I../.. \
    test_features.cpp ../../settings.cpp ../../tz_table.cpp \
    -o build/test_features
# the battery simulation is a tool, not a test: build it so it cannot rot, and run one short trace
g++ -std=c++17 -O1 -Wall -Wextra -I../.. battery_sim.cpp -o build/battery_sim

run ./build/test_logic
run ./build/test_features
./build/battery_sim trace 3 > /dev/null && echo "battery_sim: builds and runs"
