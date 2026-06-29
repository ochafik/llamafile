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

// llamafile MCP HOST — see mcp_host.h.
//
// An MCP client that spawns external MCP servers over stdio (newline-delimited
// JSON-RPC 2.0) and bridges their tools into the server's `/tools` registry.
// The stdio client shape is the validated P3 prototype
// (ddocs/prototypes/p3-mcp_client.cpp): posix_spawn + pipe2 + per-line JSON-RPC.

#include "mcp_host.h"

#include "server-tools.h"  // server_tool, server_tools (llama.cpp/tools/server)

#include <nlohmann/json.hpp>

#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

using json = nlohmann::ordered_json;

extern char **environ;

namespace {

// ---------------------------------------------------------------------------
// command-line tokenizer (minimal POSIX-shell-like: whitespace separated, with
// single quotes, double quotes, and backslash escaping)
// ---------------------------------------------------------------------------
std::vector<std::string> tokenize(const std::string & s) {
    std::vector<std::string> out;
    std::string cur;
    bool have = false;
    size_t i = 0, n = s.size();
    while (i < n) {
        char c = s[i];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            if (have) { out.push_back(cur); cur.clear(); have = false; }
            ++i;
            continue;
        }
        have = true;
        if (c == '\'') {
            ++i;
            while (i < n && s[i] != '\'') cur.push_back(s[i++]);
            if (i < n) ++i;  // closing quote
        } else if (c == '"') {
            ++i;
            while (i < n && s[i] != '"') {
                if (s[i] == '\\' && i + 1 < n &&
                    (s[i + 1] == '"' || s[i + 1] == '\\')) {
                    cur.push_back(s[i + 1]);
                    i += 2;
                } else {
                    cur.push_back(s[i++]);
                }
            }
            if (i < n) ++i;  // closing quote
        } else if (c == '\\' && i + 1 < n) {
            cur.push_back(s[i + 1]);
            i += 2;
        } else {
            cur.push_back(c);
            ++i;
        }
    }
    if (have) out.push_back(cur);
    return out;
}

// ---------------------------------------------------------------------------
// a single spawned MCP server connection
// ---------------------------------------------------------------------------
struct McpServer {
    std::string cmdline;     // original --mcp string (for diagnostics)
    std::string name;        // serverInfo.name, or a fallback
    pid_t       pid = -1;
    FILE *      to_child   = nullptr;  // we write JSON-RPC requests here
    FILE *      from_child = nullptr;  // we read JSON-RPC responses here
    bool        alive = false;
    int         next_id = 0;
    std::mutex  io_mu;       // serialize one request/response at a time
    std::vector<json> tool_defs;  // raw MCP tool objects from tools/list

    // Spawn the process and wire stdin/stdout to pipes.
    bool spawn() {
        std::vector<std::string> argv_s = tokenize(cmdline);
        if (argv_s.empty()) {
            fprintf(stderr, "mcp-host: empty --mcp command line\n");
            return false;
        }
        std::vector<char *> argv;
        argv.reserve(argv_s.size() + 1);
        for (auto & a : argv_s) argv.push_back(const_cast<char *>(a.c_str()));
        argv.push_back(nullptr);

        int c_in[2], c_out[2];
        if (pipe2(c_in, 0) != 0) { perror("mcp-host: pipe2"); return false; }
        if (pipe2(c_out, 0) != 0) {
            perror("mcp-host: pipe2");
            close(c_in[0]); close(c_in[1]);
            return false;
        }

        posix_spawn_file_actions_t fa;
        posix_spawn_file_actions_init(&fa);
        posix_spawn_file_actions_adddup2(&fa, c_in[0],  STDIN_FILENO);
        posix_spawn_file_actions_adddup2(&fa, c_out[1], STDOUT_FILENO);
        // child keeps our stderr (inherited) so its diagnostics reach the log;
        // close the parent ends in the child
        posix_spawn_file_actions_addclose(&fa, c_in[1]);
        posix_spawn_file_actions_addclose(&fa, c_out[0]);

        int rc = posix_spawnp(&pid, argv[0], &fa, nullptr, argv.data(), environ);
        posix_spawn_file_actions_destroy(&fa);
        if (rc != 0) {
            fprintf(stderr, "mcp-host: failed to spawn \"%s\": %s\n",
                    argv[0], strerror(rc));
            close(c_in[0]); close(c_in[1]);
            close(c_out[0]); close(c_out[1]);
            pid = -1;
            return false;
        }

        close(c_in[0]);
        close(c_out[1]);
        to_child   = fdopen(c_in[1],  "w");
        from_child = fdopen(c_out[0], "r");
        if (!to_child || !from_child) {
            fprintf(stderr, "mcp-host: fdopen failed\n");
            return false;
        }
        alive = true;
        return true;
    }

