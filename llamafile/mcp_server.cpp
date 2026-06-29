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

// `llamafile mcp-server [--zim PATH]` — run a Model Context Protocol server over
// stdio (newline-delimited JSON-RPC 2.0), exposing the embedded-Wikipedia tools
// to external MCP clients (Claude Code, opencode, Cursor, …) and to llamafile's
// own agentic loop.
//
// This is the "one handler, three surfaces" frame from ddocs/06: the same ZIM
// reader that backs the `wikipedia` CLI and the server-side `server_tool`s is
// here exposed as an MCP server — the surface that external loop-drivers speak.
//
// PROTOCOL CHANNEL DISCIPLINE: stdin/stdout carry ONLY JSON-RPC (one JSON object
// per line, flushed per line). All diagnostics go to stderr. A single stray
// write to stdout corrupts the stream and breaks `claude mcp add`.
//
//   llamafile mcp-server --zim simplewiki.zim
//   claude mcp add wikipedia -- /path/to/llamafile mcp-server --zim simplewiki.zim

#include "zim/zim.h"

#include "browser_tool.h"
#include "wikidata.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>
#include <strings.h>
#include <vector>

#include <nlohmann/json.hpp>

using json = nlohmann::ordered_json;

namespace {

// Tried when --zim is not given and $LLAMAFILE_ZIM is unset (in-APE bundle).
const char * const WIKI_DEFAULT_ZIM = "/zip/wikipedia.zim";

const char * const MCP_PROTOCOL_VERSION = "2025-06-18";
const char * const MCP_SERVER_NAME      = "llamafile-wikipedia";

// -----------------------------------------------------------------
// Tool registry
// -----------------------------------------------------------------
//
// A tool is {name, description, inputSchema, handler}. The handler takes the
// MCP `arguments` object and returns the MCP `tools/call` result body, i.e. a
// json object like {"content":[{"type":"text","text":...}], "isError":false}.
// To add a tool (browser_*, send_email, …), append a Tool to g_tools — the
// dispatch below is fully data-driven (tools/list and tools/call read g_tools).

struct Tool {
    std::string name;
    std::string description;
    json inputSchema;
    std::function<json(const json & args)> handler;
};

std::vector<Tool> g_tools;
zim_archive *     g_zim = nullptr;
wikidata_store *  g_wd = nullptr;

const Tool * find_tool(const std::string & name) {
    for (const auto & t : g_tools) {
        if (t.name == name) return &t;
    }
    return nullptr;
}

// -----------------------------------------------------------------
// Result helpers
// -----------------------------------------------------------------

json text_result(const std::string & text, bool is_error = false) {
    json r;
    r["content"] = json::array({ json{ { "type", "text" }, { "text", text } } });
    r["isError"] = is_error;
    return r;
}

// Collapse whitespace runs to single spaces; trim. (Shared shape with wiki_cli.)
std::string collapse_ws(const char * s, size_t n) {
    std::string out;
    out.reserve(n);
    bool in_ws = true;
    for (size_t i = 0; i < n && s[i]; i++) {
        unsigned char c = (unsigned char) s[i];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v') {
            if (!in_ws) { out.push_back(' '); in_ws = true; }
        } else {
            out.push_back((char) c);
            in_ws = false;
        }
    }
    while (!out.empty() && out.back() == ' ') out.pop_back();
    return out;
}

std::string entry_snippet(zim_archive * z, zim_entry * e, size_t max_len) {
    if (e->is_redirect) zim_resolve_redirect(z, e);
    size_t n = 0;
    char * txt = zim_get_content_text(z, e, &n);
    if (!txt) return std::string();
    std::string out = collapse_ws(txt, n);
    zim_free(txt);
    if (out.size() > max_len) out.resize(max_len);
    return out;
}

// -----------------------------------------------------------------
// Tool: wiki_search
// -----------------------------------------------------------------

