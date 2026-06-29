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

// llamafile multi-agent orchestration — see agent_loop.h.
//
// Ports the validated agent-as-tool loop from ddocs/prototypes/orchestrate.py
// to C++, server-side. The sub-agent loop talks to the server's OWN
// OpenAI-compatible endpoint over loopback HTTP (vendored cpp-httplib) and
// dispatches tool_calls to the server's OWN /tools registry, so it reuses the
// continuous-batching scheduler and the entire existing tool surface.

#include "agent_loop.h"

#include "agent_runtime.h"  // agentrt::EventBroker + now_seconds (LIVE activity SSE)
#include "server-tools.h"  // server_tool, server_tools (llama.cpp/tools/server)

#include <cpp-httplib/httplib.h>
#include <nlohmann/json.hpp>

#include <pthread.h>

#include <atomic>
#include <cstdio>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

using json = nlohmann::ordered_json;

namespace {

// ---------------------------------------------------------------------------
// module state: loopback endpoint + the live registry (read-only at run time)
// ---------------------------------------------------------------------------
std::string    g_host        = "127.0.0.1";
int            g_port        = 8080;
std::string    g_api_key;
std::string    g_model       = "default";
int            g_n_parallel  = 4;
bool           g_enabled     = false;
server_tools * g_registry    = nullptr;

// Hard caps (the design's runaway guards, ddocs/02 §6 / ddocs/03).
constexpr int  MAX_DEPTH        = 2;   // a sub-agent must not recurse forever
std::atomic<int> g_inflight{0};        // sub-agents currently generating

// Soft budget on simultaneous in-flight sub-agents = a generous multiple of the
// slot count. Surplus is refused (the orchestrator sees a tool error and can
// retry), never deadlocks: a blocked caller is NOT holding a generation slot.
int budget() { return (g_n_parallel > 0 ? g_n_parallel : 4) * 4; }

void log(const std::string & who, const char * kind, const std::string & msg) {
    fprintf(stderr, "agents: %-22s %-9s %s\n", who.c_str(), kind,
            msg.substr(0, 120).c_str());
}

// ---------------------------------------------------------------------------
// LIVE delegate-activity broker — a single process-global SSE EventBroker that
// every delegate_to_* invocation streams its steps into as it runs, so the
// /agents UI can WATCH a synchronous sub-agent work in real time (it otherwise
// runs opaque and only returns a final blob). The /agents/activity SSE endpoint
// (agent_runtime_server.cpp) tails this broker.
// ---------------------------------------------------------------------------
agentrt::EventBroker  g_activity;          // never closed; lives for the process
std::atomic<uint64_t> g_call_seq{0};       // unique id per delegate invocation

// Publish one structured activity event. `extra` carries the type-specific
// fields (turn/tool/arg/preview/text); id/role/depth/ts are stamped here.
void emit_activity(const std::string & id, const std::string & role, int depth,
                   const char * type, json extra = json::object()) {
    json ev = {
        {"type",  type},
        {"id",    id},
        {"role",  role},
        {"depth", depth},
        {"ts",    agentrt::now_seconds()},
    };
    if (extra.is_object())
        for (auto & kv : extra.items()) ev[kv.key()] = kv.value();
    g_activity.publish(ev.dump());
}

// The agentic loop runs from inside a tool handler — i.e. on an HTTP worker
// thread whose Cosmopolitan stack is too small for the httplib + nlohmann/json
// call chain (an inline loop here SILENTLY CRASHES the whole process). So every
// piece of HTTP/JSON work is run on a fresh 8 MiB-stack pthread, exactly as
// llamafile/main.cpp does for the TUI client. `pthread_create` failure falls
// back to inline (best effort).
struct ThreadJob { std::function<void()> fn; };

void * big_stack_trampoline(void * arg) {
    static_cast<ThreadJob *>(arg)->fn();
    return nullptr;
}

bool spawn_big_stack(pthread_t & tid, ThreadJob * job) {
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 8 * 1024 * 1024);
    int rc = pthread_create(&tid, &attr, big_stack_trampoline, job);
    pthread_attr_destroy(&attr);
    return rc == 0;
}

// Run fn() on an 8 MiB-stack pthread and block until it finishes.
void run_joined_big_stack(std::function<void()> fn) {
    ThreadJob job{std::move(fn)};
    pthread_t tid;
    if (!spawn_big_stack(tid, &job)) { job.fn(); return; }
    pthread_join(tid, nullptr);
}

