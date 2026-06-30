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

//
// Bundle bootstrap: precomputed-system-prompt KV preload.
//
// A bundled llamafile ships a precomputed [tools + system] KV cache (built by
// scripts/build-qwen-wiki.sh) as a DEFLATE-compressed /zip/system.kv. Two
// pieces make a fresh chat pay ~0 system-prompt prefill:
//
//   1. --default-system FILE: inject the EXACT fixed system message the KV was
//      precomputed for whenever a chat request carries none. Without it the
//      first request would render [tools][user] (no system) and the cached
//      [tools][system] prefix would not match -> nothing reused.
//
//   2. --preload-kv FILENAME: once the server is listening, fire a one-shot
//      POST /slots/0?action=restore {"filename":FILENAME} over loopback so
//      slot 0 holds the [tools+system] KV before the first user request. Needs
//      --slot-save-path set to the directory (e.g. /zip/).
//
// Both flags are consumed in llamafile/args.cpp; the injection hook is called
// from the server chat-completions handler and the boot-restore from server.cpp
// right after the listening socket is up.
//

#include <cpp-httplib/httplib.h>
#include <nlohmann/json.hpp>

#include <pthread.h>
#include <unistd.h>

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

using json = nlohmann::ordered_json;

namespace {

std::string g_default_system;       // the fixed system message (empty = disabled)
std::string g_preload_kv_filename;  // KV file to restore at boot (empty = disabled)

// Parse a "host:port" listening address into a loopback-usable host + port.
// Bind-all addresses (0.0.0.0 / :: / [::]) are rewritten to 127.0.0.1; an
// IPv6 host in brackets is unwrapped.
void parse_listen_addr(const std::string & addr, std::string & host, int & port) {
    std::string a = addr;
    if (a.rfind("http://", 0) == 0)  a = a.substr(7);
    if (a.rfind("https://", 0) == 0) a = a.substr(8);
    auto pos = a.rfind(':');
    if (pos != std::string::npos && a.find(']') < pos) {
        host = a.substr(0, pos);
        try { port = std::stoi(a.substr(pos + 1)); } catch (...) {}
    } else if (pos != std::string::npos && a.find(']') == std::string::npos &&
               a.find(':') == pos) {
        host = a.substr(0, pos);
        try { port = std::stoi(a.substr(pos + 1)); } catch (...) {}
    } else {
        host = a;
    }
    if (!host.empty() && host.front() == '[' && host.back() == ']')
        host = host.substr(1, host.size() - 2);
    if (host.empty() || host == "0.0.0.0" || host == "::" || host == "[::]")
        host = "127.0.0.1";
}

struct PreloadCtx {
    std::string host;
    int         port;
    std::string filename;
    std::string api_key;
};

// One-shot boot restore: wait for /health, then POST the slot-0 restore. Any
// failure (missing/mismatched KV, no slot-save-path) is logged and ignored so
// the server keeps serving with a normal (un-preloaded) slot 0.
void * preload_thread_fn(void * arg) {
    PreloadCtx * ctx = static_cast<PreloadCtx *>(arg);

    httplib::Client cli(ctx->host, ctx->port);
    cli.set_connection_timeout(5, 0);
    cli.set_read_timeout(600, 0);   // restoring a multi-MiB KV can take a moment
    cli.set_write_timeout(60, 0);

    httplib::Headers hdr{{"Content-Type", "application/json"}};
    if (!ctx->api_key.empty()) hdr.emplace("Authorization", "Bearer " + ctx->api_key);

    // Wait (up to ~60s) for the server to report healthy before restoring.
    bool ready = false;
    for (int i = 0; i < 600; ++i) {
        auto r = cli.Get("/health", hdr);
        if (r && r->status == 200) { ready = true; break; }
        usleep(100 * 1000);
    }
    if (!ready) {
        fprintf(stderr, "preload-kv: server never became healthy; skipping restore\n");
        delete ctx;
        return nullptr;
    }

    json body = {{"filename", ctx->filename}};
    auto r = cli.Post("/slots/0?action=restore", hdr, body.dump(), "application/json");
    if (!r) {
        fprintf(stderr, "preload-kv: restore request failed (no response from /slots)\n");
    } else if (r->status != 200) {
        fprintf(stderr, "preload-kv: restore '%s' degraded (HTTP %d): %s\n",
                ctx->filename.c_str(), r->status, r->body.c_str());
    } else {
        fprintf(stderr, "preload-kv: restored '%s' into slot 0\n", ctx->filename.c_str());
    }
    delete ctx;
    return nullptr;
}

}  // namespace

// ---------------------------------------------------------------------------
// Public API (global namespace; called across the llamafile<->llama.cpp link
// boundary, same pattern as the other hooks in server.cpp/args.cpp).
// ---------------------------------------------------------------------------

// --default-system FILE: read the fixed default system message from FILE. A
// single trailing newline is trimmed so the on-disk text can end with one.
void llamafile_set_default_system_file(const char * path) {
    if (!path || !*path) return;
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        fprintf(stderr, "default-system: cannot open %s\n", path);
        return;
    }
    std::ostringstream ss;
    ss << f.rdbuf();
    g_default_system = ss.str();
    while (!g_default_system.empty() &&
           (g_default_system.back() == '\n' || g_default_system.back() == '\r'))
        g_default_system.pop_back();
}

const std::string & llamafile_default_system_prompt() {
    return g_default_system;
}

// Inject the default system message at the front of body["messages"] when the
// request carries no leading system message. A client-supplied system message
// (messages[0].role == "system") always wins. No-op when --default-system was
// not configured.
void llamafile_inject_default_system(nlohmann::ordered_json & body) {
    if (g_default_system.empty()) return;
    if (!body.contains("messages") || !body["messages"].is_array()) return;
    auto & msgs = body["messages"];
    if (!msgs.empty() && msgs[0].is_object() &&
        msgs[0].value("role", std::string()) == "system")
        return;
    json sys = {{"role", "system"}, {"content", g_default_system}};
    msgs.insert(msgs.begin(), sys);
}

// --preload-kv FILENAME: remember the KV file to restore once the server is up.
void llamafile_preload_kv_set(const char * filename) {
    if (filename && *filename) g_preload_kv_filename = filename;
}

bool llamafile_preload_kv_enabled() {
    return !g_preload_kv_filename.empty();
}

// Fire the one-shot boot restore on a detached background thread. Called from
// server.cpp once the listening socket is up. An explicit 8 MiB stack matches
// the other llamafile loopback-HTTP workers (Cosmopolitan's default thread
// stack overflows on the httplib + nlohmann/json call chain).
void llamafile_preload_kv_fire(const std::string & listening_address,
                               const std::string & api_key) {
    if (g_preload_kv_filename.empty()) return;

    auto * ctx = new PreloadCtx();
    ctx->host = "127.0.0.1";
    ctx->port = 8080;
    parse_listen_addr(listening_address, ctx->host, ctx->port);
    ctx->filename = g_preload_kv_filename;
    ctx->api_key  = api_key;

    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 8 * 1024 * 1024);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_t tid;
    if (pthread_create(&tid, &attr, preload_thread_fn, ctx) != 0) {
        fprintf(stderr, "preload-kv: failed to spawn restore thread\n");
        delete ctx;
    }
    pthread_attr_destroy(&attr);
}
