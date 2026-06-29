#!/bin/sh
# Copyright 2026 Mozilla.ai — Apache-2.0
#
# Single entry point for the llamafile-owned module regression suite added this
# session. Fast + hermetic: tiny committed fixtures, no model, no 70GB store.
#
#   tests/run_new.sh
#
# Runs, in order:
#   1. The new C/C++ unit tests in the `make check` graph (built with cosmocc):
#        - zim_reader_test   (llamafile/zim, two tiny real ZIMs v5+v6)
#        - wikidata_test     (llamafile/wikidata, self-built SQLite+FTS5 fixture)
#        - wiki_fts_test     (llamafile/wiki_fts, builds an FTS5 sidecar from the
#                             tiny ZIM fixture, then full-text-searches bodies)
#        - path_jail_test    (llamafile/path_jail.h, server-tools jail logic)
#   2. The vlib-video unit tests (tool-parser / pixel-diff / mrope / pair-pack).
#   3. The MCP server protocol integration test (spawns `llamafile mcp-server`;
#      builds the llamafile binary first if missing). NO model.
#   4. The model-gated MCP host-bridge test (SKIPPED unless a model is given).
#
set -e
REPO=$(cd "$(dirname "$0")/.." && pwd)
cd "$REPO"
MAKE="${MAKE:-.cosmocc/4.0.2/bin/make}"
MODE_DIR="o/"   # MODE is empty in the default build -> targets live under o//

echo "########################################################"
echo "# 1. llamafile-owned unit tests (make check graph)"
echo "########################################################"
"$MAKE" -j8 \
    "${MODE_DIR}/tests/zim_reader_test" \
    "${MODE_DIR}/tests/wikidata_test" \
    "${MODE_DIR}/tests/wiki_fts_test" \
    "${MODE_DIR}/tests/path_jail_test" \
    "${MODE_DIR}/tests/agent_runtime_test" \
    "${MODE_DIR}/tests/agent_runtime_sched_test" \
    "${MODE_DIR}/tests/agent_runtime_mesh_demo" \
    "${MODE_DIR}/tests/agent_session_test"
"${MODE_DIR}/tests/zim_reader_test"
"${MODE_DIR}/tests/wikidata_test"
"${MODE_DIR}/tests/wiki_fts_test"
"${MODE_DIR}/tests/path_jail_test"
"${MODE_DIR}/tests/agent_runtime_test"
"${MODE_DIR}/tests/agent_runtime_sched_test"
"${MODE_DIR}/tests/agent_runtime_mesh_demo"
"${MODE_DIR}/tests/agent_session_test"

echo
echo "########################################################"
echo "# 2. vlib-video unit tests"
echo "########################################################"
sh llamafile/vlib_video/tests/run_tests.sh

echo
echo "########################################################"
echo "# 3. MCP server protocol integration test"
echo "########################################################"
LLAMAFILE_BIN="o/llamafile/llamafile"
if [ ! -f "$LLAMAFILE_BIN" ]; then
    echo "(building $LLAMAFILE_BIN — first run only)"
    "$MAKE" -j8 "$LLAMAFILE_BIN"
fi
python3 tests/integration/mcp_server_test.py "$LLAMAFILE_BIN"

echo
echo "########################################################"
echo "# 3b. MCP host transports: remote Streamable-HTTP e2e (mcp-probe)"
echo "########################################################"
python3 tests/integration/mcp_http_test.py "$LLAMAFILE_BIN"

echo
echo "########################################################"
echo "# 4. MCP host-bridge (model-gated)"
echo "########################################################"
sh tests/integration/mcp_bridge_gated.sh

echo
echo "ALL NEW LLAMAFILE-MODULE TESTS PASSED"
