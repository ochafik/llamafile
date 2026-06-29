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

// llamafile interactive multi-agent runtime — testable core. See agent_runtime.h.
//
// No httplib / server-tools / server-http dependency here on purpose: the LLM
// turn loop and the HTTP/SSE surface live in agent_runtime_server.cpp and plug
// in via Runtime::set_turn_fn, so this Router/Scheduler/guard machinery is unit
// tested with a fake turn function and no model.

#include "agent_runtime.h"

#include <sys/stat.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdint>
#include <thread>

namespace agentrt {

const char * status_name(Status s) {
    switch (s) {
        case Status::IDLE:     return "idle";
        case Status::RUNNABLE: return "runnable";
        case Status::RUNNING:  return "running";
        case Status::WAITING:  return "waiting";
        case Status::DONE:     return "done";
        case Status::FAILED:   return "failed";
    }
    return "?";
}

Status status_from_name(const std::string & s) {
    if (s == "runnable") return Status::RUNNABLE;
    if (s == "running")  return Status::RUNNING;
    if (s == "waiting")  return Status::WAITING;
    if (s == "done")     return Status::DONE;
    if (s == "failed")   return Status::FAILED;
    return Status::IDLE;
}

double now_seconds() {
    return std::chrono::duration<double>(
               std::chrono::system_clock::now().time_since_epoch()).count();
}
double steady_seconds() {
    return std::chrono::duration<double>(
               std::chrono::steady_clock::now().time_since_epoch()).count();
}

namespace {

std::string make_id(const char * prefix) {
    static std::atomic<uint64_t> counter{0};
    uint64_t a = (uint64_t) std::chrono::steady_clock::now().time_since_epoch().count();
    uint64_t b = counter.fetch_add(1) * 0x9E3779B97F4A7C15ull + 0x1234567ull;
    uint64_t x = a ^ (b << 1) ^ (a >> 7);
    char buf[13];
    for (int i = 11; i >= 0; --i) { buf[i] = "0123456789abcdef"[x & 0xF]; x >>= 4; }
    buf[12] = 0;
    return std::string(prefix) + buf;
}

// cosmocc-safe 8 MiB-stack pthread (HTTP + json call chains overflow the default
// worker stack). Used for scheduler workers + the sweeper.
struct ThreadEntry { std::function<void()> fn; };
void * trampoline(void * arg) {
    auto * e = static_cast<ThreadEntry *>(arg);
    e->fn();
    delete e;
    return nullptr;
}
bool spawn_big_stack(pthread_t & tid, std::function<void()> fn) {
    auto * e = new ThreadEntry{std::move(fn)};
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 8 * 1024 * 1024);
    int rc = pthread_create(&tid, &attr, trampoline, e);
    pthread_attr_destroy(&attr);
    if (rc != 0) { delete e; return false; }
    return true;
}

}  // namespace

// ---------------------------------------------------------------------------
Runtime::Runtime() = default;

Runtime::~Runtime() { stop(); }

void Runtime::configure(const std::string & session_id, const std::string & session_dir,
                        int n_workers, const Guards & guards) {
    session_id_ = session_id;
    guards_     = guards;
    n_workers_  = n_workers > 0 ? n_workers : 1;
    if (!session_dir.empty()) {
        ::mkdir(session_dir.c_str(), 0700);  // best effort; parent assumed to exist
        trace_path_ = session_dir + "/trace.jsonl";
        FILE * f = fopen(trace_path_.c_str(), "ab");  // create/truncate? keep append
        if (f) fclose(f);
    }
}

// ---------------------------------------------------------------------------
// agent lookup
// ---------------------------------------------------------------------------
Agent * Runtime::find_locked(const std::string & key) const {
    auto it = agents_.find(key);
    if (it != agents_.end()) return it->second.get();
    auto ni = name_idx_.find(key);
    if (ni != name_idx_.end()) {
        auto it2 = agents_.find(ni->second);
        if (it2 != agents_.end()) return it2->second.get();
    }
    return nullptr;
}

Agent * Runtime::find(const std::string & key) const {
    std::lock_guard<std::mutex> lk(agents_mu_);
    return find_locked(key);
}

int Runtime::live_count() const {
    int n = 0;
    for (const auto & id : order_) {
        auto it = agents_.find(id);
        if (it == agents_.end()) continue;
        Status s = it->second->status;
        if (s != Status::DONE && s != Status::FAILED) ++n;
    }
    return n;
}