    // Send one JSON-RPC message. Returns false on write failure (dead pipe).
    bool send(const json & msg) {
        std::string line = msg.dump();
        line.push_back('\n');
        if (fwrite(line.data(), 1, line.size(), to_child) != line.size()) return false;
        if (fflush(to_child) != 0) return false;
        return true;
    }

    // Read one newline-delimited line from the child into `out`. Uses a heap
    // string (NOT a large stack buffer) because tool calls run on HTTP worker
    // threads whose stacks are small under cosmocc — a multi-KB stack array
    // would overflow and silently crash the process. Returns false at EOF.
    bool read_line(std::string & out) {
        out.clear();
        int c;
        while ((c = fgetc(from_child)) != EOF) {
            if (c == '\n') return true;
            out.push_back((char)c);
        }
        return !out.empty();  // trailing partial line at EOF
    }

    // Read response lines until one carries the expected id; ignore anything
    // else (notifications, log lines that happen to be JSON, etc).
    json recv_for(int id) {
        std::string line;
        while (read_line(line)) {
            json j = json::parse(line, nullptr, /*allow_exceptions=*/false);
            if (j.is_discarded() || !j.is_object()) continue;
            if (j.contains("id") && j["id"].is_number_integer() &&
                j["id"].get<int>() == id) {
                return j;
            }
        }
        return json();  // EOF / error
    }

    // Synchronous request/response under the io lock. Returns null json on error.
    json request(const std::string & method, const json & params) {
        std::lock_guard<std::mutex> lock(io_mu);
        if (!alive) return json();
        int id = ++next_id;
        json msg = {{"jsonrpc", "2.0"}, {"id", id}, {"method", method}};
        if (!params.is_null()) msg["params"] = params;
        if (!send(msg)) { alive = false; return json(); }
        json resp = recv_for(id);
        if (resp.is_null()) alive = false;  // pipe closed -> server died
        return resp;
    }

    // Fire-and-forget notification (no id, no response).
    void notify(const std::string & method, const json & params) {
        std::lock_guard<std::mutex> lock(io_mu);
        if (!alive) return;
        json msg = {{"jsonrpc", "2.0"}, {"method", method}};
        if (!params.is_null()) msg["params"] = params;
        if (!send(msg)) alive = false;
    }

    // initialize -> notifications/initialized -> tools/list
    bool handshake() {
        json init = request("initialize",
            json{{"protocolVersion", "2025-06-18"},
                 {"capabilities", json::object()},
                 {"clientInfo", json{{"name", "llamafile"}, {"version", "1.0"}}}});
        if (init.is_null() || !init.contains("result")) {
            fprintf(stderr, "mcp-host: initialize failed for \"%s\"\n", cmdline.c_str());
            return false;
        }
        name = "mcp";
        try {
            if (init["result"].contains("serverInfo") &&
                init["result"]["serverInfo"].contains("name")) {
                name = init["result"]["serverInfo"]["name"].get<std::string>();
            }
        } catch (...) {}

        notify("notifications/initialized", json::object());

        json tl = request("tools/list", json(nullptr));
        if (tl.is_null() || !tl.contains("result") ||
            !tl["result"].contains("tools") || !tl["result"]["tools"].is_array()) {
            fprintf(stderr, "mcp-host: tools/list failed for \"%s\"\n", cmdline.c_str());
            return false;
        }
        for (auto & t : tl["result"]["tools"]) tool_defs.push_back(t);
        return true;
    }

    // tools/call -> MCP result body {"content":[...],"isError":bool}
    json call(const std::string & tool_name, const json & arguments) {
        json resp = request("tools/call",
            json{{"name", tool_name}, {"arguments", arguments}});
        if (resp.is_null()) return json();          // dead
        if (resp.contains("error")) return resp;    // JSON-RPC error
        return resp.value("result", json());
    }

