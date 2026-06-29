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

// llamafile WEBCAM AGENT — see webcam_agent.h.
//
// One dedicated llama_context + mtmd_context + vlib::session ("one slot"),
// driven by /agent/* HTTP+SSE endpoints. The frame agentic loop:
//
//   process_frame -> action
//     ACTION_DO_NOTHING / NOTE / SPEAK / NONE  -> terminal (handled in-session)
//     ACTION_OTHER (external tool_call)        -> llamafile_mcp_call_tool ->
//                                                 session.continue_after_tool ->
//                                                 repeat until terminal
//
// Threading is cosmocc-safe: HTTP handlers run on small-stack worker threads,
// so every heavy step (context/mtmd creation, decode, the tool loop, JSON) is
// trampolined onto a fresh 8 MiB-stack pthread (no std::thread). SSE is a
// chunked text/event-stream drained by the events handler from a broker.

#include "webcam_agent.h"

#include "server-http.h"

#include "common.h"
#include "llama.h"
#include "mtmd.h"
#include "mtmd-helper.h"

#include "mcp_host.h"
#include "vlib_video/vlib_video_session.h"

#include "stb/stb_image.h"
#include "stb/stb_image_resize2.h"

#include <nlohmann/json.hpp>

#include <pthread.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

using json = nlohmann::ordered_json;

namespace {

// ---------------------------------------------------------------------------
// cosmocc-safe big-stack trampoline (HTTP worker stacks are tiny; the model
// decode + httplib/json/mtmd call chains overflow them — run on 8 MiB pthreads)
// ---------------------------------------------------------------------------
struct ThreadJob { std::function<void()> fn; };

void * big_stack_trampoline(void * arg) {
    static_cast<ThreadJob *>(arg)->fn();
    return nullptr;
}

void run_joined_big_stack(std::function<void()> fn) {
    ThreadJob job{std::move(fn)};
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 8 * 1024 * 1024);
    pthread_t tid;
    int rc = pthread_create(&tid, &attr, big_stack_trampoline, &job);
    pthread_attr_destroy(&attr);
    if (rc != 0) { job.fn(); return; }  // fallback: inline
    pthread_join(tid, nullptr);
}

// ---------------------------------------------------------------------------
// SSE event broker: a monotonically growing event log + a condition variable.
// Each /agent/events subscriber tracks its own cursor and only sees events
// published AFTER it connected (live stream semantics).
// ---------------------------------------------------------------------------
struct EventBroker {
    std::mutex               mu;
    std::condition_variable  cv;
    std::vector<std::string> log;       // JSON event payloads
    bool                     closed = false;

    void publish(const std::string & ev) {
        std::lock_guard<std::mutex> lk(mu);
        log.push_back(ev);
        cv.notify_all();
    }
    void close() {
        std::lock_guard<std::mutex> lk(mu);
        closed = true;
        cv.notify_all();
    }
    void reopen() {
        std::lock_guard<std::mutex> lk(mu);
        log.clear();
        closed = false;
    }
    size_t cursor_now() {
        std::lock_guard<std::mutex> lk(mu);
        return log.size();
    }
};

// ---------------------------------------------------------------------------
// module state — exactly one session ("one slot")
// ---------------------------------------------------------------------------
struct WebcamAgent {
    bool enabled = false;

    // captured at route-registration time
    common_params                       params;          // copy
    std::function<llama_context *()>     get_server_ctx;  // server's live ctx

    // lifecycle (guarded by life_mu)
    std::mutex                           life_mu;
    bool                                 started = false;
    std::atomic<bool>                    busy{false};     // one frame in flight
    std::atomic<int>                     cur_frame{0};    // for callback events

    // live resources
    llama_context *                      lctx = nullptr;
    mtmd_context  *                      mctx = nullptr;
    std::unique_ptr<vlib::session>       sess;
    int                                  frame_size = 448;

    EventBroker                          broker;
};

WebcamAgent g_wa;

constexpr int  MAX_TOOL_ROUNDS = 4;   // per-frame external-tool loop guard

const char * kind_name(vlib::action_kind k) {
    switch (k) {
        case vlib::ACTION_NONE:       return "none";
        case vlib::ACTION_DO_NOTHING: return "ignore_frame";
        case vlib::ACTION_SPEAK:      return "speak";
        case vlib::ACTION_NOTE:       return "note";
        case vlib::ACTION_OTHER:      return "other";
    }
    return "?";
}

json args_to_json(const vlib::tool_call & tc) {
    json a = json::object();
    for (const auto & kv : tc.arguments) a[kv.first] = kv.second;
    return a;
}

