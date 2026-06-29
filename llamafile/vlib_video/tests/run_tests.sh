#!/bin/sh
# Build + run the vlib-video unit tests as standalone cosmocc (APE) binaries.
# These exercise the llamafile-owned vlib-video lib without a model load:
#   tool-parser 21/21, pixel-diff 4/4, mrope U4b rewind-sync=3, pair-packing 252.
#
# Usage: llamafile/vlib_video/tests/run_tests.sh
# (run from the repo root; uses the bundled cosmocc toolchain)
set -e

HERE=$(cd "$(dirname "$0")" && pwd)        # .../llamafile/vlib_video/tests
LIB=$(dirname "$HERE")                       # .../llamafile/vlib_video
COSMO=${COSMO:-.cosmocc/4.0.2/bin/cosmoc++}
OUT=$(mktemp -d)

echo "== tool parser =="
$COSMO -std=c++17 -I"$LIB" "$HERE/test_tool_parser.cpp" "$LIB/vlib_video_tool_parser.cpp" -o "$OUT/tp"
"$OUT/tp" "$LIB/fixtures/tool_calls.jsonl"

echo "== pixel diff filter =="
$COSMO -std=c++17 -I"$LIB" "$HERE/test_pixel_diff_filter.cpp" "$LIB/vlib_video_frame_filter.cpp" -o "$OUT/pd"
"$OUT/pd"

echo "== mrope rewind-sync (U4b) =="
$COSMO -std=c++17 -I"$LIB" "$HERE/test_mrope.cpp" -o "$OUT/mr"
"$OUT/mr"

echo "== pair packing =="
$COSMO -std=c++17 -I"$LIB" "$HERE/test_pair_packing.cpp" -o "$OUT/pp"
"$OUT/pp"

echo "ALL VLIB-VIDEO TESTS PASSED"
