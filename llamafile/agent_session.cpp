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

// SessionManager — persisted, pausable/resumable sessions. See agent_session.h.
// Model-free: the KV hooks are injected, so this whole file is unit-testable.

#include "agent_session.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <system_error>
#include <vector>

namespace fs = std::filesystem;

namespace agentrt {

// ---------------------------------------------------------------------------
// small JSON (de)serializers
// ---------------------------------------------------------------------------
const char * session_status_name(SessionStatus s) {
    switch (s) {
        case SessionStatus::RUNNING: return "running";
        case SessionStatus::PAUSED:  return "paused";
        case SessionStatus::DONE:    return "done";
    }
    return "?";
}
SessionStatus session_status_from_name(const std::string & s) {
    if (s == "paused") return SessionStatus::PAUSED;
    if (s == "done")   return SessionStatus::DONE;
    return SessionStatus::RUNNING;
}

json KvFingerprint::to_json() const {
    return json{
        {"model_id", model_id}, {"n_ctx", n_ctx}, {"n_parallel", n_parallel},
        {"rope", rope}, {"kv_type", kv_type},
        {"state_format_version", state_format_version},
    };
}
KvFingerprint KvFingerprint::from_json(const json & j) {
    KvFingerprint f;
    if (!j.is_object()) return f;
    f.model_id             = j.value("model_id", std::string());
    f.n_ctx                = j.value("n_ctx", 0);
    f.n_parallel           = j.value("n_parallel", 0);
    f.rope                 = j.value("rope", std::string());
    f.kv_type              = j.value("kv_type", std::string());
    f.state_format_version = j.value("state_format_version", 0);
    return f;
}
bool KvFingerprint::matches(const KvFingerprint & o) const {
    return model_id == o.model_id && n_ctx == o.n_ctx && n_parallel == o.n_parallel
        && rope == o.rope && kv_type == o.kv_type
        && state_format_version == o.state_format_version;
}

json SessionMeta::to_json() const {
    return json{
        {"id", id}, {"goal", goal}, {"created", created},
        {"last_activity", last_activity}, {"status", session_status_name(status)},
        {"n_agents", n_agents}, {"total_tokens", total_tokens}, {"kv_used", kv_used},
    };
}
SessionMeta SessionMeta::from_json(const json & j) {
    SessionMeta m;
    if (!j.is_object()) return m;
    m.id            = j.value("id", std::string());
    m.goal          = j.value("goal", std::string());
    m.created       = j.value("created", 0.0);
    m.last_activity = j.value("last_activity", 0.0);
    m.status        = session_status_from_name(j.value("status", std::string("running")));
    m.n_agents      = j.value("n_agents", 0);
    m.total_tokens  = j.value("total_tokens", 0L);
    m.kv_used       = j.value("kv_used", false);
    return m;
}

// ---------------------------------------------------------------------------
SessionManager::~SessionManager() {
    std::lock_guard<std::mutex> lk(mu_);
    for (auto & kv : sessions_) {
        if (kv.second && kv.second->rt) {
            // best-effort persist of any still-active session on shutdown
            persist_and_teardown_locked(*kv.second, kv.second->meta.status);
        }
    }
}

void SessionManager::configure(const std::string & root_dir, int max_sessions,
                               int n_workers, const Guards & guards,
                               TurnFn turn_fn, const KvFingerprint & model_fp,
                               KvSaveFn kv_save, KvLoadFn kv_load) {
    std::lock_guard<std::mutex> lk(mu_);
    root_         = root_dir;
    max_sessions_ = max_sessions > 0 ? max_sessions : 16;
    n_workers_    = n_workers > 0 ? n_workers : 1;
    guards_       = guards;
    turn_fn_      = std::move(turn_fn);
    model_fp_     = model_fp;
    kv_save_      = std::move(kv_save);
    kv_load_      = std::move(kv_load);
    configured_   = true;
    std::error_code ec;
    fs::create_directories(root_, ec);
    scan_disk_locked();
}

std::string SessionManager::session_dir(const std::string & id) const {
    return root_ + "/" + id;
}
std::string SessionManager::trace_path(const std::string & id) const {
    return session_dir(id) + "/trace.jsonl";
}
std::string SessionManager::active_id() {
    std::lock_guard<std::mutex> lk(mu_);
    return active_id_;
}

// ---------------------------------------------------------------------------
// disk helpers
// ---------------------------------------------------------------------------
void SessionManager::write_blob(const std::string & id, const json & blob) const {
    std::error_code ec;
    fs::create_directories(session_dir(id), ec);
    std::string path = session_dir(id) + "/agents.json";
    std::string tmp  = path + ".tmp";
    FILE * f = fopen(tmp.c_str(), "wb");
    if (!f) return;
    std::string s = blob.dump(2);
    fwrite(s.data(), 1, s.size(), f);
    fclose(f);
    rename(tmp.c_str(), path.c_str());   // atomic replace
}

bool SessionManager::read_blob(const std::string & id, json & blob) const {
    std::string path = session_dir(id) + "/agents.json";
    FILE * f = fopen(path.c_str(), "rb");
    if (!f) return false;
    std::string s;
    char buf[8192]; size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) s.append(buf, n);
    fclose(f);
    blob = json::parse(s, nullptr, false);
    return !blob.is_discarded();
}

