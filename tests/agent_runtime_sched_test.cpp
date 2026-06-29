// -*- mode:c++;indent-tabs-mode:nil;c-basic-offset:4;coding:utf-8 -*-
// vi: set et ft=cpp ts=4 sts=4 sw=4 fenc=utf-8 :vi
//
// Copyright 2026 Mozilla.ai - Apache-2.0
//
// Unit tests for PHASE 2 of the interactive multi-agent runtime (ddoc 09 §7):
// the SCHEDULING tools wait / poll_until / schedule, realized as PARK + timer
// wakeups on the Scheduler/sweeper. No model: the LLM turn is a fake TurnFn that
// drives the SAME core timer-park machinery the server's default_turn uses, plus
// direct unit tests of the poll_until predicate language (agent_predicate.h).
//
// Proven here:
//   * wait()       — the agent parks, HOLDS NO WORKER (a second agent runs on the
//                    single worker meanwhile), and resumes after >= N seconds.
//   * poll_until() — re-checks on a timer until a stub condition flips (resumes
//                    when it holds), respects the interval (no busy-spin), and a
//                    never-true case TIMES OUT instead of looping forever.
//   * schedule()   — a delayed message is delivered to the target's mailbox after
//                    the delay and WAKES the parked target; the max-jobs cap trips.
//   * predicate_match — contains / regex / equals / json_path comparators.

#include "agent_runtime.h"
#include "agent_predicate.h"

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

