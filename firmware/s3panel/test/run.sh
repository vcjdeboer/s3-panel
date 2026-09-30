#!/bin/sh
# Host-side tests for the firmware's pure C++ (no board, no Arduino core).
# Needs a C++17 compiler and ArduinoJson's headers; set ARDUINOJSON_SRC to
# point elsewhere than arduino-cli's library directory, and CXX to pick the
# compiler (on macOS, one whose headers match the SDK).
set -eu
here=$(cd "$(dirname "$0")" && pwd)
aj=${ARDUINOJSON_SRC:-"$(arduino-cli config get directories.user)/libraries/ArduinoJson/src"}
cxx=${CXX:-c++}
out=$(mktemp -d)
trap 'rm -rf "$out"' EXIT

"$cxx" -std=c++17 -Wall -Wextra -I"$here" -I"$here/.." -o "$out/util" \
  "$here/util_test.cpp" "$here/../util.cpp"
"$out/util"

if [ -f "$here/profile_parse_test.cpp" ]; then
  "$cxx" -std=c++17 -Wall -Wextra -I"$here" -I"$here/.." -I"$aj" -o "$out/parse" \
    "$here/profile_parse_test.cpp" "$here/../profile_parse.cpp" "$here/../util.cpp"
  (cd "$here" && "$out/parse")
fi
