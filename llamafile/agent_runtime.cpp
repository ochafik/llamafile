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

#include <chrono>
#include <cstdio>
#include <cstdint>

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
            q_cv_.wait(lk, [&]() { return !running_.load() || !runnable_.empty(); });
            if (!running_.load() && runnable_.empty()) return;
            id = runnable_.front();
            runnable_.pop_front();
        }
        Agent * a = find(id);
        if (a) run_cycle(a);
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

}  // namespace agentrt
