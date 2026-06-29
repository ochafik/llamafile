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

#pragma once

// llamafile INTERACTIVE MULTI-AGENT RUNTIME (Phase 1) — see
// ddocs/09-interactive-multiagent-runtime.md.
//
// A Runtime owns N Agents. Each Agent is { id, name, role/system-prompt,
// tool-allowlist, conversation (messages), mailbox (thread-safe queue), status,
// parent_id }. A Router delivers addressed Messages between agents (and to/from
// the user). A Scheduler is a fixed pool of 8 MiB-stack pthreads sized to the
// server's slot count (n_parallel); it runs an agent's next turn when the agent
// is runnable (a freshly spawned task, a delivered message, or an await that has
// resolved), and idle/parked agents hold NO worker.
//
// This header is the TESTABLE CORE: it depends only on the C++ stdlib, pthreads
// and nlohmann/json (header-only). The actual LLM turn (the loopback HTTP loop
// against /v1/chat/completions + /tools, plus the server_tools registry and the
// /runtime/* HTTP+SSE endpoints) lives in agent_runtime_server.cpp and is wired
// in as a pluggable "turn function", so the Router/Scheduler/guards can be unit
// tested with no model (see tests/agent_runtime_test.cpp).

#include <pthread.h>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <nlohmann/json.hpp>

namespace agentrt {

using json = nlohmann::ordered_json;

// ---------------------------------------------------------------------------
// SSE event broker (mirrors webcam_agent's): a monotonically growing event log
// + a condition variable. Each subscriber tracks its own cursor and only sees
// events published after it connected. Trace events are published here AND
// appended to trace.jsonl.
// ---------------------------------------------------------------------------
struct EventBroker {
    std::mutex               mu;
    std::condition_variable  cv;
    std::vector<std::string> log;
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
    size_t cursor_now() {
        std::lock_guard<std::mutex> lk(mu);
        return log.size();
    }
};

// ---------------------------------------------------------------------------
// Messages + agents
// ---------------------------------------------------------------------------
struct Message {
    std::string from;     // sender agent id, or "user"
    std::string content;
    double      ts = 0;   // wall-clock epoch seconds
};

enum class Status { IDLE, RUNNABLE, RUNNING, WAITING, DONE, FAILED };

const char * status_name(Status s);
Status       status_from_name(const std::string & s);

// A timer-fired delivery created by the schedule() tool. The sweeper fires a job
// when due by routing `content` from `from` to `to` (the same mailbox wake path
// the Router uses); a repeating job (interval > 0) re-arms until `remaining`
// hits 0. This is the runtime's small timer service.
struct ScheduledJob {
    std::string id;
    std::string from;        // creator agent id
    std::string to;          // target agent id/name
    std::string content;     // message body delivered on fire
    double      next_fire = 0;  // steady seconds
    double      interval  = 0;  // 0 = one-shot; > 0 = repeat period
    int         remaining = 1;  // fires left (decremented per fire)
    bool        active    = true;
};

// A parked agent: it yields its worker until it is woken. Two flavours, both
// resumed through the same timer/mailbox wake path (Phase 2 generalizes the
// Phase-1 await into a timer-park so wait/poll_until ride the same machinery):
//   * AWAIT  — wait until every awaited id has reported a message (collected
//              here) or the deadline passes (await tool).
//   * TIMER  — wait for the deadline only; messages do NOT wake it (used by the
//              wait/poll_until scheduling tools). The turn fn keeps its own
//              cross-resume scratch in Agent::park_state.
struct PendingAwait {
    enum class Kind { AWAIT, TIMER };
    Kind                                          kind = Kind::AWAIT;
    bool                                          active = false;
    std::string                                   tool_call_id;  // the call to answer
    std::vector<std::string>                      wait_ids;      // agents to hear from
    std::unordered_map<std::string, std::string>  collected;     // id -> reported content
    double                                        deadline = 0;  // steady seconds; 0 = none
    bool                                          timed_out = false;

    // True only for an AWAIT park whose every wait_id has reported. A TIMER park
    // is never "complete" — it is woken solely by the sweeper at its deadline (so
    // it never busy-re-enqueues itself when wait_ids is empty).
    bool complete() const {
        if (kind != Kind::AWAIT) return false;
        for (const auto & id : wait_ids)
            if (!collected.count(id)) return false;
        return true;
    }
};

struct Agent {
    std::string id;
    std::string name;
    std::string role;            // short label (orchestrator / researcher / ...)
    std::string system_prompt;
    std::vector<std::string> allow;   // tool-allowlist patterns
    std::string parent_id;
    int         depth = 0;

