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
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
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
// Clip relay store — opaque WebM bytes in, opaque bytes out (NO server decode).
// A bounded ring: at most MAX_CLIPS entries and MAX_TOTAL_BYTES total; the
// oldest is evicted first. Keyed by a random id; served back verbatim with a
// video/webm content-type so an MCP email tool can link/attach the clip.
// ---------------------------------------------------------------------------
struct ClipStore {
    static constexpr size_t MAX_CLIPS       = 8;
    static constexpr size_t MAX_TOTAL_BYTES = 256u * 1024u * 1024u;  // 256 MiB

    std::mutex                                       mu;
    std::deque<std::string>                          order;   // ids, oldest first
    std::unordered_map<std::string, std::string>     blobs;   // id -> raw bytes
    std::string                                      latest_id;
    size_t                                           total_bytes = 0;

    // Store bytes under a fresh id; evict as needed. Returns the new id.
    std::string put(std::string bytes) {
        std::lock_guard<std::mutex> lk(mu);
        std::string id = make_id();
        total_bytes += bytes.size();
        blobs.emplace(id, std::move(bytes));
        order.push_back(id);
        latest_id = id;
        while (order.size() > MAX_CLIPS ||
               (total_bytes > MAX_TOTAL_BYTES && order.size() > 1)) {
            const std::string & old = order.front();
            auto it = blobs.find(old);
            if (it != blobs.end()) { total_bytes -= it->second.size(); blobs.erase(it); }
            order.pop_front();
        }
        return id;
    }
    // Copy out the bytes for `id` (true if found).
    bool get(const std::string & id, std::string & out) {
        std::lock_guard<std::mutex> lk(mu);
        auto it = blobs.find(id);
        if (it == blobs.end()) return false;
        out = it->second;
        return true;
    }
    std::string newest() {
        std::lock_guard<std::mutex> lk(mu);
        return latest_id;
    }

