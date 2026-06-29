// -*- mode:c++;indent-tabs-mode:nil;c-basic-offset:4;coding:utf-8 -*-
// vi: set et ft=cpp ts=4 sts=4 sw=4 fenc=utf-8 :vi
//
// Copyright 2026 Mozilla.ai
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#pragma once

#include <string>

// llamafile as MCP HOST.
//
// llamafile's `--server` can connect to one or more external MCP servers
// (configured via repeatable `--mcp '<command-or-url>'` / `--mcp-http <url>`
// flags), discover their tools, and bridge each discovered tool into the
// server's existing `/tools` registry. From there the browser web UI's agentic
// loop AND the model (via /v1/chat/completions tool_calls dispatched back to
// POST /tools) can use them, exactly like the built-in `read_file`/`grep`/...
// tools.
//
// Two transports are supported behind one `McpServer` (see mcp_host.cpp):
//   * stdio: an `--mcp '<command line>'` whose value is a command is spawned and
//     spoken to over newline-delimited JSON-RPC 2.0 (the P3 prototype shape).
//   * remote Streamable-HTTP (MCP 2025-03-26): an `--mcp 'http(s)://...'` value
//     (or `--mcp-http <url>`) POSTs JSON-RPC to the endpoint, handles both
//     application/json and text/event-stream (SSE) replies, and carries the
//     `Mcp-Session-Id` across requests. https:// needs an external TLS proxy
//     (cosmocc has no in-binary TLS).
//
// This is the inward direction of the MCP duality (ddocs/06 §7.2): tools flow
// IN from external servers. The mirror — llamafile exposing its own tools OUT —
// is `llamafile mcp-server` (mcp_server.cpp). Both reuse the validated P3 stdio
// JSON-RPC client/server shape.

struct server_tools;  // llama.cpp/tools/server/server-tools.h

// Register a configured MCP server command line (the raw string passed to one
// `--mcp` flag, e.g. "llamafile mcp-server --zim simple.zim"). Called during
// argument parsing; merely records the command — no process is spawned yet.
void llamafile_mcp_add_server(const std::string & cmdline);

// Number of MCP server command lines configured so far.
int llamafile_mcp_server_count();

// For every configured MCP server: posix_spawn it, perform the MCP handshake
// (initialize -> notifications/initialized -> tools/list), and register each
// discovered tool into `registry` with an executor that forwards `tools/call`
// to that subprocess. Diagnostics go to the server log (stderr), never stdout.
// A server that fails to spawn or handshake is skipped (its tools are simply
// not registered); the HTTP server keeps running. Returns the number of tools
// bridged across all servers.
int llamafile_mcp_register_tools(server_tools & registry);

// Best-effort: close pipes and reap all spawned MCP subprocesses. Safe to call
// multiple times. (On hard `_exit()` paths this is skipped, but children read
// stdin and exit on the EOF they get when our fds close at process death.)
void llamafile_mcp_shutdown();

// ---------------------------------------------------------------------------
// Direct tool access (used by the webcam-agent's per-frame ACTION_OTHER loop)
// ---------------------------------------------------------------------------
// These let llamafile-owned code (webcam_agent.cpp) execute an MCP tool
// WITHOUT going through the /tools HTTP registry. They operate on the servers
// already spawned by llamafile_mcp_register_tools(), so they only work after
// that has been called (which happens once --mcp servers are bridged).

// True if any spawned MCP server exposes a tool with this (bare MCP) name.
bool llamafile_mcp_has_tool(const std::string & name);

// Call a bridged MCP tool by its bare name with JSON-encoded arguments. Returns
// the tool's text result, or a "(mcp error: ...)" string on failure. Intended
// to run on a worker thread with a generous stack (JSON + subprocess I/O).
std::string llamafile_mcp_call_tool(const std::string & name,
                                    const std::string & arguments_json);

// A newline-delimited, LLM-readable description of every bridged MCP tool
// (name, description, JSON-schema arguments), suitable for merging into a
// system prompt so the model knows it may call them. Empty if none.
std::string llamafile_mcp_tools_prompt();

// `llamafile mcp-probe <command-or-url> [tool] [args-json]` — model-free harness
// that connects ONE MCP server (stdio command line OR http URL), runs the
// handshake, prints {"server","transport","tools"[,"call"]} as JSON to stdout
// and returns 0 on success. Used by the hermetic transport integration tests to
// exercise BOTH transports through the real binary without loading a model.
int llamafile_mcp_probe_main(int argc, char ** argv);
