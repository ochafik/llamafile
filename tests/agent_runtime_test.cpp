// -*- mode:c++;indent-tabs-mode:nil;c-basic-offset:4;coding:utf-8 -*-
// vi: set et ft=cpp ts=4 sts=4 sw=4 fenc=utf-8 :vi
//
// Copyright 2026 Mozilla.ai - Apache-2.0
//
// Unit tests for the interactive multi-agent runtime CORE (agent_runtime.cpp):
// Router delivery (by id + name), mailbox FIFO + thread-safety, the Scheduler
// (runs-on-input, parks on await, resumes on a delivered message), and the
// runaway guards (max spawn depth, max live agents, a->b message-rate cap). No
// model: the LLM turn is replaced by a fake TurnFn.

#include "agent_runtime.h"

#include <pthread.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

using namespace agentrt;

static int g_fail = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { fprintf(stderr, "FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); ++g_fail; } \
    else         { fprintf(stderr, "ok:   %s\n", msg); } \
} while (0)

static void sleep_ms(int ms) {
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

// Poll until predicate or timeout. Returns true if predicate held.
template <class F>
static bool wait_until(F f, int timeout_ms) {
    for (int i = 0; i < timeout_ms / 5; ++i) { if (f()) return true; sleep_ms(5); }
    return f();
}

// ---------------------------------------------------------------------------
// 1. Router delivery by id and by name + mailbox FIFO (single-threaded)
// ---------------------------------------------------------------------------
static void test_router_delivery() {
    fprintf(stderr, "\n== test_router_delivery ==\n");
    Runtime rt;
    rt.configure("t1", "", 2, Guards{});
    // scheduler NOT started: messages just accumulate in the mailbox.

    std::string err;
    std::string id = rt.spawn("", "worker", "bob", "sys", "", {}, err);
    CHECK(!id.empty(), "spawn returns an id");

    CHECK(rt.send("user", id, "by-id-1", err), "send by id ok");
    CHECK(rt.send("user", "bob", "by-name-2", err), "send by name ok");
    CHECK(!rt.send("user", "nobody", "x", err), "send to unknown target refused");

    Agent * a = rt.find("bob");
    CHECK(a != nullptr, "find by name resolves");
    {
        std::lock_guard<std::mutex> lk(a->mu);
        CHECK(a->mailbox.size() == 2, "two messages queued");
        CHECK(a->mailbox[0].content == "by-id-1", "FIFO: first message first");
        CHECK(a->mailbox[1].content == "by-name-2", "FIFO: second message second");
    }
}

// ---------------------------------------------------------------------------
// 2. Mailbox thread-safety: many concurrent senders, no lost messages
// ---------------------------------------------------------------------------
static void test_mailbox_threadsafe() {
    fprintf(stderr, "\n== test_mailbox_threadsafe ==\n");
    Runtime rt;
    Guards g; g.max_pair_messages = 1000000;  // don't trip the rate cap here
    rt.configure("t2", "", 2, g);
    std::string err;
    std::string id = rt.spawn("", "sink", "sink", "sys", "", {}, err);

    const int N = 8, M = 200;
    struct Arg { Runtime * rt; std::string id; int t; int m; };
    static auto sender = +[](void * p) -> void * {
        Arg * a = static_cast<Arg *>(p);
        std::string from = "s" + std::to_string(a->t);
        for (int i = 0; i < a->m; ++i) { std::string e; a->rt->send(from, a->id, std::to_string(i), e); }
        return nullptr;
    };
    std::vector<Arg> args(N);
    std::vector<pthread_t> tids(N);
    pthread_attr_t attr; pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 8 * 1024 * 1024);  // json work needs a big stack
    for (int t = 0; t < N; ++t) {
        args[t] = Arg{&rt, id, t, M};
        pthread_create(&tids[t], &attr, sender, &args[t]);
    }
    pthread_attr_destroy(&attr);
    for (int t = 0; t < N; ++t) pthread_join(tids[t], nullptr);

    Agent * a = rt.find(id);
    std::lock_guard<std::mutex> lk(a->mu);
    CHECK((int) a->mailbox.size() == N * M, "all concurrent messages delivered (no loss/dupe)");
}

// ---------------------------------------------------------------------------
// 3. Scheduler: runs-on-input, parks on await, resumes on delivered message
// ---------------------------------------------------------------------------
static std::atomic<int> g_child_runs{0};
static std::atomic<int> g_orch_resumes{0};

static TurnOutcome fake_turn(Runtime & rt, Agent & a, std::vector<Message> &) {
    if (a.role == "orch") {
        bool resuming;
        { std::lock_guard<std::mutex> lk(a.mu); resuming = a.await.active; }
        if (resuming) {
            g_orch_resumes.fetch_add(1);
            TurnOutcome o; o.kind = TurnOutcome::DONE; o.result = "synthesized";
            rt.set_final_answer(a.id, o.result);
            return o;
        }
        // first run: spawn two children IN PARALLEL, then await both
        std::string e1, e2;
        std::string c1 = rt.spawn(a.id, "child", "", "sys", "task1", {}, e1);
        std::string c2 = rt.spawn(a.id, "child", "", "sys", "task2", {}, e2);
        rt.add_usage(a, 10, 5);  // exercise token accounting
        TurnOutcome o; o.kind = TurnOutcome::PARK;
        o.wait_ids = {c1, c2};
        o.await_call_id = "call_await_1";
        return o;
    }
    // child: do a unit of "work", report a result
    g_child_runs.fetch_add(1);
    sleep_ms(20);  // overlap window so two children are clearly concurrent
    rt.add_usage(a, 7, 3);
    TurnOutcome o; o.kind = TurnOutcome::DONE; o.result = "child " + a.id + " done";
    return o;
}

static void test_scheduler_park_resume() {
    fprintf(stderr, "\n== test_scheduler_park_resume ==\n");
    g_child_runs.store(0);
    g_orch_resumes.store(0);
    Runtime rt;
    rt.configure("t3", "", 4, Guards{});
    rt.set_turn_fn(fake_turn);
    rt.start();

    std::string err;
    std::string oid = rt.spawn("", "orch", "orchestrator", "sys", "goal", {}, err);
    CHECK(!oid.empty(), "orchestrator spawned");

    bool done = wait_until([&]() {
        Agent * a = rt.find(oid);
        if (!a) return false;
        std::lock_guard<std::mutex> lk(a->mu);
        return a->status == Status::DONE;
    }, 3000);
    CHECK(done, "scheduler drove orchestrator to DONE (runs-on-input + resume)");
    CHECK(g_child_runs.load() == 2, "both children ran (runs-on-input)");
    CHECK(g_orch_resumes.load() == 1, "orchestrator resumed once after await");

    // token accounting: orch(10/5) + 2 children(7/3 each) = prompt 24, completion 11
    json m = rt.metrics();
    CHECK(m["total_prompt_tokens"] == 24, "session prompt-token total summed across agents");
    CHECK(m["total_completion_tokens"] == 11, "session completion-token total summed across agents");
    CHECK(m["n_agents"] == 3, "n_agents = orchestrator + 2 children");
    CHECK(rt.final_answer() == "synthesized", "final answer recorded");

    rt.stop();
}

// ---------------------------------------------------------------------------
// 4. Guards: max spawn depth, max live agents, a->b message-rate cap
// ---------------------------------------------------------------------------
static void test_guard_depth() {
    fprintf(stderr, "\n== test_guard_depth ==\n");
    Runtime rt;
    Guards g; g.max_depth = 1;
    rt.configure("t4", "", 2, g);
    std::string err;
    std::string a = rt.spawn("", "r", "", "sys", "", {}, err);      // depth 0
    CHECK(!a.empty(), "depth-0 spawn ok");
    std::string b = rt.spawn(a, "r", "", "sys", "", {}, err);       // depth 1
    CHECK(!b.empty(), "depth-1 spawn ok (== cap)");
    std::string c = rt.spawn(b, "r", "", "sys", "", {}, err);       // depth 2 > cap
    CHECK(c.empty(), "depth-2 spawn refused (max_depth)");
    CHECK(err.find("depth") != std::string::npos, "depth refusal explains itself");
}

static void test_guard_live_agents() {
    fprintf(stderr, "\n== test_guard_live_agents ==\n");
    Runtime rt;
    Guards g; g.max_live_agents = 2;
    rt.configure("t5", "", 2, g);
    std::string err;
    CHECK(!rt.spawn("", "r", "", "s", "", {}, err).empty(), "agent 1 ok");
    CHECK(!rt.spawn("", "r", "", "s", "", {}, err).empty(), "agent 2 ok");
    CHECK(rt.spawn("", "r", "", "s", "", {}, err).empty(), "agent 3 refused (max_live_agents)");
    CHECK(err.find("live agents") != std::string::npos, "live-agent refusal explains itself");
}

static void test_guard_message_rate() {
    fprintf(stderr, "\n== test_guard_message_rate ==\n");
    Runtime rt;
    Guards g; g.max_pair_messages = 3;
    rt.configure("t6", "", 2, g);
    std::string err;
    std::string id = rt.spawn("", "r", "b", "s", "", {}, err);
    CHECK(rt.send("a", id, "1", err), "msg 1 ok");
    CHECK(rt.send("a", id, "2", err), "msg 2 ok");
    CHECK(rt.send("a", id, "3", err), "msg 3 ok (== cap)");
    CHECK(!rt.send("a", id, "4", err), "msg 4 refused (cycle/storm rate cap)");
    CHECK(err.find("rate cap") != std::string::npos, "rate-cap refusal explains itself");
}

int main() {
    test_router_delivery();
    test_mailbox_threadsafe();
    test_scheduler_park_resume();
    test_guard_depth();
    test_guard_live_agents();
    test_guard_message_rate();

    if (g_fail) { fprintf(stderr, "\n%d CHECK(s) FAILED\n", g_fail); return 1; }
    fprintf(stderr, "\nALL agent_runtime unit tests passed\n");
    return 0;
}
