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

// llamafile interactive multi-agent runtime — LLM turn loop + tools + HTTP/SSE.
//
// This is the model-facing half of the runtime (the testable core is
// agent_runtime.cpp). It provides:
//   * the default TurnFn: an agent's inner loop, talking to the server's own
//     /v1/chat/completions over loopback and dispatching tool_calls in-process
//     against the server_tools registry (built-ins + MCP-bridged + the runtime
//     tools below), with per-turn token accounting and structured traces;
//   * the runtime tools spawn_agent / send_message / await / list_agents
//     registered into the /tools registry (available per allowlist);
//   * the /runtime/* HTTP + SSE endpoints that drive a session.
//
// See ddocs/09-interactive-multiagent-runtime.md (Phase 1 = section 9.1).

#include "agent_runtime.h"
#include "agent_session.h"    // Phase 4: SessionManager (persist/pause/resume)
#include "agent_predicate.h"  // poll_until predicate language

#include "server-tools.h"   // server_tool, server_tools
#include "server-http.h"    // server_http_context, server_http_req/res

#include <cpp-httplib/httplib.h>
#include <nlohmann/json.hpp>

#include <pthread.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

using agentrt::Runtime;
using agentrt::Agent;
using agentrt::Message;
using agentrt::TurnOutcome;
using agentrt::Guards;
using agentrt::PendingAwait;
using agentrt::SessionManager;
using agentrt::KvFingerprint;
using json = nlohmann::ordered_json;

namespace {

// ---------------------------------------------------------------------------
// module state
// ---------------------------------------------------------------------------
std::string    g_host       = "127.0.0.1";
int            g_port       = 8080;
std::string    g_api_key;
std::string    g_model      = "default";
int            g_n_parallel = 4;
server_tools * g_registry   = nullptr;

// Phase 4: the multi-session registry. v1 keeps ONE ACTIVE (decoding) session
// at a time (the slot pool is shared); paused sessions are disk state. The
// legacy /runtime/* endpoints alias the "current" session id for back-compat.
SessionManager             g_sm;
std::mutex                 g_session_mu;
std::string                g_session_root;      // --session-dir (default below)
std::string                g_current_sid;       // last created/active (for /runtime/*)
bool                       g_sm_configured = false;

// The current agent/runtime a scheduler worker is executing — read by the
// runtime tools' invoke() so they know who is calling. Set only on the worker
// thread immediately around a synchronous registry.invoke(), so it is valid.
thread_local Runtime * t_rt    = nullptr;
thread_local Agent   * t_agent = nullptr;

bool is_runtime_tool(const std::string & n) {
    return n == "spawn_agent" || n == "send_message" || n == "await" ||
           n == "list_agents" || n == "wait" || n == "poll_until" || n == "schedule";
}

// HTTP worker stacks are tiny under cosmocc; json + Runtime work overflows them.
// Run heavy handler bodies on a fresh 8 MiB-stack pthread (mirrors webcam_agent).
struct BigJob { std::function<void()> fn; };
void * big_trampoline(void * a) { static_cast<BigJob *>(a)->fn(); return nullptr; }
void run_joined_big_stack(std::function<void()> fn) {
    BigJob job{std::move(fn)};
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 8 * 1024 * 1024);
    pthread_t tid;
    int rc = pthread_create(&tid, &attr, big_trampoline, &job);
    pthread_attr_destroy(&attr);
    if (rc != 0) { job.fn(); return; }
    pthread_join(tid, nullptr);
}

// ---------------------------------------------------------------------------
// loopback HTTP helpers (one fresh Client per call; mirror agent_loop.cpp)
// ---------------------------------------------------------------------------
httplib::Client make_client() {
    httplib::Client cli(g_host, g_port);
    cli.set_connection_timeout(10, 0);
    cli.set_read_timeout(900, 0);
    cli.set_write_timeout(60, 0);
    cli.set_keep_alive(true);
    return cli;
}
void auth(httplib::Headers & h) {
    if (!g_api_key.empty()) h.emplace("Authorization", "Bearer " + g_api_key);
}

// POST /v1/chat/completions; fills usage_prompt/usage_completion. Returns the
// choices[0].message (or {__error}).
json post_chat(const json & messages, const json & tools,
               long & usage_prompt, long & usage_completion) {
    usage_prompt = usage_completion = 0;
    json body = {{"model", g_model}, {"messages", messages},
                 {"temperature", 0.6}, {"stream", false}};
    if (!tools.empty()) { body["tools"] = tools; body["tool_choice"] = "auto"; }
    httplib::Headers h{{"Content-Type", "application/json"}};
    auth(h);
    auto cli = make_client();
    auto res = cli.Post("/v1/chat/completions", h, body.dump(), "application/json");
    if (!res) return json{{"__error", "no response from /v1/chat/completions"}};
    if (res->status != 200)
        return json{{"__error", "chat HTTP " + std::to_string(res->status) + ": " + res->body}};
    json resp = json::parse(res->body, nullptr, false);
    if (resp.is_discarded() || !resp.contains("choices") || resp["choices"].empty())
        return json{{"__error", "malformed chat response"}};
    if (resp.contains("usage") && resp["usage"].is_object()) {
        const json & u = resp["usage"];
        usage_prompt     = u.value("prompt_tokens", 0);
        usage_completion = u.value("completion_tokens", 0);
    }
    return resp["choices"][0].value("message", json::object());
}

// allow-list matching: an entry ending in '*' is a prefix match, else exact.
bool allow_match(const std::vector<std::string> & allow, const std::string & name) {
    for (const auto & pat : allow) {
        if (!pat.empty() && pat.back() == '*') {
            if (name.compare(0, pat.size() - 1, pat, 0, pat.size() - 1) == 0) return true;
        } else if (pat == name) return true;
    }
    return false;
}

// Build the OpenAI tools array from the live registry, filtered by allow-list.
// delegate_* tools are never advertised (that is the old synchronous path).
json tools_for(const std::vector<std::string> & allow) {
    json arr = json::array();
    if (!g_registry || allow.empty()) return arr;
    for (const auto & t : g_registry->tools) {
        if (t->name.rfind("delegate_to_", 0) == 0) continue;
        if (!allow_match(allow, t->name)) continue;
        try { arr.push_back(t->get_definition()); } catch (...) {}
    }
    return arr;
}