    std::mutex            mu;          // guards mailbox, status, await, conversation
    std::deque<Message>   mailbox;
    Status                status = Status::IDLE;
    PendingAwait          await;
    json                  park_state = json::object();  // turn-fn scratch across a
                                                        // timer park (wait/poll_until)
    json                  conversation = json::array();  // OpenAI messages[]
    std::string           last_result;

    int                   turns_run = 0;
    long                  tok_prompt = 0;      // cumulative this agent
    long                  tok_completion = 0;
    bool                  enqueued = false;    // dedupe: in the runnable queue
};

// What a single scheduled cycle of an agent decided. The turn function runs the
// agent's inner loop (one or more chat turns + tool dispatches) and returns one
// of these so the Scheduler can transition the agent.
struct TurnOutcome {
    enum Kind { DONE, PARK, FAILED } kind = DONE;
    std::string              result;          // DONE/FAILED: terminal text
    std::vector<std::string> wait_ids;        // PARK: agents to await
    std::string              await_call_id;   // PARK: tool_call id to answer
    double                   timeout_s = 0;   // PARK: 0 = no timeout (AWAIT) /
                                              //       the timer interval (TIMER)
    bool                     timer_park = false;  // PARK: TIMER (wait/poll) vs AWAIT
};

class Runtime;
// inbox = messages drained from the mailbox for this cycle (already injected as
// turns by the caller is NOT assumed; the turn fn decides how to use them).
using TurnFn = std::function<TurnOutcome(Runtime &, Agent &, std::vector<Message> & inbox)>;

// ---------------------------------------------------------------------------
// Guard caps (the mesh runaway guards). Refuse past caps; never deadlock.
// ---------------------------------------------------------------------------
struct Guards {
    int  max_live_agents   = 32;   // live (not DONE/FAILED) agents per session
    int  max_depth         = 4;    // spawn depth
    int  max_turns_agent   = 16;   // chat turns per agent
    int  global_turn_budget = 256; // total chat turns across the session
    long global_token_budget = 0;  // 0 = unlimited; else cap total tokens
    int  max_pair_messages = 64;   // a->b message cap (cycle/storm limit)

    // Phase 2 scheduling caps (refuse past these; never deadlock).
    double max_wait_s          = 3600;  // cap on wait(seconds)
    double max_poll_interval_s = 300;   // cap on poll_until interval
    double max_poll_timeout_s  = 3600;  // cap on poll_until / schedule horizon
    int    max_poll_iterations = 1000;  // cap on poll_until tool invocations
    int    max_scheduled_jobs  = 64;    // live schedule() jobs per session
};

// ---------------------------------------------------------------------------
// Runtime — owns agents, the router, the scheduler, the trace + broker.
// ---------------------------------------------------------------------------
class Runtime {
  public:
    Runtime();
    ~Runtime();

    // Configure once. session_dir gets trace.jsonl appended to it (created if
    // needed); empty disables file tracing. n_workers = scheduler pool size.
    void configure(const std::string & session_id, const std::string & session_dir,
                   int n_workers, const Guards & guards);
    void set_turn_fn(TurnFn fn) { turn_fn_ = std::move(fn); }

    void start();   // launch the worker pool (idempotent)
    void stop();    // join workers, close broker

    // Phase 4 — pause/resume scheduling (no teardown). pause_scheduling() stops
    // workers from picking up NEW cycles (and the sweeper from firing) but lets
    // any in-flight cycle finish. quiesce() blocks until no agent is RUNNING (so
    // a snapshot is consistent), up to timeout_s (returns false on timeout).
    // resume_scheduling() re-arms the pool. is_paused() reports the flag.
    void pause_scheduling();
    void resume_scheduling();
    bool quiesce(double timeout_s);
    bool is_paused() const { return paused_.load(); }

