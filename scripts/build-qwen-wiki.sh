#!/bin/sh
# build-qwen-wiki.sh — assemble the self-contained qwen-wiki.llamafile:
#   the (freshly built) llamafile binary + model + mmproj + Wikipedia ZIM + a baked-in
#   /zip/.args, so it runs with ZERO flags and opens a browser.
#
# Run this after code changes to refresh the bundle (it is NOT rebuilt by `make`,
# since re-embedding the 22 GB model on every build would be wasteful).
#
# Env overrides: OUT, MODEL, MMPROJ, ZIM, PORT, CTX, NP.
#   e.g.  NP=2 CTX=0 scripts/build-qwen-wiki.sh        # 128K x 2 instead of 256K x 1
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

OUT="${OUT:-/Volumes/AI Models at Home/qwen-wiki.llamafile}"
MODEL_LINK="${MODEL:-/Users/ochafik/Data/Models/Qwen3.6-35B-A3B-UD-Q4_K_M.gguf}"
MMPROJ_LINK="${MMPROJ:-/Users/ochafik/Data/Models/Qwen3.6-35B-A3B-mmproj-F16.gguf}"
ZIM_LINK="${ZIM:-/Users/ochafik/Data/Models/wikipedia_en_simple_all_nopic_2026-05.zim}"
PORT="${PORT:-8080}"; CTX="${CTX:-0}"; NP="${NP:-1}"

# zipalign needs the real file, not a symlink (macOS readlink has no -f).
resolve() { if [ -L "$1" ]; then readlink "$1"; else echo "$1"; fi; }
MODEL="$(resolve "$MODEL_LINK")"; MMPROJ="$(resolve "$MMPROJ_LINK")"; ZIM="$(resolve "$ZIM_LINK")"

echo "==> building llamafile binary"
.cosmocc/4.0.2/bin/make -j"$(sysctl -n hw.ncpu 2>/dev/null || echo 8)" o//llamafile/llamafile
ZA="$ROOT/o//third_party/zipalign/zipalign"
[ -x "$ZA" ] || .cosmocc/4.0.2/bin/make o//third_party/zipalign/zipalign

echo "==> assembling $OUT (model + mmproj + zim, mmappable from /zip)"
cp o//llamafile/llamafile "$OUT"
"$ZA" -j0 "$OUT" "$MODEL"
"$ZA" -j0 "$OUT" "$MMPROJ"
"$ZA" -j0 "$OUT" "$ZIM"

# .args: full agentic + wiki + multimodal, native context, auto-open browser, no user flags.
TMP="$(mktemp -d)"
printf -- '--server\n--jinja\n-m\n/zip/%s\n--mmproj\n/zip/%s\n--zim\n/zip/%s\n--agents\n--open-browser\n-c\n%s\n-np\n%s\n--host\n127.0.0.1\n--port\n%s\n...\n' \
  "$(basename "$MODEL")" "$(basename "$MMPROJ")" "$(basename "$ZIM")" "$CTX" "$NP" "$PORT" > "$TMP/.args"
( cd "$TMP" && "$ZA" "$OUT" .args )
rm -rf "$TMP"

echo "==> done: $OUT ($(ls -lh "$OUT" | awk '{print $5}'))"
echo "    run:  sh \"$OUT\"   -> opens http://127.0.0.1:$PORT/  (chat + /agents + clickable /wiki/)"
