// -*- mode:c++;indent-tabs-mode:nil;c-basic-offset:4;coding:utf-8 -*-
// vi: set et ft=cpp ts=4 sts=4 sw=4 fenc=utf-8 :vi
//
// Copyright 2026 Mozilla.ai - Apache-2.0
//
// Unit tests for the Phase-4 persisted/pausable/resumable SESSIONS:
//   1. Runtime export_state/import_state roundtrip (agents + conversations +
//      metadata + scheduled jobs survive identically).
//   2. SessionManager create/list/pause/resume/stop state transitions, incl.
//      a simulated FULL PROCESS RESTART (new SessionManager, same disk root).
//   3. KV-fingerprint guard: a matching fingerprint restores KV; a TAMPERED
//      fingerprint falls back to conversation re-prefill (kv_load not called),
//      and resume still succeeds (no crash).
//   4. Bounded-sessions eviction (oldest DONE session evicted past the cap).
// Model-free: the LLM turn + the KV hooks are fakes.

#include "agent_runtime.h"
#include "agent_session.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;
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

// A turn fn that parks the agent on a never-arriving await, so a seeded session
// settles into a stable WAITING state (no busy loop, nothing RUNNING) — perfect
// for exercising the session lifecycle without a model.
static TurnOutcome park_turn(Runtime &, Agent &, std::vector<Message> &) {
    TurnOutcome o;
    o.kind          = TurnOutcome::PARK;
    o.wait_ids      = {"__never__"};
    o.await_call_id = "c0";
    return o;
}

static void seed_one(Runtime & rt, const std::string & goal) {
    std::string err;
    rt.spawn("", "orchestrator", "orchestrator", "you are the orchestrator", goal, {}, err);
}

static std::string tmp_root(const char * tag) {
    std::string base = (getenv("TMPDIR") && *getenv("TMPDIR")) ? getenv("TMPDIR") : "/tmp";
    if (!base.empty() && base.back() == '/') base.pop_back();
    std::string root = base + "/llamafile_session_test_" + tag + "_"
                     + std::to_string((long)(now_seconds() * 1000));
    std::error_code ec;
    fs::remove_all(root, ec);
    return root;
}

// Project an export blob to its stable (time-independent) fields for comparison.
static json normalize(const json & blob) {
    json out;
    out["counters"] = blob.value("counters", json::object());
    out["pair_counts"] = blob.value("pair_counts", json::object());
    json agents = json::array();
    for (const auto & a : blob.value("agents", json::array())) {
        json aw = a.value("await", json::object());
        agents.push_back(json{
            {"id", a.value("id", "")}, {"name", a.value("name", "")},
            {"role", a.value("role", "")}, {"system_prompt", a.value("system_prompt", "")},
            {"allow", a.value("allow", json::array())}, {"parent_id", a.value("parent_id", "")},
            {"depth", a.value("depth", 0)}, {"status", a.value("status", "")},
            {"conversation", a.value("conversation", json::array())},
            {"last_result", a.value("last_result", "")},
            {"turns_run", a.value("turns_run", 0)},
            {"tok_prompt", a.value("tok_prompt", 0)}, {"tok_completion", a.value("tok_completion", 0)},
            {"mailbox", a.value("mailbox", json::array())},
            {"await_active", aw.value("active", false)},
            {"await_kind", aw.value("kind", "")},
            {"await_wait_ids", aw.value("wait_ids", json::array())},
        });
    }
    out["agents"] = agents;
    json jobs = json::array();
    for (const auto & j : blob.value("jobs", json::array())) {
        jobs.push_back(json{{"from", j.value("from", "")}, {"to", j.value("to", "")},
                            {"content", j.value("content", "")},
                            {"interval", j.value("interval", 0.0)},
                            {"remaining", j.value("remaining", 0)}});
    }
    out["jobs"] = jobs;
    out["final_answer"] = blob.value("final_answer", "");
    return out;
}