    void shutdown() {
        if (to_child)   { fclose(to_child);   to_child = nullptr; }  // EOF -> child exits
        if (from_child) { fclose(from_child); from_child = nullptr; }
        if (pid > 0) {
            int st = 0;
            // give it a moment to exit on EOF, then SIGTERM
            for (int i = 0; i < 50; ++i) {
                pid_t r = waitpid(pid, &st, WNOHANG);
                if (r == pid || r < 0) { pid = -1; return; }
                usleep(10 * 1000);
            }
            kill(pid, SIGTERM);
            waitpid(pid, &st, 0);
            pid = -1;
        }
        alive = false;
    }
};

// ---------------------------------------------------------------------------
// a server_tool that forwards to an MCP subprocess
// ---------------------------------------------------------------------------
struct McpBridgedTool : server_tool {
    McpServer * server;       // not owned
    std::string mcp_name;     // the tool name as the MCP server knows it
    std::string description;
    json        input_schema; // MCP "inputSchema" (a JSON Schema object)

    McpBridgedTool(McpServer * s, const std::string & registry_name,
                   const std::string & mcp_tool_name,
                   const std::string & desc, json schema)
        : server(s), mcp_name(mcp_tool_name), description(desc),
          input_schema(std::move(schema)) {
        name = registry_name;
        display_name = mcp_tool_name;
        permission_write = false;  // MCP-bridged tools are treated read-only
    }

    json get_definition() override {
        json fn = {
            {"name", name},
            {"description", description},
        };
        if (input_schema.is_object() && !input_schema.empty()) {
            fn["parameters"] = input_schema;
        } else {
            fn["parameters"] = json{{"type", "object"}, {"properties", json::object()}};
        }
        return {{"type", "function"}, {"function", fn}};
    }

    json invoke(json params) override {
        if (!server || !server->alive) {
            return {{"error", "MCP server is unavailable (subprocess not running)"}};
        }
        json result = server->call(mcp_name, params.is_null() ? json::object() : params);
        if (result.is_null()) {
            return {{"error", "MCP server did not respond (subprocess may have died)"}};
        }
        if (result.contains("error")) {
            // JSON-RPC error object
            std::string msg = "MCP error";
            try {
                if (result["error"].is_object() && result["error"].contains("message")) {
                    msg = result["error"]["message"].get<std::string>();
                } else {
                    msg = result["error"].dump();
                }
            } catch (...) {}
            return {{"error", msg}};
        }

        // MCP tools/call result: {"content":[{"type":"text","text":...}], "isError":bool}
        bool is_error = result.value("isError", false);
        std::string text;
        if (result.contains("content") && result["content"].is_array()) {
            for (auto & block : result["content"]) {
                if (block.is_object() && block.value("type", "") == "text") {
                    text += block.value("text", "");
                }
            }
        }
        if (text.empty()) text = result.dump();  // surface non-text payloads raw

        if (is_error) return {{"error", text}};
        return {{"plain_text_response", text}};
    }
};

// ---------------------------------------------------------------------------
// module state
// ---------------------------------------------------------------------------
std::vector<std::string> g_cmdlines;
std::vector<std::unique_ptr<McpServer>> g_servers;

}  // namespace

// ---------------------------------------------------------------------------
// public API
// ---------------------------------------------------------------------------
void llamafile_mcp_add_server(const std::string & cmdline) {
    if (!cmdline.empty()) g_cmdlines.push_back(cmdline);
}

int llamafile_mcp_server_count() {
    return (int)g_cmdlines.size();
}