  private:
    static std::string make_id() {
        static std::atomic<uint64_t> counter{0};
        uint64_t a = (uint64_t) std::chrono::steady_clock::now().time_since_epoch().count();
        uint64_t b = counter.fetch_add(1) * 0x9E3779B97F4A7C15ull + 0x1234567ull;
        uint64_t x = a ^ (b << 1) ^ (a >> 7);
        char buf[17];
        for (int i = 15; i >= 0; --i) { buf[i] = "0123456789abcdef"[x & 0xF]; x >>= 4; }
        buf[16] = 0;
        return std::string(buf);
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

    // clip relay (step 3) + the most-recent raw frame JPEG (for the live page)
    ClipStore                            clips;
    std::mutex                           frame_mu;
    std::string                          latest_frame_jpeg;   // raw JPEG bytes
    std::atomic<int>                     latest_frame_no{0};
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

// Attach the freshest 30s clip + the live link to an outbound external tool call,
// so an email/SMS tool fired on a trigger can include "here's what I saw". Best-effort:
// ask the browser to assemble+upload a fresh clip (capture_clip event), wait briefly for
// it, then inject the clip URL (and always the live link). Server-relative URLs; the
// recipient resolves them against the running server. Adds clip_url/live_url fields and
// appends a human-readable line to a body/text/message/content field if one is present.
void inject_clip_links(json & args) {
    const std::string before = g_wa.clips.newest();
    g_wa.broker.publish(json{{"type", "capture_clip"}}.dump());  // UI uploads -> /agent/clip
    for (int i = 0; i < 30; ++i) {                               // up to ~1.5s for a fresh clip
        if (g_wa.clips.newest() != before) break;
        usleep(50 * 1000);
    }
    const std::string id = g_wa.clips.newest();
    const std::string live_url = "/agent/live";
    if (!args.contains("live_url")) args["live_url"] = live_url;
    std::string note = "Live view: " + live_url;
    if (!id.empty()) {
        const std::string clip_url = "/agent/clip/" + id + ".webm";
        if (!args.contains("clip_url")) args["clip_url"] = clip_url;
        note = "30s clip: " + clip_url + "  |  " + note;
    }
    for (const char * k : {"body", "text", "message", "content"}) {
        if (args.contains(k) && args[k].is_string()) {
            args[k] = args[k].get<std::string>() + "\n\n" + note;
            break;
        }
    }
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
        json              args = args_to_json(act.call);

        std::string result;
        if (llamafile_mcp_has_tool(name)) {
            inject_clip_links(args);  // attach the 30s clip + live link to the outbound call
            g_wa.broker.publish(json{
                {"type", "tool_call"}, {"frame", frame}, {"round", rounds},
                {"name", name}, {"arguments", args}}.dump());
            result = llamafile_mcp_call_tool(name, args.dump());
        } else {
            g_wa.broker.publish(json{
                {"type", "tool_call"}, {"frame", frame}, {"round", rounds},
                {"name", name}, {"arguments", args}}.dump());
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

    // Keep the most recent raw JPEG so the live page can show the latest frame
    // (opaque relay — never decoded here).
    {
        std::lock_guard<std::mutex> lk(g_wa.frame_mu);
        g_wa.latest_frame_jpeg.assign(jpeg.begin(), jpeg.end());
        g_wa.latest_frame_no.store(g_wa.cur_frame.load() + 1);
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

// ---------------------------------------------------------------------------
// static asset helpers (UI embedded in the APE zip, served from /zip/)
// ---------------------------------------------------------------------------
bool read_zip_asset(const char * zip_path, std::string & out) {
    FILE * f = fopen(zip_path, "rb");
    if (!f) return false;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return false; }
    long n = ftell(f);
    if (n < 0) { fclose(f); return false; }
    fseek(f, 0, SEEK_SET);
    out.resize((size_t) n);
    size_t rd = n ? fread(&out[0], 1, (size_t) n, f) : 0;
    fclose(f);
    out.resize(rd);
    return true;
}

server_http_res_ptr serve_asset(const char * zip_path, const char * content_type) {
    std::string body;
    if (!read_zip_asset(zip_path, body)) {
        return json_res(404, json{{"error", "embedded asset not found"}, {"path", zip_path}});
    }
    auto r = std::make_unique<server_http_res>();
    r->status = 200;
    r->content_type = content_type;
    r->data = std::move(body);
    return r;
}

server_http_res_ptr serve_bytes(std::string bytes, const char * content_type) {
    auto r = std::make_unique<server_http_res>();
    r->status = 200;
    r->content_type = content_type;
    r->data = std::move(bytes);
    return r;
}

// ---------------------------------------------------------------------------
// /agent/clip — the 30s-clip relay (opaque WebM bytes; NO server-side decode)
//   POST /agent/clip          (raw WebM body)  -> {url, id, size}
//   GET  /agent/clip/<id>     -> the stored WebM bytes (video/webm)
//   GET  /agent/clip/latest   -> {url} of the most recently uploaded clip
// ---------------------------------------------------------------------------
server_http_res_ptr handle_clip_post(const server_http_req & req) {
    std::string bytes;
    if (!req.files.empty()) {
        const uploaded_file & f = req.files.begin()->second;
        bytes.assign(f.data.begin(), f.data.end());
    } else {
        bytes = req.body;
    }
    if (bytes.empty()) {
        return json_res(400, json{{"error", "empty body (POST the assembled WebM blob)"}});
    }
    size_t sz = bytes.size();
    std::string id = g_wa.clips.put(std::move(bytes));
    std::string url = "/agent/clip/" + id + ".webm";
    g_wa.broker.publish(json{{"type", "clip"}, {"url", url}, {"size", sz}}.dump());
    return json_res(200, json{{"ok", true}, {"id", id}, {"url", url}, {"size", sz}});
}

server_http_res_ptr handle_clip_get(const server_http_req & req) {
    std::string id = req.get_param("id");
    // strip an optional .webm extension (the URL we hand out ends in .webm)
    const std::string ext = ".webm";
    if (id.size() > ext.size() && id.compare(id.size() - ext.size(), ext.size(), ext) == 0) {
        id = id.substr(0, id.size() - ext.size());
    }
    if (id == "latest") {
        std::string newest = g_wa.clips.newest();
        if (newest.empty()) return json_res(404, json{{"error", "no clips stored yet"}});
        return json_res(200, json{{"url", "/agent/clip/" + newest + ".webm"}, {"id", newest}});
    }
    std::string bytes;
    if (!g_wa.clips.get(id, bytes)) {
        return json_res(404, json{{"error", "no such clip"}, {"id", id}});
    }
    return serve_bytes(std::move(bytes), "video/webm");
}

// ---------------------------------------------------------------------------
// /agent/live — a tiny live page: latest frame + the SSE action stream.
// (Pure relay: the frame is the last opaque JPEG the browser sent; the agent's
//  notes/speak/tool_calls arrive over /agent/events. Somewhere real for the
//  "live link" in an email to point.)
//   GET /agent/live            -> the live HTML page
//   GET /agent/live/frame.jpg  -> the latest raw frame JPEG (or 404)
// ---------------------------------------------------------------------------
const char * kLivePage = R"HTML(<!doctype html>
<html lang="en"><head><meta charset="utf-8"/>
<meta name="viewport" content="width=device-width, initial-scale=1"/>
<title>llamafile webcam — live</title>
<style>
 :root{color-scheme:dark;font-family:ui-sans-serif,system-ui,sans-serif}
 body{margin:0;background:#0b0d10;color:#e6e8eb;display:grid;grid-template-columns:1fr 360px;gap:12px;padding:12px;height:100vh;box-sizing:border-box}
 .stage{background:#000;border-radius:10px;overflow:hidden;display:flex;align-items:center;justify-content:center}
 img{max-width:100%;max-height:100%;object-fit:contain}
 #log{overflow:auto;background:#0e1217;border:1px solid #222831;border-radius:8px;padding:8px;font:12px/1.5 ui-monospace,monospace}
 .ev{margin-bottom:4px}.t{color:#5b6672}
 .note{color:#9ab6e0}.speak{color:#c79bf0}.tool{color:#7fd1b9}.act{color:#e0b057}.err{color:#e06b6b}
 h2{font:13px ui-sans-serif;color:#9aa4af;margin:0 0 6px}
</style></head><body>
<div class="stage"><img id="frame" alt="waiting for frames…"/></div>
<div style="display:flex;flex-direction:column;gap:8px;min-width:0">
 <h2>Live agent feed (relay — no server-side decode)</h2>
 <div id="log"></div>
</div>
<script>
const log=document.getElementById('log');
const add=(c,m)=>{const d=document.createElement('div');d.className='ev '+c;
 d.innerHTML='<span class="t">'+new Date().toLocaleTimeString()+'</span> '+m;log.prepend(d)};
const esc=s=>String(s).replace(/[&<>]/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;'}[c]));
const img=document.getElementById('frame');
setInterval(()=>{img.src='/agent/live/frame.jpg?t='+Date.now()},700);
const es=new EventSource('/agent/events');
es.onmessage=m=>{let e;try{e=JSON.parse(m.data)}catch{return}
 if(e.type==='note')add('note','📝 '+esc(e.text));
 else if(e.type==='speak')add('speak','🔊 '+esc(e.text));
 else if(e.type==='tool_call')add('tool','🛠 '+esc(e.name)+' '+esc(JSON.stringify(e.arguments||{})));
 else if(e.type==='tool_result')add('tool','↳ '+esc(e.name)+' → '+esc(String(e.result)).slice(0,120));
 else if(e.type==='frame')add('act','frame #'+e.frame+' → '+esc(e.action));
 else if(e.type==='clip')add('act','clip ready: <a href="'+esc(e.url)+'" target="_blank">'+esc(e.url)+'</a>');
 else if(e.type==='error')add('err',esc(e.message));};
add('act','connected — polling latest frame + agent events');
</script></body></html>)HTML";

server_http_res_ptr handle_live(const server_http_req &) {
    return serve_bytes(kLivePage, "text/html; charset=utf-8");
}

server_http_res_ptr handle_live_frame(const server_http_req &) {
    std::string jpeg;
    {
        std::lock_guard<std::mutex> lk(g_wa.frame_mu);
        jpeg = g_wa.latest_frame_jpeg;
    }
    if (jpeg.empty()) return json_res(404, json{{"error", "no frame yet"}});
    return serve_bytes(std::move(jpeg), "image/jpeg");
}

// ---------------------------------------------------------------------------
// /agent/tools — the MCP-connect/status panel feed (built-ins + bridged MCP)
// ---------------------------------------------------------------------------
server_http_res_ptr handle_tools(const server_http_req &) {
    std::string mcp_prompt;
    int mcp_count = 0;
    run_joined_big_stack([&]() {
        mcp_prompt = llamafile_mcp_tools_prompt();
        mcp_count  = llamafile_mcp_server_count();
    });
    return json_res(200, json{
        {"builtin_tools", json::array({"ignore_frame", "note", "speak"})},
        {"mcp_servers", mcp_count},
        {"mcp_tool_count", mcp_prompt.empty() ? 0 : 1},  // >0 iff any bridged tool
        {"mcp_tools_prompt", mcp_prompt},
        {"started", g_wa.started},
    });
}

// ---------------------------------------------------------------------------
// /agent/ui (+ /webcam alias) — the embedded webcam-agent web UI
// ---------------------------------------------------------------------------
server_http_res_ptr handle_ui_html(const server_http_req &) {
    return serve_asset("/zip/llamafile/webcam_ui/webcam-agent.html",
                       "text/html; charset=utf-8");
}
server_http_res_ptr handle_ui_js(const server_http_req &) {
    return serve_asset("/zip/llamafile/webcam_ui/frame-selection.js",
                       "text/javascript; charset=utf-8");
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

    // step 3: clip relay (30s WebM email attachment), live link, embedded UI
    http.post("/agent/clip",          handle_clip_post);
    http.get ("/agent/clip/:id",      handle_clip_get);    // also /agent/clip/latest
    http.get ("/agent/live",          handle_live);
    http.get ("/agent/live/frame.jpg", handle_live_frame);
    http.get ("/agent/tools",         handle_tools);
    http.get ("/agent/ui",            handle_ui_html);
    http.get ("/agent/ui/frame-selection.js", handle_ui_js);
    http.get ("/webcam",              handle_ui_html);     // friendly alias
}