std::string str_arg(const json & a, std::initializer_list<const char *> keys) {
    if (a.is_string()) return a.get<std::string>();
    if (!a.is_object()) return "";
    for (const char * k : keys)
        if (a.contains(k) && a[k].is_string()) return a[k].get<std::string>();
    return "";
}

// ---------------------------------------------------------------------------
// known sub-agent roles (kept aligned with agent_loop.cpp's delegate roles)
// ---------------------------------------------------------------------------
struct Role { std::string system; std::vector<std::string> allow; };

Role role_for(const std::string & role) {
    static const std::vector<std::string> research_tools =
        {"wiki_search", "wiki_get_article", "wiki_*", "wikidata_*",
         "browser_*", "web_fetch", "code_run_js", "send_message", "list_agents",
         "wait", "poll_until", "schedule"};
    if (role == "researcher")
        return {"You are a RESEARCHER sub-agent. Use the available tools "
                "(wiki_search, wiki_get_article, wikidata_*) to gather facts that "
                "answer the task. Search, read the most relevant result, then ANSWER. "
                "Do not repeat an identical tool call. When you have the facts, stop "
                "calling tools and write a short factual summary citing its source "
                "titles. Be concise.", research_tools};
    if (role == "verifier")
        return {"You are a VERIFIER sub-agent. Adversarially re-check each given "
                "claim against the read tools. Return PASS/FAIL per claim with a "
                "one-line evidence note. Be concise.", research_tools};
    if (role == "ideator")
        return {"You are an IDEATOR sub-agent. You have no external tools. Brainstorm "
                "angles and sub-questions for the task; return a concise bulleted "
                "list and nothing else.", {"send_message", "list_agents"}};
    // free-form: treat `role` as the system prompt itself
    return {role, research_tools};
}

// ---------------------------------------------------------------------------
// runtime tools (registered into /tools; dispatched in-process via t_agent/t_rt)
// ---------------------------------------------------------------------------
struct SpawnAgentTool : server_tool {
    SpawnAgentTool() { name = "spawn_agent"; display_name = name; permission_write = true; }
    json get_definition() override {
        return {{"type", "function"}, {"function", json{
            {"name", name},
            {"description",
             "Create a NEW sub-agent that works in parallel and reports back to you. "
             "NON-BLOCKING: returns the new agent's id immediately. Use 'role' = "
             "researcher | verifier | ideator (or a free-form system prompt) and "
             "'task' = the specific job. After spawning, call await(ids) to collect "
             "their results."},
            {"parameters", json{{"type", "object"}, {"properties", json{
                {"role", json{{"type", "string"}, {"description", "researcher|verifier|ideator or a custom system prompt"}}},
                {"task", json{{"type", "string"}, {"description", "the task/question for the sub-agent"}}},
                {"name", json{{"type", "string"}, {"description", "optional human name for the sub-agent"}}},
            }}, {"required", json::array({"role", "task"})}}}}}};
    }
    json invoke(json p) override {
        if (!t_rt || !t_agent) return {{"error", "spawn_agent is only callable inside an agent runtime turn"}};
        std::string role = str_arg(p, {"role", "prompt", "system"});
        std::string task = str_arg(p, {"task", "prompt", "question", "input"});
        std::string nm   = str_arg(p, {"name"});
        if (task.empty()) return {{"error", "spawn_agent requires a 'task'"}};
        Role r = role_for(role.empty() ? "researcher" : role);
        std::vector<std::string> allow = r.allow;
        if (p.is_object() && p.contains("tools") && p["tools"].is_array()) {
            allow.clear();
            for (const auto & t : p["tools"]) if (t.is_string()) allow.push_back(t.get<std::string>());
        }
        std::string err;
        std::string id = t_rt->spawn(t_agent->id,
                                     role.empty() ? "researcher" : (role.size() < 24 ? role : "agent"),
                                     nm, r.system, task, allow, err);
        if (id.empty()) return {{"error", err}};
        return {{"plain_text_response",
                 "spawned agent " + id + (nm.empty() ? "" : " (" + nm + ")") +
                 "; it is running in parallel and will report back. Call await to collect."},
                {"agent_id", id}};
    }
};

struct SendMessageTool : server_tool {
    SendMessageTool() { name = "send_message"; display_name = name; permission_write = true; }
    json get_definition() override {
        return {{"type", "function"}, {"function", json{
            {"name", name},
            {"description", "Send a message to another agent's mailbox (address by id or name). "
                            "NON-BLOCKING acknowledgement; the target processes it on its next turn."},
            {"parameters", json{{"type", "object"}, {"properties", json{
                {"to", json{{"type", "string"}, {"description", "target agent id or name"}}},
                {"content", json{{"type", "string"}, {"description", "message body"}}},
            }}, {"required", json::array({"to", "content"})}}}}}};
    }
    json invoke(json p) override {
        if (!t_rt || !t_agent) return {{"error", "send_message is only callable inside an agent runtime turn"}};
        std::string to = str_arg(p, {"to", "target", "agent", "id"});
        std::string content = str_arg(p, {"content", "message", "text"});
        if (to.empty() || content.empty()) return {{"error", "send_message requires 'to' and 'content'"}};
        std::string err;
        if (!t_rt->send(t_agent->id, to, content, err)) return {{"error", err}};
        return {{"plain_text_response", "delivered to " + to}};
    }
};

