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
// An MCP client that connects to external MCP servers and bridges their tools
// into the server's `/tools` registry. There are TWO transports behind a single
// `McpServer`:
//
//   * stdio (the validated P3 prototype shape): posix_spawn + pipe2 +
//     newline-delimited JSON-RPC 2.0. A `--mcp '<command line>'` whose value is
//     a command spawns a subprocess. (ddocs/prototypes/p3-mcp_client.cpp)
//
//   * remote Streamable-HTTP (MCP spec 2025-03-26): HTTP POST <endpoint> of one
//     JSON-RPC message with `Accept: application/json, text/event-stream`; the
//     reply is EITHER a single application/json body OR a text/event-stream SSE
//     body (`data:` lines parsed as JSON-RPC messages). The `Mcp-Session-Id`
//     response header from `initialize` is echoed on every later request. A
//     `--mcp 'http://host:port/path'` whose value is an http/https URL uses this
//     transport. (`--mcp-http <url>` is an explicit alias; see args.cpp.)
//
// cosmocc has NO in-binary TLS, so an `https://` endpoint cannot be reached
// directly — it fails with a clear message pointing at a local http:// proxy.
//
// Both transports expose the same surface (initialize -> notifications/initialized
// -> tools/list -> tools/call) and are bridged identically via `McpBridgedTool`.

#include "mcp_host.h"

#include "server-tools.h"  // server_tool, server_tools (llama.cpp/tools/server)

#include <nlohmann/json.hpp>

#include <cpp-httplib/httplib.h>  // vendored; also used by browser_tool.cpp

#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
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

// An `--mcp` value is a REMOTE endpoint when it is an http/https URL; otherwise
// it is a command line to spawn over stdio.
bool looks_like_url(const std::string & s) {
    return s.rfind("http://", 0) == 0 || s.rfind("https://", 0) == 0;
}

// ---------------------------------------------------------------------------
// transport interface: how an McpServer ships one JSON-RPC message and gets the
// matching reply back. stdio (spawned subprocess) vs remote Streamable-HTTP.
// All methods run under McpServer::io_mu (one request in flight at a time), so
// implementations need not lock.
// ---------------------------------------------------------------------------
struct McpTransport {
    virtual ~McpTransport() = default;

    // Establish the transport (spawn / validate URL). On failure sets `err` and
    // returns false. Does NOT perform the MCP handshake.
    virtual bool start(std::string & err) = 0;

    // Send a fully-formed JSON-RPC request (it carries jsonrpc/id/method[/params])
    // and return the reply object whose `id` matches. Null json on error.
    virtual json send_request(const json & msg, int id) = 0;

    // Fire-and-forget a JSON-RPC notification (no id, no reply expected).
    virtual void send_notify(const json & msg) = 0;

    virtual void stop() = 0;
    virtual bool alive() const = 0;
};

// ---------------------------------------------------------------------------
// stdio transport — posix_spawn + pipe2 + newline-delimited JSON-RPC (P3 shape)
// ---------------------------------------------------------------------------
struct StdioTransport : McpTransport {
    std::string cmdline;
    pid_t  pid = -1;
    FILE * to_child   = nullptr;  // we write JSON-RPC requests here
    FILE * from_child = nullptr;  // we read JSON-RPC responses here
    bool   alive_ = false;

    bool alive() const override { return alive_; }

    bool start(std::string & err) override {
        std::vector<std::string> argv_s = tokenize(cmdline);
        if (argv_s.empty()) { err = "empty --mcp command line"; return false; }
        std::vector<char *> argv;
        argv.reserve(argv_s.size() + 1);
        for (auto & a : argv_s) argv.push_back(const_cast<char *>(a.c_str()));
        argv.push_back(nullptr);

        int c_in[2], c_out[2];
        if (pipe2(c_in, 0) != 0) { err = std::string("pipe2: ") + strerror(errno); return false; }
        if (pipe2(c_out, 0) != 0) {
            err = std::string("pipe2: ") + strerror(errno);
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
            err = std::string("failed to spawn \"") + argv[0] + "\": " + strerror(rc);
            close(c_in[0]); close(c_in[1]);
            close(c_out[0]); close(c_out[1]);
            pid = -1;
            return false;
        }

        close(c_in[0]);
        close(c_out[1]);
        to_child   = fdopen(c_in[1],  "w");
        from_child = fdopen(c_out[0], "r");
        if (!to_child || !from_child) { err = "fdopen failed"; return false; }
        alive_ = true;
        return true;
    }