// ---------------------------------------------------------------------------
// HTTP response helpers
// ---------------------------------------------------------------------------
server_http_res_ptr json_res(int status, const json & body) {
    auto r = std::make_unique<server_http_res>();
    r->status = status;
    r->content_type = "application/json; charset=utf-8";
    r->data = body.dump();
    return r;
}

// ---------------------------------------------------------------------------
// system prompt: watch goal + built-in video tools + bridged MCP tools
// ---------------------------------------------------------------------------
std::string build_system_prompt(const std::string & goal) {
    std::string g = goal.empty()
        ? "Watch the scene and report meaningful changes." : goal;
    std::string mcp_tools = llamafile_mcp_tools_prompt();
    std::string s =
        "<|im_start|>system\n"
        "You are a real-time video-watching agent. You are shown frames one at a "
        "time; each frame is encoded together with the last frame you reacted to, "
        "so you effectively see what changed. After each frame respond with "
        "EXACTLY ONE tool call wrapped in <tool_call>...</tool_call> as JSON: "
        "{\"name\": <tool>, \"arguments\": {...}}.\n"
        "Available tools:\n"
        "- ignore_frame: nothing noteworthy changed. arguments: {}\n"
        "- note: record a brief observation for later. arguments: {\"observation\": \"...\"}\n"
        "- speak: say something out loud to the user right now. arguments: {\"text\": \"...\"}\n";
    if (!mcp_tools.empty()) {
        s += "Connected action tools (call one of these when the watched event occurs):\n";
        s += mcp_tools;
    }
    s += "Watch goal: " + g + "\n"
         "You MUST wrap the call exactly like "
         "<tool_call>{\"name\": ..., \"arguments\": {...}}</tool_call>. "
         "Do not write any other text and do not think out loud. /no_think<|im_end|>\n";
    return s;
}

// ---------------------------------------------------------------------------
// session lifecycle (run on a big stack)
// ---------------------------------------------------------------------------
void destroy_session_locked() {
    g_wa.sess.reset();
    if (g_wa.mctx) { mtmd_free(g_wa.mctx); g_wa.mctx = nullptr; }
    if (g_wa.lctx) { llama_free(g_wa.lctx); g_wa.lctx = nullptr; }
    g_wa.started = false;
}

// Returns 0 on success or a negative error code; fills err_msg on failure.
int start_session_locked(const std::string & prompt, int frame_size, std::string & err_msg) {
    destroy_session_locked();

    llama_context * server_ctx = g_wa.get_server_ctx ? g_wa.get_server_ctx() : nullptr;
    if (!server_ctx) { err_msg = "model not loaded yet"; return -1; }
    llama_model * model = const_cast<llama_model *>(llama_get_model(server_ctx));
    if (!model) { err_msg = "no model bound to server context"; return -1; }

    if (frame_size <= 0) frame_size = g_wa.params.mmproj.path.empty() ? 448 : 448;
    if (frame_size % 28 != 0) frame_size = (frame_size / 28) * 28;
    if (frame_size < 28) frame_size = 28;
    g_wa.frame_size = frame_size;

    // Dedicated context from the shared model (separate KV from the server's
    // slots — the session manipulates the KV cache directly).
    common_params cp = g_wa.params;
    if (cp.n_ctx <= 0 || cp.n_ctx > (int) llama_model_n_ctx_train(model)) {
        cp.n_ctx = llama_model_n_ctx_train(model);
    }
    llama_context_params lcp = common_context_params_to_llama(cp);
    g_wa.lctx = llama_init_from_model(model, lcp);
    if (!g_wa.lctx) { err_msg = "failed to create dedicated llama_context"; return -2; }

    // Dedicated mtmd from the same mmproj file.
    if (g_wa.params.mmproj.path.empty()) {
        destroy_session_locked();
        err_msg = "no --mmproj configured (webcam-agent needs a vision projector)";
        return -3;
    }
    mtmd_context_params mparams = mtmd_context_params_default();
    mparams.use_gpu       = (g_wa.params.n_gpu_layers != 0);
    mparams.print_timings = false;
    mparams.n_threads     = g_wa.params.cpuparams.n_threads;
    g_wa.mctx = mtmd_init_from_file(g_wa.params.mmproj.path.c_str(), model, mparams);
    if (!g_wa.mctx) {
        destroy_session_locked();
        err_msg = "failed to init mmproj / mtmd";
        return -4;
    }

    vlib::session_params sp;
    sp.nx = (uint32_t) frame_size;
    sp.ny = (uint32_t) frame_size;
    sp.n_batch = cp.n_batch > 0 ? cp.n_batch : 512;
    sp.max_tool_tokens = 256;
    sp.system_prompt = build_system_prompt(prompt);

    g_wa.sess = vlib::session::create(g_wa.lctx, model, g_wa.mctx, sp);

    g_wa.sess->set_speak_cb([](const std::string & text, void *) {
        g_wa.broker.publish(json{
            {"type", "speak"}, {"frame", g_wa.cur_frame.load()}, {"text", text}}.dump());
    }, nullptr);
    g_wa.sess->set_note_cb([](const std::string & obs, void *) {
        g_wa.broker.publish(json{
            {"type", "note"}, {"frame", g_wa.cur_frame.load()}, {"text", obs}}.dump());
    }, nullptr);

    if (g_wa.sess->start() != 0) {
        destroy_session_locked();
        err_msg = "session start (system prompt decode) failed";
        return -5;
    }

    g_wa.started = true;
    g_wa.cur_frame.store(0);
    g_wa.broker.reopen();
    g_wa.broker.publish(json{{"type", "started"}, {"frame_size", frame_size}}.dump());
    return 0;
}

