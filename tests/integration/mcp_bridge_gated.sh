#!/bin/sh
# Copyright 2026 Mozilla.ai — Apache-2.0
#
# MODEL-GATED (skipped by default) integration test for the MCP HOST bridge
# (llamafile/mcp_host.cpp) and the --server /tools surface.
#
# Unlike `mcp_server_test.py` (which drives the mcp-server OUT surface with no
# model), the host bridge wires discovered MCP tools INTO the running HTTP
# server's /tools registry, which only exists once a model is loaded. So this
# path needs a real model and is therefore SKIPPED unless a model is provided.
#
# To run it, set LLAMAFILE_TEST_MODEL to a small .gguf (or pass it as $1):
#
#   LLAMAFILE_TEST_MODEL=/path/to/tiny.gguf tests/integration/mcp_bridge_gated.sh
#
# Manual repro of the full bridge (what this would automate):
#
#   ./o/llamafile/llamafile --server -m tiny.gguf \
#       --mcp "$PWD/o/llamafile/llamafile mcp-server --zim tests/fixtures/small_nons.zim"
#   # then, against the running server:
#   curl -s localhost:8080/tools | grep -o zim_search          # tool bridged in
#   curl -s localhost:8080/tools -X POST -H 'content-type: application/json' \
#        -d '{"name":"zim_search","arguments":{"query":"Test"}}'   # call it
#
set -e
REPO=$(cd "$(dirname "$0")/../.." && pwd)
MODEL="${1:-$LLAMAFILE_TEST_MODEL}"
EXE="${LLAMAFILE_EXE:-$REPO/o/llamafile/llamafile}"

if [ -z "$MODEL" ] || [ ! -f "$MODEL" ]; then
    echo "SKIP: MCP host-bridge test is model-gated (no model provided)."
    echo "      run with: LLAMAFILE_TEST_MODEL=/path/to/tiny.gguf $0"
    echo "      (see header for the manual --server --mcp repro)"
    exit 0
fi

echo "== MCP host-bridge test (model: $MODEL) =="
PORT=18080
"$EXE" --server -m "$MODEL" --host 127.0.0.1 --port "$PORT" --nobrowser \
    --mcp "$EXE mcp-server --zim $REPO/tests/fixtures/small_nons.zim" \
    >/tmp/mcp_bridge_server.log 2>&1 &
SRV=$!
trap 'kill $SRV 2>/dev/null || true' EXIT

# wait for readiness
for i in $(seq 1 60); do
    if curl -fs "http://127.0.0.1:$PORT/health" >/dev/null 2>&1; then break; fi
    sleep 1
done

TOOLS=$(curl -fs "http://127.0.0.1:$PORT/tools" || true)
echo "$TOOLS" | grep -q zim_search || { echo "FAIL: zim_search not bridged into /tools"; exit 1; }
echo "PASS: zim_search bridged into the server /tools registry"