// ---------------------------------------------------------------------------
// 1. export/import roundtrip
// ---------------------------------------------------------------------------
static void test_roundtrip() {
    fprintf(stderr, "\n== test_roundtrip ==\n");
    Runtime rt;
    rt.configure("rt1", "", 2, Guards{});
    std::string err;
    std::string oid = rt.spawn("", "orchestrator", "orch", "sys-prompt", "the-goal",
                               {"wiki_*", "spawn_agent"}, err);
    std::string cid = rt.spawn(oid, "researcher", "rsr", "rsys", "subtask", {}, err);
    CHECK(!oid.empty() && !cid.empty(), "two agents spawned");

    // mutate some state: conversation turns, tokens, a delivered message, a job
    Agent * o = rt.find(oid);
    {
        std::lock_guard<std::mutex> lk(o->mu);
        o->conversation.push_back({{"role", "assistant"}, {"content", "thinking..."}});
        o->turns_run = 3;
    }
    rt.add_usage(*o, 100, 40);
    rt.send("user", oid, "hello orchestrator", err);
    std::string jid = rt.schedule_job(oid, oid, "wake up", 60.0, 0.0, 1, err);
    CHECK(!jid.empty(), "scheduled a job");

    json blob1 = rt.export_state();

    Runtime rt2;
    rt2.configure("rt1b", "", 2, Guards{});
    rt2.import_state(blob1);
    json blob2 = rt2.export_state();

    CHECK(normalize(blob1) == normalize(blob2), "export->import->export is identical (agents/conv/jobs/meta)");
    CHECK(rt2.metrics()["n_agents"] == 2, "imported agent count matches");
    CHECK(rt2.metrics()["total_prompt_tokens"] == 100, "imported token counters match");
    CHECK(rt2.scheduled_job_count() == 1, "imported scheduled job survives");

    Agent * o2 = rt2.find("orch");
    CHECK(o2 != nullptr, "imported agent resolvable by name");
    if (o2) {
        std::lock_guard<std::mutex> lk(o2->mu);
        CHECK(o2->conversation.size() == blob1["agents"][0]["conversation"].size(),
              "conversation preserved verbatim");
        CHECK(o2->mailbox.size() == 1 && o2->mailbox[0].content == "hello orchestrator",
              "mailbox message preserved");
    }
}

// ---------------------------------------------------------------------------
// helpers for SessionManager tests
// ---------------------------------------------------------------------------
static KvFingerprint test_fp() {
    KvFingerprint fp; fp.model_id = "test-model"; fp.n_parallel = 4; return fp;
}
static void configure_sm(SessionManager & sm, const std::string & root, int max_sessions,
                         KvSaveFn save, KvLoadFn load) {
    sm.configure(root, max_sessions, 2, Guards{}, park_turn, test_fp(), save, load);
}
static KvSaveFn no_save() { return [](const Agent &, const std::string &, KvFingerprint &) { return false; }; }
static KvLoadFn no_load() { return [](Agent &, const std::string &, const KvFingerprint &) { return false; }; }

static std::string status_of(SessionManager & sm, const std::string & id) {
    bool found = false;
    json d = sm.describe(id, found);
    return found ? d.value("status", std::string("?")) : std::string("(missing)");
}

// ---------------------------------------------------------------------------
// 2. create / list / pause / resume / stop + restart-from-disk
// ---------------------------------------------------------------------------
static void test_lifecycle() {
    fprintf(stderr, "\n== test_lifecycle ==\n");
    std::string root = tmp_root("life");
    std::string id;
    {
        SessionManager sm;
        configure_sm(sm, root, 16, no_save(), no_load());
        std::string err;
        id = sm.create("find the height of the Eiffel Tower", seed_one, err);
        CHECK(!id.empty(), "create returns a session id");
        sleep_ms(100);  // let the orchestrator's first cycle park

        json l = sm.list();
        CHECK(l.size() == 1, "list shows one session");
        CHECK(status_of(sm, id) == "running", "new session is running");
        CHECK(fs::exists(root + "/" + id + "/agents.json"), "session checkpointed to disk on create");

        std::string e2;
        CHECK(sm.pause(id, e2), "pause ok");
        CHECK(status_of(sm, id) == "paused", "paused after pause()");
        CHECK(sm.runtime(id) == nullptr, "paused session has no live runtime (torn down to disk)");

        CHECK(sm.resume(id, e2), "resume ok");
        CHECK(status_of(sm, id) == "running", "running again after resume()");
        CHECK(sm.runtime(id) != nullptr, "resume re-activated the runtime");
        sleep_ms(80);

        CHECK(sm.pause(id, e2), "pause again ok");  // leave it on disk for the restart test
    }

    // ---- simulate a FULL PROCESS RESTART: brand new SessionManager, same root ----
    {
        SessionManager sm2;
        configure_sm(sm2, root, 16, no_save(), no_load());
        json l = sm2.list();
        CHECK(l.size() == 1, "fresh manager lists the session from disk (survives restart)");
        bool found = false;
        json d = sm2.describe(id, found);
        CHECK(found && d.value("goal", "") == "find the height of the Eiffel Tower",
              "session goal recovered from disk");
        CHECK(d.value("agents", json::array()).size() == 1, "agent recovered from disk before resume");

        std::string err;
        CHECK(sm2.resume(id, err), "resume after restart ok");
        CHECK(sm2.runtime(id) != nullptr, "runtime re-created after restart");
        sleep_ms(80);
        Agent * a = sm2.runtime(id)->find("orchestrator");
        CHECK(a != nullptr, "orchestrator agent restored after restart");

        std::string e2;
        CHECK(sm2.stop(id, e2), "stop ok");
        CHECK(status_of(sm2, id) == "done", "done after stop()");
    }
    std::error_code ec; fs::remove_all(root, ec);
}