struct AwaitTool : server_tool {
    AwaitTool() { name = "await"; display_name = name; permission_write = false; }
    json get_definition() override {
        return {{"type", "function"}, {"function", json{
            {"name", name},
            {"description", "Block YOURSELF until the named agents report back (or a timeout). "
                            "You yield your slot while waiting and resume when results arrive. "
                            "Omit 'ids' to await every sub-agent you spawned."},
            {"parameters", json{{"type", "object"}, {"properties", json{
                {"ids", json{{"type", "array"}, {"items", json{{"type", "string"}}},
                             {"description", "agent ids/names to wait for (default: all your children)"}}},
                {"timeout", json{{"type", "number"}, {"description", "seconds before giving up (optional)"}}},
            }}, {"required", json::array()}}}}}};
    }
    // never actually invoked: the turn loop intercepts await to park the agent.
    json invoke(json) override { return {{"error", "await handled by the scheduler"}}; }
};

struct ListAgentsTool : server_tool {
    ListAgentsTool() { name = "list_agents"; display_name = name; permission_write = false; }
    json get_definition() override {
        return {{"type", "function"}, {"function", json{
            {"name", name},
            {"description", "List every agent in this session with id, name, role, status and parent."},
            {"parameters", json{{"type", "object"}, {"properties", json::object()}, {"required", json::array()}}}}}};
    }
    json invoke(json) override {
        if (!t_rt) return {{"error", "list_agents is only callable inside an agent runtime turn"}};
        return {{"plain_text_response", t_rt->list().dump(2)}};
    }
};

// --- scheduling tools (Phase 2, ddoc 09 §7) -------------------------------
// wait + poll_until are PARK tools: the turn loop intercepts them to suspend the
// agent on the timer (it holds NO worker while parked) and resumes it later, so
// invoke() is never reached for real work.
struct WaitTool : server_tool {
    WaitTool() { name = "wait"; display_name = name; permission_write = false; }
    json get_definition() override {
        return {{"type", "function"}, {"function", json{
            {"name", name},
            {"description", "Pause YOURSELF for N seconds, then resume. You yield your "
                            "slot while waiting (you hold no worker). Use for backoff "
                            "between checks. Capped by the runtime."},
            {"parameters", json{{"type", "object"}, {"properties", json{
                {"seconds", json{{"type", "number"}, {"description", "seconds to wait"}}},
            }}, {"required", json::array({"seconds"})}}}}}};
    }
    json invoke(json) override { return {{"error", "wait handled by the scheduler"}}; }
};

struct PollUntilTool : server_tool {
    PollUntilTool() { name = "poll_until"; display_name = name; permission_write = false; }
    json get_definition() override {
        return {{"type", "function"}, {"function", json{
            {"name", name},
            {"description",
             "Periodically call another tool until its result satisfies a predicate "
             "(or a timeout). You yield your slot between checks (no busy-wait). The "
             "predicate is one of: {\"contains\":\"x\"}, {\"regex\":\"re\"}, "
             "{\"equals\":\"x\"}, or {\"json_path\":\".a.b\", \"equals|contains|regex\":..}. "
             "Returns the matched value, or a timeout marker."},
            {"parameters", json{{"type", "object"}, {"properties", json{
                {"tool", json{{"type", "string"}, {"description", "tool name to poll"}}},
                {"args", json{{"type", "object"}, {"description", "arguments passed to that tool"}}},
                {"predicate", json{{"type", "object"}, {"description", "match spec (see description)"}}},
                {"interval_s", json{{"type", "number"}, {"description", "seconds between checks (default 1)"}}},
                {"timeout_s", json{{"type", "number"}, {"description", "give up after this many seconds (default 30)"}}},
                {"max_iterations", json{{"type", "integer"}, {"description", "max checks (capped)"}}},
            }}, {"required", json::array({"tool", "predicate"})}}}}}};
    }
    json invoke(json) override { return {{"error", "poll_until handled by the scheduler"}}; }
};

struct ScheduleTool : server_tool {
    ScheduleTool() { name = "schedule"; display_name = name; permission_write = true; }
    json get_definition() override {
        return {{"type", "function"}, {"function", json{
            {"name", name},
            {"description",
             "Schedule a message to be delivered to an agent's mailbox LATER (a "
             "wake-on-timer). NON-BLOCKING: returns a job id immediately and you "
             "keep going. Set 'to' (default: yourself), 'message', 'delay_s'. For a "
             "repeating timer set 'interval_s' (+ optional 'count'). Bounded by the "
             "runtime (max jobs / horizon)."},
            {"parameters", json{{"type", "object"}, {"properties", json{
                {"to", json{{"type", "string"}, {"description", "target agent id/name (default: self)"}}},
                {"message", json{{"type", "string"}, {"description", "message body to deliver"}}},
                {"delay_s", json{{"type", "number"}, {"description", "seconds until the first delivery"}}},
                {"interval_s", json{{"type", "number"}, {"description", "repeat period (omit for one-shot)"}}},
                {"count", json{{"type", "integer"}, {"description", "number of repeats (default 1)"}}},
            }}, {"required", json::array({"message"})}}}}}};
    }
    json invoke(json p) override {
        if (!t_rt || !t_agent) return {{"error", "schedule is only callable inside an agent runtime turn"}};
        std::string to      = str_arg(p, {"to", "target", "agent"});
        if (to.empty()) to = t_agent->id;            // default: deliver to self
        std::string content = str_arg(p, {"message", "content", "task", "text"});
        if (content.empty()) return {{"error", "schedule requires a 'message'"}};
        double delay    = (p.is_object() && p.contains("delay_s")    && p["delay_s"].is_number())    ? p["delay_s"].get<double>()    : 0.0;
        double interval = (p.is_object() && p.contains("interval_s") && p["interval_s"].is_number()) ? p["interval_s"].get<double>() : 0.0;
        int    count    = (p.is_object() && p.contains("count")      && p["count"].is_number_integer()) ? p["count"].get<int>()     : 0;
        std::string err;
        std::string jid = t_rt->schedule_job(t_agent->id, to, content, delay, interval, count, err);
        if (jid.empty()) return {{"error", err}};
        char buf[64];
        snprintf(buf, sizeof buf, "%.3g", delay);
        return {{"plain_text_response",
                 "scheduled job " + jid + " -> " + to + " in " + std::string(buf) + "s"
                 + (interval > 0 ? " (repeating)" : "")},
                {"job_id", jid}};
    }
};