static void sleep_ms(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

template <class F>
static bool wait_until(F f, int timeout_ms) {
    for (int i = 0; i < timeout_ms / 5; ++i) { if (f()) return true; sleep_ms(5); }
    return f();
}

static bool is_status(Runtime & rt, const std::string & id, Status s) {
    Agent * a = rt.find(id);
    if (!a) return false;
    std::lock_guard<std::mutex> lk(a->mu);
    return a->status == s;
}

// ===========================================================================
// 1. predicate language (pure, no runtime)
// ===========================================================================
static void test_predicate_language() {
    fprintf(stderr, "\n== test_predicate_language ==\n");
    using json = nlohmann::ordered_json;

    CHECK(predicate_match(json{{"contains", "France"}}, "country: France").matched,
          "contains matches a substring");
    CHECK(!predicate_match(json{{"contains", "Spain"}}, "country: France").matched,
          "contains rejects an absent substring");

    auto re = predicate_match(json{{"regex", "[0-9]{3}m"}}, "height 330m tall");
    CHECK(re.matched && re.value == "330m", "regex matches + returns match[0]");

    CHECK(predicate_match(json{{"equals", "done"}}, "done").matched, "equals matches whole");
    CHECK(!predicate_match(json{{"equals", "done"}}, "pending").matched, "equals rejects mismatch");

    std::string R = json{{"status", "ready"}, {"n", 3}, {"items", json::array({"a", "b"})}}.dump();
    CHECK(predicate_match(json{{"json_path", ".status"}, {"equals", "ready"}}, R).matched,
          "json_path + equals (string)");
    CHECK(!predicate_match(json{{"json_path", ".status"}, {"equals", "busy"}}, R).matched,
          "json_path + equals rejects mismatch");
    CHECK(predicate_match(json{{"json_path", ".status"}, {"contains", "rea"}}, R).matched,
          "json_path + contains");
    CHECK(predicate_match(json{{"json_path", ".n"}, {"equals", 3}}, R).matched,
          "json_path + typed numeric equals");
    CHECK(predicate_match(json{{"json_path", ".items.1"}, {"equals", "b"}}, R).matched,
          "json_path walks array indices");
    CHECK(predicate_match(json{{"json_path", ".status"}}, R).matched,
          "json_path with no comparator matches an existing node");
    CHECK(!predicate_match(json{{"json_path", ".missing"}}, R).matched,
          "json_path on an absent node does not match");
    CHECK(!predicate_match(json{{"json_path", ".status"}, {"equals", "ready"}}, "not json").matched,
          "json_path on non-JSON input does not match");

    CHECK(predicate_match(json::object(), "x").matched, "empty predicate matches any non-empty");
    CHECK(!predicate_match(json::object(), "").matched, "empty predicate rejects empty result");
}

// ===========================================================================
// 2. wait(): parks, holds no worker, resumes after N
// ===========================================================================
static std::atomic<double> g_wait_park{0}, g_wait_resume{0}, g_tick_t{0};
static std::atomic<bool>   g_tick_ran{false};

static TurnOutcome wait_turn(Runtime &, Agent & a, std::vector<Message> &) {
    if (a.role == "waiter") {
        bool resuming;
        { std::lock_guard<std::mutex> lk(a.mu); resuming = a.await.active; }
        if (resuming) {
            g_wait_resume.store(steady_seconds());
            { std::lock_guard<std::mutex> lk(a.mu); a.await.active = false; a.park_state = nlohmann::ordered_json::object(); }
            TurnOutcome o; o.kind = TurnOutcome::DONE; o.result = "waited"; return o;
        }
        g_wait_park.store(steady_seconds());
        { std::lock_guard<std::mutex> lk(a.mu);
          a.park_state = nlohmann::ordered_json{{"kind", "wait"}, {"seconds", 0.2}}; }
        TurnOutcome o; o.kind = TurnOutcome::PARK; o.timer_park = true;
        o.timeout_s = 0.2; o.await_call_id = "w1"; return o;
    }
    // ticker: runs once, instantly, on the freed worker
    g_tick_t.store(steady_seconds());
    g_tick_ran.store(true);
    TurnOutcome o; o.kind = TurnOutcome::DONE; o.result = "tick"; return o;
}

static void test_wait_no_worker_held() {
    fprintf(stderr, "\n== test_wait_no_worker_held ==\n");
    g_wait_park.store(0); g_wait_resume.store(0); g_tick_t.store(0); g_tick_ran.store(false);

    Runtime rt;
    rt.configure("w", "", 1, Guards{});   // ONE worker: if wait held it, ticker could not run
    rt.set_turn_fn(wait_turn);
    rt.start();

    std::string err;
    std::string wid = rt.spawn("", "waiter", "waiter", "sys", "go", {}, err);
    CHECK(!wid.empty(), "waiter spawned");
    CHECK(wait_until([&]() { return is_status(rt, wid, Status::WAITING); }, 2000),
          "waiter parked on the wait timer");

    // with the waiter parked, the single worker must be free to run the ticker
    std::string tid = rt.spawn("", "ticker", "ticker", "sys", "go", {}, err);
    CHECK(wait_until([&]() { return g_tick_ran.load(); }, 2000),
          "a second agent ran on the freed worker while the waiter was parked");

    CHECK(wait_until([&]() { return is_status(rt, wid, Status::DONE); }, 3000),
          "waiter resumed to DONE after its timer fired");

    double park = g_wait_park.load(), resume = g_wait_resume.load(), tick = g_tick_t.load();
    CHECK(resume - park >= 0.19, "waiter resumed after >= ~0.2s (timer honored)");
    CHECK(g_tick_ran.load() && tick < resume,
          "ticker ran DURING the wait (proves the waiter held no worker)");
    rt.stop();
}

// ===========================================================================
// 3. poll_until(): flips after K checks, respects interval, no busy-spin
// ===========================================================================
static std::atomic<int>    g_poll_iters{0};
static std::atomic<double> g_poll_start{0}, g_poll_end{0};
static std::atomic<bool>   g_poll_matched{false}, g_poll_tick_ran{false};
static std::atomic<double> g_poll_tick_t{0};
static const int    POLL_K   = 3;
static const double POLL_INT = 0.1;

static TurnOutcome poll_turn(Runtime &, Agent & a, std::vector<Message> &) {
    if (a.role == "poller") {
        bool resuming;
        { std::lock_guard<std::mutex> lk(a.mu); resuming = a.await.active; }
        if (!resuming) g_poll_start.store(steady_seconds());
        // "invoke the stub tool + test predicate": the condition flips at the Kth check
        int c = g_poll_iters.fetch_add(1) + 1;
        if (c >= POLL_K) {
            g_poll_matched.store(true);
            g_poll_end.store(steady_seconds());
            { std::lock_guard<std::mutex> lk(a.mu); a.await.active = false; a.park_state = nlohmann::ordered_json::object(); }
            TurnOutcome o; o.kind = TurnOutcome::DONE; o.result = "matched"; return o;
        }
        { std::lock_guard<std::mutex> lk(a.mu);
          a.park_state = nlohmann::ordered_json{{"kind", "poll"}, {"iters", c}}; }
        TurnOutcome o; o.kind = TurnOutcome::PARK; o.timer_park = true;
        o.timeout_s = POLL_INT; o.await_call_id = "p1"; return o;
    }
    g_poll_tick_t.store(steady_seconds());
    g_poll_tick_ran.store(true);
    TurnOutcome o; o.kind = TurnOutcome::DONE; o.result = "tick"; return o;
}

static void test_poll_until_match() {
    fprintf(stderr, "\n== test_poll_until_match ==\n");
    g_poll_iters.store(0); g_poll_start.store(0); g_poll_end.store(0);
    g_poll_matched.store(false); g_poll_tick_ran.store(false); g_poll_tick_t.store(0);

    Runtime rt;
    rt.configure("p", "", 1, Guards{});
    rt.set_turn_fn(poll_turn);
    rt.start();

    std::string err;
    std::string pid = rt.spawn("", "poller", "poller", "sys", "go", {}, err);
    // a concurrent ticker must get a turn between polls (proves no busy-spin)
    CHECK(wait_until([&]() { return is_status(rt, pid, Status::WAITING); }, 2000),
          "poller parked between checks");
    rt.spawn("", "ticker", "ticker", "sys", "go", {}, err);

    CHECK(wait_until([&]() { return is_status(rt, pid, Status::DONE); }, 3000),
          "poller resolved to DONE when the condition held");
    CHECK(g_poll_matched.load() && g_poll_iters.load() == POLL_K,
          "poller matched on exactly the Kth check");
    double elapsed = g_poll_end.load() - g_poll_start.load();
    CHECK(elapsed >= (POLL_K - 1) * POLL_INT - 0.01,
          "interval respected across checks (not a busy loop)");
    CHECK(g_poll_tick_ran.load() && g_poll_tick_t.load() < g_poll_end.load(),
          "a concurrent agent ran during the poll waits (no worker held)");
    rt.stop();
}

// --- never-true predicate -> times out (no infinite loop) ------------------
static std::atomic<int>    g_nt_iters{0};
static std::atomic<double> g_nt_start{0}, g_nt_end{0};
static std::atomic<bool>   g_nt_timedout{false};

static TurnOutcome poll_timeout_turn(Runtime &, Agent & a, std::vector<Message> &) {
    bool resuming; double deadline;
    {
        std::lock_guard<std::mutex> lk(a.mu);
        resuming = a.await.active;
        deadline = a.park_state.value("deadline", 0.0);
    }
    if (!resuming) { g_nt_start.store(steady_seconds()); deadline = steady_seconds() + 0.3; }
    if (steady_seconds() >= deadline) {           // overall timeout: give up
        g_nt_timedout.store(true);
        g_nt_end.store(steady_seconds());
        { std::lock_guard<std::mutex> lk(a.mu); a.await.active = false; a.park_state = nlohmann::ordered_json::object(); }
        TurnOutcome o; o.kind = TurnOutcome::DONE; o.result = "timeout"; return o;
    }
    g_nt_iters.fetch_add(1);                       // a check that never matches
    { std::lock_guard<std::mutex> lk(a.mu); a.park_state = nlohmann::ordered_json{{"deadline", deadline}}; }
    TurnOutcome o; o.kind = TurnOutcome::PARK; o.timer_park = true;
    o.timeout_s = 0.1; o.await_call_id = "p2"; return o;
}

static void test_poll_until_timeout() {
    fprintf(stderr, "\n== test_poll_until_timeout ==\n");
    g_nt_iters.store(0); g_nt_start.store(0); g_nt_end.store(0); g_nt_timedout.store(false);

    Runtime rt;
    rt.configure("pt", "", 1, Guards{});
    rt.set_turn_fn(poll_timeout_turn);
    rt.start();
    std::string err;
    std::string id = rt.spawn("", "nt", "nt", "sys", "go", {}, err);

    CHECK(wait_until([&]() { return is_status(rt, id, Status::DONE); }, 3000),
          "never-true poll terminated (did not loop forever)");
    CHECK(g_nt_timedout.load(), "it ended via timeout, not a match");
    CHECK(g_nt_end.load() - g_nt_start.load() >= 0.29, "timed out after ~the overall horizon");
    CHECK(g_nt_iters.load() > 0 && g_nt_iters.load() < 20, "bounded number of checks (not a spin)");
    rt.stop();
}

// ===========================================================================
// 4. schedule(): delayed delivery wakes the target; job cap trips
// ===========================================================================
static std::atomic<double> g_sched_woke{0};
static std::atomic<bool>   g_sched_woke_flag{false};

static TurnOutcome sched_turn(Runtime &, Agent & a, std::vector<Message> &) {
    // park awaiting the timer's sender; the scheduled message wakes it
    bool resuming;
    { std::lock_guard<std::mutex> lk(a.mu); resuming = a.await.active; }
    if (resuming) {
        g_sched_woke.store(steady_seconds());
        g_sched_woke_flag.store(true);
        TurnOutcome o; o.kind = TurnOutcome::DONE; o.result = "woken"; return o;
    }
    TurnOutcome o; o.kind = TurnOutcome::PARK;     // AWAIT park: woken by a message from "timer"
    o.wait_ids = {"timer"}; o.await_call_id = "s1"; o.timeout_s = 0;  // no timeout
    return o;
}

static void test_schedule_delayed_wake() {
    fprintf(stderr, "\n== test_schedule_delayed_wake ==\n");
    g_sched_woke.store(0); g_sched_woke_flag.store(false);

    Runtime rt;
    rt.configure("sc", "", 2, Guards{});
    rt.set_turn_fn(sched_turn);
    rt.start();

    std::string err;
    std::string tid = rt.spawn("", "target", "target", "sys", "go", {}, err);
    CHECK(wait_until([&]() { return is_status(rt, tid, Status::WAITING); }, 2000),
          "target parked awaiting a scheduled wake");

    double t0 = steady_seconds();
    std::string jid = rt.schedule_job("timer", "target", "ping", 0.15, 0, 0, err);
    CHECK(!jid.empty(), "schedule_job accepted a delayed delivery");
    CHECK(rt.scheduled_job_count() == 1, "one job is live before it fires");

    CHECK(wait_until([&]() { return g_sched_woke_flag.load(); }, 3000),
          "scheduled message fired and woke the parked target");
    CHECK(g_sched_woke.load() - t0 >= 0.14, "delivery happened after >= the delay");
    CHECK(wait_until([&]() { return rt.scheduled_job_count() == 0; }, 1000),
          "one-shot job de-armed after firing");

    std::string bad;
    CHECK(rt.schedule_job("timer", "nobody", "x", 1, 0, 0, bad).empty(), "schedule to unknown target refused");
    CHECK(bad.find("no agent") != std::string::npos, "unknown-target refusal explains itself");
    rt.stop();
}

static void test_schedule_cap() {
    fprintf(stderr, "\n== test_schedule_cap ==\n");
    Runtime rt;
    Guards g; g.max_scheduled_jobs = 2;
    rt.configure("scap", "", 2, g);
    // no scheduler needed: schedule_job just registers (large delay so none fire)
    std::string err;
    std::string id = rt.spawn("", "t", "t", "sys", "", {}, err);
    CHECK(!rt.schedule_job("u", id, "1", 100, 0, 0, err).empty(), "job 1 ok");
    CHECK(!rt.schedule_job("u", id, "2", 100, 0, 0, err).empty(), "job 2 ok (== cap)");
    CHECK(rt.schedule_job("u", id, "3", 100, 0, 0, err).empty(), "job 3 refused (max scheduled jobs)");
    CHECK(err.find("max scheduled jobs") != std::string::npos, "cap refusal explains itself");
}

int main() {
    test_predicate_language();
    test_wait_no_worker_held();
    test_poll_until_match();
    test_poll_until_timeout();
    test_schedule_delayed_wake();
    test_schedule_cap();

    if (g_fail) { fprintf(stderr, "\n%d CHECK(s) FAILED\n", g_fail); return 1; }
    fprintf(stderr, "\nALL agent_runtime scheduling unit tests passed\n");
    return 0;
}