    // Write one JSON-RPC message line. Returns false on dead pipe.
    bool write_msg(const json & msg) {
        std::string line = msg.dump();
        line.push_back('\n');
        if (fwrite(line.data(), 1, line.size(), to_child) != line.size()) return false;
        if (fflush(to_child) != 0) return false;
        return true;
    }

    // Read one newline-delimited line into `out`. Uses a heap string (NOT a big
    // stack buffer) because tool calls run on small-stack HTTP worker threads
    // under cosmocc. Returns false at EOF.
    bool read_line(std::string & out) {
        out.clear();
        int c;
        while ((c = fgetc(from_child)) != EOF) {
            if (c == '\n') return true;
            out.push_back((char)c);
        }
        return !out.empty();  // trailing partial line at EOF
    }

    json send_request(const json & msg, int id) override {
        if (!alive_) return json();
        if (!write_msg(msg)) { alive_ = false; return json(); }
        // Read response lines until one carries the expected id; ignore anything
        // else (notifications, log lines that happen to be JSON, etc).
        std::string line;
        while (read_line(line)) {
            json j = json::parse(line, nullptr, /*allow_exceptions=*/false);
            if (j.is_discarded() || !j.is_object()) continue;
            if (j.contains("id") && j["id"].is_number_integer() &&
                j["id"].get<int>() == id) {
                return j;
            }
        }
        alive_ = false;  // pipe closed -> server died
        return json();
    }

    void send_notify(const json & msg) override {
        if (!alive_) return;
        if (!write_msg(msg)) alive_ = false;
    }

    void stop() override {
        if (to_child)   { fclose(to_child);   to_child = nullptr; }  // EOF -> child exits
        if (from_child) { fclose(from_child); from_child = nullptr; }
        if (pid > 0) {
            int st = 0;
            // give it a moment to exit on EOF, then SIGTERM
            for (int i = 0; i < 50; ++i) {
                pid_t r = waitpid(pid, &st, WNOHANG);
                if (r == pid || r < 0) { pid = -1; break; }
                usleep(10 * 1000);
            }
            if (pid > 0) { kill(pid, SIGTERM); waitpid(pid, &st, 0); pid = -1; }
        }
        alive_ = false;
    }
};

// ---------------------------------------------------------------------------
// remote Streamable-HTTP transport (MCP 2025-03-26)
// ---------------------------------------------------------------------------
struct HttpTransport : McpTransport {
    std::string url;         // original --mcp value (diagnostics)
    std::string host;
    int         port = 80;
    std::string path = "/";  // POST endpoint path
    bool        is_https = false;
    std::string session_id;  // captured from initialize's Mcp-Session-Id header
    bool        ok = false;
    std::string last_error;

    bool alive() const override { return ok; }

    // scheme://host[:port][/path]
    bool parse(const std::string & u) {
        url = u;
        std::string rest = u;
        std::string scheme = "http";
        size_t s = rest.find("://");
        if (s != std::string::npos) { scheme = rest.substr(0, s); rest = rest.substr(s + 3); }
        is_https = (scheme == "https");
        size_t slash = rest.find('/');
        std::string hostport = (slash == std::string::npos) ? rest : rest.substr(0, slash);
        path = (slash == std::string::npos) ? std::string("/") : rest.substr(slash);
        if (path.empty()) path = "/";
        size_t colon = hostport.find(':');
        if (colon != std::string::npos) {
            host = hostport.substr(0, colon);
            port = atoi(hostport.substr(colon + 1).c_str());
        } else {
            host = hostport;
            port = is_https ? 443 : 80;
        }
        return !host.empty() && port > 0;
    }

    bool start(std::string & err) override {
        if (is_https) {
            err = "remote MCP endpoint \"" + url + "\" uses https://, but this build has no "
                  "in-binary TLS (cosmocc). Terminate TLS in a local proxy and point --mcp at "
                  "its http://127.0.0.1 address instead.";
            last_error = err;
            ok = false;
            return false;
        }
        ok = true;
        return true;
    }

    void stop() override { ok = false; }

    httplib::Headers make_headers() const {
        httplib::Headers h;
        h.emplace("Accept", "application/json, text/event-stream");
        if (!session_id.empty()) h.emplace("Mcp-Session-Id", session_id);
        return h;
    }