void SessionManager::scan_disk_locked() {
    std::error_code ec;
    if (!fs::exists(root_, ec)) return;
    for (auto & de : fs::directory_iterator(root_, ec)) {
        if (ec) break;
        if (!de.is_directory()) continue;
        std::string id = de.path().filename().string();
        if (sessions_.count(id)) continue;   // already known (memory authoritative)
        json blob;
        if (!read_blob(id, blob)) continue;
        auto e = std::make_unique<Entry>();
        e->meta = SessionMeta::from_json(blob.value("meta", json::object()));
        if (e->meta.id.empty()) e->meta.id = id;
        sessions_[id] = std::move(e);
    }
}

SessionManager::Entry * SessionManager::ensure_entry_locked(const std::string & id) {
    auto it = sessions_.find(id);
    if (it != sessions_.end()) return it->second.get();
    json blob;
    if (!read_blob(id, blob)) return nullptr;
    auto e = std::make_unique<Entry>();
    e->meta = SessionMeta::from_json(blob.value("meta", json::object()));
    if (e->meta.id.empty()) e->meta.id = id;
    Entry * raw = e.get();
    sessions_[id] = std::move(e);
    return raw;
}

// ---------------------------------------------------------------------------
// blob construction + persistence
// ---------------------------------------------------------------------------
json SessionManager::build_blob_locked(Entry & e) {
    json blob;
    // refresh live meta from the running Runtime
    if (e.rt) {
        json m = e.rt->metrics();
        e.meta.n_agents     = m.value("n_agents", e.meta.n_agents);
        e.meta.total_tokens = m.value("total_tokens", e.meta.total_tokens);
        e.meta.last_activity = now_seconds();
    }
    blob["meta"]              = e.meta.to_json();
    blob["model_fingerprint"] = model_fp_.to_json();
    blob["guards"]            = json{
        {"max_live_agents", guards_.max_live_agents},
        {"max_depth", guards_.max_depth},
        {"max_turns_agent", guards_.max_turns_agent},
        {"global_turn_budget", guards_.global_turn_budget},
        {"global_token_budget", guards_.global_token_budget},
        {"max_pair_messages", guards_.max_pair_messages},
    };

    json kv = json::object();
    if (e.rt) {
        blob["runtime"] = e.rt->export_state();
        // attempt a per-agent KV snapshot (best effort; injected hook)
        if (kv_save_ && blob["runtime"].contains("agents")) {
            for (const auto & ae : blob["runtime"]["agents"]) {
                std::string aid = ae.value("id", std::string());
                if (aid.empty()) continue;
                Agent * a = e.rt->find(aid);
                if (!a) continue;
                std::string file = aid + ".kv";
                KvFingerprint fp;
                if (kv_save_(*a, session_dir(e.meta.id) + "/" + file, fp))
                    kv[aid] = json{{"file", file}, {"fingerprint", fp.to_json()}};
            }
        }
    } else {
        blob["runtime"] = json::object();
    }
    blob["kv"] = kv;
    return blob;
}

void SessionManager::checkpoint_running_locked(Entry & e) {
    if (!e.rt) return;
    json blob = build_blob_locked(e);
    write_blob(e.meta.id, blob);
}

void SessionManager::persist_and_teardown_locked(Entry & e, SessionStatus new_status) {
    if (e.rt) {
        e.rt->pause_scheduling();
        e.rt->quiesce(5.0);                 // let in-flight turns finish
        e.meta.status = new_status;
        json blob = build_blob_locked(e);   // snapshot AFTER quiesce (consistent)
        write_blob(e.meta.id, blob);
        e.rt->stop();
        e.rt.reset();
    } else {
        e.meta.status = new_status;
    }
    if (active_id_ == e.meta.id) active_id_.clear();
}