// ---------------------------------------------------------------------------
// 3. KV fingerprint match restores; tampered fingerprint falls back
// ---------------------------------------------------------------------------
static void test_kv_fingerprint_fallback() {
    fprintf(stderr, "\n== test_kv_fingerprint_fallback ==\n");

    // a save hook that writes a dummy .kv and stamps the live fingerprint
    auto save = [](const Agent &, const std::string & path, KvFingerprint & fp) {
        FILE * f = fopen(path.c_str(), "wb");
        if (f) { fputs("DUMMYKV", f); fclose(f); }
        fp = test_fp();
        return true;
    };
    std::atomic<int> load_calls{0};
    auto load = [&load_calls](Agent &, const std::string & path, const KvFingerprint &) {
        ++load_calls;
        FILE * f = fopen(path.c_str(), "rb");
        if (!f) return false;
        fclose(f);
        return true;
    };

    // --- (a) matching fingerprint: KV restore is used ---
    {
        std::string root = tmp_root("kvmatch");
        std::string id;
        {
            SessionManager sm; configure_sm(sm, root, 16, save, load);
            std::string err; id = sm.create("goal", seed_one, err); sleep_ms(80);
            sm.pause(id, err);  // writes agents.json + <agent>.kv + fingerprint
        }
        load_calls.store(0);
        {
            SessionManager sm2; configure_sm(sm2, root, 16, save, load);
            std::string err;
            CHECK(sm2.resume(id, err), "resume with matching fingerprint ok");
            CHECK(load_calls.load() >= 1, "KV load hook invoked when fingerprint matches");
            bool found = false; json d = sm2.describe(id, found);
            CHECK(d.value("kv_used", false), "kv_used=true when KV restored");
        }
        std::error_code ec; fs::remove_all(root, ec);
    }

    // --- (b) tampered fingerprint: fall back to re-prefill, kv_load NOT called ---
    {
        std::string root = tmp_root("kvtamper");
        std::string id;
        {
            SessionManager sm; configure_sm(sm, root, 16, save, load);
            std::string err; id = sm.create("goal", seed_one, err); sleep_ms(80);
            sm.pause(id, err);
        }
        // tamper the persisted model fingerprint on disk
        std::string path = root + "/" + id + "/agents.json";
        FILE * f = fopen(path.c_str(), "rb");
        std::string s; char buf[8192]; size_t n;
        while (f && (n = fread(buf, 1, sizeof buf, f)) > 0) s.append(buf, n);
        if (f) fclose(f);
        json blob = json::parse(s, nullptr, false);
        CHECK(!blob.is_discarded(), "read persisted agents.json");
        blob["model_fingerprint"]["model_id"] = "A-DIFFERENT-MODEL";
        f = fopen(path.c_str(), "wb");
        if (f) { std::string o = blob.dump(2); fwrite(o.data(), 1, o.size(), f); fclose(f); }

        load_calls.store(0);
        {
            SessionManager sm2; configure_sm(sm2, root, 16, save, load);
            std::string err;
            CHECK(sm2.resume(id, err), "resume with tampered fingerprint still succeeds (no crash)");
            CHECK(load_calls.load() == 0, "KV load hook NOT called on fingerprint mismatch (falls back)");
            bool found = false; json d = sm2.describe(id, found);
            CHECK(!d.value("kv_used", false), "kv_used=false -> re-prefilled from conversation");
            CHECK(found && d.value("agents", json::array()).size() == 1,
                  "conversation/agents intact after fallback");
        }
        std::error_code ec; fs::remove_all(root, ec);
    }
}

// ---------------------------------------------------------------------------
// 4. bounded-sessions eviction (oldest DONE evicted past the cap)
// ---------------------------------------------------------------------------
static void test_eviction() {
    fprintf(stderr, "\n== test_eviction ==\n");
    std::string root = tmp_root("evict");
    SessionManager sm;
    configure_sm(sm, root, /*max_sessions*/ 2, no_save(), no_load());

    std::string err;
    std::string s1 = sm.create("g1", seed_one, err); sleep_ms(20); sm.stop(s1, err);
    std::string s2 = sm.create("g2", seed_one, err); sleep_ms(20); sm.stop(s2, err);
    std::string s3 = sm.create("g3", seed_one, err); sleep_ms(20);  // triggers eviction

    CHECK(!s1.empty() && !s2.empty() && !s3.empty(), "three sessions created");
    CHECK(!fs::exists(root + "/" + s1), "oldest DONE session (s1) evicted from disk");
    CHECK(fs::exists(root + "/" + s2),  "s2 retained");
    CHECK(fs::exists(root + "/" + s3),  "s3 (active) retained");

    json l = sm.list();
    CHECK(l.size() == 2, "registry bounded to max_sessions after eviction");

    sm.stop(s3, err);
    std::error_code ec; fs::remove_all(root, ec);
}

int main() {
    test_roundtrip();
    test_lifecycle();
    test_kv_fingerprint_fallback();
    test_eviction();

    if (g_fail) { fprintf(stderr, "\n%d CHECK(s) FAILED\n", g_fail); return 1; }
    fprintf(stderr, "\nALL agent_session unit tests passed\n");
    return 0;
}