json tool_wiki_search(const json & args) {
    if (!args.contains("query") || !args["query"].is_string()) {
        return text_result("error: missing required string argument 'query'", true);
    }
    std::string query = args["query"].get<std::string>();
    int limit = 5;
    if (args.contains("limit") && args["limit"].is_number_integer()) {
        limit = args["limit"].get<int>();
    }
    if (limit < 1)  limit = 1;
    if (limit > 50) limit = 50;

    zim_search_result * results = (zim_search_result *) calloc(limit, sizeof(*results));
    if (!results) return text_result("error: out of memory", true);

    int nr = zim_search(g_zim, query.c_str(), results, limit);
    json hits = json::array();
    for (int i = 0; i < nr; i++) {
        std::string snippet;
        zim_entry e;
        if (zim_get_entry_by_index(g_zim, results[i].index, &e)) {
            snippet = entry_snippet(g_zim, &e, 200);
        }
        hits.push_back(json{
            { "title",   results[i].title ? results[i].title : "" },
            { "path",    results[i].path ? results[i].path : "" },
            { "snippet", snippet },
        });
    }
    if (nr > 0) zim_search_free(results, nr);
    free(results);

    if (nr <= 0) {
        return text_result("No results for \"" + query + "\".");
    }
    // Return the structured hit list as pretty JSON text — readable to a model
    // and machine-parseable by a loop-driver.
    return text_result(hits.dump(2));
}

// -----------------------------------------------------------------
// Tool: wiki_get_article
// -----------------------------------------------------------------

json tool_wiki_get_article(const json & args) {
    std::string title;
    if (args.contains("title") && args["title"].is_string()) {
        title = args["title"].get<std::string>();
    }
    if (title.empty()) {
        return text_result("error: missing required string argument 'title'", true);
    }

    zim_entry e;
    bool found = false;
    // Try as a (namespace-qualified) path first, then the bare path under the
    // content namespaces ('C' new / 'A' old), then fall back to a title search.
    if (zim_get_entry_by_path(g_zim, title.c_str(), &e)) {
        found = true;
    } else {
        std::string c = "C/" + title, a = "A/" + title;
        if (zim_get_entry_by_path(g_zim, c.c_str(), &e) ||
            zim_get_entry_by_path(g_zim, a.c_str(), &e)) {
            found = true;
        }
    }
    if (!found) {
        zim_search_result results[10];
        int nr = zim_search(g_zim, title.c_str(), results, 10);
        int pick = -1;
        for (int i = 0; i < nr; i++) {
            if (results[i].title && strcasecmp(results[i].title, title.c_str()) == 0) {
                pick = i;
                break;
            }
        }
        if (pick < 0 && nr > 0) pick = 0;
        if (pick >= 0) found = zim_get_entry_by_index(g_zim, results[pick].index, &e);
        if (nr > 0) zim_search_free(results, nr);
    }

    if (!found) {
        return text_result("Article not found: " + title, true);
    }

    if (e.is_redirect) zim_resolve_redirect(g_zim, &e);
    size_t n = 0;
    char * txt = zim_get_content_text(g_zim, &e, &n);
    if (!txt || n == 0) {
        if (txt) zim_free(txt);
        return text_result("Article has no readable text: " + title, true);
    }
    std::string body(txt, n);
    zim_free(txt);
    return text_result(body);
}

// -----------------------------------------------------------------
// Tool registration
// -----------------------------------------------------------------

void register_wiki_tools() {
    g_tools.push_back(Tool{
        "wiki_search",
        "Search the offline Wikipedia (ZIM archive) for articles matching a "
        "query. Returns a JSON list of {title, path, snippet} hits. Use the "
        "returned title or path with wiki_get_article to read the full article.",
        json{
            { "type", "object" },
            { "properties", json{
                { "query", json{ { "type", "string" },
                                 { "description", "Search query (article title or keywords)." } } },
                { "limit", json{ { "type", "integer" },
                                 { "description", "Maximum number of results (default 5, max 50)." } } },
            } },
            { "required", json::array({ "query" }) },
        },
        tool_wiki_search,
    });

    g_tools.push_back(Tool{
        "wiki_get_article",
        "Fetch the full plain text of a Wikipedia article from the offline ZIM "
        "archive, by title or path (as returned by wiki_search). Redirects are "
        "resolved automatically.",
        json{
            { "type", "object" },
            { "properties", json{
                { "title", json{ { "type", "string" },
                                 { "description", "Article title or path (e.g. \"Eiffel Tower\")." } } },
            } },
            { "required", json::array({ "title" }) },
        },
        tool_wiki_get_article,
    });
}

