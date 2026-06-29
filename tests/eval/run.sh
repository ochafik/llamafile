#!/bin/sh
# Copyright 2026 Mozilla.ai - Apache-2.0
#
# Agentic-flow EVAL runner (design: ddocs/09-interactive-multiagent-runtime.md
# §11). Boots a llamafile --server runtime (wiki + wikidata MCP tools, agents
# enabled), runs the behavioral eval matrix (tests/eval/agentic_flows.py), prints
# the report table, and tears the server down.
#
# MODEL-GATED, on-demand: this needs the real model + the ZIM + the Wikidata
# store and runs many live generations -> it is SLOW on CPU and is NOT part of
# `make check` / tests/run_new.sh. Run it by hand when you want a reliability +
# token-cost signal for the multi-agent flows.
#
# usage:
#   tests/eval/run.sh                       # full matrix: E1..E4 x 10 attempts
#   ATTEMPTS=2 OBJECTIVES=E1,E3 tests/eval/run.sh   # quick validation run
#
# env overrides:
#   BIN        llamafile binary           (default: o/llamafile/llamafile)
#   MODEL      model .gguf                 (default: Qwen3.6-35B-A3B-UD-Q4_K_M)
#   ZIM        wikipedia .zim
#   WIKIDATA   wikidata .sqlite
#   PORT       server port                (default: 18182)
#   NP         -np slots                  (default: 4)
#   CTX        -c context                 (default: 32768)  # MUST be >> orchestrator prompt*NP; n_ctx is divided across slots
#   ATTEMPTS   attempts per objective     (default: 10)
#   OBJECTIVES comma list                 (default: E1,E2,E3,E4)
#   TIMEOUT    per-attempt seconds        (default: 180)
set -e
REPO=$(cd "$(dirname "$0")/../.." && pwd)
cd "$REPO"

BIN="${BIN:-$REPO/o/llamafile/llamafile}"
MODEL="${MODEL:-/Users/ochafik/Data/Models/Qwen3.6-35B-A3B-UD-Q4_K_M.gguf}"
ZIM="${ZIM:-/Users/ochafik/Data/Models/wikipedia_en_simple_all_nopic_2026-05.zim}"
WIKIDATA="${WIKIDATA:-/Volumes/AI Models at Home/wikidata/wikidata.sqlite}"
PORT="${PORT:-18182}"
NP="${NP:-4}"
CTX="${CTX:-32768}"
ATTEMPTS="${ATTEMPTS:-10}"
OBJECTIVES="${OBJECTIVES:-E1,E2,E3,E4}"
TIMEOUT="${TIMEOUT:-180}"

[ -x "$BIN" ]   || { echo "missing binary: $BIN (build it first)"      >&2; exit 1; }
[ -f "$MODEL" ] || { echo "missing model:  $MODEL"                     >&2; exit 1; }
[ -f "$ZIM" ]   || echo "warning: ZIM not found ($ZIM); E1/E3/E4 will struggle" >&2
[ -f "$WIKIDATA" ] || echo "warning: WIKIDATA not found ($WIKIDATA); E2 will struggle" >&2

echo "==> agentic-flow eval: objectives=$OBJECTIVES attempts=$ATTEMPTS port=$PORT"
exec python3 "$REPO/tests/eval/agentic_flows.py" \
    --bin "$BIN" --model "$MODEL" --zim "$ZIM" --wikidata "$WIKIDATA" \
    --port "$PORT" --np "$NP" --ctx "$CTX" \
    --attempts "$ATTEMPTS" --objectives "$OBJECTIVES" --timeout "$TIMEOUT"