void SessionManager::deactivate_others_locked(const std::string & keep) {
    for (auto & kv : sessions_) {
        if (kv.first == keep) continue;
        if (kv.second && kv.second->rt) {
            // one-active rule: snapshot + pause any other decoding session
            persist_and_teardown_locked(*kv.second, SessionStatus::PAUSED);
        }
    }
}

void SessionManager::evict_if_needed_locked() {
    if ((int) sessions_.size() <= max_sessions_) return;
    // evict oldest DONE sessions first (never a running/paused one)
    std::vector<Entry *> done;
    for (auto & kv : sessions_)
        if (kv.second && kv.second->meta.status == SessionStatus::DONE && !kv.second->rt)
            done.push_back(kv.second.get());
    std::sort(done.begin(), done.end(),
              [](Entry * a, Entry * b) { return a->meta.created < b->meta.created; });
    for (Entry * e : done) {
        if ((int) sessions_.size() <= max_sessions_) break;
        std::string id = e->meta.id;
        std::error_code ec;
        fs::remove_all(session_dir(id), ec);
        sessions_.erase(id);
    }
}

// ---------------------------------------------------------------------------
// create
// ---------------------------------------------------------------------------
std::string SessionManager::create(const std::string & goal, SeedFn seed, std::string & err) {
    std::lock_guard<std::mutex> lk(mu_);
    if (!configured_) { err = "session manager not configured"; return ""; }

    static std::atomic<uint64_t> seq{0};
    std::string id = "s_" + std::to_string((long) (now_seconds() * 1000)) + "_"
                   + std::to_string(seq.fetch_add(1));

    deactivate_others_locked("");   // enforce one-active

    auto e = std::make_unique<Entry>();
    e->meta.id            = id;
    e->meta.goal          = goal;
    e->meta.created       = now_seconds();
    e->meta.last_activity = e->meta.created;
    e->meta.status        = SessionStatus::RUNNING;

    std::error_code ec;
    fs::create_directories(session_dir(id), ec);

    e->rt = std::make_unique<Runtime>();
    e->rt->configure(id, session_dir(id), n_workers_, guards_);
    e->rt->set_turn_fn(turn_fn_);
    e->rt->start();
    if (seed) seed(*e->rt, goal);

    checkpoint_running_locked(*e);   // disk copy so it survives a crash too

    active_id_ = id;
    sessions_[id] = std::move(e);
    evict_if_needed_locked();
    return id;
}

// ---------------------------------------------------------------------------
// load (resume) from disk into an active Runtime
// ---------------------------------------------------------------------------
bool SessionManager::load_active_locked(const std::string & id, std::string & err) {
    json blob;
    if (!read_blob(id, blob)) { err = "no session '" + id + "' on disk"; return false; }

    Entry * e = ensure_entry_locked(id);
    if (!e) { err = "cannot load session '" + id + "'"; return false; }
    if (e->rt) return true;   // already active

    e->rt = std::make_unique<Runtime>();
    e->rt->configure(id, session_dir(id), n_workers_, guards_);
    e->rt->set_turn_fn(turn_fn_);
    if (blob.contains("runtime") && blob["runtime"].is_object())
        e->rt->import_state(blob["runtime"]);

    // KV restore (fingerprint-gated; fall back to conversation re-prefill).
    bool any_kv = false;
    KvFingerprint global_fp = KvFingerprint::from_json(blob.value("model_fingerprint", json::object()));
    bool global_ok = global_fp.matches(model_fp_);
    if (kv_load_ && global_ok && blob.contains("kv") && blob["kv"].is_object()) {
        for (auto it = blob["kv"].begin(); it != blob["kv"].end(); ++it) {
            const std::string & aid = it.key();
            const json & ke = it.value();
            if (!ke.is_object() || !ke.contains("file")) continue;
            KvFingerprint afp = KvFingerprint::from_json(ke.value("fingerprint", json::object()));
            if (!afp.matches(model_fp_)) continue;        // mismatch -> re-prefill
            Agent * a = e->rt->find(aid);
            if (!a) continue;
            std::string path = session_dir(id) + "/" + ke.value("file", std::string());
            if (kv_load_(*a, path, afp)) any_kv = true;    // else falls back
        }
    }
    e->meta.kv_used = any_kv;
    e->meta.status  = SessionStatus::RUNNING;
    e->meta.last_activity = now_seconds();

    e->rt->start();   // re-arm scheduler/timer; RUNNABLE agents re-enqueued
    e->rt->trace(json{{"type", "session"}, {"event", "resume"},
                      {"kv_used", any_kv}, {"model_fingerprint_match", global_ok}});
    active_id_ = id;
    return true;
}

