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
// llamafile's `--server` can spawn one or more external MCP servers (configured
// via repeatable `--mcp '<command line>'` flags), discover their tools over
// stdio JSON-RPC 2.0, and bridge each discovered tool into the server's existing
// `/tools` registry. From there the browser web UI's agentic loop AND the model
// (via /v1/chat/completions tool_calls dispatched back to POST /tools) can use
// them, exactly like the built-in `read_file`/`grep`/... tools.
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