// -----------------------------------------------------------------
// Wikidata tools (structured facts; SQLite+FTS5 store, see wikidata.cpp)
// -----------------------------------------------------------------

json wd_value_json(const wikidata_value & v) {
    json o;
    o["type"] = v.type;
    o["text"] = v.text;
    if (!v.id.empty()) o["id"] = v.id;
    return o;
}

json wd_statement_json(const wikidata_statement & st) {
    json o;
    o["property"] = json{ { "id", st.pid }, { "label", st.prop_label } };
    o["value"] = wd_value_json(st.value);
    if (!st.qualifiers.empty()) {
        json q = json::array();
        for (const auto & ql : st.qualifiers) {
            q.push_back(json{
                { "property", json{ { "id", ql.pid }, { "label", ql.prop_label } } },
                { "value", wd_value_json(ql.value) },
            });
        }
        o["qualifiers"] = q;
    }
    return o;
}

json tool_wikidata_search(const json & args) {
    if (!args.contains("query") || !args["query"].is_string()) {
        return text_result("error: missing required string argument 'query'", true);
    }
    std::string query = args["query"].get<std::string>();
    int limit = 10;
    if (args.contains("limit") && args["limit"].is_number_integer()) {
        limit = args["limit"].get<int>();
    }
    if (limit < 1)  limit = 1;
    if (limit > 50) limit = 50;

    std::vector<wikidata_hit> hits = wikidata_search(g_wd, query, limit);
    if (hits.empty()) {
        return text_result("No results for \"" + query + "\".");
    }
    json out = json::array();
    for (const auto & h : hits) {
        out.push_back(json{
            { "id", h.id },
            { "label", h.label },
            { "description", h.description },
        });
    }
    return text_result(out.dump(2));
}

json tool_wikidata_entity(const json & args) {
    if (!args.contains("id") || !args["id"].is_string()) {
        return text_result("error: missing required string argument 'id'", true);
    }
    std::string id = args["id"].get<std::string>();
    wikidata_entity e = wikidata_get(g_wd, id);
    if (!e.found) {
        return text_result("Entity not found: " + id, true);
    }
    json claims = json::array();
    for (const auto & st : e.claims) claims.push_back(wd_statement_json(st));
    json out = {
        { "id", e.id },
        { "label", e.label },
        { "description", e.description },
        { "aliases", e.aliases },
        { "claims", claims },
    };
    return text_result(out.dump(2));
}

json tool_wikidata_property(const json & args) {
    if (!args.contains("id") || !args["id"].is_string() ||
        !args.contains("property") || !args["property"].is_string()) {
        return text_result("error: requires string arguments 'id' and 'property'", true);
    }
    std::string id = args["id"].get<std::string>();
    std::string pid = args["property"].get<std::string>();
    std::vector<wikidata_statement> vals = wikidata_property(g_wd, id, pid);
    if (vals.empty()) {
        return text_result("No values for " + id + " / " + pid + ".");
    }
    json out = json::array();
    for (const auto & st : vals) out.push_back(wd_statement_json(st));
    return text_result(out.dump(2));
}