// ---------------------------------------------------------------------------
// frame intake: decode JPEG -> RGB, run process_frame + the ACTION_OTHER loop
// ---------------------------------------------------------------------------
std::vector<unsigned char> decode_resize(const unsigned char * jpeg, size_t len, int size) {
    int nx = 0, ny = 0, comp = 0;
    unsigned char * px = stbi_load_from_memory(jpeg, (int) len, &nx, &ny, &comp, 3);
    if (!px) return {};
    std::vector<unsigned char> out((size_t) size * size * 3);
    int rc = (int) (intptr_t) stbir_resize_uint8_srgb(
        px, nx, ny, 0, out.data(), size, size, 0, (stbir_pixel_layout) STBIR_RGB);
    stbi_image_free(px);
    if (rc == 0) return {};
    return out;
}

// Build the per-frame action JSON (also used as the SSE "frame" event).
json action_json(const char * type, int frame, const vlib::action & act) {
    json j = {
        {"type", type},
        {"frame", frame},
        {"action", kind_name(act.kind)},
    };
    if (!act.call.name.empty()) {
        j["tool_call"] = json{{"name", act.call.name}, {"arguments", args_to_json(act.call)}};
    }
    if (!act.raw_assistant_text.empty()) {
        std::string raw = act.raw_assistant_text;
        if (raw.size() > 400) raw = raw.substr(0, 400) + "...";
        j["raw"] = raw;
    }
    return j;
}

// Runs on a big stack. Fills `out_body` with the final action JSON.
void process_frame_job(std::vector<unsigned char> jpeg, json & out_body) {
    int frame = g_wa.cur_frame.fetch_add(1) + 1;

    std::vector<unsigned char> rgb = decode_resize(jpeg.data(), jpeg.size(), g_wa.frame_size);
    if (rgb.empty()) {
        out_body = json{{"error", "failed to decode/resize JPEG"}};
        g_wa.broker.publish(json{{"type", "error"}, {"frame", frame},
                                 {"message", "jpeg decode failed"}}.dump());
        return;
    }

    vlib::action act = g_wa.sess->process_frame(rgb.data());
    g_wa.broker.publish(action_json("frame", frame, act).dump());

    int rounds = 0;
    while (act.kind == vlib::ACTION_OTHER && rounds < MAX_TOOL_ROUNDS) {
        ++rounds;
        const std::string name = act.call.name;
        const json        args = args_to_json(act.call);

        g_wa.broker.publish(json{
            {"type", "tool_call"}, {"frame", frame}, {"round", rounds},
            {"name", name}, {"arguments", args}}.dump());

        std::string result;
        if (llamafile_mcp_has_tool(name)) {
            result = llamafile_mcp_call_tool(name, args.dump());
        } else {
            result = "(no connected tool named '" + name + "')";
        }
        g_wa.broker.publish(json{
            {"type", "tool_result"}, {"frame", frame}, {"round", rounds},
            {"name", name}, {"result", result}}.dump());

        act = g_wa.sess->continue_after_tool(result);
        g_wa.broker.publish(action_json("continuation", frame, act).dump());
    }

    out_body = action_json("frame", frame, act);
    out_body["frame_count"]   = g_wa.sess->frame_count();
    out_body["n_past"]        = (int) g_wa.sess->n_past();
    out_body["tool_rounds"]   = rounds;
}

