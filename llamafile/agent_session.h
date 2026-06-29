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

// llamafile INTERACTIVE MULTI-AGENT RUNTIME (Phase 4) — persisted, pausable and
// resumable SESSIONS. See ddocs/09-interactive-multiagent-runtime.md §5, §9.4.
//
// A SessionManager keeps a registry of sessions keyed by id. Each session is a
// Runtime (Phase 1-3) + metadata { id, goal, created, status, n_agents,
// last_activity, total_tokens }. v1 keeps ONE ACTIVE (decoding) session at a
// time per model/backend (the slot pool is shared); a paused session is just
// disk state under <root>/<id>/.
//
// PAUSE always persists + tears down to disk; RESUME always loads from disk.
// That single property makes "pause -> resume" and "pause -> kill -> restart ->
// resume" run the EXACT same code path, so a resumed session survives a full
// process restart.
//
// Persistence per session dir <root>/<id>/:
//   * agents.json  — { meta, guards, model_fingerprint, runtime:<export_state>,
//                      kv:{<agent>:{file,fingerprint}} }
//   * <agent>.kv   — per-agent llama KV snapshot (best effort; see KvSaveFn)
//   * trace.jsonl  — the Runtime's structured event log (written by Runtime).
//
// KV is an OPTIMIZATION. The conversation (inside agents.json) is ALWAYS
// persisted and is the source of truth. On resume each agent's KV is restored
// only if its fingerprint matches the live model/settings AND the load
// succeeds; otherwise the agent re-prefills from its conversation (correctness
// over speed — mandatory for hybrid/recurrent models like Qwen3.6 whose
// recurrent state may not round-trip through llama_state_seq_save_file).
//
// This header depends only on agent_runtime.h + the C++ stdlib + nlohmann/json,
// so the whole manager is unit-testable with no model (the KV hooks are
// injected; tests pass fakes). The real KV hooks + the HTTP/SSE surface live in
// agent_runtime_server.cpp.

#include "agent_runtime.h"

#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>

namespace agentrt {

// Current runtime KV state-format version. Bump when the snapshot format or the
// KV serialization assumptions change, so stale snapshots fall back cleanly.
constexpr int kRuntimeStateVersion = 1;

// ---------------------------------------------------------------------------
// KvFingerprint — guards a KV restore. Any field differing at resume time means
// the saved KV is ignored and the agent re-prefills from its conversation.
// ---------------------------------------------------------------------------
struct KvFingerprint {
    std::string model_id;             // model alias / path / hash
    int         n_ctx = 0;
    int         n_parallel = 0;
    std::string rope;                 // rope / scaling descriptor
    std::string kv_type;              // kv cache dtype
    int         state_format_version = kRuntimeStateVersion;

    json to_json() const;
    static KvFingerprint from_json(const json & j);
    bool matches(const KvFingerprint & o) const;
};

enum class SessionStatus { RUNNING, PAUSED, DONE };
const char * session_status_name(SessionStatus s);
SessionStatus session_status_from_name(const std::string & s);

// Per-agent KV hooks (injected). save() writes the agent's KV to kv_path and
// fills fp; returning false means "no KV available" -> resume re-prefills.
// load() restores the KV (fingerprint already matched) -> true on success.
using KvSaveFn = std::function<bool(const Agent &, const std::string & kv_path, KvFingerprint & fp)>;
using KvLoadFn = std::function<bool(Agent &, const std::string & kv_path, const KvFingerprint & fp)>;

// Seed callback: spawn the initial agent(s) (e.g. the orchestrator) into a fresh
// Runtime for a new session, given the goal.
using SeedFn = std::function<void(Runtime &, const std::string & goal)>;

struct SessionMeta {
    std::string   id;
    std::string   goal;
    double        created = 0;
    double        last_activity = 0;
    SessionStatus status = SessionStatus::RUNNING;
    int           n_agents = 0;
    long          total_tokens = 0;
    bool          kv_used = false;    // did the last resume use KV (vs re-prefill)?

    json to_json() const;
    static SessionMeta from_json(const json & j);
};

// ---------------------------------------------------------------------------
// SessionManager
// ---------------------------------------------------------------------------
class SessionManager {
  public:
    SessionManager() = default;
    ~SessionManager();

    // Configure once before use. root_dir is created if needed. max_sessions
    // bounds the registry (oldest DONE sessions are evicted past it).
    void configure(const std::string & root_dir, int max_sessions,
                   int n_workers, const Guards & guards,
                   TurnFn turn_fn, const KvFingerprint & model_fp,
                   KvSaveFn kv_save, KvLoadFn kv_load);

    // Create + start a new session; seed() spawns its orchestrator. Any other
    // active session is snapshotted + paused first (one-active rule). Returns
    // the new session id, or "" + err on failure.
    std::string create(const std::string & goal, SeedFn seed, std::string & err);

    json list();                                   // all sessions (memory+disk), meta
    json describe(const std::string & id, bool & found);  // meta + agents

    bool pause(const std::string & id, std::string & err);
    bool resume(const std::string & id, std::string & err);   // load+start from disk
    bool stop(const std::string & id, std::string & err);

    // Live runtime for an ACTIVE session (null otherwise; does not activate).
    Runtime * runtime(const std::string & id);
    // Ensure the session is active (resume if needed) and return its broker.
    EventBroker * broker_for(const std::string & id, std::string & err);

    std::string session_dir(const std::string & id) const;
    std::string trace_path(const std::string & id) const;
    std::string active_id();

  private:
    struct Entry {
        SessionMeta              meta;
        std::unique_ptr<Runtime> rt;   // null unless this session is active
    };

    // all helpers below assume mu_ is held
    void   scan_disk_locked();
    Entry* ensure_entry_locked(const std::string & id);   // load meta from disk
    void   deactivate_others_locked(const std::string & keep);
    void   persist_and_teardown_locked(Entry & e, SessionStatus new_status);
    void   checkpoint_running_locked(Entry & e);          // write disk w/o teardown
    bool   load_active_locked(const std::string & id, std::string & err);
    void   evict_if_needed_locked();
    json   build_blob_locked(Entry & e);                  // agents.json contents
    void   write_blob(const std::string & id, const json & blob) const;
    bool   read_blob(const std::string & id, json & blob) const;

    std::mutex mu_;
    std::map<std::string, std::unique_ptr<Entry>> sessions_;
    std::string active_id_;

    std::string   root_;
    int           max_sessions_ = 16;
    int           n_workers_ = 4;
    Guards        guards_;
    TurnFn        turn_fn_;
    KvFingerprint model_fp_;
    KvSaveFn      kv_save_;
    KvLoadFn      kv_load_;
    bool          configured_ = false;
};

}  // namespace agentrt