void register_wikidata_tools() {
    g_tools.push_back(Tool{
        "wikidata_search",
        "Search the offline Wikidata fact store for entities by label/alias. "
        "Returns a JSON list of {id, label, description} candidates for "
        "disambiguation; pass the chosen id to wikidata_entity. Plain words are "
        "matched as an implicit AND; you may also issue an FTS5 boolean "
        "expression (e.g. \"Eiffel OR \\\"Tour Eiffel\\\"\") to broaden recall.",
        json{
            { "type", "object" },
            { "properties", json{
                { "query", json{ { "type", "string" },
                                 { "description", "Entity name / keywords (or an FTS5 expression)." } } },
                { "limit", json{ { "type", "integer" },
                                 { "description", "Maximum number of results (default 10, max 50)." } } },
            } },
            { "required", json::array({ "query" }) },
        },
        tool_wikidata_search,
    });

    g_tools.push_back(Tool{
        "wikidata_entity",
        "Fetch a Wikidata entity by id (e.g. \"Q243\"). Returns "
        "{id, label, description, aliases, claims}, where each claim has its "
        "property and value resolved to human labels (e.g. height = \"330 metre\") "
        "with qualifiers (time ranges, applies-to-part, ...). Use wikidata_search "
        "first to find the id.",
        json{
            { "type", "object" },
            { "properties", json{
                { "id", json{ { "type", "string" },
                              { "description", "Wikidata entity id, e.g. \"Q243\"." } } },
            } },
            { "required", json::array({ "id" }) },
        },
        tool_wikidata_entity,
    });

    g_tools.push_back(Tool{
        "wikidata_property",
        "Fetch just one property's values for a Wikidata entity (convenience over "
        "wikidata_entity). Example: id=\"Q243\", property=\"P2048\" -> the height "
        "values. Returns a JSON list of resolved statements with qualifiers.",
        json{
            { "type", "object" },
            { "properties", json{
                { "id", json{ { "type", "string" },
                              { "description", "Wikidata entity id, e.g. \"Q243\"." } } },
                { "property", json{ { "type", "string" },
                                    { "description", "Wikidata property id, e.g. \"P2048\"." } } },
            } },
            { "required", json::array({ "id", "property" }) },
        },
        tool_wikidata_property,
    });
}

// -----------------------------------------------------------------
// JSON-RPC wire helpers (stdout is the protocol channel — keep it clean)
// -----------------------------------------------------------------

void send_message(const json & msg) {
    std::string line = msg.dump();
    line.push_back('\n');
    fputs(line.c_str(), stdout);
    fflush(stdout);
}

void send_result(const json & id, const json & result) {
    send_message(json{ { "jsonrpc", "2.0" }, { "id", id }, { "result", result } });
}

void send_error(const json & id, int code, const std::string & message) {
    send_message(json{
        { "jsonrpc", "2.0" },
        { "id", id },
        { "error", json{ { "code", code }, { "message", message } } },
    });
}

// -----------------------------------------------------------------
// Method dispatch
// -----------------------------------------------------------------

void handle_initialize(const json & id) {
    json result = {
        { "protocolVersion", MCP_PROTOCOL_VERSION },
        { "capabilities", json{ { "tools", json::object() } } },
        { "serverInfo", json{ { "name", MCP_SERVER_NAME },
                              { "version", "0.1" } } },
    };
    send_result(id, result);
}

void handle_tools_list(const json & id) {
    json tools = json::array();
    for (const auto & t : g_tools) {
        tools.push_back(json{
            { "name", t.name },
            { "description", t.description },
            { "inputSchema", t.inputSchema },
        });
    }
    send_result(id, json{ { "tools", tools } });
}

void handle_tools_call(const json & id, const json & params) {
    if (!params.is_object() || !params.contains("name") || !params["name"].is_string()) {
        send_error(id, -32602, "invalid params: missing tool name");
        return;
    }
    std::string name = params["name"].get<std::string>();
    const Tool * tool = find_tool(name);
    if (!tool) {
        send_error(id, -32602, "unknown tool: " + name);
        return;
    }
    json args = json::object();
    if (params.contains("arguments") && params["arguments"].is_object()) {
        args = params["arguments"];
    }
    json result;
    try {
        result = tool->handler(args);
    } catch (const std::exception & e) {
        result = text_result(std::string("tool error: ") + e.what(), true);
    }
    send_result(id, result);
}