int llamafile_mcp_register_tools(server_tools & registry) {
    // Writing to an MCP subprocess that has died would raise SIGPIPE, whose
    // default action silently terminates the whole server. Ignore it so the
    // failing write returns EPIPE instead and we can degrade gracefully
    // (mark the server dead, return a tool error). Process-wide and harmless;
    // network servers universally ignore SIGPIPE.
    signal(SIGPIPE, SIG_IGN);

    int total = 0;
    for (const auto & cmd : g_cmdlines) {
        auto srv = std::make_unique<McpServer>();
        srv->cmdline = cmd;
        if (!srv->spawn()) {
            fprintf(stderr, "mcp-host: skipping \"%s\" (spawn failed)\n", cmd.c_str());
            continue;
        }
        if (!srv->handshake()) {
            fprintf(stderr, "mcp-host: skipping \"%s\" (handshake failed)\n", cmd.c_str());
            srv->shutdown();
            continue;
        }
        fprintf(stderr, "mcp-host: \"%s\" up (server=%s, %zu tool(s))\n",
                cmd.c_str(), srv->name.c_str(), srv->tool_defs.size());

        McpServer * srv_ptr = srv.get();
        for (auto & def : srv->tool_defs) {
            std::string mcp_name;
            std::string desc;
            json schema = json::object();
            try {
                mcp_name = def.at("name").get<std::string>();
                if (def.contains("description") && def["description"].is_string()) {
                    desc = def["description"].get<std::string>();
                }
                if (def.contains("inputSchema") && def["inputSchema"].is_object()) {
                    schema = def["inputSchema"];
                }
            } catch (...) {
                continue;  // malformed tool entry
            }

            // Namespacing: keep the bare name unless it collides with a tool
            // already in the registry (built-in or a prior MCP server), in which
            // case prefix it to avoid silent hijack.
            std::string registry_name = mcp_name;
            bool collision = false;
            for (const auto & t : registry.tools) {
                if (t->name == registry_name) { collision = true; break; }
            }
            if (collision) {
                registry_name = "mcp__" + srv_ptr->name + "__" + mcp_name;
                fprintf(stderr, "mcp-host: tool \"%s\" collides; bridging as \"%s\"\n",
                        mcp_name.c_str(), registry_name.c_str());
            }

            registry.tools.push_back(std::make_unique<McpBridgedTool>(
                srv_ptr, registry_name, mcp_name, desc, schema));
            ++total;
        }
        g_servers.push_back(std::move(srv));
    }
    return total;
}

void llamafile_mcp_shutdown() {
    for (auto & s : g_servers) {
        if (s) s->shutdown();
    }
    g_servers.clear();
}

// ---------------------------------------------------------------------------
// direct tool access (webcam-agent ACTION_OTHER loop)
// ---------------------------------------------------------------------------
namespace {

// Flatten an MCP tools/call result (or JSON-RPC error) into plain text.
std::string mcp_result_to_text(const json & result) {
    if (result.is_null()) return "(mcp error: server did not respond)";
    if (result.is_object() && result.contains("error")) {
        try {
            if (result["error"].is_object() && result["error"].contains("message")) {
                return "(mcp error: " + result["error"]["message"].get<std::string>() + ")";
            }
            return "(mcp error: " + result["error"].dump() + ")";
        } catch (...) { return "(mcp error)"; }
    }
    bool is_error = result.value("isError", false);
    std::string text;
    if (result.contains("content") && result["content"].is_array()) {
        for (auto & block : result["content"]) {
            if (block.is_object() && block.value("type", "") == "text") {
                text += block.value("text", "");
            }
        }
    }
    if (text.empty()) text = result.dump();
    return is_error ? "(mcp error: " + text + ")" : text;
}

bool server_has_tool(const McpServer * s, const std::string & name) {
    if (!s) return false;
    for (const auto & def : s->tool_defs) {
        try {
            if (def.at("name").get<std::string>() == name) return true;
        } catch (...) {}
    }
    return false;
}

}  // namespace

bool llamafile_mcp_has_tool(const std::string & name) {
    for (const auto & s : g_servers) {
        if (server_has_tool(s.get(), name)) return true;
    }
    return false;
}

std::string llamafile_mcp_call_tool(const std::string & name,
                                    const std::string & arguments_json) {
    json args = json::parse(arguments_json.empty() ? "{}" : arguments_json,
                            nullptr, /*allow_exceptions=*/false);
    if (args.is_discarded() || !args.is_object()) args = json::object();
    for (const auto & s : g_servers) {
        if (!s || !s->alive) continue;
        if (!server_has_tool(s.get(), name)) continue;
        json result = s->call(name, args);
        return mcp_result_to_text(result);
    }
    return "(mcp error: no MCP server exposes tool '" + name + "')";
}

std::string llamafile_mcp_tools_prompt() {
    std::string out;
    for (const auto & s : g_servers) {
        if (!s) continue;
        for (const auto & def : s->tool_defs) {
            try {
                std::string n = def.at("name").get<std::string>();
                std::string d = (def.contains("description") && def["description"].is_string())
                                    ? def["description"].get<std::string>() : std::string();
                out += "- " + n + ": " + d;
                if (def.contains("inputSchema") && def["inputSchema"].is_object()) {
                    out += " arguments (JSON schema): " + def["inputSchema"].dump();
                }
                out += "\n";
            } catch (...) {}
        }
    }
    return out;
}
