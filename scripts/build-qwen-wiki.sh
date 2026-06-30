#!/bin/sh
# build-qwen-wiki.sh — assemble the self-contained qwen-wiki.llamafile:
#   the (freshly built) llamafile binary + model + mmproj + Wikipedia ZIM + a baked-in
#   /zip/.args, so it runs with ZERO flags and opens a browser.
#
# Run this after code changes to refresh the bundle (it is NOT rebuilt by `make`,
# since re-embedding the 22 GB model on every build would be wasteful).
#
# Env overrides: OUT, MODEL, MMPROJ, ZIM, PORT, CTX, NP, PRELOAD_KV.
#   e.g.  NP=2 CTX=0 scripts/build-qwen-wiki.sh        # 128K x 2 instead of 256K x 1
#         PRELOAD_KV=1 scripts/build-qwen-wiki.sh      # also precompute+embed the system-prompt KV
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

OUT="${OUT:-/Volumes/AI Models at Home/qwen-wiki.llamafile}"
MODEL_LINK="${MODEL:-/Users/ochafik/Data/Models/Qwen3.6-35B-A3B-UD-Q4_K_M.gguf}"
MMPROJ_LINK="${MMPROJ:-/Users/ochafik/Data/Models/Qwen3.6-35B-A3B-mmproj-F16.gguf}"
ZIM_LINK="${ZIM:-/Users/ochafik/Data/Models/wikipedia_en_simple_all_nopic_2026-05.zim}"
PORT="${PORT:-8080}"; CTX="${CTX:-0}"; NP="${NP:-1}"
PRELOAD_KV="${PRELOAD_KV:-0}"
SYSTEM_TXT="$ROOT/scripts/default-system.txt"

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

# Default survival system prompt: embedded so every fresh chat begins with this
# exact text (llamafile injects it server-side when a request carries no system
# message; see llamafile/preload_kv.cpp + --default-system). Plain `zip` is fine
# (read sequentially, not mmap'd) and the file is tiny.
echo "==> embedding default system prompt (/zip/default-system.txt)"
( cd "$(dirname "$SYSTEM_TXT")" && "$ZA" "$OUT" "$(basename "$SYSTEM_TXT")" )

# ---------------------------------------------------------------------------
# Precomputed system-prompt KV (OPT-IN: PRELOAD_KV=1).
#
# WARNING: measured ineffective on Qwen3.6. Qwen3.6 is a hybrid gated-delta-net
# (recurrent) + SWA model; llama.cpp reuses prompt cache for such models only by
# rewinding to an in-memory "context checkpoint", and a freshly restored slot
# state registers no checkpoint -> the first chat re-prefills the whole prefix
# anyway (measured REUSED=0). The plumbing below is correct and works on
# pure-attention models; it is left here, gated off, until a checkpoint-on-restore
# fix lands. The deflated f16 KV also only shrinks ~6% (high-entropy), so it would
# add ~size-of-prefix MiB to the bundle for no benefit on this model.
# ---------------------------------------------------------------------------
PRELOAD_ARGS=""
if [ "$PRELOAD_KV" = "1" ]; then
  echo "==> precomputing system-prompt KV (PRELOAD_KV=1)"
  KVDIR="$(mktemp -d)"
  PREPORT=$((PORT + 1001))
  SYS="$(cat "$SYSTEM_TXT")"
  # Launch the freshly-assembled bundle headless with its real flags so the KV
  # is computed with the EXACT model + tool set + system text it will ship with.
  "$OUT" --server --jinja -m "/zip/$(basename "$MODEL")" --mmproj "/zip/$(basename "$MMPROJ")" \
    --zim "/zip/$(basename "$ZIM")" --agents --slot-save-path "$KVDIR/" \
    -c "$CTX" -np 1 -ngl 0 --host 127.0.0.1 --port "$PREPORT" > "$KVDIR/server.log" 2>&1 &
  PREPID=$!
  trap 'kill "$PREPID" 2>/dev/null; rm -rf "$KVDIR"' EXIT
  for i in $(seq 1 600); do
    [ "$(curl -s -o /dev/null -w '%{http_code}' "http://127.0.0.1:$PREPORT/health" 2>/dev/null)" = "200" ] && break
    sleep 1
  done
  # Render the EXACT [tools + system] prefix via the server's own template + live
  # tool registry, prefill it into slot 0 (n_predict:0), then save the KV.
  TOOLS="$(curl -s "http://127.0.0.1:$PREPORT/tools" | python3 -c 'import json,sys; r=json.load(sys.stdin); print(json.dumps([t.get("definition",t) for t in (r.get("tools",r) if isinstance(r,dict) else r)]))')"
  PREFIX="$(python3 -c 'import json,sys,urllib.request; sys_txt=sys.argv[1]; tools=json.loads(sys.argv[2]); body=json.dumps({"messages":[{"role":"system","content":sys_txt}],"tools":tools,"add_generation_prompt":False}).encode(); r=urllib.request.urlopen(urllib.request.Request(sys.argv[3]+"/apply-template",body,{"Content-Type":"application/json"})); print(json.load(r)["prompt"])' "$SYS" "$TOOLS" "http://127.0.0.1:$PREPORT")"
  python3 -c 'import json,sys,urllib.request; p=sys.argv[1]; urllib.request.urlopen(urllib.request.Request(sys.argv[2]+"/completion",json.dumps({"prompt":p,"n_predict":0,"cache_prompt":True,"id_slot":0}).encode(),{"Content-Type":"application/json"})).read()' "$PREFIX" "http://127.0.0.1:$PREPORT"
  curl -s "http://127.0.0.1:$PREPORT/slots/0?action=save" -H 'Content-Type: application/json' -d '{"filename":"system.kv"}' > "$KVDIR/save.json"
  kill "$PREPID" 2>/dev/null; trap 'rm -rf "$KVDIR"' EXIT
  RAW=$(stat -f%z "$KVDIR/system.kv" 2>/dev/null || stat -c%s "$KVDIR/system.kv")
  echo "    raw KV: $RAW bytes -> embedding DEFLATED as /zip/system.kv"
  # Plain `zip` (DEFLATE), NOT zipalign -j0: the KV is read+inflated on the state
  # path (llamafile_open_zip now inflates deflated /zip entries), never mmap'd.
  ( cd "$KVDIR" && zip -9 -q "$OUT" system.kv )
  rm -rf "$KVDIR"; trap - EXIT
  PRELOAD_ARGS='--slot-save-path\n/zip/\n--preload-kv\nsystem.kv\n'
fi

# .args: full agentic + wiki + multimodal, native context, auto-open browser, no user flags.
# --default-system makes the survival persona the default system message of every fresh chat.
TMP="$(mktemp -d)"
printf -- '--server\n--jinja\n-m\n/zip/%s\n--mmproj\n/zip/%s\n--zim\n/zip/%s\n--agents\n--default-system\n/zip/%s\n%s--open-browser\n-c\n%s\n-np\n%s\n--host\n127.0.0.1\n--port\n%s\n...\n' \
  "$(basename "$MODEL")" "$(basename "$MMPROJ")" "$(basename "$ZIM")" "$(basename "$SYSTEM_TXT")" "$PRELOAD_ARGS" "$CTX" "$NP" "$PORT" > "$TMP/.args"
( cd "$TMP" && "$ZA" "$OUT" .args )
rm -rf "$TMP"

echo "==> done: $OUT ($(ls -lh "$OUT" | awk '{print $5}'))"
echo "    run:  sh \"$OUT\"   -> opens http://127.0.0.1:$PORT/  (chat + /agents + clickable /wiki/)"