// ---------------------------------------------------------------------------
// route handlers
// ---------------------------------------------------------------------------
server_http_res_ptr handle_start(const server_http_req & req) {
    std::string prompt;
    int frame_size = 0;
    std::string body = req.body;

    std::string err;
    int rc = 0;
    run_joined_big_stack([&]() {
        json j = json::parse(body.empty() ? "{}" : body, nullptr, /*allow_exceptions=*/false);
        if (j.is_object()) {
            if (j.contains("prompt") && j["prompt"].is_string()) prompt = j["prompt"].get<std::string>();
            if (j.contains("frame_size") && j["frame_size"].is_number_integer())
                frame_size = j["frame_size"].get<int>();
        }
        std::lock_guard<std::mutex> lk(g_wa.life_mu);
        rc = start_session_locked(prompt, frame_size, err);
    });

    if (rc != 0) return json_res(503, json{{"ok", false}, {"error", err}});
    return json_res(200, json{
        {"ok", true},
        {"session", "webcam"},
        {"frame_size", g_wa.frame_size},
    });
}

server_http_res_ptr handle_frame(const server_http_req & req) {
    if (!g_wa.started) {
        return json_res(409, json{{"error", "no active session; POST /agent/start first"}});
    }
    // One frame in flight (single slot) — mirror the doc's 409-on-busy.
    bool expected = false;
    if (!g_wa.busy.compare_exchange_strong(expected, true)) {
        return json_res(409, json{{"error", "busy: a frame is already in flight"}});
    }

    // Extract JPEG bytes: multipart file field (any) OR raw request body.
    std::vector<unsigned char> jpeg;
    if (!req.files.empty()) {
        const uploaded_file & f = req.files.begin()->second;
        jpeg.assign(f.data.begin(), f.data.end());
    } else {
        jpeg.assign(req.body.begin(), req.body.end());
    }

    if (jpeg.empty()) {
        g_wa.busy.store(false);
        return json_res(400, json{{"error", "no JPEG bytes (send raw body or multipart file)"}});
    }

    json out;
    run_joined_big_stack([&]() { process_frame_job(std::move(jpeg), out); });
    g_wa.busy.store(false);

    int status = out.contains("error") ? 400 : 200;
    return json_res(status, out);
}

server_http_res_ptr handle_events(const server_http_req &) {
    auto r = std::make_unique<server_http_res>();
    r->status = 200;
    r->content_type = "text/event-stream";
    r->headers["Cache-Control"] = "no-cache";
    r->headers["Connection"]    = "keep-alive";

    auto cursor = std::make_shared<size_t>(g_wa.broker.cursor_now());
    EventBroker * b = &g_wa.broker;
    r->next = [b, cursor](std::string & chunk) -> bool {
        std::unique_lock<std::mutex> lk(b->mu);
        b->cv.wait_for(lk, std::chrono::seconds(15), [&]() {
            return b->closed || *cursor < b->log.size();
        });
        if (*cursor < b->log.size()) {
            chunk = "data: " + b->log[*cursor] + "\n\n";
            ++(*cursor);
            return true;  // keep the stream open
        }
        if (b->closed) {
            chunk = "event: close\ndata: {\"type\":\"close\"}\n\n";
            return false;  // end the stream
        }
        chunk = ": keepalive\n\n";  // heartbeat; write fails if client gone
        return true;
    };
    return r;
}

server_http_res_ptr handle_stop(const server_http_req &) {
    run_joined_big_stack([&]() {
        std::lock_guard<std::mutex> lk(g_wa.life_mu);
        if (g_wa.started) {
            g_wa.broker.publish(json{{"type", "stopped"}}.dump());
            destroy_session_locked();
        }
    });
    g_wa.broker.close();
    g_wa.busy.store(false);
    return json_res(200, json{{"ok", true}});
}

server_http_res_ptr handle_not_implemented(const server_http_req &) {
    // /agent/clip + /agent/live are step 3 (the 30s-buffer email attachment +
    // live-link relay; the browser assembles the clip).
    return json_res(501, json{
        {"error", "not implemented yet"},
        {"note", "clip/live (30s-buffer email attachment + live-link relay) is step 3"}});
}

}  // namespace

// ---------------------------------------------------------------------------
// public API
// ---------------------------------------------------------------------------
void llamafile_webcam_enable()  { g_wa.enabled = true; }
bool llamafile_webcam_enabled() { return g_wa.enabled; }

void llamafile_webcam_register_routes(server_http_context & http,
                                      const common_params & params,
                                      std::function<llama_context *()> get_server_ctx) {
    g_wa.params         = params;
    g_wa.get_server_ctx = std::move(get_server_ctx);

    http.post("/agent/start",  handle_start);
    http.post("/agent/frame",  handle_frame);
    http.get ("/agent/events", handle_events);
    http.post("/agent/stop",   handle_stop);
    http.post("/agent/clip",   handle_not_implemented);
    http.post("/agent/live",   handle_not_implemented);
}