// ---------------------------------------------------------------------------
// the default TurnFn — an agent's scheduled cycle
// ---------------------------------------------------------------------------
std::vector<std::string> resolve_await_ids(Runtime & rt, Agent & a, const json & args) {
    std::vector<std::string> ids;
    if (args.is_object() && args.contains("ids")) {
        const json & v = args["ids"];
        if (v.is_array()) {
            for (const auto & e : v) if (e.is_string()) {
                Agent * t = rt.find(e.get<std::string>());
                ids.push_back(t ? t->id : e.get<std::string>());
            }
        } else if (v.is_string()) {
            Agent * t = rt.find(v.get<std::string>());
            ids.push_back(t ? t->id : v.get<std::string>());
        }
    }
    if (ids.empty()) {
        // default: every child of this agent
        for (const auto & e : rt.list())
            if (e.value("parent_id", "") == a.id) ids.push_back(e.value("id", ""));
    }
    return ids;
}

// Advance one poll_until step: check the overall timeout/iteration caps, invoke
// the polled tool once, test the predicate. On a satisfied predicate / timeout /
// cap it appends the parked tool's result to the conversation, clears the park
// state, and returns true (the turn loop continues). Otherwise it persists the
// updated poll state and fills `out` with a TIMER PARK for the next interval,
// returning false (the agent yields its worker until the timer fires again).
bool poll_advance(Runtime & rt, Agent & a, json & pstate,
                  const std::string & call_id, TurnOutcome & out) {
    double      now       = agentrt::steady_seconds();
    int         iters     = pstate.value("iters", 0);
    double      deadline  = pstate.value("deadline", 0.0);
    int         max_iters = pstate.value("max_iters", 0);
    double      interval  = pstate.value("interval", 1.0);
    std::string tool      = pstate.value("tool", std::string());
    json        args      = pstate.contains("args")      ? pstate["args"]      : json::object();
    json        pred      = pstate.contains("predicate") ? pstate["predicate"] : json::object();

    auto resolve = [&](const json & content) {
        std::lock_guard<std::mutex> lk(a.mu);
        a.conversation.push_back({{"role", "tool"}, {"tool_call_id", call_id}, {"content", content.dump()}});
        a.await.active = false;
        a.park_state   = json::object();
    };

    // overall timeout / iteration cap reached -> give up (no deadlock)
    if ((deadline > 0 && now >= deadline) || (max_iters > 0 && iters >= max_iters)) {
        resolve(json{{"poll", "timeout"}, {"tool", tool}, {"iterations", iters}});
        rt.trace(json{{"type", "poll"}, {"agent_id", a.id}, {"event", "timeout"},
                      {"tool", tool}, {"iterations", iters}});
        return true;
    }

    // invoke the polled tool once (in-process, this worker stack)
    json result;
    if (g_registry && !tool.empty()) {
        t_rt = &rt; t_agent = &a;
        try { result = g_registry->invoke(tool, args); }
        catch (const std::exception & e) { result = json{{"error", std::string("tool error: ") + e.what()}}; }
        t_rt = nullptr; t_agent = nullptr;
    } else {
        result = json{{"error", tool.empty() ? "poll_until: no tool" : "no tool registry"}};
    }
    std::string rstr = result.is_string() ? result.get<std::string>() : result.dump();
    ++iters;

    agentrt::PredResult pr = agentrt::predicate_match(pred, rstr);
    rt.trace(json{{"type", "poll"}, {"agent_id", a.id},
                  {"event", pr.matched ? "match" : "check"},
                  {"tool", tool}, {"iteration", iters}});

    if (pr.matched) {
        resolve(json{{"poll", "matched"}, {"value", pr.value}, {"iterations", iters},
                     {"result", rstr.substr(0, 400)}});
        return true;
    }

    // not satisfied -> persist progress and re-park for another interval
    pstate["iters"] = iters;
    { std::lock_guard<std::mutex> lk(a.mu); a.park_state = pstate; }
    out.kind          = TurnOutcome::PARK;
    out.timer_park    = true;
    out.await_call_id = call_id;
    out.timeout_s     = interval;
    return false;
}