    // Phase 4 — full-state serialization for session persistence. export_state()
    // captures every agent (id/name/role/system_prompt/allow/parent/depth/status/
    // conversation/last_result/turn+token counters/mailbox/await+park) + the
    // scheduled jobs + the session counters + the final answer, as one JSON blob.
    // import_state() rebuilds that into a freshly configured (NOT yet started)
    // Runtime; call start() afterwards to re-arm the scheduler. Timer deadlines
    // are stored as *remaining* seconds so they survive a steady-clock reset / a
    // full process restart.
    json export_state() const;
    void import_state(const json & j);

    // Create an agent. Returns its id, or "" on a guard refusal (err set).
    // parent_id "" => a root agent (the orchestrator). The conversation is
    // seeded with [system, user(task)] when task is non-empty.
    std::string spawn(const std::string & parent_id,
                      const std::string & role,
                      const std::string & name,
                      const std::string & system_prompt,
                      const std::string & task,
                      const std::vector<std::string> & allow,
                      std::string & err);

    // Deliver a message to `to` (resolved by id or name). Returns false + err on
    // refusal (unknown target / pair-rate cap). Wakes a parked/idle target.
    bool send(const std::string & from, const std::string & to,
              const std::string & content, std::string & err);

    // list_agents() snapshot.
    json list() const;

    // schedule() — register a timer-fired message delivery. The sweeper routes
    // `content` from `from` to `to` after `delay_s` seconds; if `interval_s > 0`
    // it repeats every interval for `count` fires (count<=0 => 1). Returns the
    // job id, or "" + err on a guard refusal (unknown target / max jobs / caps).
    std::string schedule_job(const std::string & from, const std::string & to,
                             const std::string & content,
                             double delay_s, double interval_s, int count,
                             std::string & err);

    // Count of live (active) scheduled jobs (for diagnostics / tests).
    int scheduled_job_count() const;

    // Per-turn accounting hooks used by the turn fn.
    bool charge_turn();                       // false if the global turn budget is spent
    void add_usage(Agent & a, long prompt, long completion);

    // Trace: publish to SSE + append to trace.jsonl. `ev` is augmented with
    // ts/session here. Use this for every structured event.
    void trace(json ev);

    // Look up an agent (nullptr if absent). Caller must not hold its lock.
    Agent * find(const std::string & id_or_name) const;

    EventBroker & broker() { return broker_; }
    const std::string & session_id() const { return session_id_; }
    const Guards & guards() const { return guards_; }

    // Session token/turn totals (for the eval harness).
    json metrics() const;

    // Mark the run's final answer (orchestrator terminal text). Surfaced as a
    // `final` SSE event + stored for the drive endpoint.
    void set_final_answer(const std::string & agent_id, const std::string & text);
    std::string final_answer() const;

  private:
    void worker_loop();
    void run_cycle(Agent * a);
    void sweeper_loop();                      // await-timeout watcher
    int  live_count() const;

    std::string session_id_;
    std::string trace_path_;
    Guards      guards_;
    TurnFn      turn_fn_;

    mutable std::mutex                                       agents_mu_;
    std::unordered_map<std::string, std::unique_ptr<Agent>>  agents_;
    std::vector<std::string>                                 order_;   // creation order
    std::unordered_map<std::string, std::string>             name_idx_;
    std::map<std::string, int>                               pair_counts_; // "from>to" -> n

    // scheduler
    std::mutex               q_mu_;
    std::condition_variable  q_cv_;
    std::deque<std::string>  runnable_;
    int                      n_workers_ = 4;
    std::vector<pthread_t>   worker_tids_;
    pthread_t                sweeper_tid_ = 0;
    bool                     sweeper_started_ = false;
    std::atomic<bool>        running_{false};
    std::atomic<bool>        paused_{false};

    // timer service (schedule() jobs); fired by the sweeper.
    mutable std::mutex          jobs_mu_;
    std::vector<ScheduledJob>   jobs_;

    std::atomic<int>         global_turns_{0};
    std::atomic<long>        total_prompt_tok_{0};
    std::atomic<long>        total_completion_tok_{0};

    EventBroker              broker_;
    std::mutex               trace_file_mu_;

    mutable std::mutex       final_mu_;
    std::string              final_answer_;
    std::string             final_agent_;

    Agent * find_locked(const std::string & id_or_name) const;
    void    enqueue(const std::string & id);
};

double now_seconds();       // wall-clock epoch seconds
double steady_seconds();    // monotonic seconds

}  // namespace agentrt
