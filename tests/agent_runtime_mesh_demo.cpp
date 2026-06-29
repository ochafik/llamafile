// -*- mode:c++;indent-tabs-mode:nil;c-basic-offset:4;coding:utf-8 -*-
// vi: set et ft=cpp ts=4 sts=4 sw=4 fenc=utf-8 :vi
//
// Copyright 2026 Mozilla.ai - Apache-2.0
//
// SCRIPTED MESH RUN (the ddoc-09 section-9.1 integration gate, no model):
// drives the REAL Runtime/Router/Scheduler with a deterministic fake TurnFn that
// reproduces the target flow -- an orchestrator fans out TWO researchers IN
// PARALLEL, parks on await(), the researchers run concurrently and report back,
// the orchestrator resumes and synthesizes -- and writes the REAL trace.jsonl.
// It then prints the trace + proves: (a) overlapping researcher work
// (concurrency), (b) the agent tree via parent_ids, (c) results delivered back,
// (d) await + final answer, (e) per-agent + session token totals.
//
// This exercises the same Runtime the server uses; only the per-turn LLM call is
// replaced. The model-gated end-to-end driver is tests/integration/
// runtime_mesh_test.py.

#include "agent_runtime.h"

#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

using namespace agentrt;

static void sleep_ms(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

static TurnOutcome demo_turn(Runtime & rt, Agent & a, std::vector<Message> &) {
    if (a.role == "orchestrator") {
        bool resuming;
        { std::lock_guard<std::mutex> lk(a.mu); resuming = a.await.active; }
        if (resuming) {
            rt.add_usage(a, 220, 90);  // synthesis turn
            TurnOutcome o; o.kind = TurnOutcome::DONE;
            o.result = "The Eiffel Tower (~330 m) is taller than the Statue of "
                       "Liberty (~93 m).";
            rt.set_final_answer(a.id, o.result);
            return o;
        }
        rt.add_usage(a, 180, 40);  // planning turn
        std::string e1, e2;
        std::string c1 = rt.spawn(a.id, "researcher", "eiffel-researcher",
            "Find the Eiffel Tower height.", "How tall is the Eiffel Tower?", {"wiki_*"}, e1);
        std::string c2 = rt.spawn(a.id, "researcher", "liberty-researcher",
            "Find the Statue of Liberty height.", "How tall is the Statue of Liberty?", {"wiki_*"}, e2);
        TurnOutcome o; o.kind = TurnOutcome::PARK;
        o.wait_ids = {c1, c2};
        o.await_call_id = "call_await";
        return o;
    }
    // researcher: mark the turn window, do a wiki tool call, report a result.
    // The 120 ms window makes the two researchers' execution clearly overlap.
    rt.trace(json{{"type", "turn"}, {"agent_id", a.id}, {"parent_id", a.parent_id},
                  {"phase", "begin"}});
    sleep_ms(120);
    rt.trace(json{{"type", "tool_call"}, {"agent_id", a.id}, {"tool", "wiki_search"}});
    rt.add_usage(a, 140, 35);
    rt.trace(json{{"type", "tool_result"}, {"agent_id", a.id}, {"tool", "wiki_search"}});
    rt.trace(json{{"type", "turn"}, {"agent_id", a.id}, {"parent_id", a.parent_id},
                  {"phase", "end"},
                  {"agent_tokens_prompt", a.tok_prompt},
                  {"agent_tokens_completion", a.tok_completion},
                  {"session", rt.metrics()}});
    TurnOutcome o; o.kind = TurnOutcome::DONE;
    o.result = (a.name == "eiffel-researcher")
                   ? "Eiffel Tower height: about 330 metres (source: Eiffel Tower)."
                   : "Statue of Liberty height: about 93 metres incl. pedestal (source: Statue of Liberty).";
    return o;
}

int main() {
    std::string dir = "/tmp/llamafile_runtime_demo";
    Runtime rt;
    rt.configure("demo", dir, /*n_workers=*/8, Guards{});
    rt.set_turn_fn(demo_turn);
    rt.start();

    std::string err;
    std::string oid = rt.spawn("", "orchestrator", "orchestrator",
        "You coordinate researchers.",
        "Find the heights of the Eiffel Tower and the Statue of Liberty in parallel, then compare.",
        {"spawn_agent", "await"}, err);

    // wait for the orchestrator to finish
    for (int i = 0; i < 1000; ++i) {
        Agent * a = rt.find(oid);
        bool done = false;
        if (a) { std::lock_guard<std::mutex> lk(a->mu); done = a->status == Status::DONE; }
        if (done) break;
        sleep_ms(5);
    }
    sleep_ms(50);
    rt.stop();

    // ---- print the real trace.jsonl ----
    printf("\n===== trace.jsonl (%s/trace.jsonl) =====\n", dir.c_str());
    FILE * f = fopen((dir + "/trace.jsonl").c_str(), "rb");
    std::vector<json> trace;
    if (f) {
        char buf[4096];
        std::string all;
        size_t n; while ((n = fread(buf, 1, sizeof buf, f)) > 0) all.append(buf, n);
        fclose(f);
        size_t p = 0;
        while (p < all.size()) {
            size_t e = all.find('\n', p);
            if (e == std::string::npos) e = all.size();
            std::string line = all.substr(p, e - p);
            if (!line.empty()) { json j = json::parse(line, nullptr, false); if (!j.is_discarded()) trace.push_back(j); }
            p = e + 1;
        }
        printf("%s", all.c_str());
    }

    // ---- agent tree ----
    printf("\n===== agent tree (parent_id) =====\n");
    std::unordered_map<std::string, std::string> nm;
    for (auto & e : trace) if (e.value("type", "") == "spawn") nm[e.value("agent_id", "")] = e.value("name", "");
    for (auto & e : trace) if (e.value("type", "") == "spawn") {
        std::string pid = e.value("parent_id", "");
        printf("  %-18s -> %-18s id=%s depth=%d\n",
               pid.empty() ? "(root)" : nm[pid].c_str(),
               nm[e.value("agent_id", "")].c_str(),
               e.value("agent_id", "").c_str(), e.value("depth", 0));
    }

    // ---- concurrency: do the two researchers' [begin,end] windows overlap? ----
    printf("\n===== concurrency proof (researcher turn windows) =====\n");
    std::vector<std::string> res;
    for (auto & e : trace) if (e.value("type", "") == "spawn" && e.value("parent_id", "") == oid)
        res.push_back(e.value("agent_id", ""));
    auto window = [&](const std::string & id, double & b, double & e2) {
        b = 0; e2 = 0;
        for (auto & e : trace)
            if (e.value("type", "") == "turn" && e.value("agent_id", "") == id) {
                if (e.value("phase", "") == "begin") b = e.value("ts", 0.0);
                if (e.value("phase", "") == "end")   e2 = e.value("ts", 0.0);
            }
    };
    bool overlap = false;
    if (res.size() >= 2) {
        double a0, a1, b0, b1;
        window(res[0], a0, a1); window(res[1], b0, b1);
        printf("  %s: [%.3f .. %.3f]\n", nm[res[0]].c_str(), a0, a1);
        printf("  %s: [%.3f .. %.3f]\n", nm[res[1]].c_str(), b0, b1);
        overlap = (a0 < b1 && b0 < a1);
        printf("  windows OVERLAP in wall-clock time: %s\n", overlap ? "YES" : "no");
    }

    // ---- results delivered back + await + final ----
    int back = 0, awaits = 0;
    for (auto & e : trace) {
        if (e.value("type", "") == "message" && e.value("to", "") == oid) ++back;
        if (e.value("type", "") == "await") ++awaits;
    }
    printf("\n===== mesh =====\n  researcher results delivered back to orchestrator: %d\n"
           "  await events: %d\n  final answer: %s\n",
           back, awaits, rt.final_answer().c_str());

    // ---- token totals (summed across ALL agents) ----
    json m = rt.metrics();
    printf("\n===== session token totals (summed across ALL agents) =====\n"
           "  n_agents=%d n_turns=%d prompt=%ld completion=%ld total=%ld\n",
           (int) m["n_agents"], (int) m["n_turns"],
           (long) m["total_prompt_tokens"], (long) m["total_completion_tokens"],
           (long) m["total_tokens"]);
    printf("  per-agent: %s\n", rt.list().dump().c_str());

    bool ok = res.size() == 2 && overlap && back == 2 && awaits == 1 &&
              !rt.final_answer().empty() && (long) m["total_tokens"] > 0;
    printf("\n%s\n", ok ? "SCRIPTED MESH RUN: PASS" : "SCRIPTED MESH RUN: FAIL");
    return ok ? 0 : 1;
}