// ---------------------------------------------------------------------------
// scheduler queue
// ---------------------------------------------------------------------------
void Runtime::enqueue(const std::string & id) {
    {
        std::lock_guard<std::mutex> lk(q_mu_);
        runnable_.push_back(id);
    }
    q_cv_.notify_one();
}

// ---------------------------------------------------------------------------
// spawn
// ---------------------------------------------------------------------------
std::string Runtime::spawn(const std::string & parent_id,
                           const std::string & role,
                           const std::string & name,
                           const std::string & system_prompt,
                           const std::string & task,
                           const std::vector<std::string> & allow,
                           std::string & err) {
    int depth = 0;
    {
        std::lock_guard<std::mutex> lk(agents_mu_);
        if (!parent_id.empty()) {
            Agent * p = find_locked(parent_id);
            if (p) depth = p->depth + 1;
        }
        if (depth > guards_.max_depth) {
            err = "spawn refused: max spawn depth " + std::to_string(guards_.max_depth)
                  + " reached";
            return "";
        }
        if (live_count() >= guards_.max_live_agents) {
            err = "spawn refused: max live agents " + std::to_string(guards_.max_live_agents)
                  + " reached";
            return "";
        }
        if (!name.empty() && name_idx_.count(name)) {
            err = "spawn refused: agent name '" + name + "' already exists";
            return "";
        }
    }

    auto a = std::make_unique<Agent>();
    a->id            = make_id("a_");
    a->name          = name.empty() ? a->id : name;
    a->role          = role;
    a->system_prompt = system_prompt;
    a->allow         = allow;
    a->parent_id     = parent_id;
    a->depth         = depth;
    a->status        = Status::RUNNABLE;
    a->enqueued      = true;
    if (!system_prompt.empty())
        a->conversation.push_back({{"role", "system"}, {"content", system_prompt}});
    if (!task.empty())
        a->conversation.push_back({{"role", "user"}, {"content", task}});

    std::string id = a->id;
    {
        std::lock_guard<std::mutex> lk(agents_mu_);
        name_idx_[a->name] = id;
        order_.push_back(id);
        agents_[id] = std::move(a);
    }

    trace(json{
        {"type", "spawn"}, {"agent_id", id}, {"parent_id", parent_id},
        {"name", name.empty() ? id : name}, {"role", role}, {"depth", depth},
        {"task", task.substr(0, 240)},
    });

    if (running_.load()) enqueue(id);
    return id;
}

// ---------------------------------------------------------------------------
// router: deliver a message
// ---------------------------------------------------------------------------
bool Runtime::send(const std::string & from, const std::string & to,
                   const std::string & content, std::string & err) {
    Agent * t = nullptr;
    {
        std::lock_guard<std::mutex> lk(agents_mu_);
        t = find_locked(to);
        if (!t) { err = "send refused: no agent '" + to + "'"; return false; }
        std::string key = from + ">" + t->id;
        int & n = pair_counts_[key];
        if (++n > guards_.max_pair_messages) {
            err = "send refused: message rate cap (" + std::to_string(guards_.max_pair_messages)
                  + ") between " + from + " and " + to + " hit (cycle/storm guard)";
            return false;
        }
    }

    Message m{from, content, now_seconds()};
    bool wake = false;
    {
        std::lock_guard<std::mutex> lk(t->mu);
        if (t->status == Status::WAITING && t->await.active) {
            bool awaited = false;
            for (const auto & w : t->await.wait_ids) if (w == from) { awaited = true; break; }
            if (awaited) {
                t->await.collected[from] = content;
                if (t->await.complete() && !t->enqueued) { t->status = Status::RUNNABLE; t->enqueued = true; wake = true; }
            } else {
                t->mailbox.push_back(m);
            }
        } else {
            t->mailbox.push_back(m);
            if (t->status == Status::IDLE && !t->enqueued) {
                t->status = Status::RUNNABLE; t->enqueued = true; wake = true;
            }
        }
    }

    trace(json{
        {"type", "message"}, {"from", from}, {"to", t->id},
        {"content", content.substr(0, 400)},
    });
    if (wake && running_.load()) enqueue(t->id);
    return true;
}

