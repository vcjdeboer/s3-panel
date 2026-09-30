#!/bin/sh
# Host-side tests for the firmware's pure C++ (no board, no Arduino core).
# Needs a C++17 compiler and ArduinoJson's headers; set ARDUINOJSON_SRC to
# point elsewhere than arduino-cli's library directory.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
aj=${ARDUINOJSON_SRC:-"$(arduino-cli config get directories.user)/libraries/ArduinoJson/src"}
out=$(mktemp -d)
trap 'rm -rf "$out"' EXIT

c++ -std=c++17 -Wall -Wextra -I"$here" -I"$here/.." -o "$out/util" \
  "$here/util_test.cpp" "$here/../util.cpp"
"$out/util"

if [ -f "$here/profile_parse_test.cpp" ]; then
  c++ -std=c++17 -Wall -Wextra -I"$here" -I"$here/.." -I"$aj" -o "$out/parse" \
    "$here/profile_parse_test.cpp" "$here/../profile_parse.cpp" "$here/../util.cpp"
  (cd "$here" && "$out/parse")
fi
