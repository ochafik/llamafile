#!/bin/sh
# build-qwen-wiki.sh — assemble the self-contained qwen-wiki.llamafile:
#   the (freshly built) llamafile binary + model + mmproj + a LIST of ZIMs + a
#   baked-in /zip/.args, so it runs with ZERO flags and opens a browser.
#
# Run this after code changes to refresh the bundle (it is NOT rebuilt by `make`,
# since re-embedding the model + ZIMs on every build would be wasteful).
#
# Env overrides: OUT, MODEL, MMPROJ, ZIMS (newline-separated list) or ZIM
#   (single, back-compat), PORT, CTX, NP, PRELOAD_KV.
#   e.g.  MODEL=/path/to/Qwen3.6-...-Q3_K_S-2.71bpw.gguf scripts/build-qwen-wiki.sh   # RPi variant
#         ZIMS="$(printf '%s\n%s' /a.zim /b.zim)" scripts/build-qwen-wiki.sh
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

OUT="${OUT:-/Volumes/AI Models at Home/qwen-wiki.llamafile}"
MODEL_LINK="${MODEL:-/Users/ochafik/Data/Models/Qwen3.6-35B-A3B-UD-Q4_K_M.gguf}"
MMPROJ_LINK="${MMPROJ:-/Users/ochafik/Data/Models/Qwen3.6-35B-A3B-mmproj-F16.gguf}"
PORT="${PORT:-8080}"; CTX="${CTX:-0}"; NP="${NP:-1}"
# Runtime flags Qwen3.6 (hybrid gated-delta-net + SWA) needs to run reliably:
#   -ngl 99   GPU offload (Metal on Mac; falls back to CPU where no GPU) — override NGL=0 for pure-CPU/RPi
#   -fa off   flash-attn is incompatible with the hybrid linear-attention path (SIGBUS)
#   -fit off  the auto device-fit step crashes on this arch (and -fit off is what lets -c 0 / 256K load)
#   --no-warmup  the mmproj/agents warmup decode crashes
NGL="${NGL:-99}"
PRELOAD_KV="${PRELOAD_KV:-0}"
SYSTEM_TXT="$ROOT/scripts/default-system.txt"

# The civilization bundle's ZIMs (newline-separated so paths-with-spaces work).
# Override with ZIMS=... ; ZIM=<single> kept for back-compat.
ZD="/Volumes/zOlive Disk 4T/AI/zims"
if [ -n "$ZIM" ]; then
  ZIMS="$ZIM"
fi
ZIMS="${ZIMS:-$(cat <<EOF
$ZD/wikipedia_en_all_maxi_2026-02.zim
$ZD/wiktionary_en_all_nopic_2026-05.zim
$ZD/wikibooks_en_all_maxi_2026-04.zim
$ZD/wikivoyage_en_all_maxi_2026-06.zim
$ZD/mdwiki_en_all_maxi_2025-11.zim
$ZD/wikem_en_all_maxi_2026-04.zim
$ZD/zimgit-post-disaster_en_2024-05.zim
$ZD/appropedia_en_all_maxi_2026-02.zim
$ZD/ifixit_en_all_2025-12.zim
$ZD/energypedia_en_all_maxi_2026-06.zim
$ZD/survival-docs_en.zim
$ZD/electronics.stackexchange.com_en_all_2026-02.zim
$ZD/diy.stackexchange.com_en_all_2026-02.zim
$ZD/gardening.stackexchange.com_en_all_2026-02.zim
$ZD/biology.stackexchange.com_en_all_2026-02.zim
$ZD/mechanics.stackexchange.com_en_all_2026-02.zim
$ZD/cooking.stackexchange.com_en_all_2026-02.zim
$ZD/outdoors.stackexchange.com_en_all_2026-02.zim
$ZD/woodworking.stackexchange.com_en_all_2026-02.zim
$ZD/ham.stackexchange.com_en_all_2026-02.zim
$ZD/sustainability.stackexchange.com_en_all_2026-02.zim
EOF
)}"

# zipalign needs the real file, not a symlink (macOS readlink has no -f).
resolve() { if [ -L "$1" ]; then readlink "$1"; else echo "$1"; fi; }
MODEL="$(resolve "$MODEL_LINK")"; MMPROJ="$(resolve "$MMPROJ_LINK")"

echo "==> building llamafile binary"
.cosmocc/4.0.2/bin/make -j"$(sysctl -n hw.ncpu 2>/dev/null || echo 8)" o//llamafile/llamafile
ZA="$ROOT/o//third_party/zipalign/zipalign"
[ -x "$ZA" ] || .cosmocc/4.0.2/bin/make o//third_party/zipalign/zipalign

echo "==> assembling $OUT (model + mmproj + ZIMs, mmappable from /zip)"
cp o//llamafile/llamafile "$OUT"
"$ZA" -j0 "$OUT" "$MODEL"
[ -f "$MMPROJ" ] && "$ZA" -j0 "$OUT" "$MMPROJ"

# Embed each ZIM (page-aligned, uncompressed -> mmappable) and accumulate the
# --zim flags for .args. Skip (warn) any missing file so a partial bundle works.
ZIM_ARGS=""
printf '%s\n' "$ZIMS" | while IFS= read -r z; do
  [ -z "$z" ] && continue
  if [ ! -f "$z" ]; then echo "    !! MISSING, skipping: $z" >&2; continue; fi
  echo "    + $(basename "$z") ($(ls -lh "$z" | awk '{print $5}'))"
  "$ZA" -j0 "$OUT" "$z"