// ---------------------------------------------------------------------------
// list / metrics
// ---------------------------------------------------------------------------
json Runtime::list() const {
    std::lock_guard<std::mutex> lk(agents_mu_);
    json arr = json::array();
    for (const auto & id : order_) {
        auto it = agents_.find(id);
        if (it == agents_.end()) continue;
        Agent * a = it->second.get();
        arr.push_back(json{
            {"id", a->id}, {"name", a->name}, {"role", a->role},
            {"parent_id", a->parent_id}, {"depth", a->depth},
            {"status", status_name(a->status)}, {"turns", a->turns_run},
            {"tokens_prompt", a->tok_prompt}, {"tokens_completion", a->tok_completion},
        });
    }
    return arr;
}

json Runtime::metrics() const {
    int n_agents, n_turns;
    {
        std::lock_guard<std::mutex> lk(agents_mu_);
        n_agents = (int) order_.size();
    }
    n_turns = global_turns_.load();
    long p = total_prompt_tok_.load(), c = total_completion_tok_.load();
    return json{
        {"session", session_id_},
        {"n_agents", n_agents},
        {"n_turns", n_turns},
        {"total_prompt_tokens", p},
        {"total_completion_tokens", c},
        {"total_tokens", p + c},
    };
}

bool Runtime::charge_turn() {
    int t = global_turns_.fetch_add(1) + 1;
    if (t > guards_.global_turn_budget) return false;
    if (guards_.global_token_budget > 0 &&
        (total_prompt_tok_.load() + total_completion_tok_.load()) > guards_.global_token_budget)
        return false;
    return true;
}

void Runtime::add_usage(Agent & a, long prompt, long completion) {
    a.tok_prompt     += prompt;
    a.tok_completion += completion;
    total_prompt_tok_.fetch_add(prompt);
    total_completion_tok_.fetch_add(completion);
}

void Runtime::set_final_answer(const std::string & agent_id, const std::string & text) {
    {
        std::lock_guard<std::mutex> lk(final_mu_);
        final_answer_ = text;
        final_agent_  = agent_id;
    }
    json m = metrics();
    trace(json{
        {"type", "final"}, {"agent_id", agent_id}, {"answer", text},
        {"metrics", m},
    });
}

std::string Runtime::final_answer() const {
    std::lock_guard<std::mutex> lk(final_mu_);
    return final_answer_;
}

// ---------------------------------------------------------------------------
// trace
// ---------------------------------------------------------------------------
void Runtime::trace(json ev) {
    ev["ts"]      = now_seconds();
    ev["session"] = session_id_;
    std::string line = ev.dump();
    broker_.publish(line);
    if (!trace_path_.empty()) {
        std::lock_guard<std::mutex> lk(trace_file_mu_);
        FILE * f = fopen(trace_path_.c_str(), "ab");
        if (f) { fputs(line.c_str(), f); fputc('\n', f); fclose(f); }
    }
}

// ---------------------------------------------------------------------------
// scheduler
// ---------------------------------------------------------------------------
void Runtime::start() {
    if (running_.exchange(true)) return;
    for (int i = 0; i < n_workers_; ++i) {
        pthread_t tid;
        if (spawn_big_stack(tid, [this]() { worker_loop(); }))
            worker_tids_.push_back(tid);
    }
    if (spawn_big_stack(sweeper_tid_, [this]() { sweeper_loop(); }))
        sweeper_started_ = true;

    // enqueue any agents created before start()
    std::lock_guard<std::mutex> lk(agents_mu_);
    for (const auto & id : order_) {
        auto it = agents_.find(id);
        if (it != agents_.end() && it->second->status == Status::RUNNABLE)
            enqueue(id);
    }
}

void Runtime::stop() {
    if (!running_.exchange(false)) return;
    q_cv_.notify_all();
    for (pthread_t tid : worker_tids_) pthread_join(tid, nullptr);
    worker_tids_.clear();
    if (sweeper_started_) { pthread_join(sweeper_tid_, nullptr); sweeper_started_ = false; }
    broker_.close();
}

void Runtime::worker_loop() {
    for (;;) {
        std::string id;
        {
            std::unique_lock<std::mutex> lk(q_mu_);
            q_cv_.wait(lk, [&]() {
                return !running_.load() || (!paused_.load() && !runnable_.empty());
            });
            if (!running_.load() && runnable_.empty()) return;
            // Paused: hold every worker idle (no new cycles) until resumed/stopped.
            // In-flight cycles already past this point run to completion.
            if (paused_.load() && running_.load()) continue;
            if (runnable_.empty()) continue;
            id = runnable_.front();
            runnable_.pop_front();
        }
        Agent * a = find(id);
        if (a) run_cycle(a);
    }
}

