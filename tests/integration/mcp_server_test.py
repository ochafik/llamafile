#!/usr/bin/env python3
# -*- coding: utf-8 -*-
#
# Copyright 2026 Mozilla.ai
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
#
# Hermetic integration test for `llamafile mcp-server` (llamafile/mcp_server.cpp)
# and, by extension, the protocol the MCP host bridge (llamafile/mcp_host.cpp)
# speaks. NO model is loaded — mcp-server only opens the ZIM reader.
#
# It spawns:  llamafile mcp-server --zim tests/fixtures/small_nons.zim
# and drives newline-delimited JSON-RPC 2.0 over stdin/stdout, asserting:
#
#   * initialize            -> result.protocolVersion present
#   * tools/list            -> wiki_search AND wiki_get_article registered
#   * tools/call wiki_search "Test" -> a hit mentioning "Test ZIM file"
#   * tools/call wiki_get_article    -> the article body
#   * tools/call <unknown tool>      -> JSON-RPC error -32602 (registry miss)
#   * <unknown method>               -> JSON-RPC error -32601 (method not found)
#   * notification (no id)           -> NO response emitted
#   * STDOUT IS PURE JSON-RPC        -> every stdout line parses as JSON
#                                       (no banner / diagnostic leakage)
#
# Usage:
#   tests/integration/mcp_server_test.py [EXECUTABLE] [ZIM]
# Defaults: EXECUTABLE=o/llamafile/llamafile  ZIM=tests/fixtures/small_nons.zim
# (paths are resolved relative to the repo root = two levels up from this file).

import json
import os
import subprocess
import sys

REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))


def main():
    exe = sys.argv[1] if len(sys.argv) > 1 else os.path.join(REPO, "o/llamafile/llamafile")
    zim = sys.argv[2] if len(sys.argv) > 2 else os.path.join(REPO, "tests/fixtures/small_nons.zim")

    if not os.path.exists(exe):
        print(f"SKIP: executable not found: {exe}\n  build it with: .cosmocc/4.0.2/bin/make -j o/llamafile/llamafile")
        return 0  # not a failure: the binary just isn't built
    if not os.path.exists(zim):
        print(f"FAIL: fixture ZIM not found: {zim}")
        return 1

    # The requests we feed (one JSON object per line). Notifications carry no id.
    requests = [
        {"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": {}},
        {"jsonrpc": "2.0", "method": "notifications/initialized"},
        {"jsonrpc": "2.0", "id": 2, "method": "tools/list", "params": {}},
        {"jsonrpc": "2.0", "id": 3, "method": "tools/call",
         "params": {"name": "wiki_search", "arguments": {"query": "Test"}}},
        {"jsonrpc": "2.0", "id": 4, "method": "tools/call",
         "params": {"name": "wiki_get_article", "arguments": {"title": "Test ZIM file"}}},
        {"jsonrpc": "2.0", "id": 5, "method": "tools/call",
         "params": {"name": "no_such_tool", "arguments": {}}},
        {"jsonrpc": "2.0", "id": 6, "method": "this/method/does/not/exist", "params": {}},
    ]
    stdin_data = "".join(json.dumps(r) + "\n" for r in requests)

    # llamafile is an APE (Actually Portable Executable). On macOS the kernel
    # cannot execve() it directly (the shell stub bootstraps it), so route the
    # launch through /bin/sh, which runs the polyglot correctly on every host.
    cmd = ["/bin/sh", exe, "mcp-server", "--zim", zim]
    proc = subprocess.run(
        cmd, input=stdin_data, capture_output=True, text=True, timeout=60, cwd=REPO,
    )

    fails = []

    def check(cond, msg):
        if not cond:
            fails.append(msg)

    # --- stdout must be PURE JSON-RPC: every non-empty line parses as JSON ---
    responses = {}
    note_ids = []
    for line in proc.stdout.splitlines():
        if not line.strip():
            continue
        try:
            obj = json.loads(line)
        except Exception as e:
            fails.append(f"stdout line is not valid JSON ({e}): {line!r}")
            continue
        check(obj.get("jsonrpc") == "2.0", f"response missing jsonrpc 2.0: {line!r}")
        if "id" in obj and obj["id"] is not None:
            responses[obj["id"]] = obj
        else:
            note_ids.append(obj)

    # --- initialize ---
    init = responses.get(1, {})
    check("result" in init and "protocolVersion" in init["result"],
          "initialize returns result.protocolVersion")

    # --- tools/list contains both wiki tools ---
    tl = responses.get(2, {})
    names = [t.get("name") for t in tl.get("result", {}).get("tools", [])]
    check("wiki_search" in names, f"tools/list contains wiki_search (got {names})")
    check("wiki_get_article" in names, f"tools/list contains wiki_get_article (got {names})")
    # each tool exposes a description + inputSchema (registry shaping)
    for t in tl.get("result", {}).get("tools", []):
        check("description" in t and "inputSchema" in t,
              f"tool {t.get('name')} has description + inputSchema")

    # --- tools/call wiki_search "Test" -> a hit ---
    search = responses.get(3, {})
    sres = search.get("result", {})
    stext = "".join(c.get("text", "") for c in sres.get("content", []))
    check(sres.get("isError") is False, "wiki_search not an error")
    check("Test ZIM file" in stext, f"wiki_search hit mentions 'Test ZIM file' (got {stext[:120]!r})")

    # --- tools/call wiki_get_article -> body ---
    art = responses.get(4, {})
    ares = art.get("result", {})
    atext = "".join(c.get("text", "") for c in ares.get("content", []))
    check(ares.get("isError") is False, "wiki_get_article not an error")
    check("Test ZIM file" in atext, f"article body mentions 'Test ZIM file' (got {atext[:120]!r})")

    # --- unknown tool -> -32602 ---
    ut = responses.get(5, {})
    check(ut.get("error", {}).get("code") == -32602,
          f"unknown tool returns -32602 (got {ut.get('error')})")

    # --- unknown method -> -32601 ---
    um = responses.get(6, {})
    check(um.get("error", {}).get("code") == -32601,
          f"unknown method returns -32601 (got {um.get('error')})")

    # --- the notification produced no response ---
    check(len(responses) == 6, f"exactly 6 id-bearing responses (got {sorted(responses)})")
    check(len(note_ids) == 0, "notification produced no response line")

    if fails:
        print("=== MCP SERVER TEST: FAIL ===")
        for f in fails:
            print("  FAIL:", f)
        print("--- captured stderr ---")
        print(proc.stderr)
        return 1

    print(f"=== MCP server protocol test: PASS ({len(requests)} requests, "
          f"{len(responses)} responses, stdout clean) ===")
    return 0


if __name__ == "__main__":
    sys.exit(main())
