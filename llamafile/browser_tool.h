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
// AND the code interpreter tools (code_run_js / code_render_html, ddoc 09 §4)
// into the registry via `add`. No-op if opts.enabled is false. Prints a loud
// stderr warning for ATTACH mode (the model can act in the user's live browser).
void register_browser_tools(const Options & opts, const AddTool & add);

// =====================================================================
// Code-interpreter wrapping helpers (pure; unit-tested in
// tests/code_run_wrapper_test.cpp — no httplib/CDP dependency)
// =====================================================================

// Default / hard caps for code_run_js (mirrors the constants used by the tool).
constexpr int CODE_DEFAULT_TIMEOUT_MS = 10000;
constexpr int CODE_HARD_TIMEOUT_MS    = 60000;
constexpr int CODE_OUTPUT_CAP_CHARS   = 16000;

// Build the JS expression evaluated via CDP Runtime.evaluate for code_run_js.
//
// It wraps the user `code` as an async IIFE so that a multi-statement snippet
// with a trailing expression still yields a value (via the completion value of
// a direct `eval`), captures console.{log,info,warn,error,debug} into a buffer,
// races the computation against an in-JS timeout (covers never-resolving
// promises; sync busy-loops are killed by CDP's own `timeout` param), and
// returns a by-value object {ok,type,result,console,error}.
//
// `code` is embedded as a JSON string literal, so arbitrary quotes/newlines/
// backslashes are safe — there is no string-concatenation injection surface.
inline std::string build_code_eval_wrapper(const std::string & code, int timeout_ms) {
    const std::string lit = json(code).dump();  // valid JS string literal
    const std::string ms  = std::to_string(timeout_ms);
    std::string w;
    w += "(async () => {";
    w +=   "const __logs = [];";
    w +=   "const __fmt = (a) => { try { return (typeof a === 'string') ? a : JSON.stringify(a); } catch (e) { return String(a); } };";
    w +=   "const __orig = {};";
    w +=   "const __levels = ['log','info','warn','error','debug'];";
    w +=   "for (const __k of __levels) { __orig[__k] = console[__k]; console[__k] = (...a) => { try { __logs.push(a.map(__fmt).join(' ')); } catch (e) {} }; }";
    w +=   "let __result, __error;";
    w +=   "const __code = " + lit + ";";
    w +=   "const __run = (async () => await eval(__code))();";
    w +=   "const __timer = new Promise((_, rej) => setTimeout(() => rej(new Error('code_run_js timed out after " + ms + "ms')), " + ms + "));";
    w +=   "try { __result = await Promise.race([__run, __timer]); }";
    w +=   "catch (e) { __error = (e && e.stack) ? String(e.stack) : String(e); }";
    w +=   "finally { for (const __k of __levels) console[__k] = __orig[__k]; }";
    w +=   "let __type = typeof __result, __value = null;";
    w +=   "if (!__error) { try { JSON.stringify(__result); __value = (__result === undefined) ? null : __result; } catch (e) { __value = String(__result); __type = 'string'; } }";
    w +=   "return { ok: !__error, type: __type, result: __value, console: __logs.join('\\n'), error: __error || null };";
    w += "})()";
    return w;
}

// Format the by-value result object returned by build_code_eval_wrapper() into
// an MCP tools/call result body ({content:[{type:text,...}],isError}). Truncates
// console output and string results to `cap` chars (sets "truncated":true).
inline json format_code_result(const json & val, int cap) {
    json out = json::object();
    const bool is_err = val.contains("error") && !val["error"].is_null();
    out["type"] = val.value("type", std::string("undefined"));
    json result = val.contains("result") ? val["result"] : json();
    bool truncated = false;
    if (result.is_string() && (int) result.get<std::string>().size() > cap) {
        std::string s = result.get<std::string>();
        s.resize(cap);
        result = s;
        truncated = true;
    }
    out["result"] = result;
    std::string con = val.value("console", std::string());
    if ((int) con.size() > cap) {
        con.resize(cap);
        truncated = true;
    }
    if (!con.empty()) out["console"] = con;
    if (is_err) out["error"] = val["error"];
    out["truncated"] = truncated;
    json r;
    r["content"] = json::array({ json{ { "type", "text" }, { "text", out.dump(2) } } });
    r["isError"] = is_err;
    return r;
}

} // namespace browser

#endif // LLAMAFILE_BROWSER_TOOL_H_