TurnOutcome default_turn(Runtime & rt, Agent & a, std::vector<Message> & inbox) {
    // 1. resume from a park (append the parked tool's result), THEN inject any
    //    delivered messages as user turns (after the tool result, so the
    //    assistant(tool_calls)->tool ordering invariant holds).
    {
        PendingAwait::Kind kind = PendingAwait::Kind::AWAIT;
        bool   active   = false;
        std::string call_id;
        json   pstate;
        { std::lock_guard<std::mutex> lk(a.mu);
          active = a.await.active; kind = a.await.kind;
          call_id = a.await.tool_call_id; pstate = a.park_state; }

        if (active && kind == PendingAwait::Kind::AWAIT) {
            std::lock_guard<std::mutex> lk(a.mu);
            json results = json::object();
            for (const auto & id : a.await.wait_ids) {
                auto it = a.await.collected.find(id);
                results[id] = it != a.await.collected.end()
                                  ? it->second : std::string("(no response / timed out)");
            }
            a.conversation.push_back({{"role", "tool"},
                                      {"tool_call_id", a.await.tool_call_id},
                                      {"content", results.dump()}});
            rt.trace(json{{"type", "await_resume"}, {"agent_id", a.id},
                          {"reported", (int) a.await.collected.size()},
                          {"timed_out", a.await.timed_out}});
            a.await.active = false;
            a.await.collected.clear();
            a.await.wait_ids.clear();
        } else if (active && kind == PendingAwait::Kind::TIMER) {
            std::string pk = pstate.value("kind", std::string("wait"));
            if (pk == "poll") {
                TurnOutcome o;
                if (!poll_advance(rt, a, pstate, call_id, o)) {
                    // re-parked: keep any drained messages for a later cycle
                    if (!inbox.empty()) {
                        std::lock_guard<std::mutex> lk(a.mu);
                        for (auto it = inbox.rbegin(); it != inbox.rend(); ++it)
                            a.mailbox.push_front(*it);
                    }
                    return o;
                }
            } else {  // wait
                double secs = pstate.value("seconds", 0.0);
                std::lock_guard<std::mutex> lk(a.mu);
                a.conversation.push_back({{"role", "tool"}, {"tool_call_id", call_id},
                                          {"content", json{{"waited", secs}}.dump()}});
                a.await.active = false;
                a.park_state   = json::object();
                rt.trace(json{{"type", "wait"}, {"agent_id", a.id}, {"event", "resume"},
                              {"seconds", secs}});
            }
        }

        std::lock_guard<std::mutex> lk(a.mu);
        for (const auto & m : inbox)
            a.conversation.push_back({{"role", "user"},
                {"content", "[message from " + m.from + "]\n" + m.content}});
    }

    json tools = tools_for(a.allow);
    const Guards & g = rt.guards();

    for (;;) {
        if (a.turns_run >= g.max_turns_agent) {
            rt.trace(json{{"type", "guard"}, {"agent_id", a.id}, {"reason", "max_turns_agent"}});
            TurnOutcome o; o.kind = TurnOutcome::DONE;
            o.result = a.last_result.empty() ? "(stopped: per-agent turn cap)" : a.last_result;
            if (a.parent_id.empty()) rt.set_final_answer(a.id, o.result);
            return o;
        }
        if (!rt.charge_turn()) {
            rt.trace(json{{"type", "guard"}, {"agent_id", a.id}, {"reason", "global_budget"}});
            TurnOutcome o; o.kind = TurnOutcome::DONE;
            o.result = a.last_result.empty() ? "(stopped: global turn/token budget)" : a.last_result;
            if (a.parent_id.empty()) rt.set_final_answer(a.id, o.result);
            return o;
        }

        json conv;
        { std::lock_guard<std::mutex> lk(a.mu); conv = a.conversation; }

        double t0 = agentrt::steady_seconds();
        long up = 0, uc = 0;
        json msg = post_chat(conv, tools, up, uc);
        double dur_ms = (agentrt::steady_seconds() - t0) * 1000.0;
        a.turns_run++;
        rt.add_usage(a, up, uc);

        if (msg.contains("__error")) {
            rt.trace(json{{"type", "turn"}, {"agent_id", a.id}, {"error", msg["__error"]}});
            TurnOutcome o; o.kind = TurnOutcome::FAILED;
            o.result = "(stopped: " + msg["__error"].get<std::string>() + ")";
            return o;
        }

        std::string content = (msg.contains("content") && msg["content"].is_string())
                                  ? msg["content"].get<std::string>() : "";
        json tool_calls = (msg.contains("tool_calls") && msg["tool_calls"].is_array())
                              ? msg["tool_calls"] : json::array();

        rt.trace(json{{"type", "turn"}, {"agent_id", a.id}, {"parent_id", a.parent_id},
                      {"dur_ms", dur_ms},
                      {"tokens_prompt", up}, {"tokens_completion", uc},
                      {"agent_tokens_prompt", a.tok_prompt},
                      {"agent_tokens_completion", a.tok_completion},
                      {"session", rt.metrics()},
                      {"n_tool_calls", (int) tool_calls.size()}});

        if (tool_calls.empty()) {
            TurnOutcome o; o.kind = TurnOutcome::DONE; o.result = content;
            if (a.parent_id.empty()) rt.set_final_answer(a.id, content);
            return o;
        }

        { std::lock_guard<std::mutex> lk(a.mu); a.conversation.push_back(msg); }

        std::string await_call_id;
        std::vector<std::string> await_ids;
        double await_timeout = 0;
        bool        pending_timer_park = false;  // wait/poll_until requested a TIMER park
        TurnOutcome timer_out;

        // dispatch each tool call (runtime tools in-process; leaf tools via the
        // registry, also in-process on this 8 MiB worker stack)
        for (const auto & call : tool_calls) {
            json fn = call.value("function", json::object());
            std::string tname = fn.value("name", "");
            std::string raw   = fn.value("arguments", "");
            json args = json::parse(raw.empty() ? "{}" : raw, nullptr, false);
            if (args.is_discarded()) args = json::object();
            std::string cid = call.value("id", "");

            rt.trace(json{{"type", "tool_call"}, {"agent_id", a.id},
                          {"tool", tname}, {"args", args}});

            if (tname == "await") {
                await_call_id = cid;
                await_ids     = resolve_await_ids(rt, a, args);
                if (args.is_object() && args.contains("timeout") && args["timeout"].is_number())
                    await_timeout = args["timeout"].get<double>();
                continue;  // park after the other calls' results are recorded
            }

            if (tname == "wait") {
                double secs = (args.is_object() && args.contains("seconds") && args["seconds"].is_number())
                                  ? args["seconds"].get<double>()
                                  : (args.is_object() && args.contains("s") && args["s"].is_number()
                                         ? args["s"].get<double>() : 0.0);
                if (secs < 0) secs = 0;
                if (secs > g.max_wait_s) secs = g.max_wait_s;
                { std::lock_guard<std::mutex> lk(a.mu);
                  a.park_state = json{{"kind", "wait"}, {"seconds", secs}}; }
                rt.trace(json{{"type", "wait"}, {"agent_id", a.id}, {"event", "park"},
                              {"seconds", secs}});
                timer_out = TurnOutcome();
                timer_out.kind          = TurnOutcome::PARK;
                timer_out.timer_park    = true;
                timer_out.await_call_id = cid;
                timer_out.timeout_s     = secs > 0 ? secs : 0.001;  // always wake via timer
                pending_timer_park = true;
                continue;
            }

            if (tname == "poll_until") {
                std::string ptool = args.value("tool", std::string());
                if (ptool.empty()) {
                    std::lock_guard<std::mutex> lk(a.mu);
                    a.conversation.push_back({{"role", "tool"}, {"tool_call_id", cid},
                        {"content", json{{"error", "poll_until requires 'tool'"}}.dump()}});
                    continue;
                }
                double interval = args.value("interval_s", args.value("interval", 1.0));
                if (interval <= 0) interval = 1.0;
                if (interval > g.max_poll_interval_s) interval = g.max_poll_interval_s;
                double timeout = args.value("timeout_s", args.value("timeout", 30.0));
                if (timeout < 0) timeout = 0;
                if (timeout > g.max_poll_timeout_s) timeout = g.max_poll_timeout_s;
                int max_iters = args.value("max_iterations", 0);
                if (max_iters <= 0 || max_iters > g.max_poll_iterations) max_iters = g.max_poll_iterations;

                json ps;
                ps["kind"]      = "poll";
                ps["tool"]      = ptool;
                ps["args"]      = (args.is_object() && args.contains("args") && args["args"].is_object())
                                      ? args["args"] : json::object();
                ps["predicate"] = (args.is_object() && args.contains("predicate"))
                                      ? args["predicate"] : json::object();
                ps["interval"]  = interval;
                ps["deadline"]  = timeout > 0 ? agentrt::steady_seconds() + timeout : 0.0;
                ps["max_iters"] = max_iters;
                ps["iters"]     = 0;
                rt.trace(json{{"type", "poll"}, {"agent_id", a.id}, {"event", "park"},
                              {"tool", ptool}, {"interval_s", interval}, {"timeout_s", timeout}});

                TurnOutcome o;
                if (!poll_advance(rt, a, ps, cid, o)) { timer_out = o; pending_timer_park = true; }
                continue;  // either resolved (tool result appended) or parked
            }

            json result;
            if (g_registry) {
                t_rt = &rt; t_agent = &a;
                try { result = g_registry->invoke(tname, args); }
                catch (const std::exception & e) { result = json{{"error", std::string("tool error: ") + e.what()}}; }
                t_rt = nullptr; t_agent = nullptr;
            } else {
                result = json{{"error", "no tool registry"}};
            }
            std::string rstr = result.is_string() ? result.get<std::string>() : result.dump();
            { std::lock_guard<std::mutex> lk(a.mu);
              a.conversation.push_back({{"role", "tool"}, {"tool_call_id", cid}, {"content", rstr}}); }
            rt.trace(json{{"type", "tool_result"}, {"agent_id", a.id}, {"tool", tname},
                          {"result", rstr.substr(0, 400)},
                          {"session", rt.metrics()}});
        }

        if (!await_call_id.empty()) {
            TurnOutcome o; o.kind = TurnOutcome::PARK;
            o.await_call_id = await_call_id;
            o.wait_ids = await_ids;
            o.timeout_s = await_timeout;
            return o;
        }
        if (pending_timer_park) return timer_out;  // wait / poll_until yielded the worker
        // else loop: feed tool results back to the model
    }
}