// Process one parsed JSON-RPC message.
void dispatch(const json & msg) {
    std::string method = msg.value("method", std::string());
    bool has_id = msg.contains("id") && !msg["id"].is_null();
    json id = has_id ? msg["id"] : json(nullptr);
    json params = msg.contains("params") ? msg["params"] : json::object();

    if (method == "initialize") {
        handle_initialize(id);
    } else if (method == "notifications/initialized" || method == "initialized") {
        // Notification — no reply.
    } else if (method == "ping") {
        if (has_id) send_result(id, json::object());
    } else if (method == "tools/list") {
        handle_tools_list(id);
    } else if (method == "tools/call") {
        handle_tools_call(id, params);
    } else if (method.rfind("notifications/", 0) == 0) {
        // Any other notification — ignore silently.
    } else {
        // Unknown method. Only respond to requests (those with an id).
        if (has_id) send_error(id, -32601, "method not found: " + method);
    }
}

void mcp_server_usage(FILE * f) {
    fprintf(f,
        "llamafile mcp-server - run a Model Context Protocol server over stdio\n"
        "\n"
        "Exposes the embedded-Wikipedia tools (wiki_search, wiki_get_article) to\n"
        "MCP clients (Claude Code, opencode, Cursor) over newline-delimited\n"
        "JSON-RPC 2.0 on stdin/stdout.\n"
        "\n"
        "usage:\n"
        "  llamafile mcp-server [--zim PATH] [--wikidata PATH] [--browser[-attach [PORT]]]\n"
        "\n"
        "options:\n"
        "  --zim PATH         path to a .zim archive (default: $LLAMAFILE_ZIM or a\n"
        "                     bundled /zip/wikipedia.zim if present); enables\n"
        "                     wiki_search / wiki_get_article.\n"
        "  --wikidata PATH    path to a Wikidata .sqlite store (default:\n"
        "                     $LLAMAFILE_WIKIDATA); enables wikidata_search /\n"
        "                     wikidata_entity / wikidata_property.\n"
        "  --browser          expose the browser_* tools AND the code interpreter\n"
        "                     (code_run_js / code_render_html) via in-process CDP;\n"
        "                     LAUNCH a headless Chrome on demand. OFF by default.\n"
        "  --browser-attach [PORT]\n"
        "                     ATTACH to a Chrome you already started with\n"
        "                     --remote-debugging-port (default 9222) instead of\n"
        "                     launching one. The model can act in your LIVE browser.\n"
        "  --browser-headed   LAUNCH a visible window instead of headless.\n"
        "  --browser-path P   path to the Chrome/Chromium executable (override).\n"
        "  --browser-allow L  comma-separated domain allowlist (e.g. en.wikipedia.org).\n"
        "\n"
        "At least one source (--zim, --wikidata or --browser) must be provided.\n"
        "\n"
        "register with Claude Code:\n"
        "  claude mcp add wikipedia -- /path/to/llamafile mcp-server --zim PATH\n"
        "  claude mcp add browser   -- /path/to/llamafile mcp-server --browser\n");
}

} // namespace