    // Pull every JSON-RPC message out of a response body. For application/json
    // it is one object (or a batch array); for text/event-stream it is the
    // `data:` payloads of the SSE events.
    static void collect_messages(const std::string & body, const std::string & ctype,
                                 std::vector<json> & out) {
        bool sse = ctype.find("text/event-stream") != std::string::npos;
        if (!sse) {
            json j = json::parse(body, nullptr, /*allow_exceptions=*/false);
            if (!j.is_discarded()) {
                if (j.is_array()) { for (auto & e : j) out.push_back(e); }
                else              { out.push_back(j); }
            }
            return;
        }
        // SSE: events separated by a blank line; a `data:` payload may span
        // multiple lines (joined by '\n'). Parse each event's data as JSON.
        std::string data;
        bool have_data = false;
        auto flush = [&]() {
            if (have_data) {
                json j = json::parse(data, nullptr, /*allow_exceptions=*/false);
                if (!j.is_discarded()) out.push_back(j);
            }
            data.clear();
            have_data = false;
        };
        size_t i = 0, n = body.size();
        while (i < n) {
            size_t eol = body.find('\n', i);
            std::string line = body.substr(i, (eol == std::string::npos ? n : eol) - i);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            i = (eol == std::string::npos) ? n : eol + 1;
            if (line.empty()) { flush(); continue; }       // event boundary
            if (line[0] == ':') continue;                  // SSE comment
            if (line.rfind("data:", 0) == 0) {
                std::string v = line.substr(5);
                if (!v.empty() && v[0] == ' ') v.erase(0, 1);
                if (have_data) data.push_back('\n');
                data += v;
                have_data = true;
            }
            // other SSE fields (event:, id:, retry:) ignored
        }
        flush();
    }

    json send_request(const json & msg, int id) override {
        if (!ok) return json();
        httplib::Client cli(host, port);
        cli.set_connection_timeout(5, 0);
        cli.set_read_timeout(60, 0);
        httplib::Result res = cli.Post(path, make_headers(), msg.dump(), "application/json");
        if (!res) {
            last_error = "no HTTP response from " + host + ":" + std::to_string(port) + path +
                         " (connect/read failed)";
            return json();
        }
        if (res->status / 100 != 2) {
            last_error = "HTTP status " + std::to_string(res->status) + " from " + path;
            return json();
        }
        if (session_id.empty()) {
            std::string sid = res->get_header_value("Mcp-Session-Id");
            if (!sid.empty()) session_id = sid;
        }
        std::vector<json> msgs;
        collect_messages(res->body, res->get_header_value("Content-Type"), msgs);
        for (auto & m : msgs) {
            if (m.is_object() && m.contains("id") && m["id"].is_number_integer() &&
                m["id"].get<int>() == id) {
                return m;
            }
        }
        last_error = "no matching JSON-RPC reply (id=" + std::to_string(id) + ") in HTTP response";
        return json();
    }

    void send_notify(const json & msg) override {
        if (!ok) return;
        httplib::Client cli(host, port);
        cli.set_connection_timeout(5, 0);
        cli.set_read_timeout(10, 0);
        cli.Post(path, make_headers(), msg.dump(), "application/json");  // 202 expected; ignore
    }
};

// ---------------------------------------------------------------------------
// a single MCP server connection (transport-agnostic)
// ---------------------------------------------------------------------------
struct McpServer {
    std::string spec;        // original --mcp string (command line OR url)
    std::string name;        // serverInfo.name, or a fallback
    std::unique_ptr<McpTransport> transport;
    int         next_id = 0;
    std::mutex  io_mu;       // serialize one request/response at a time
    std::vector<json> tool_defs;  // raw MCP tool objects from tools/list

    bool alive() const { return transport && transport->alive(); }

    // Build the right transport for `spec`, start it, and run the handshake.
    bool connect() {
        std::string err;
        if (looks_like_url(spec)) {
            auto t = std::make_unique<HttpTransport>();
            if (!t->parse(spec)) {
                fprintf(stderr, "mcp-host: malformed MCP URL \"%s\"\n", spec.c_str());
                return false;
            }
            transport = std::move(t);
        } else {
            auto t = std::make_unique<StdioTransport>();
            t->cmdline = spec;
            transport = std::move(t);
        }
        if (!transport->start(err)) {
            fprintf(stderr, "mcp-host: %s\n", err.c_str());
            return false;
        }
        return handshake();
    }

    // Synchronous request/response under the io lock. Returns null json on error.
    json request(const std::string & method, const json & params) {
        std::lock_guard<std::mutex> lock(io_mu);
        if (!alive()) return json();
        int id = ++next_id;
        json msg = {{"jsonrpc", "2.0"}, {"id", id}, {"method", method}};
        if (!params.is_null()) msg["params"] = params;
        return transport->send_request(msg, id);
    }