// ---------------------------------------------------------------------------
// orchestrator system prompt
// ---------------------------------------------------------------------------
std::string orchestrator_prompt() {
    return
        "You are the ORCHESTRATOR of a team of AI agents. You coordinate work but "
        "do little research yourself. Your tools:\n"
        "- spawn_agent(role, task): create a sub-agent that runs IN PARALLEL and "
        "reports back. Returns its id immediately (non-blocking). To work on "
        "independent sub-tasks concurrently, emit SEVERAL spawn_agent calls before "
        "awaiting.\n"
        "- await(ids): pause until those sub-agents report (you free your slot while "
        "waiting). Omit ids to await all your children.\n"
        "- send_message(to, content): message another agent.\n"
        "- list_agents(): see the team.\n"
        "STRATEGY: decompose the goal into independent sub-tasks, spawn one "
        "researcher per sub-task (all at once, for parallelism), await them, then "
        "synthesize a final answer from their reports. When done, reply with the "
        "final answer and NO tool calls.";
}

// ---------------------------------------------------------------------------
// HTTP helpers
// ---------------------------------------------------------------------------
server_http_res_ptr json_res(int status, const json & body) {
    auto r = std::make_unique<server_http_res>();
    r->status = status;
    r->content_type = "application/json; charset=utf-8";
    r->data = body.dump();
    return r;
}

// Default session root: <TMPDIR|/tmp>/llamafile_sessions (overridable with
// --session-dir, which sets g_session_root via the setter below).
std::string default_session_root() {
    const char * tmp = getenv("TMPDIR");
    std::string base = (tmp && *tmp) ? tmp : "/tmp";
    if (!base.empty() && base.back() == '/') base.pop_back();
    return base + "/llamafile_sessions";
}

// The model/settings fingerprint guarding KV restore. n_ctx/rope/kv_type are not
// reachable from the loopback-HTTP turn path, so we fingerprint by model alias +
// n_parallel + the state-format version; that still detects a model/slot change.
KvFingerprint model_fingerprint() {
    KvFingerprint fp;
    fp.model_id   = g_model;
    fp.n_parallel = g_n_parallel;
    fp.state_format_version = agentrt::kRuntimeStateVersion;
    return fp;
}

// Per-agent KV hooks. In this runtime agents decode over loopback HTTP (the
// server's shared slot pool, continuous batching) — they own NO addressable
// per-agent llama_context/sequence, so there is no correct KV to snapshot. We
// therefore decline (return false) and the conversation (always persisted) is
// re-prefilled on resume. This is also the MANDATED fallback for the hybrid
// Qwen3.6 (recurrent state would not round-trip llama_state_seq_save_file). The
// hook is here so a future per-agent-context wiring drops straight in.
bool kv_save_hook(const Agent &, const std::string &, KvFingerprint &) {
    return false;
}
bool kv_load_hook(Agent &, const std::string &, const KvFingerprint &) {
    return false;
}

// Configure the SessionManager once (lazy: first create/resume). Holds
// g_session_mu (the caller does).
void ensure_sm_configured_locked() {
    if (g_sm_configured) return;
    if (g_session_root.empty()) g_session_root = default_session_root();
    Guards guards;
    g_sm.configure(g_session_root, /*max_sessions*/ 32, g_n_parallel, guards,
                   default_turn, model_fingerprint(), kv_save_hook, kv_load_hook);
    g_sm_configured = true;
}