// Returns a process exit code.
int mcp_server_main(int argc, char ** argv) {
    // argv: [0]=llamafile [1]=mcp-server [...]=args
    const char * zim_path = nullptr;
    const char * wikidata_path = nullptr;
    bool zim_explicit = false;
    browser::Options bopts;

    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "--zim") && i + 1 < argc) {
            zim_path = argv[++i];
            zim_explicit = true;
        } else if (!strcmp(argv[i], "--wikidata") && i + 1 < argc) {
            wikidata_path = argv[++i];
        } else if (!strcmp(argv[i], "--browser")) {
            bopts.enabled = true;
        } else if (!strcmp(argv[i], "--browser-attach")) {
            bopts.enabled = true;
            bopts.attach = true;
            // Optional PORT follows (only if it looks like a number).
            if (i + 1 < argc && argv[i + 1][0] >= '0' && argv[i + 1][0] <= '9') {
                bopts.attach_port = atoi(argv[++i]);
            }
        } else if (!strcmp(argv[i], "--browser-headed")) {
            bopts.enabled = true;
            bopts.headed = true;
        } else if (!strcmp(argv[i], "--browser-path") && i + 1 < argc) {
            bopts.enabled = true;
            bopts.browser_path = argv[++i];
        } else if (!strcmp(argv[i], "--browser-allow") && i + 1 < argc) {
            std::string list = argv[++i];
            size_t start = 0;
            while (start <= list.size()) {
                size_t comma = list.find(',', start);
                std::string item = list.substr(
                    start, comma == std::string::npos ? std::string::npos : comma - start);
                if (!item.empty()) bopts.allow.push_back(item);
                if (comma == std::string::npos) break;
                start = comma + 1;
            }
        } else if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) {
            mcp_server_usage(stdout);
            return 0;
        } else {
            fprintf(stderr, "mcp-server: ignoring unknown argument '%s'\n", argv[i]);
        }
    }

    if (!zim_path) zim_path = getenv("LLAMAFILE_ZIM");
    bool zim_used_default = false;
    if (!zim_path) { zim_path = WIKI_DEFAULT_ZIM; zim_used_default = true; }

    // Each source (Wikipedia ZIM, Wikidata store, browser) is optional and
    // additive: register whatever opens. A hard error only if NOTHING enabled
    // (checked once below). Diagnostics go to stderr — stdout is the protocol.
    g_zim = zim_open(zim_path);
    if (g_zim) {
        register_wiki_tools();
    } else if (zim_explicit || !zim_used_default) {
        fprintf(stderr, "mcp-server: failed to open ZIM '%s': %s\n",
                zim_path, zim_error());
    }

    // Wikidata structured-fact store (SQLite+FTS5).
    if (!wikidata_path) wikidata_path = getenv("LLAMAFILE_WIKIDATA");
    if (wikidata_path) {
        g_wd = wikidata_open(wikidata_path);
        if (g_wd) {
            register_wikidata_tools();
        } else {
            fprintf(stderr, "mcp-server: failed to open Wikidata store '%s': %s\n",
                    wikidata_path, wikidata_error());
        }
    }

    browser::register_browser_tools(bopts, [](const std::string & name,
                                              const std::string & description,
                                              const browser::json & inputSchema,
                                              std::function<browser::json(const browser::json &)> handler) {
        g_tools.push_back(Tool{ name, description, inputSchema, handler });
    });

    if (g_tools.empty()) {
        fprintf(stderr, "mcp-server: no tools enabled. Pass --zim PATH, "
                        "--wikidata PATH and/or --browser.\n");
        return 1;
    }

    fprintf(stderr, "mcp-server: serving %zu tool(s) over stdio\n", g_tools.size());

    // Read newline-delimited JSON-RPC from stdin, one message per line.
    std::string line;
    int ch;
    auto flush_line = [&]() {
        if (line.empty()) return;
        json msg;
        bool ok = true;
        try {
            msg = json::parse(line);
        } catch (const std::exception & e) {
            ok = false;
            // Parse error: per JSON-RPC, respond with -32700 and null id.
            send_error(json(nullptr), -32700, std::string("parse error: ") + e.what());
        }
        if (ok) {
            try {
                dispatch(msg);
            } catch (const std::exception & e) {
                fprintf(stderr, "mcp-server: dispatch error: %s\n", e.what());
            }
        }
        line.clear();
    };

    while ((ch = getchar()) != EOF) {
        if (ch == '\n') {
            flush_line();
        } else if (ch != '\r') {
            line.push_back((char) ch);
        }
    }
    flush_line(); // handle a final line with no trailing newline

    zim_close(g_zim);
    g_zim = nullptr;
    if (g_wd) { wikidata_close(g_wd); g_wd = nullptr; }
    return 0;
}
