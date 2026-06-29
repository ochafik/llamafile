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

#ifndef LLAMAFILE_BROWSER_TOOL_H_
#define LLAMAFILE_BROWSER_TOOL_H_

// In-process Chrome DevTools Protocol (CDP) `browser_*` MCP tools for live web
// research. See ddocs/04-browser-tool-design.md. The CDP transport rides the
// vendored cpp-httplib (HTTP `httplib::Client` for /json target discovery +
// `httplib::ws::WebSocketClient` for the per-target devtools channel) — no new
// dependency. This header is the bridge to the MCP tool registry in
// mcp_server.cpp; it deliberately does NOT pull in httplib so the registry
// stays light.

#include <functional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace browser {

using json = nlohmann::ordered_json;

// CLI/config knobs (see register_browser_tools / mcp_server.cpp flag parsing).
struct Options {
    bool enabled = false;            // --browser            : expose browser_* at all
    bool attach = false;             // --browser-attach[N]  : ATTACH to a running Chrome
    int attach_port = 9222;          // ATTACH debug port
    bool headed = false;             // --browser-headed     : visible window (LAUNCH)
    std::string browser_path;        // --browser-path PATH  : override discovery
    std::vector<std::string> allow;  // --browser-allow a,b  : domain allowlist (suffix/glob)
};

// Callback used to append a tool to the MCP registry without exposing the
// registry's internal Tool struct here. `handler` returns the MCP tools/call
// result body (i.e. {"content":[...],"isError":bool}).
using AddTool = std::function<void(const std::string & name,
                                   const std::string & description,
                                   const json & inputSchema,
                                   std::function<json(const json & args)> handler)>;

// Register the browser_* tools (navigate/get_text/get_links/current/list_tabs)
// into the registry via `add`. No-op if opts.enabled is false. Prints a loud
// stderr warning for ATTACH mode (the model can act in the user's live browser).
void register_browser_tools(const Options & opts, const AddTool & add);

} // namespace browser

#endif // LLAMAFILE_BROWSER_TOOL_H_