// ---------------------------------------------------------------------------
// public state transitions
// ---------------------------------------------------------------------------
bool SessionManager::pause(const std::string & id, std::string & err) {
    std::lock_guard<std::mutex> lk(mu_);
    Entry * e = ensure_entry_locked(id);
    if (!e) { err = "no session '" + id + "'"; return false; }
    if (!e->rt) {                       // already on disk (paused/done)
        if (e->meta.status == SessionStatus::RUNNING) e->meta.status = SessionStatus::PAUSED;
        return true;
    }
    persist_and_teardown_locked(*e, SessionStatus::PAUSED);
    return true;
}

bool SessionManager::resume(const std::string & id, std::string & err) {
    std::lock_guard<std::mutex> lk(mu_);
    Entry * e = ensure_entry_locked(id);
    if (!e) { err = "no session '" + id + "'"; return false; }
    if (e->meta.status == SessionStatus::DONE) { err = "session '" + id + "' is done"; return false; }
    if (e->rt) return true;             // already active
    deactivate_others_locked(id);       // enforce one-active
    return load_active_locked(id, err);
}

bool SessionManager::stop(const std::string & id, std::string & err) {
    std::lock_guard<std::mutex> lk(mu_);
    Entry * e = ensure_entry_locked(id);
    if (!e) { err = "no session '" + id + "'"; return false; }
    persist_and_teardown_locked(*e, SessionStatus::DONE);
    return true;
}

Runtime * SessionManager::runtime(const std::string & id) {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = sessions_.find(id);
    if (it == sessions_.end() || !it->second) return nullptr;
    return it->second->rt.get();
}

EventBroker * SessionManager::broker_for(const std::string & id, std::string & err) {
    std::lock_guard<std::mutex> lk(mu_);
    Entry * e = ensure_entry_locked(id);
    if (!e) { err = "no session '" + id + "'"; return nullptr; }
    if (!e->rt) {
        if (e->meta.status == SessionStatus::DONE) { err = "session '" + id + "' is done"; return nullptr; }
        deactivate_others_locked(id);
        if (!load_active_locked(id, err)) return nullptr;
    }
    return &e->rt->broker();
}

// ---------------------------------------------------------------------------
// queries
// ---------------------------------------------------------------------------
json SessionManager::list() {
    std::lock_guard<std::mutex> lk(mu_);
    scan_disk_locked();
    json arr = json::array();
    for (auto & kv : sessions_) {
        Entry & e = *kv.second;
        if (e.rt) {                     // refresh live meta
            json m = e.rt->metrics();
            e.meta.n_agents     = m.value("n_agents", e.meta.n_agents);
            e.meta.total_tokens = m.value("total_tokens", e.meta.total_tokens);
        }
        json mj = e.meta.to_json();
        mj["active"] = (e.rt != nullptr);
        arr.push_back(mj);
    }
    return arr;
}

json SessionManager::describe(const std::string & id, bool & found) {
    std::lock_guard<std::mutex> lk(mu_);
    Entry * e = ensure_entry_locked(id);
    if (!e) { found = false; return json::object(); }
    found = true;
    json out;
    if (e->rt) {
        json m = e->rt->metrics();
        e->meta.n_agents     = m.value("n_agents", e->meta.n_agents);
        e->meta.total_tokens = m.value("total_tokens", e->meta.total_tokens);
        out = e->meta.to_json();
        out["active"]       = true;
        out["agents"]       = e->rt->list();
        out["final_answer"] = e->rt->final_answer();
        out["metrics"]      = m;
    } else {
        json blob;
        read_blob(id, blob);
        out = e->meta.to_json();
        out["active"] = false;
        json agents = json::array();
        if (blob.contains("runtime") && blob["runtime"].is_object() &&
            blob["runtime"].contains("agents")) {
            for (const auto & ae : blob["runtime"]["agents"]) {
                agents.push_back(json{
                    {"id", ae.value("id", "")}, {"name", ae.value("name", "")},
                    {"role", ae.value("role", "")}, {"parent_id", ae.value("parent_id", "")},
                    {"depth", ae.value("depth", 0)}, {"status", ae.value("status", "")},
                    {"turns", ae.value("turns_run", 0)},
                    {"tokens_prompt", ae.value("tok_prompt", 0)},
                    {"tokens_completion", ae.value("tok_completion", 0)},
                });
            }
        }
        out["agents"] = agents;
        if (blob.contains("runtime") && blob["runtime"].is_object())
            out["final_answer"] = blob["runtime"].value("final_answer", "");
    }
    return out;
}

}  // namespace agentrt
