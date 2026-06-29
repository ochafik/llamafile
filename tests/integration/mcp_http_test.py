#!/usr/bin/env python3
# Copyright 2026 Mozilla.ai - Apache-2.0
#
# Hermetic e2e test for llamafile's REMOTE Streamable-HTTP MCP transport (MCP spec
# 2025-03-26). Stands up a tiny in-process streamable-HTTP MCP server exposing one
# `add` tool, then drives it through the REAL binary via `llamafile mcp-probe
# http://127.0.0.1:PORT/mcp` (model-free) and asserts the full handshake +
# tools/list + tools/call round-trip over HTTP.
#
# The server deliberately exercises BOTH server->client reply modes the client must
# handle: application/json for initialize + tools/list, and text/event-stream (SSE)
# for tools/call. It also assigns + requires the Mcp-Session-Id header.
#
# Usage: mcp_http_test.py /path/to/llamafile

import json, subprocess, sys, threading, http.server, socket

SESSION_ID = "test-session-abc123"

def jrpc(id_, result):
    return {"jsonrpc": "2.0", "id": id_, "result": result}

TOOLS = [{
    "name": "add",
    "description": "Add two numbers a and b.",
    "inputSchema": {"type": "object",
                    "properties": {"a": {"type": "number"}, "b": {"type": "number"}},
                    "required": ["a", "b"]},
}]

class Handler(http.server.BaseHTTPRequestHandler):
    def log_message(self, *a):  # quiet
        pass

    def _json(self, obj, extra_headers=None):
        body = json.dumps(obj).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        for k, v in (extra_headers or {}).items():
            self.send_header(k, v)
        self.end_headers()
        self.wfile.write(body)

    def _sse(self, obj):
        # text/event-stream reply carrying one JSON-RPC message as a `data:` line
        body = ("event: message\ndata: " + json.dumps(obj) + "\n\n").encode()
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_POST(self):
        n = int(self.headers.get("Content-Length", 0))
        msg = json.loads(self.rfile.read(n) or b"{}")
        method, mid = msg.get("method"), msg.get("id")

        # After initialize, the client MUST echo the session id back.
        if method not in ("initialize",) and self.headers.get("Mcp-Session-Id") != SESSION_ID:
            self._json({"jsonrpc": "2.0", "id": mid,
                        "error": {"code": -32000, "message": "missing/invalid Mcp-Session-Id"}})
            return

        if method == "initialize":
            self._json(jrpc(mid, {
                "protocolVersion": "2025-03-26",
                "capabilities": {"tools": {}},
                "serverInfo": {"name": "test-http-mcp", "version": "1.0"},
            }), extra_headers={"Mcp-Session-Id": SESSION_ID})
        elif method == "notifications/initialized":
            self.send_response(202); self.end_headers()           # notification: no body
        elif method == "tools/list":
            self._json(jrpc(mid, {"tools": TOOLS}))                # application/json reply
        elif method == "tools/call":
            args = msg.get("params", {}).get("arguments", {})
            total = args.get("a", 0) + args.get("b", 0)
            self._sse(jrpc(mid, {"content": [{"type": "text", "text": str(total)}]}))  # SSE reply
        else:
            self._json({"jsonrpc": "2.0", "id": mid,
                        "error": {"code": -32601, "message": "method not found"}})

def main():
    binary = sys.argv[1] if len(sys.argv) > 1 else "o//llamafile/llamafile"
    sock = socket.socket(); sock.bind(("127.0.0.1", 0)); port = sock.getsockname()[1]; sock.close()
    srv = http.server.HTTPServer(("127.0.0.1", port), Handler)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    url = f"http://127.0.0.1:{port}/mcp"

    # macOS can't execve the APE directly -> launch through the /bin/sh trampoline.
    out = subprocess.run(["/bin/sh", binary, "mcp-probe", url, "add", '{"a": 2, "b": 3}'],
                         capture_output=True, text=True, timeout=30)
    srv.shutdown()
    print("--- mcp-probe stdout ---"); print(out.stdout)
    if out.returncode != 0:
        print("--- stderr ---"); print(out.stderr); print("FAIL: probe exit", out.returncode); sys.exit(1)
    try:
        j = json.loads([l for l in out.stdout.splitlines() if l.strip().startswith("{")][-1])
    except Exception as e:
        print("FAIL: could not parse probe JSON:", e); sys.exit(1)

    ok = True
    if j.get("transport") != "http":
        print("FAIL: transport != http:", j.get("transport")); ok = False
    if not any(t.get("name") == "add" for t in j.get("tools", [])):
        print("FAIL: 'add' not in tools/list over HTTP"); ok = False
    call = j.get("call")
    got = json.dumps(call) if call is not None else ""
    if "5" not in got:
        print("FAIL: tools/call (SSE reply) did not return 5:", got); ok = False
    print("PASS: remote Streamable-HTTP MCP e2e (initialize+session-id, tools/list json, "
          "tools/call SSE) through the real binary" if ok else "FAILED")
    sys.exit(0 if ok else 1)

if __name__ == "__main__":
    main()