void Runtime::pause_scheduling() {
    paused_.store(true);
    q_cv_.notify_all();
    trace(json{{"type", "session"}, {"event", "pause_scheduling"}});
}

void Runtime::resume_scheduling() {
    paused_.store(false);
    q_cv_.notify_all();
    trace(json{{"type", "session"}, {"event", "resume_scheduling"}});
}

bool Runtime::quiesce(double timeout_s) {
    double deadline = steady_seconds() + (timeout_s > 0 ? timeout_s : 0);
    for (;;) {
        bool any_running = false;
        {
            std::lock_guard<std::mutex> lk(agents_mu_);
            for (const auto & id : order_) {
                auto it = agents_.find(id);
                if (it == agents_.end()) continue;
                std::lock_guard<std::mutex> al(it->second->mu);
                if (it->second->status == Status::RUNNING) { any_running = true; break; }
            }
        }
        if (!any_running) return true;
        if (steady_seconds() >= deadline) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

void Runtime::run_cycle(Agent * a) {
    std::vector<Message> inbox;
    {
        std::lock_guard<std::mutex> lk(a->mu);
        a->enqueued = false;
        if (a->status == Status::DONE || a->status == Status::FAILED) return;
        a->status = Status::RUNNING;
        // hand the turn fn any non-await messages waiting in the mailbox
        while (!a->mailbox.empty()) { inbox.push_back(a->mailbox.front()); a->mailbox.pop_front(); }
    }

    TurnOutcome out;
    if (turn_fn_) {
        out = turn_fn_(*this, *a, inbox);
    } else {
        out.kind = TurnOutcome::FAILED;
        out.result = "(no turn function configured)";
    }

    bool requeue = false;
    {
        std::lock_guard<std::mutex> lk(a->mu);
        switch (out.kind) {
            case TurnOutcome::DONE:
                a->status = Status::DONE;
                a->last_result = out.result;
                break;
            case TurnOutcome::FAILED:
                a->status = Status::FAILED;
                a->last_result = out.result;
                break;
            case TurnOutcome::PARK:
                a->status = Status::WAITING;
                a->await.kind         = out.timer_park ? PendingAwait::Kind::TIMER
                                                       : PendingAwait::Kind::AWAIT;
                a->await.active       = true;
                a->await.tool_call_id = out.await_call_id;
                a->await.wait_ids     = out.wait_ids;
                a->await.collected.clear();
                a->await.timed_out    = false;
                a->await.deadline     = out.timeout_s > 0 ? steady_seconds() + out.timeout_s : 0;
                // sweep already-delivered awaited messages out of the mailbox
                {
                    std::deque<Message> keep;
                    while (!a->mailbox.empty()) {
                        Message m = a->mailbox.front(); a->mailbox.pop_front();
                        bool awaited = false;
                        for (const auto & w : a->await.wait_ids) if (w == m.from) { awaited = true; break; }
                        if (awaited) a->await.collected[m.from] = m.content;
                        else keep.push_back(m);
                    }
                    a->mailbox = std::move(keep);
                }
                if (a->await.complete() && !a->enqueued) {
                    a->status = Status::RUNNABLE; a->enqueued = true; requeue = true;
                }
                break;
        }
    }

    if (out.kind == TurnOutcome::PARK && !out.timer_park) {
        trace(json{{"type", "await"}, {"agent_id", a->id}, {"parent_id", a->parent_id},
                   {"wait_ids", out.wait_ids}, {"timeout", out.timeout_s}});
    }

    if (out.kind == TurnOutcome::DONE || out.kind == TurnOutcome::FAILED) {
        trace(json{
            {"type", out.kind == TurnOutcome::DONE ? "agent_done" : "agent_failed"},
            {"agent_id", a->id}, {"parent_id", a->parent_id},
            {"result", out.result.substr(0, 600)},
            {"tokens_prompt", a->tok_prompt}, {"tokens_completion", a->tok_completion},
        });
        // report the result back to the parent's mailbox (mesh delivery)
        if (!a->parent_id.empty()) {
            std::string err;
            send(a->id, a->parent_id, out.result, err);
        }
    }

    if (requeue && running_.load()) enqueue(a->id);
}

void Runtime::sweeper_loop() {
    using namespace std::chrono;
    while (running_.load()) {
        {
            std::unique_lock<std::mutex> lk(q_mu_);
            // 50 ms granularity keeps timer wakes (wait/poll/schedule + await
            // timeouts) responsive enough for sub-second jobs without busy-spin.
            q_cv_.wait_for(lk, milliseconds(50), [&]() { return !running_.load(); });
        }
        if (!running_.load()) break;
        // Paused: do not fire timers / scheduled jobs (keeps the snapshot stable).
        if (paused_.load()) continue;
        double t = steady_seconds();

        // --- park deadlines: AWAIT timeouts + TIMER (wait/poll) wakeups ---
        std::vector<std::string> wake;
        std::vector<bool>        wake_is_timer;
        {
            std::lock_guard<std::mutex> al(agents_mu_);
            for (const auto & id : order_) {
                auto it = agents_.find(id);
                if (it == agents_.end()) continue;
                Agent * a = it->second.get();
                std::lock_guard<std::mutex> lk(a->mu);
                if (a->status == Status::WAITING && a->await.active &&
                    a->await.deadline > 0 && t >= a->await.deadline && !a->enqueued) {
                    a->await.timed_out = true;
                    a->status = Status::RUNNABLE; a->enqueued = true;
                    wake.push_back(id);
                    wake_is_timer.push_back(a->await.kind == PendingAwait::Kind::TIMER);
                }
            }
        }
        for (size_t i = 0; i < wake.size(); ++i) {
            trace(json{{"type", wake_is_timer[i] ? "timer_fire" : "await_timeout"},
                       {"agent_id", wake[i]}});
            enqueue(wake[i]);
        }

        // --- scheduled jobs (schedule() tool): fire + re-arm repeats ---
        std::vector<ScheduledJob> due;
        {
            std::lock_guard<std::mutex> lk(jobs_mu_);
            for (auto & j : jobs_) {
                if (!j.active || t < j.next_fire) continue;
                due.push_back(j);                  // snapshot pre-mutation
                if (j.interval > 0 && j.remaining > 1) {
                    j.remaining -= 1;
                    j.next_fire  = t + j.interval;
                } else {
                    j.active = false;
                }
            }
        }
        for (const auto & j : due) {
            trace(json{{"type", "timer_fire"}, {"job", j.id}, {"from", j.from},
                       {"to", j.to}, {"repeat", j.interval > 0}});
            std::string e;
            send(j.from, j.to, j.content, e);      // routes + wakes the target
        }
    }
}

// ---------------------------------------------------------------------------
// timer service: schedule() jobs
// ---------------------------------------------------------------------------
std::string Runtime::schedule_job(const std::string & from, const std::string & to,
                                  const std::string & content,
                                  double delay_s, double interval_s, int count,
                                  std::string & err) {
    if (delay_s < 0) delay_s = 0;
    if (delay_s > guards_.max_poll_timeout_s) delay_s = guards_.max_poll_timeout_s;
    if (interval_s < 0) interval_s = 0;
    if (interval_s > 0 && interval_s < 0.001) interval_s = 0.001;  // avoid storms
    if (interval_s > guards_.max_poll_timeout_s) interval_s = guards_.max_poll_timeout_s;

    {
        std::lock_guard<std::mutex> lk(agents_mu_);
        if (!find_locked(to)) { err = "schedule refused: no agent '" + to + "'"; return ""; }
    }

    int remaining = 1;
    if (interval_s > 0) {
        remaining = count > 0 ? count : 1;
        if (remaining > guards_.max_poll_iterations) remaining = guards_.max_poll_iterations;
    }

    ScheduledJob j;
    j.id        = make_id("j_");
    j.from      = from;
    j.to        = to;
    j.content   = content;
    j.next_fire = steady_seconds() + delay_s;
    j.interval  = interval_s;
    j.remaining = remaining;
    j.active    = true;

    std::string id = j.id;
    {
        std::lock_guard<std::mutex> lk(jobs_mu_);
        int active = 0;
        for (const auto & e : jobs_) if (e.active) ++active;
        if (active >= guards_.max_scheduled_jobs) {
            err = "schedule refused: max scheduled jobs ("
                  + std::to_string(guards_.max_scheduled_jobs) + ") reached";
            return "";
        }
        jobs_.push_back(j);
    }

    trace(json{{"type", "schedule"}, {"job", id}, {"from", from}, {"to", to},
               {"delay_s", delay_s}, {"interval_s", interval_s},
               {"count", remaining}, {"content", content.substr(0, 200)}});
    return id;
}

int Runtime::scheduled_job_count() const {
    std::lock_guard<std::mutex> lk(jobs_mu_);
    int n = 0;
    for (const auto & j : jobs_) if (j.active) ++n;
    return n;
}

// ---------------------------------------------------------------------------
// Phase 4: full-state (de)serialization for session persistence
// ---------------------------------------------------------------------------
json Runtime::export_state() const {
    json j;
    j["counters"] = json{
        {"global_turns", global_turns_.load()},
        {"total_prompt_tokens", total_prompt_tok_.load()},
        {"total_completion_tokens", total_completion_tok_.load()},
    };

    json agents = json::array();
    double t = steady_seconds();
    {
        std::lock_guard<std::mutex> lk(agents_mu_);
        for (const auto & id : order_) {
            auto it = agents_.find(id);
            if (it == agents_.end()) continue;
            Agent * a = it->second.get();
            std::lock_guard<std::mutex> al(a->mu);

            json mb = json::array();
            for (const auto & m : a->mailbox)
                mb.push_back(json{{"from", m.from}, {"content", m.content}, {"ts", m.ts}});

            json aw = json::object();
            aw["kind"]         = a->await.kind == PendingAwait::Kind::TIMER ? "timer" : "await";
            aw["active"]       = a->await.active;
            aw["tool_call_id"] = a->await.tool_call_id;
            aw["wait_ids"]     = a->await.wait_ids;
            aw["timed_out"]    = a->await.timed_out;
            json coll = json::object();
            for (const auto & kv : a->await.collected) coll[kv.first] = kv.second;
            aw["collected"]    = coll;
            // store the AWAIT deadline as remaining seconds (steady-clock safe)
            aw["remaining_s"]  = (a->await.deadline > 0) ? std::max(0.0, a->await.deadline - t) : 0.0;

            // park_state may carry an absolute steady "deadline" (poll_until's
            // overall timeout) — rewrite it to remaining seconds for portability.
            json ps = a->park_state;
            if (ps.is_object() && ps.contains("deadline") && ps["deadline"].is_number()) {
                double dl = ps["deadline"].get<double>();
                ps["deadline_remaining_s"] = (dl > 0) ? std::max(0.0, dl - t) : 0.0;
                ps.erase("deadline");
            }

            agents.push_back(json{
                {"id", a->id}, {"name", a->name}, {"role", a->role},
                {"system_prompt", a->system_prompt}, {"allow", a->allow},
                {"parent_id", a->parent_id}, {"depth", a->depth},
                {"status", status_name(a->status)},
                {"conversation", a->conversation},
                {"last_result", a->last_result},
                {"turns_run", a->turns_run},
                {"tok_prompt", a->tok_prompt},
                {"tok_completion", a->tok_completion},
                {"mailbox", mb},
                {"await", aw},
                {"park_state", ps},
            });
        }
    }
    j["agents"] = agents;

    json pc = json::object();
    {
        std::lock_guard<std::mutex> lk(agents_mu_);
        for (const auto & kv : pair_counts_) pc[kv.first] = kv.second;
    }
    j["pair_counts"] = pc;

    json jobs = json::array();
    {
        std::lock_guard<std::mutex> lk(jobs_mu_);
        for (const auto & jb : jobs_) {
            if (!jb.active) continue;
            jobs.push_back(json{
                {"id", jb.id}, {"from", jb.from}, {"to", jb.to},
                {"content", jb.content},
                {"delay_remaining_s", std::max(0.0, jb.next_fire - t)},
                {"interval", jb.interval}, {"remaining", jb.remaining},
            });
        }
    }
    j["jobs"] = jobs;

    {
        std::lock_guard<std::mutex> lk(final_mu_);
        j["final_answer"] = final_answer_;
        j["final_agent"]  = final_agent_;
    }
    return j;
}

void Runtime::import_state(const json & j) {
    double t = steady_seconds();
    std::lock_guard<std::mutex> lk(agents_mu_);
    agents_.clear();
    order_.clear();
    name_idx_.clear();
    pair_counts_.clear();

    if (j.contains("agents") && j["agents"].is_array()) {
        for (const auto & e : j["agents"]) {
            if (!e.is_object()) continue;
            auto a = std::make_unique<Agent>();
            a->id            = e.value("id", make_id("a_"));
            a->name          = e.value("name", a->id);
            a->role          = e.value("role", std::string());
            a->system_prompt = e.value("system_prompt", std::string());
            if (e.contains("allow") && e["allow"].is_array())
                for (const auto & p : e["allow"]) if (p.is_string()) a->allow.push_back(p.get<std::string>());
            a->parent_id     = e.value("parent_id", std::string());
            a->depth         = e.value("depth", 0);
            a->conversation  = e.contains("conversation") ? e["conversation"] : json::array();
            a->last_result   = e.value("last_result", std::string());
            a->turns_run     = e.value("turns_run", 0);
            a->tok_prompt    = e.value("tok_prompt", 0L);
            a->tok_completion= e.value("tok_completion", 0L);

            if (e.contains("mailbox") && e["mailbox"].is_array())
                for (const auto & m : e["mailbox"])
                    a->mailbox.push_back(Message{m.value("from", std::string()),
                                                 m.value("content", std::string()),
                                                 m.value("ts", 0.0)});

            Status s = status_from_name(e.value("status", std::string("idle")));

            if (e.contains("await") && e["await"].is_object()) {
                const json & aw = e["await"];
                a->await.kind         = aw.value("kind", std::string("await")) == "timer"
                                            ? PendingAwait::Kind::TIMER : PendingAwait::Kind::AWAIT;
                a->await.active       = aw.value("active", false);
                a->await.tool_call_id = aw.value("tool_call_id", std::string());
                if (aw.contains("wait_ids") && aw["wait_ids"].is_array())
                    for (const auto & w : aw["wait_ids"]) if (w.is_string()) a->await.wait_ids.push_back(w.get<std::string>());
                if (aw.contains("collected") && aw["collected"].is_object())
                    for (auto it = aw["collected"].begin(); it != aw["collected"].end(); ++it)
                        a->await.collected[it.key()] = it.value().is_string() ? it.value().get<std::string>() : it.value().dump();
                a->await.timed_out = aw.value("timed_out", false);
                double rem = aw.value("remaining_s", 0.0);
                a->await.deadline = rem > 0 ? t + rem : 0.0;
            }

            a->park_state = e.contains("park_state") ? e["park_state"] : json::object();
            // restore poll_until's overall deadline from the portable remaining
            if (a->park_state.is_object() && a->park_state.contains("deadline_remaining_s")) {
                double rem = a->park_state["deadline_remaining_s"].get<double>();
                a->park_state["deadline"] = rem > 0 ? t + rem : 0.0;
                a->park_state.erase("deadline_remaining_s");
            }

            // An interrupted in-flight turn (RUNNING) is re-run from the
            // conversation (the source of truth) on resume.
            if (s == Status::RUNNING) s = Status::RUNNABLE;
            a->status   = s;
            a->enqueued = false;

            std::string id = a->id;
            name_idx_[a->name] = id;
            order_.push_back(id);
            agents_[id] = std::move(a);
        }
    }

    if (j.contains("pair_counts") && j["pair_counts"].is_object())
        for (auto it = j["pair_counts"].begin(); it != j["pair_counts"].end(); ++it)
            pair_counts_[it.key()] = it.value().get<int>();

    if (j.contains("counters") && j["counters"].is_object()) {
        global_turns_.store(j["counters"].value("global_turns", 0));
        total_prompt_tok_.store(j["counters"].value("total_prompt_tokens", 0L));
        total_completion_tok_.store(j["counters"].value("total_completion_tokens", 0L));
    }

    {
        std::lock_guard<std::mutex> lk2(jobs_mu_);
        jobs_.clear();
        if (j.contains("jobs") && j["jobs"].is_array()) {
            for (const auto & e : j["jobs"]) {
                ScheduledJob jb;
                jb.id        = e.value("id", make_id("j_"));
                jb.from      = e.value("from", std::string());
                jb.to        = e.value("to", std::string());
                jb.content   = e.value("content", std::string());
                jb.next_fire = t + e.value("delay_remaining_s", 0.0);
                jb.interval  = e.value("interval", 0.0);
                jb.remaining = e.value("remaining", 1);
                jb.active    = true;
                jobs_.push_back(jb);
            }
        }
    }

    if (j.contains("final_answer") || j.contains("final_agent")) {
        std::lock_guard<std::mutex> lk3(final_mu_);
        final_answer_ = j.value("final_answer", std::string());
        final_agent_  = j.value("final_agent", std::string());
    }
}

}  // namespace agentrt