done
# (the while-subshell can't export ZIM_ARGS in sh; rebuild it in the main shell)
ZIM_ARGS="$(printf '%s\n' "$ZIMS" | while IFS= read -r z; do
  [ -z "$z" ] && continue; [ -f "$z" ] || continue
  printf -- '--zim\n/zip/%s\n' "$(basename "$z")"
done)"

# Default survival system prompt: embedded so every fresh chat begins with this
# exact text (injected server-side when a request carries no system message).
echo "==> embedding default system prompt (/zip/default-system.txt)"
( cd "$(dirname "$SYSTEM_TXT")" && "$ZA" "$OUT" "$(basename "$SYSTEM_TXT")" )

# ---------------------------------------------------------------------------
# Precomputed system-prompt KV (OPT-IN: PRELOAD_KV=1).
# WARNING: measured ineffective on Qwen3.6 (hybrid gated-delta-net + SWA -> a
# restored slot registers no context checkpoint -> first chat re-prefills anyway,
# REUSED=0; f16 KV also barely deflates). Gated off; works on pure-attention models.
# ---------------------------------------------------------------------------
PRELOAD_ARGS=""
if [ "$PRELOAD_KV" = "1" ]; then
  echo "==> precomputing system-prompt KV (PRELOAD_KV=1)"
  KVDIR="$(mktemp -d)"; PREPORT=$((PORT + 1001)); SYS="$(cat "$SYSTEM_TXT")"
  # shellcheck disable=SC2086
  ZFLAGS="$(printf '%s\n' "$ZIMS" | while IFS= read -r z; do [ -f "$z" ] && printf -- '--zim\n/zip/%s\n' "$(basename "$z")"; done)"
  printf -- '--server\n--jinja\n-m\n/zip/%s\n--agents\n--slot-save-path\n%s/\n-c\n%s\n-np\n1\n-ngl\n0\n--host\n127.0.0.1\n--port\n%s\n%s' \
    "$(basename "$MODEL")" "$KVDIR" "$CTX" "$PREPORT" "$ZFLAGS" | tr '\n' '\0' | xargs -0 "$OUT" > "$KVDIR/server.log" 2>&1 &
  PREPID=$!; trap 'kill "$PREPID" 2>/dev/null; rm -rf "$KVDIR"' EXIT
  for i in $(seq 1 600); do [ "$(curl -s -o /dev/null -w '%{http_code}' "http://127.0.0.1:$PREPORT/health" 2>/dev/null)" = "200" ] && break; sleep 1; done
  TOOLS="$(curl -s "http://127.0.0.1:$PREPORT/tools" | python3 -c 'import json,sys; r=json.load(sys.stdin); print(json.dumps([t.get("definition",t) for t in (r.get("tools",r) if isinstance(r,dict) else r)]))')"
  PREFIX="$(python3 -c 'import json,sys,urllib.request; body=json.dumps({"messages":[{"role":"system","content":sys.argv[1]}],"tools":json.loads(sys.argv[2]),"add_generation_prompt":False}).encode(); print(json.load(urllib.request.urlopen(urllib.request.Request(sys.argv[3]+"/apply-template",body,{"Content-Type":"application/json"})))["prompt"])' "$SYS" "$TOOLS" "http://127.0.0.1:$PREPORT")"
  python3 -c 'import json,sys,urllib.request; urllib.request.urlopen(urllib.request.Request(sys.argv[2]+"/completion",json.dumps({"prompt":sys.argv[1],"n_predict":0,"cache_prompt":True,"id_slot":0}).encode(),{"Content-Type":"application/json"})).read()' "$PREFIX" "http://127.0.0.1:$PREPORT"
  curl -s "http://127.0.0.1:$PREPORT/slots/0?action=save" -H 'Content-Type: application/json' -d '{"filename":"system.kv"}' > "$KVDIR/save.json"
  kill "$PREPID" 2>/dev/null; trap 'rm -rf "$KVDIR"' EXIT
  ( cd "$KVDIR" && zip -9 -q "$OUT" system.kv )
  rm -rf "$KVDIR"; trap - EXIT
  PRELOAD_ARGS='--slot-save-path\n/zip/\n--preload-kv\nsystem.kv\n'
fi

# .args: full agentic + multi-ZIM + multimodal, native context, auto-open browser.
TMP="$(mktemp -d)"
{
  printf -- '--server\n--jinja\n-m\n/zip/%s\n' "$(basename "$MODEL")"
  [ -f "$MMPROJ" ] && printf -- '--mmproj\n/zip/%s\n' "$(basename "$MMPROJ")"
  printf '%s\n' "$ZIM_ARGS"
  printf -- '--agents\n--default-system\n/zip/%s\n' "$(basename "$SYSTEM_TXT")"
  printf -- '-ngl\n%s\n-fa\noff\n-fit\noff\n--no-warmup\n' "$NGL"
  printf '%b' "$PRELOAD_ARGS"
  printf -- '--open-browser\n-c\n%s\n-np\n%s\n--host\n127.0.0.1\n--port\n%s\n...\n' "$CTX" "$NP" "$PORT"
} > "$TMP/.args"
( cd "$TMP" && "$ZA" "$OUT" .args )
rm -rf "$TMP"

echo "==> done: $OUT ($(ls -lh "$OUT" | awk '{print $5}'))"
echo "    run:  sh \"$OUT\"   -> chat + /agents + /zim/<id>/<article> (with images)"