// ---------------------------------------------------------------------------
// allow-list matching: an entry ending in '*' is a prefix match, else exact.
// ---------------------------------------------------------------------------
bool allow_match(const std::vector<std::string> & allow, const std::string & name) {
    for (const auto & pat : allow) {
        if (!pat.empty() && pat.back() == '*') {
            if (name.compare(0, pat.size() - 1, pat, 0, pat.size() - 1) == 0) return true;
        } else if (pat == name) {
            return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
// HTTP helpers (one fresh Client per call; cpp-httplib Clients aren't shared).
// ---------------------------------------------------------------------------
httplib::Client make_client() {
    httplib::Client cli(g_host, g_port);
    cli.set_connection_timeout(10, 0);
    cli.set_read_timeout(900, 0);   // sub-agent decode on CPU can be slow
    cli.set_write_timeout(60, 0);
    cli.set_keep_alive(true);
    return cli;
}

void auth(httplib::Headers & h) {
    if (!g_api_key.empty()) h.emplace("Authorization", "Bearer " + g_api_key);
}

// POST /tools {"tool":name,"params":args} -> result text (stringified JSON).
std::string post_tool(const std::string & name, const json & args) {
    json body = {{"tool", name}, {"params", args}};
    httplib::Headers h{{"Content-Type", "application/json"}};
    auth(h);
    auto cli = make_client();
    auto res = cli.Post("/tools", h, body.dump(), "application/json");
    if (!res) {
        return json{{"error", "tool dispatch failed (no response from /tools)"}}.dump();
    }
    if (res->status != 200) {
        return json{{"error", "tool /tools HTTP " + std::to_string(res->status)},
                    {"body", res->body}}.dump();
    }
    return res->body;  // already JSON; fed back verbatim as the tool result
}

// POST /v1/chat/completions (non-streaming). Returns the choices[0].message.
json post_chat(const json & messages, const json & tools) {
    json body = {
        {"model", g_model},
        {"messages", messages},
        {"temperature", 0.6},
        {"stream", false},
    };
    if (!tools.empty()) {
        body["tools"]       = tools;
        body["tool_choice"] = "auto";
    }
    httplib::Headers h{{"Content-Type", "application/json"}};
    auth(h);
    auto cli = make_client();
    auto res = cli.Post("/v1/chat/completions", h, body.dump(), "application/json");
    if (!res) {
        return json{{"__error", "no response from /v1/chat/completions"}};
    }
    if (res->status != 200) {
        return json{{"__error", "chat HTTP " + std::to_string(res->status) + ": " + res->body}};
    }
    json resp = json::parse(res->body, nullptr, /*allow_exceptions=*/false);
    if (resp.is_discarded() || !resp.contains("choices") || resp["choices"].empty()) {
        return json{{"__error", "malformed chat response"}};
    }
    return resp["choices"][0].value("message", json::object());
}

// Build the OpenAI `tools` array from the live registry, filtered by allow-list.
// delegate_* tools are NEVER advertised to a sub-agent (recursion guard).
json tools_for(const std::vector<std::string> & allow) {
    json arr = json::array();
    if (!g_registry || allow.empty()) return arr;
    for (const auto & t : g_registry->tools) {
        if (t->name.rfind("delegate_to_", 0) == 0) continue;
        if (!allow_match(allow, t->name)) continue;
        try {
            arr.push_back(t->get_definition());
        } catch (...) { /* skip a tool whose schema can't be built */ }
    }
    return arr;
}

}  // namespace

// ---------------------------------------------------------------------------
// public configuration API
// ---------------------------------------------------------------------------
void llamafile_agents_set_endpoint(const std::string & host, int port,
                                   const std::string & api_key,
                                   const std::string & model,
                                   int n_parallel) {
    g_host       = host.empty() ? "127.0.0.1" : host;
    if (g_host == "0.0.0.0" || g_host == "::") g_host = "127.0.0.1";  // bind-all -> loopback
    g_port       = port;
    g_api_key    = api_key;
    if (!model.empty()) g_model = model;
    g_n_parallel = n_parallel;
}

void llamafile_agents_enable()  { g_enabled = true; }
bool llamafile_agents_enabled() { return g_enabled; }

// ---------------------------------------------------------------------------
// the reusable agentic loop (mirrors orchestrate.py Agent.run, server-side)
// ---------------------------------------------------------------------------
namespace {

std::string run_agent_impl(const std::string & role,
                           const std::string & call_id,
                           const std::string & system_prompt,
                           const std::string & user_task,
                           const std::vector<std::string> & tool_allowlist,
                           int max_turns,
                           int depth) {
    const std::string who = "depth" + std::to_string(depth);
    json tools = tools_for(tool_allowlist);

    json messages = json::array();
    // Tell the agent its turn budget up front so it can pace itself (and not die
    // at the cap with no answer). It gets escalating reminders near the end below.
    std::string sys = system_prompt +
        "\n\nYou have up to " + std::to_string(max_turns) +
        " tool-calling turns to finish. Work efficiently: gather what you need, then STOP "
        "calling tools and write your final answer. You'll be warned as your turns run low — "
        "always produce your best answer before they run out.";
    messages.push_back({{"role", "system"}, {"content", sys}});
    messages.push_back({{"role", "user"}, {"content", user_task}});

    log(who, "start", user_task);

    std::string last_text;
    // Per-step trace appended to the result so the caller (and the user, via the chat
    // tool-result view) can SEE what the sub-agent did — what it searched/read, not just
    // its final blob. delegate_* otherwise runs opaque.
    std::string steps;
    auto preview = [](const std::string & s, size_t max) {
        std::string out; bool ws = true;
        for (char ch : s) {
            unsigned char c = (unsigned char) ch;
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') { if (!ws) { out.push_back(' '); ws = true; } }
            else { out.push_back((char) c); ws = false; }
            if (out.size() >= max) { out += "…"; break; }
        }
        while (!out.empty() && out.back() == ' ') out.pop_back();
        return out;
    };
    auto finalize = [&](const std::string & answer) -> std::string {
        if (steps.empty()) return answer;
        return answer + "\n\n<details><summary>🔎 sub-agent steps</summary>\n\n" + steps + "</details>";
    };

    // LIVE: announce the run (role + task) so the /agents Activity panel opens a
    // card the moment a delegate_to_* tool fires — before any token is decoded.
    emit_activity(call_id, role, depth, "start", json{{"text", preview(user_task, 140)}});

    for (int turn = 0; turn < max_turns; ++turn) {
        // LIVE: a new turn begins (the sub-agent is about to call the model).
        emit_activity(call_id, role, depth, "turn", json{{"turn", turn}});

        // Escalating turn-budget reminder so the agent wraps up instead of getting
        // cut off mid-research. (remaining counts this turn.)
        int remaining = max_turns - turn;
        if (remaining <= 2) {
            messages.push_back({{"role", "user"}, {"content",
                remaining == 1
                  ? std::string("[turn budget: this is your FINAL turn — do NOT call tools; "
                                "write your complete final answer now with what you have.]")
                  : std::string("[turn budget: ") + std::to_string(remaining) +
                    " turns left — start wrapping up; prefer answering over more tool calls.]"}});
        }
        json msg = post_chat(messages, tools);
        if (msg.contains("__error")) {
            log(who, "error", msg["__error"].get<std::string>());
            emit_activity(call_id, role, depth, "error",
                          json{{"text", msg["__error"].get<std::string>()}});
            return "(stopped: " + msg["__error"].get<std::string>() + ")";
        }

        std::string content;
        if (msg.contains("content") && msg["content"].is_string()) {
            content = msg["content"].get<std::string>();
        }
        json tool_calls = (msg.contains("tool_calls") && msg["tool_calls"].is_array())
                              ? msg["tool_calls"] : json::array();

        if (tool_calls.empty()) {
            log(who, "final", content);
            emit_activity(call_id, role, depth, "final", json{{"text", content}});
            return finalize(content);
        }
        last_text = content;

        // append the assistant turn verbatim (content may be null/empty)
        messages.push_back(msg);

        std::string names;
        for (const auto & c : tool_calls) {
            if (!names.empty()) names += ", ";
            names += c.value("function", json::object()).value("name", "?");
        }
        log(who, ("turn" + std::to_string(turn)).c_str(), "calls: " + names);

        // Dispatch every tool call in this turn CONCURRENTLY (exercises the
        // server's continuous batching: N in-flight /tools + nested chats).
        // Call 0 runs inline (we are already on an 8 MiB stack); the extras get
        // their own big-stack pthreads. We join all before reading results.
        size_t n = tool_calls.size();
        std::vector<std::string> results(n);
        std::vector<ThreadJob> jobs(n);
        std::vector<pthread_t> tids(n);
        std::vector<bool> spawned(n, false);
        for (size_t i = 0; i < n; ++i) {
            const json & call = tool_calls[i];
            std::string tname = call.value("function", json::object()).value("name", "");
            std::string raw   = call.value("function", json::object()).value("arguments", "");
            json args = json::parse(raw.empty() ? "{}" : raw, nullptr, false);
            if (args.is_discarded()) args = json::object();
            // LIVE: stream the tool call (name + first string-arg preview) the
            // instant the sub-agent decides to call it — BEFORE it runs.
            std::string aprev;
            if (args.is_object())
                for (auto & kv : args.items())
                    if (kv.value().is_string()) { aprev = kv.value().get<std::string>(); break; }
            emit_activity(call_id, role, depth, "tool_call",
                          json{{"turn", turn}, {"tool", tname},
                               {"arg", preview(aprev, 60)}});
            jobs[i].fn = [i, tname, args, &results]() {
                results[i] = post_tool(tname, args);
            };
            if (i == 0) {
                jobs[i].fn();  // inline on the current big stack
            } else {
                spawned[i] = spawn_big_stack(tids[i], &jobs[i]);
                if (!spawned[i]) jobs[i].fn();  // fallback: inline
            }
        }
        for (size_t i = 1; i < n; ++i) {
            if (spawned[i]) pthread_join(tids[i], nullptr);
        }

        for (size_t i = 0; i < n; ++i) {
            const json & call = tool_calls[i];
            messages.push_back({
                {"role", "tool"},
                {"tool_call_id", call.value("id", "")},
                {"content", results[i]},
            });
            // one user-visible line per tool call: name `key-arg` -> result preview
            const json & fn = call.value("function", json::object());
            std::string tn = fn.value("name", "?");
            std::string ar = fn.value("arguments", "");
            json aj = json::parse(ar.empty() ? "{}" : ar, nullptr, false);
            std::string ap;
            if (aj.is_object())
                for (auto & kv : aj.items())
                    if (kv.value().is_string()) { ap = kv.value().get<std::string>(); break; }
            steps += "- **" + tn + "**" + (ap.empty() ? "" : " `" + preview(ap, 60) + "`") +
                     " → " + preview(results[i], 140) + "\n";
            // LIVE: stream the tool result preview as soon as it lands.
            emit_activity(call_id, role, depth, "tool_result",
                          json{{"turn", turn}, {"tool", tn},
                               {"arg", preview(ap, 60)},
                               {"preview", preview(results[i], 140)}});
        }
    }

    log(who, "maxturns", "hit turn cap");
    emit_activity(call_id, role, depth, "maxturns",
                  json{{"text", preview(last_text, 140)}});
    return finalize(last_text.empty() ? "(stopped: max turns reached)"
                                      : last_text + "\n\n(note: stopped at max turns)");
}

}  // namespace

std::string llamafile_run_agent(const std::string & role,
                                const std::string & system_prompt,
                                const std::string & user_task,
                                const std::vector<std::string> & tool_allowlist,
                                int max_turns,
                                int depth) {
    if (depth > MAX_DEPTH) {
        return "(stopped: max delegate depth reached)";
    }
    if (g_inflight.fetch_add(1) >= budget()) {
        g_inflight.fetch_sub(1);
        return "(stopped: too many sub-agents in flight; retry shortly)";
    }
    struct Guard { ~Guard() { g_inflight.fetch_sub(1); } } guard;

    // A unique id per invocation so the /agents Activity panel can group every
    // event of THIS sub-agent run into one card (and indent nested delegates).
    std::string call_id = (role.empty() ? std::string("agent") : role) + "-" +
                          std::to_string(g_call_seq.fetch_add(1) + 1);

    // Run the whole loop on a fresh 8 MiB-stack pthread: the caller is an HTTP
    // worker thread whose stack would overflow on httplib + nlohmann/json.
    std::string out;
    run_joined_big_stack([&]() {
        out = run_agent_impl(role, call_id, system_prompt, user_task,
                             tool_allowlist, max_turns, depth);
    });
    return out;
}

agentrt::EventBroker * llamafile_agents_activity_broker() {
    return &g_activity;
}

// ---------------------------------------------------------------------------
// role definitions + delegate-tool registration
// ---------------------------------------------------------------------------
namespace {

struct RoleSpec {
    std::string role;     // researcher / ideator / verifier
    std::string arg;      // primary argument name (task / claims)
    std::string desc;     // delegate tool description (shown to the orchestrator)
    std::string system;   // role system prompt
    std::vector<std::string> allow;  // tool allow-list patterns ("name" or "prefix*")
    int max_turns;
};

std::vector<RoleSpec> role_specs() {
    return {
        {
            "researcher", "task",
            "Run a RESEARCHER sub-agent: it gathers facts using wiki/browser/web "
            "tools in its own loop and returns a short SOURCED summary. Pass the "
            "specific question to research as 'task'.",
            "You are a RESEARCHER sub-agent. Use the available tools (wiki_search, "
            "wiki_get_article, browser_*, web_fetch) to gather facts that answer the "
            "task. Search, read the most relevant result, then ANSWER. Do not repeat "
            "an identical tool call. When you have the facts, stop calling tools and "
            "write a short factual summary that cites its source titles. Be concise.",
            {"wiki_search", "wiki_get_article", "wiki_*", "browser_*", "web_fetch", "code_run_js"},
            12,  // room for several search/read rounds before synthesizing
        },
        {
            "ideator", "task",
            "Run an IDEATOR sub-agent: it brainstorms angles/options for the task "
            "(no external tools) and returns a bulleted list. Pass the prompt as 'task'.",
            "You are an IDEATOR sub-agent. You have NO external tools. Brainstorm "
            "angles, hypotheses and sub-questions for the task. Return a concise "
            "bulleted list and nothing else.",
            {},  // no tools
            2,
        },
        {
            "verifier", "claims",
            "Run a VERIFIER sub-agent: it adversarially re-checks the given claims "
            "against read-only sources and returns PASS/FAIL per claim with evidence. "
            "Pass the claim(s) to check as 'claims'.",
            "You are a VERIFIER sub-agent. Adversarially re-check each given claim "
            "against the read tools (wiki_search, wiki_get_article, web_fetch). Flag "
            "anything unsupported. Return PASS/FAIL per claim with a one-line evidence "
            "note. Be concise; stop calling tools once you can judge.",
            {"wiki_search", "wiki_get_article", "wiki_*", "web_fetch",
             "read_file", "grep_search", "file_glob_search"},
            10,  // room to re-check several claims against sources
        },
    };
}

// A virtual tool whose "execution" is a nested agentic loop.
struct DelegateTool : server_tool {
    RoleSpec spec;

    explicit DelegateTool(RoleSpec s) : spec(std::move(s)) {
        name             = "delegate_to_" + spec.role;
        display_name     = name;
        permission_write = false;  // a sub-agent only uses its own (read) tools
    }

    json get_definition() override {
        json props = json::object();
        props[spec.arg] = {
            {"type", "string"},
            {"description", spec.role == "verifier"
                                ? "The claim(s) to fact-check."
                                : "The task / question for the sub-agent."},
        };
        return {
            {"type", "function"},
            {"function", json{
                {"name", name},
                {"description", spec.desc},
                {"parameters", json{
                    {"type", "object"},
                    {"properties", props},
                    {"required", json::array({spec.arg})},
                }},
            }},
        };
    }

    json invoke(json params) override {
        std::string task;
        if (params.is_object()) {
            for (const char * k : {"task", "claims", "question", "query", "input"}) {
                if (params.contains(k) && params[k].is_string()) {
                    task = params[k].get<std::string>();
                    break;
                }
            }
            if (task.empty() && !params.empty()) task = params.dump();
        } else if (params.is_string()) {
            task = params.get<std::string>();
        }
        if (task.empty()) {
            return {{"error", "missing '" + spec.arg + "' argument"}};
        }
        std::string result =
            llamafile_run_agent(spec.role, spec.system, task, spec.allow,
                                spec.max_turns, /*depth=*/1);
        return {{"plain_text_response", result}};
    }
};

}  // namespace

int llamafile_agents_register_tools(server_tools & registry) {
    g_registry = &registry;
    int added = 0;
    for (auto & spec : role_specs()) {
        registry.tools.push_back(std::make_unique<DelegateTool>(spec));
        ++added;
    }
    fprintf(stderr, "agents: registered %d delegate tool(s) "
                    "(loopback %s:%d, slots=%d)\n",
            added, g_host.c_str(), g_port, g_n_parallel);
    return added;
}