// Spawn the orchestrator into a fresh session Runtime (the SeedFn).
void seed_orchestrator(Runtime & rt, const std::string & goal) {
    std::string err;
    std::vector<std::string> orch_tools = {"spawn_agent", "send_message", "await", "list_agents",
                                           "wait", "poll_until", "schedule",
                                           "wiki_*", "wikidata_*", "code_run_js"};
    rt.spawn("", "orchestrator", "orchestrator", orchestrator_prompt(), goal, orch_tools, err);
}

// Create + start a session; records it as g_current_sid for the /runtime/*
// aliases. Returns the session id ("" + err on failure).
std::string create_session(const std::string & goal, std::string & err) {
    std::lock_guard<std::mutex> lk(g_session_mu);
    ensure_sm_configured_locked();
    std::string sid = g_sm.create(goal, seed_orchestrator, err);
    if (!sid.empty()) g_current_sid = sid;
    return sid;
}

// --- SSE: stream a session's broker (replay-from-0 then live) --------------
server_http_res_ptr events_stream(agentrt::EventBroker * b) {
    auto r = std::make_unique<server_http_res>();
    r->status = 200;
    r->content_type = "text/event-stream";
    r->headers["Cache-Control"] = "no-cache";
    r->headers["Connection"]    = "keep-alive";
    auto cursor = std::make_shared<size_t>(0);
    r->next = [b, cursor](std::string & chunk) -> bool {
        std::unique_lock<std::mutex> lk(b->mu);
        b->cv.wait_for(lk, std::chrono::seconds(15),
                       [&]() { return b->closed || *cursor < b->log.size(); });
        if (*cursor < b->log.size()) {
            chunk = "data: " + b->log[*cursor] + "\n\n";
            ++(*cursor);
            return true;
        }
        if (b->closed) { chunk = "event: close\ndata: {\"type\":\"close\"}\n\n"; return false; }
        chunk = ": keepalive\n\n";
        return true;
    };
    return r;
}

// --- static UI assets (embedded in the APE zip, served from the /zip/ VFS) --
// The interactive-runtime web UI (agent lanes/timeline + session manager). It
// is a self-contained, dependency-free page that drives the endpoints below.
// Mirrors webcam_agent.cpp's serve_asset(): read the embedded /zip/ file.
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
    if (!read_zip_asset(zip_path, body))
        return json_res(404, json{{"error", "embedded asset not found"}, {"path", zip_path}});
    auto r = std::make_unique<server_http_res>();
    r->status = 200;
    r->content_type = content_type;
    r->data = std::move(body);
    return r;
}

// GET /agents (+ /agents/ui alias) — the embedded multi-agent runtime web UI.
server_http_res_ptr handle_agents_ui(const server_http_req &) {
    return serve_asset("/zip/llamafile/agent_ui/agents.html",
                       "text/html; charset=utf-8");
}

server_http_res_ptr serve_trace_file(const std::string & path) {
    FILE * f = fopen(path.c_str(), "rb");
    if (!f) return json_res(404, json{{"error", "no trace yet"}});
    std::string body;
    char buf[8192]; size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) body.append(buf, n);
    fclose(f);
    auto r = std::make_unique<server_http_res>();
    r->status = 200;
    r->content_type = "application/x-ndjson; charset=utf-8";
    r->data = std::move(body);
    return r;
}

// ===========================================================================
// Phase 4 endpoints: /session (+ :id/...) + /sessions
// ===========================================================================

// POST /session {goal} -> {session, orchestrator?}
server_http_res_ptr handle_session_create(const server_http_req & req) {
    std::string body = req.body;
    int status = 200;
    json out;
    run_joined_big_stack([&]() {
        std::string goal;
        json j = json::parse(body.empty() ? "{}" : body, nullptr, false);
        if (j.is_object()) {
            if (j.contains("goal") && j["goal"].is_string()) goal = j["goal"].get<std::string>();
            if (goal.empty() && j.contains("task") && j["task"].is_string()) goal = j["task"].get<std::string>();
        }
        if (goal.empty()) { status = 400; out = json{{"error", "missing 'goal'"}}; return; }
        std::string err;
        std::string sid = create_session(goal, err);
        if (sid.empty()) { status = 500; out = json{{"error", err}}; return; }
        out = json{{"ok", true}, {"session", sid}, {"goal", goal},
                   {"trace", g_sm.trace_path(sid)}};
    });
    return json_res(status, out);
}

// GET /sessions -> [ {id,status,n_agents,last_activity,total_tokens,...} ]
server_http_res_ptr handle_sessions_list(const server_http_req &) {
    int status = 200; json out;
    run_joined_big_stack([&]() {
        std::lock_guard<std::mutex> lk(g_session_mu);
        ensure_sm_configured_locked();
        out = json{{"sessions", g_sm.list()}, {"active", g_sm.active_id()}};
    });
    return json_res(status, out);
}

// GET /session/:id -> full state incl agents
server_http_res_ptr handle_session_get(const server_http_req & req) {
    std::string id = req.get_param("id");
    int status = 200; json out;
    run_joined_big_stack([&]() {
        std::lock_guard<std::mutex> lk(g_session_mu);
        ensure_sm_configured_locked();
        bool found = false;
        out = g_sm.describe(id, found);
        if (!found) { status = 404; out = json{{"error", "no session '" + id + "'"}}; }
    });
    return json_res(status, out);
}

// GET /session/:id/events (SSE) — activates (resumes) the session if needed.
server_http_res_ptr handle_session_events(const server_http_req & req) {
    std::string id = req.get_param("id");
    std::lock_guard<std::mutex> lk(g_session_mu);
    ensure_sm_configured_locked();
    std::string err;
    agentrt::EventBroker * b = g_sm.broker_for(id, err);
    if (!b) return json_res(404, json{{"error", err}});
    return events_stream(b);
}

// GET /session/:id/trace
server_http_res_ptr handle_session_trace(const server_http_req & req) {
    std::string id = req.get_param("id");
    std::string path;
    { std::lock_guard<std::mutex> lk(g_session_mu);
      ensure_sm_configured_locked();
      path = g_sm.trace_path(id); }
    return serve_trace_file(path);
}

