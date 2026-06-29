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

// A tiny, deterministic, JS-less PREDICATE language for poll_until (ddoc 09 §7).
// It tests a tool's result string against a small spec object so the runtime can
// decide when a poll is satisfied — no scripting engine, fully unit-testable.
//
// Predicate forms (a JSON object; the first matching key wins):
//   {"contains": "x"}                       substring of the (subject) string
//   {"regex": "re"}                         std::regex_search; value = match[0]
//   {"equals": "x"}                         whole subject == x  (typed if pathed)
//   {"json_path": ".a.b"}                   subject := that node; matches if it
//                                           exists (non-null) when no comparator
//   {"json_path": ".a.b", "equals": v}      navigate, then equals
//   {"json_path": ".a.b", "contains": "x"}  navigate, then substring
//   {"json_path": ".a.b", "regex": "re"}    navigate, then regex
// A non-object / empty predicate matches any NON-EMPTY result (a useful default).
//
// When "json_path" is present the result is parsed as JSON and the addressed node
// becomes the "subject" (its string form for contains/regex, typed for equals);
// otherwise the subject is the raw result string. A path like ".a.b.0.c" walks
// objects by key and arrays by integer index (leading '.' optional).

#include <cstdlib>
#include <regex>
#include <string>

#include <nlohmann/json.hpp>

namespace agentrt {

using pred_json = nlohmann::ordered_json;

// Walk a dotted path into `root`. Returns the addressed node, or null if absent.
inline pred_json json_path_get(const pred_json & root, const std::string & path) {
    const pred_json * cur = &root;
    size_t i = (!path.empty() && path[0] == '.') ? 1 : 0;
    std::string token;
    auto step = [&](const std::string & key) -> bool {
        if (cur->is_object()) {
            auto it = cur->find(key);
            if (it == cur->end()) return false;
            cur = &(*it);
            return true;
        }
        if (cur->is_array()) {
            char * end = nullptr;
            long idx = std::strtol(key.c_str(), &end, 10);
            if (end && *end == 0 && idx >= 0 && (size_t) idx < cur->size()) {
                cur = &(*cur)[(size_t) idx];
                return true;
            }
        }
        return false;
    };
    for (; ; ++i) {
        if (i == path.size() || path[i] == '.') {
            if (!token.empty()) {
                if (!step(token)) return pred_json();  // null
                token.clear();
            }
            if (i == path.size()) break;
        } else {
            token += path[i];
        }
    }
    return *cur;
}

struct PredResult {
    bool        matched = false;
    std::string value;     // the matched value (subject or regex match[0])
};

inline PredResult predicate_match(const pred_json & pred, const std::string & result) {
    PredResult r;

    // Resolve the subject (raw result, or a json_path node).
    std::string subject = result;
    pred_json   node;
    bool        have_path = pred.is_object() && pred.contains("json_path") &&
                            pred["json_path"].is_string();
    if (have_path) {
        pred_json parsed = pred_json::parse(result, nullptr, false);
        if (parsed.is_discarded()) return r;  // not JSON -> no match
        node    = json_path_get(parsed, pred["json_path"].get<std::string>());
        subject = node.is_string() ? node.get<std::string>() : node.dump();
    }

    if (!pred.is_object()) {              // default: any non-empty result
        r.matched = !result.empty();
        r.value   = result;
        return r;
    }

    if (pred.contains("equals")) {
        const pred_json & want = pred["equals"];
        if (have_path) {
            r.matched = want.is_string() ? (subject == want.get<std::string>())
                                         : (node == want);
        } else {
            std::string w = want.is_string() ? want.get<std::string>() : want.dump();
            r.matched = (subject == w);
        }
        if (r.matched) r.value = subject;
        return r;
    }
    if (pred.contains("contains") && pred["contains"].is_string()) {
        r.matched = subject.find(pred["contains"].get<std::string>()) != std::string::npos;
        if (r.matched) r.value = subject;
        return r;
    }
    if (pred.contains("regex") && pred["regex"].is_string()) {
        try {
            std::regex  re(pred["regex"].get<std::string>());
            std::smatch m;
            if (std::regex_search(subject, m, re)) { r.matched = true; r.value = m.str(0); }
        } catch (...) { r.matched = false; }
        return r;
    }
    if (have_path) {                      // path, no comparator: exists?
        r.matched = !node.is_null();
        r.value   = subject;
        return r;
    }

    r.matched = !result.empty();          // empty {} predicate -> any non-empty
    r.value   = result;
    return r;
}

}  // namespace agentrt