    // Fire-and-forget notification (no id, no response).
    void notify(const std::string & method, const json & params) {
        std::lock_guard<std::mutex> lock(io_mu);
        if (!alive()) return;
        json msg = {{"jsonrpc", "2.0"}, {"method", method}};
        if (!params.is_null()) msg["params"] = params;
        transport->send_notify(msg);
    }

    // initialize -> notifications/initialized -> tools/list
    bool handshake() {
        json init = request("initialize",
            json{{"protocolVersion", "2025-06-18"},
                 {"capabilities", json::object()},
                 {"clientInfo", json{{"name", "llamafile"}, {"version", "1.0"}}}});
        if (init.is_null() || !init.contains("result")) {
            fprintf(stderr, "mcp-host: initialize failed for \"%s\"\n", spec.c_str());
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
            fprintf(stderr, "mcp-host: tools/list failed for \"%s\"\n", spec.c_str());
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
        if (transport) transport->stop();
    }
};

// ---------------------------------------------------------------------------
// a server_tool that forwards to an MCP server (stdio or http)
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
        if (!server || !server->alive()) {
            return {{"error", "MCP server is unavailable (transport not running)"}};
        }
        json result = server->call(mcp_name, params.is_null() ? json::object() : params);
        if (result.is_null()) {
            return {{"error", "MCP server did not respond (transport may have died)"}};
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
std::vector<std::string> g_specs;
std::vector<std::unique_ptr<McpServer>> g_servers;

}  // namespace

// ---------------------------------------------------------------------------
// public API
// ---------------------------------------------------------------------------
void llamafile_mcp_add_server(const std::string & cmdline) {
    if (!cmdline.empty()) g_specs.push_back(cmdline);
}

int llamafile_mcp_server_count() {
    return (int)g_specs.size();
}

int llamafile_mcp_register_tools(server_tools & registry) {
    // Writing to an MCP subprocess that has died would raise SIGPIPE, whose
    // default action silently terminates the whole server. Ignore it so the
    // failing write returns EPIPE instead and we can degrade gracefully
    // (mark the server dead, return a tool error). Process-wide and harmless;
    // network servers universally ignore SIGPIPE.
    signal(SIGPIPE, SIG_IGN);

    int total = 0;
    for (const auto & spec : g_specs) {
        auto srv = std::make_unique<McpServer>();
        srv->spec = spec;
        if (!srv->connect()) {
            fprintf(stderr, "mcp-host: skipping \"%s\" (connect/handshake failed)\n", spec.c_str());
            srv->shutdown();
            continue;
        }
        fprintf(stderr, "mcp-host: \"%s\" up (server=%s, %zu tool(s))\n",
                spec.c_str(), srv->name.c_str(), srv->tool_defs.size());

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
        if (!s || !s->alive()) continue;
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

// ---------------------------------------------------------------------------
// `llamafile mcp-probe <cmd-or-url> [tool] [args-json]` — model-free harness
// ---------------------------------------------------------------------------
// Connects ONE MCP server (stdio command line OR http URL), runs the handshake,
// and prints a single JSON object to stdout:
//
//   {"server": <name>, "transport": "stdio"|"http",
//    "tools": [<raw MCP tool defs>],
//    "call": <tools/call result>}   // only if [tool] given
//
// Diagnostics go to stderr; stdout stays pure JSON. This exercises BOTH
// transports end-to-end through the real binary without loading a model, and is
// what the hermetic integration tests drive.
int llamafile_mcp_probe_main(int argc, char ** argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: llamafile mcp-probe <command-or-url> [tool] [args-json]\n");
        return 2;
    }
    signal(SIGPIPE, SIG_IGN);

    McpServer srv;
    srv.spec = argv[2];
    bool http = looks_like_url(srv.spec);
    if (!srv.connect()) {
        fprintf(stderr, "mcp-probe: failed to connect/handshake \"%s\"\n", srv.spec.c_str());
        srv.shutdown();
        return 1;
    }

    json out;
    out["server"] = srv.name;
    out["transport"] = http ? "http" : "stdio";
    out["tools"] = json::array();
    for (auto & t : srv.tool_defs) out["tools"].push_back(t);

    if (argc >= 4) {
        std::string tool = argv[3];
        json args = json::object();
        if (argc >= 5 && argv[4] && argv[4][0]) {
            json parsed = json::parse(argv[4], nullptr, /*allow_exceptions=*/false);
            if (!parsed.is_discarded() && parsed.is_object()) args = parsed;
        }
        out["call"] = srv.call(tool, args);
    }

    printf("%s\n", out.dump().c_str());
    fflush(stdout);
    srv.shutdown();
    return 0;
}