server_http_res_ptr handle_session_pause(const server_http_req & req) {
    std::string id = req.get_param("id");
    int status = 200; json out;
    run_joined_big_stack([&]() {
        std::lock_guard<std::mutex> lk(g_session_mu);
        ensure_sm_configured_locked();
        std::string err;
        if (!g_sm.pause(id, err)) { status = 404; out = json{{"error", err}}; return; }
        out = json{{"ok", true}, {"session", id}, {"status", "paused"}};
    });
    return json_res(status, out);
}

server_http_res_ptr handle_session_resume(const server_http_req & req) {
    std::string id = req.get_param("id");
    int status = 200; json out;
    run_joined_big_stack([&]() {
        std::lock_guard<std::mutex> lk(g_session_mu);
        ensure_sm_configured_locked();
        std::string err;
        if (!g_sm.resume(id, err)) { status = 409; out = json{{"error", err}}; return; }
        g_current_sid = id;
        bool found = false;
        out = g_sm.describe(id, found);
        out["ok"] = true;
    });
    return json_res(status, out);
}

server_http_res_ptr handle_session_stop(const server_http_req & req) {
    std::string id = req.get_param("id");
    int status = 200; json out;
    run_joined_big_stack([&]() {
        std::lock_guard<std::mutex> lk(g_session_mu);
        ensure_sm_configured_locked();
        std::string err;
        if (!g_sm.stop(id, err)) { status = 404; out = json{{"error", err}}; return; }
        out = json{{"ok", true}, {"session", id}, {"status", "done"}};
    });
    return json_res(status, out);
}

// ---------------------------------------------------------------------------
// Legacy /runtime/* — aliases over the "current" (last created/active) session.
// ---------------------------------------------------------------------------
std::string current_sid_locked() { return g_current_sid; }

server_http_res_ptr handle_start(const server_http_req & req) {
    return handle_session_create(req);   // POST /runtime/start == POST /session
}

server_http_res_ptr handle_events(const server_http_req &) {
    std::string id;
    { std::lock_guard<std::mutex> lk(g_session_mu); id = current_sid_locked(); }
    if (id.empty()) return json_res(409, json{{"error", "no active session; POST /runtime/start first"}});
    std::lock_guard<std::mutex> lk(g_session_mu);
    std::string err;
    agentrt::EventBroker * b = g_sm.broker_for(id, err);
    if (!b) return json_res(404, json{{"error", err}});
    return events_stream(b);
}

server_http_res_ptr handle_status(const server_http_req &) {
    int status = 200; json out;
    run_joined_big_stack([&]() {
        std::lock_guard<std::mutex> lk(g_session_mu);
        if (g_current_sid.empty()) { status = 409; out = json{{"error", "no active session"}}; return; }
        bool found = false;
        out = g_sm.describe(g_current_sid, found);
        if (!found) { status = 409; out = json{{"error", "no active session"}}; }
    });
    return json_res(status, out);
}

server_http_res_ptr handle_trace(const server_http_req &) {
    std::string path;
    { std::lock_guard<std::mutex> lk(g_session_mu);
      if (g_current_sid.empty()) return json_res(404, json{{"error", "no trace yet"}});
      path = g_sm.trace_path(g_current_sid); }
    return serve_trace_file(path);
}

server_http_res_ptr handle_stop(const server_http_req &) {
    std::lock_guard<std::mutex> lk(g_session_mu);
    if (!g_current_sid.empty()) { std::string err; g_sm.stop(g_current_sid, err); }
    return json_res(200, json{{"ok", true}});
}

}  // namespace

// ---------------------------------------------------------------------------
// public API (declared in server.cpp under LLAMAFILE_TUI)
// ---------------------------------------------------------------------------
void llamafile_runtime_configure(const std::string & host, int port,
                                 const std::string & api_key,
                                 const std::string & model, int n_parallel) {
    g_host = host.empty() ? "127.0.0.1" : host;
    if (g_host == "0.0.0.0" || g_host == "::") g_host = "127.0.0.1";
    g_port = port;
    g_api_key = api_key;
    if (!model.empty()) g_model = model;
    g_n_parallel = n_parallel > 0 ? n_parallel : 4;
}

int llamafile_runtime_register_tools(server_tools & registry) {
    g_registry = &registry;
    registry.tools.push_back(std::make_unique<SpawnAgentTool>());
    registry.tools.push_back(std::make_unique<SendMessageTool>());
    registry.tools.push_back(std::make_unique<AwaitTool>());
    registry.tools.push_back(std::make_unique<ListAgentsTool>());
    registry.tools.push_back(std::make_unique<WaitTool>());
    registry.tools.push_back(std::make_unique<PollUntilTool>());
    registry.tools.push_back(std::make_unique<ScheduleTool>());
    return 7;
}

// --session-dir DIR — set the session persistence root (parsed in args.cpp).
void llamafile_runtime_set_session_root(const char * dir) {
    if (dir && *dir) g_session_root = dir;
}

void llamafile_runtime_register_routes(server_http_context & http) {
    // Phase 4: multi-session endpoints.
    http.post("/session",              handle_session_create);
    http.get ("/sessions",             handle_sessions_list);
    http.get ("/session/:id",          handle_session_get);
    http.get ("/session/:id/events",   handle_session_events);
    http.get ("/session/:id/trace",    handle_session_trace);
    http.post("/session/:id/pause",    handle_session_pause);
    http.post("/session/:id/resume",   handle_session_resume);
    http.post("/session/:id/stop",     handle_session_stop);

    // Phase 5: the embedded web UI (agent lanes/timeline + session manager).
    http.get ("/agents",     handle_agents_ui);
    http.get ("/agents/ui",  handle_agents_ui);

    // Legacy /runtime/* aliases (back-compat: drive the current session).
    http.post("/runtime/start",  handle_start);
    http.get ("/runtime/events", handle_events);
    http.get ("/runtime/status", handle_status);
    http.get ("/runtime/agents", handle_status);
    http.get ("/runtime/trace",  handle_trace);
    http.post("/runtime/stop",   handle_stop);
}
