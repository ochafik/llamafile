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

#include "wikidata.h"

#include "sqlite/sqlite3.h"

#include <cctype>
#include <cstring>
#include <string>
#include <unordered_map>

#include <nlohmann/json.hpp>

using json = nlohmann::json;

// ---------------------------------------------------------------------------
// Error reporting
// ---------------------------------------------------------------------------
namespace {
thread_local std::string g_wd_error;
}

const char * wikidata_error() {
    return g_wd_error.c_str();
}

// ---------------------------------------------------------------------------
// Store
// ---------------------------------------------------------------------------
struct wikidata_store {
    sqlite3 * db = nullptr;
    sqlite3_stmt * st_get = nullptr;    // SELECT label,description,aliases,claims WHERE id=?
    sqlite3_stmt * st_label = nullptr;  // SELECT label WHERE id=?
    sqlite3_stmt * st_search = nullptr; // FTS5 join
    // small cache so resolving P/Q ids inside one entity doesn't re-query.
    std::unordered_map<std::string, std::string> label_cache;
};

namespace {

// Is this string a bare Wikidata entity id (Qnnn / Pnnn / Lnnn)?
bool is_entity_id(const std::string & s) {
    if (s.size() < 2) return false;
    char c = s[0];
    if (c != 'Q' && c != 'P' && c != 'L') return false;
    for (size_t i = 1; i < s.size(); i++) {
        if (!isdigit((unsigned char) s[i])) return false;
    }
    return true;
}

// Resolve a Qid/Pid to its label (empty if unknown). Cached per store.
std::string resolve_label(wikidata_store * s, const std::string & id) {
    if (id.empty()) return std::string();
    auto it = s->label_cache.find(id);
    if (it != s->label_cache.end()) return it->second;
    std::string out;
    if (s->st_label) {
        sqlite3_reset(s->st_label);
        sqlite3_clear_bindings(s->st_label);
        sqlite3_bind_text(s->st_label, 1, id.c_str(), -1, SQLITE_TRANSIENT);
        if (sqlite3_step(s->st_label) == SQLITE_ROW) {
            const unsigned char * t = sqlite3_column_text(s->st_label, 0);
            if (t) out = (const char *) t;
        }
        sqlite3_reset(s->st_label);
    }
    s->label_cache.emplace(id, out);
    return out;
}

// Render a time snak ("+2022-03-15T00:00:00Z", "+1950-00-00T...") to a clean,
// precision-aware date ("2022-03-15", "1950", "1889-03").
std::string format_time(const std::string & raw) {
    std::string r = raw;
    bool bce = false;
    if (!r.empty() && (r[0] == '+' || r[0] == '-')) {
        bce = (r[0] == '-');
        r = r.substr(1);
    }
    // Expect YYYY-MM-DD at the head.
    size_t t = r.find('T');
    std::string date = (t == std::string::npos) ? r : r.substr(0, t);
    // Split YYYY-MM-DD.
    std::string y, mo, d;
    size_t p1 = date.find('-');
    if (p1 != std::string::npos) {
        y = date.substr(0, p1);
        size_t p2 = date.find('-', p1 + 1);
        if (p2 != std::string::npos) {
            mo = date.substr(p1 + 1, p2 - p1 - 1);
            d = date.substr(p2 + 1);
        } else {
            mo = date.substr(p1 + 1);
        }
    } else {
        y = date;
    }
    std::string out = y;
    if (!mo.empty() && mo != "00") {
        out += "-" + mo;
        if (!d.empty() && d != "00") out += "-" + d;
    }
    if (bce) out += " BCE";
    return out.empty() ? raw : out;
}

// Decode one describe_snak() string into a typed value, resolving entity/unit
// ids to labels via the store.
wikidata_value decode_value(wikidata_store * s, const std::string & raw) {
    wikidata_value v;
    if (raw.empty()) {
        v.type = "other";
        return v;
    }
    char c0 = raw[0];

    // quoted string / monolingual text -> strip surrounding quotes.
    if (c0 == '"') {
        v.type = "string";
        std::string inner = raw;
        if (inner.size() >= 2 && inner.back() == '"') {
            inner = inner.substr(1, inner.size() - 2);
        } else {
            inner = inner.substr(1);
        }
        v.text = inner;
        return v;
    }

    // external-id (ETL usually drops these, but be safe): "#value\""
    if (c0 == '#') {
        v.type = "string";
        std::string inner = raw.substr(1);
        if (!inner.empty() && inner.back() == '"') inner.pop_back();
        v.text = inner;
        return v;
    }

    // coordinate "@ lat, lon"
    if (c0 == '@') {
        v.type = "coordinate";
        size_t b = raw.find_first_not_of(" ", 1);
        v.text = (b == std::string::npos) ? "" : raw.substr(b);
        return v;
    }

    // bare entity id
    if (is_entity_id(raw)) {
        v.type = "entity";
        v.id = raw;
        std::string lbl = resolve_label(s, raw);
        v.text = lbl.empty() ? raw : lbl;
        return v;
    }

    // quantity or time (lead with +/-)
    if (c0 == '+' || c0 == '-') {
        // time: +YYYY-MM-DDT...
        // heuristic: has a 'T' and dashes, no " x " unit marker
        size_t xpos = raw.find(" x ");
        bool looks_time = (raw.find('T') != std::string::npos &&
                           raw.find('-', 1) != std::string::npos &&
                           xpos == std::string::npos);
        if (looks_time) {
            v.type = "time";
            v.text = format_time(raw);
            return v;
        }
        // quantity: "+AMOUNT" or "+AMOUNT x <unit-uri>"
        v.type = "quantity";
        std::string amount = raw;
        std::string unit;
        if (xpos != std::string::npos) {
            amount = raw.substr(0, xpos);
            std::string uri = raw.substr(xpos + 3);
            // unit is "http://www.wikidata.org/entity/Qnnnn" -> take basename id
            size_t slash = uri.find_last_of('/');
            std::string uid = (slash == std::string::npos) ? uri : uri.substr(slash + 1);
            if (is_entity_id(uid)) {
                std::string lbl = resolve_label(s, uid);
                unit = lbl.empty() ? uid : lbl;
            } else {
                unit = uri;
            }
        }
        // strip a leading '+'
        if (!amount.empty() && amount[0] == '+') amount = amount.substr(1);
        v.text = amount;
        if (!unit.empty()) v.text += " " + unit;
        return v;
    }

    // anything else: pass through.
    v.type = "other";
    v.text = raw;
    return v;
}

// Parse the claims JSON of an entity into decoded statements.
void decode_claims(wikidata_store * s, const std::string & claims_json,
                   std::vector<wikidata_statement> & out) {
    if (claims_json.empty()) return;
    json c;
    try {
        c = json::parse(claims_json);
    } catch (...) {
        return;
    }
    if (!c.is_array()) return;
    for (const auto & stmt : c) {
        if (!stmt.is_array() || stmt.empty()) continue;
        // Each pair is ["Pxxx","value"].
        auto read_pair = [](const json & pair, std::string & pid, std::string & val) -> bool {
            if (!pair.is_array() || pair.size() < 2) return false;
            if (!pair[0].is_string()) return false;
            pid = pair[0].get<std::string>();
            if (pair[1].is_string()) {
                val = pair[1].get<std::string>();
            } else {
                val = pair[1].dump();
            }
            return true;
        };
        std::string pid, raw;
        if (!read_pair(stmt[0], pid, raw)) continue;
        wikidata_statement ws;
        ws.pid = pid;
        ws.prop_label = resolve_label(s, pid);
        ws.value = decode_value(s, raw);
        for (size_t i = 1; i < stmt.size(); i++) {
            std::string qpid, qraw;
            if (!read_pair(stmt[i], qpid, qraw)) continue;
            wikidata_qualifier q;
            q.pid = qpid;
            q.prop_label = resolve_label(s, qpid);
            q.value = decode_value(s, qraw);
            ws.qualifiers.push_back(std::move(q));
        }
        out.push_back(std::move(ws));
    }
}

// Parse the aliases JSON (array of strings) into a vector.
void parse_aliases(const std::string & aliases_json, std::vector<std::string> & out) {
    if (aliases_json.empty()) return;
    json a;
    try {
        a = json::parse(aliases_json);
    } catch (...) {
        return;
    }
    if (!a.is_array()) return;
    for (const auto & x : a) {
        if (x.is_string()) out.push_back(x.get<std::string>());
    }
}

// Does the query look like an explicit FTS5 expression we should pass verbatim?
// (boolean operators, phrases, prefixes, columns, parens). Otherwise we treat it
// as plain words and quote each token (implicit AND) so arbitrary punctuation is
// safe.
bool looks_like_fts_expr(const std::string & q) {
    if (q.find('"') != std::string::npos) return true;
    if (q.find('*') != std::string::npos) return true;
    if (q.find('(') != std::string::npos) return true;
    if (q.find(':') != std::string::npos) return true;
    if (q.find('^') != std::string::npos) return true;
    // standalone uppercase operators
    static const char * ops[] = { "OR", "AND", "NOT", "NEAR" };
    size_t i = 0, n = q.size();
    while (i < n) {
        while (i < n && isspace((unsigned char) q[i])) i++;
        size_t j = i;
        while (j < n && !isspace((unsigned char) q[j])) j++;
        std::string tok = q.substr(i, j - i);
        for (const char * op : ops) {
            if (tok == op) return true;
        }
        i = j;
    }
    return false;
}

// Build a safe FTS5 MATCH string: quote each whitespace token (doubling any
// embedded double-quotes), join with spaces (implicit AND). Drops tokens with no
// indexable characters.
std::string quote_terms(const std::string & q) {
    std::string out;
    size_t i = 0, n = q.size();
    while (i < n) {
        while (i < n && isspace((unsigned char) q[i])) i++;
        size_t j = i;
        while (j < n && !isspace((unsigned char) q[j])) j++;
        if (j > i) {
            std::string tok = q.substr(i, j - i);
            // skip tokens that are pure punctuation (no alnum)
            bool has_alnum = false;
            for (char ch : tok) {
                if (isalnum((unsigned char) ch) || (unsigned char) ch >= 0x80) {
                    has_alnum = true;
                    break;
                }
            }
            if (has_alnum) {
                std::string esc;
                for (char ch : tok) {
                    if (ch == '"') esc += "\"\"";
                    else esc += ch;
                }
                if (!out.empty()) out += ' ';
                out += '"' + esc + '"';
            }
        }
        i = j;
    }
    return out;
}

} // namespace

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

wikidata_store * wikidata_open(const char * path) {
    g_wd_error.clear();
    if (!path || !*path) {
        g_wd_error = "no wikidata store path";
        return nullptr;
    }
    sqlite3 * db = nullptr;
    int rc = sqlite3_open_v2(path, &db, SQLITE_OPEN_READONLY, nullptr);
    if (rc != SQLITE_OK) {
        g_wd_error = std::string("cannot open '") + path + "': " +
                     (db ? sqlite3_errmsg(db) : sqlite3_errstr(rc));
        if (db) sqlite3_close(db);
        return nullptr;
    }
    sqlite3_busy_timeout(db, 2000);

    auto * s = new wikidata_store();
    s->db = db;

    // Prepare statements. A store missing the expected schema fails loudly.
    const char * sql_get =
        "SELECT label, description, aliases, claims FROM entity WHERE id = ?1";
    const char * sql_label = "SELECT label FROM entity WHERE id = ?1";
    const char * sql_search =
        "SELECT e.id, e.label, e.description "
        "FROM fts JOIN entity e ON e.rowid = fts.rowid "
        "WHERE fts MATCH ?1 "
        "ORDER BY (lower(e.label) = lower(?2)) DESC, bm25(fts) "
        "LIMIT ?3";

    if (sqlite3_prepare_v2(db, sql_get, -1, &s->st_get, nullptr) != SQLITE_OK ||
        sqlite3_prepare_v2(db, sql_label, -1, &s->st_label, nullptr) != SQLITE_OK ||
        sqlite3_prepare_v2(db, sql_search, -1, &s->st_search, nullptr) != SQLITE_OK) {
        g_wd_error = std::string("not a wikidata store (schema mismatch): ") +
                     sqlite3_errmsg(db);
        wikidata_close(s);
        return nullptr;
    }
    return s;
}

void wikidata_close(wikidata_store * s) {
    if (!s) return;
    if (s->st_get) sqlite3_finalize(s->st_get);
    if (s->st_label) sqlite3_finalize(s->st_label);
    if (s->st_search) sqlite3_finalize(s->st_search);
    if (s->db) sqlite3_close(s->db);
    delete s;
}

std::vector<wikidata_hit> wikidata_search(wikidata_store * s, const std::string & query, int limit) {
    std::vector<wikidata_hit> hits;
    if (!s || !s->st_search) return hits;
    if (limit < 1) limit = 1;

    std::string match = looks_like_fts_expr(query) ? query : quote_terms(query);
    if (match.empty()) return hits;

    sqlite3_reset(s->st_search);
    sqlite3_clear_bindings(s->st_search);
    sqlite3_bind_text(s->st_search, 1, match.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(s->st_search, 2, query.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(s->st_search, 3, limit);

    int rc;
    while ((rc = sqlite3_step(s->st_search)) == SQLITE_ROW) {
        wikidata_hit h;
        const unsigned char * id = sqlite3_column_text(s->st_search, 0);
        const unsigned char * lbl = sqlite3_column_text(s->st_search, 1);
        const unsigned char * desc = sqlite3_column_text(s->st_search, 2);
        if (id) h.id = (const char *) id;
        if (lbl) h.label = (const char *) lbl;
        if (desc) h.description = (const char *) desc;
        hits.push_back(std::move(h));
    }
    if (rc != SQLITE_DONE) {
        // Likely an FTS5 syntax error on a raw expression — retry sanitized.
        if (looks_like_fts_expr(query)) {
            std::string safe = quote_terms(query);
            if (!safe.empty() && safe != match) {
                sqlite3_reset(s->st_search);
                sqlite3_clear_bindings(s->st_search);
                sqlite3_bind_text(s->st_search, 1, safe.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(s->st_search, 2, query.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_int(s->st_search, 3, limit);
                while (sqlite3_step(s->st_search) == SQLITE_ROW) {
                    wikidata_hit h;
                    const unsigned char * id = sqlite3_column_text(s->st_search, 0);
                    const unsigned char * lbl = sqlite3_column_text(s->st_search, 1);
                    const unsigned char * desc = sqlite3_column_text(s->st_search, 2);
                    if (id) h.id = (const char *) id;
                    if (lbl) h.label = (const char *) lbl;
                    if (desc) h.description = (const char *) desc;
                    hits.push_back(std::move(h));
                }
            }
        }
    }
    sqlite3_reset(s->st_search);
    return hits;
}

wikidata_entity wikidata_get(wikidata_store * s, const std::string & id) {
    wikidata_entity e;
    if (!s || !s->st_get || id.empty()) return e;
    e.id = id;
    sqlite3_reset(s->st_get);
    sqlite3_clear_bindings(s->st_get);
    sqlite3_bind_text(s->st_get, 1, id.c_str(), -1, SQLITE_TRANSIENT);
    if (sqlite3_step(s->st_get) == SQLITE_ROW) {
        e.found = true;
        const unsigned char * lbl = sqlite3_column_text(s->st_get, 0);
        const unsigned char * desc = sqlite3_column_text(s->st_get, 1);
        const unsigned char * aliases = sqlite3_column_text(s->st_get, 2);
        const unsigned char * claims = sqlite3_column_text(s->st_get, 3);
        if (lbl) e.label = (const char *) lbl;
        if (desc) e.description = (const char *) desc;
        if (aliases) parse_aliases((const char *) aliases, e.aliases);
        if (claims) decode_claims(s, (const char *) claims, e.claims);
    }
    sqlite3_reset(s->st_get);
    return e;
}

std::vector<wikidata_statement> wikidata_property(wikidata_store * s,
                                                  const std::string & id,
                                                  const std::string & pid) {
    std::vector<wikidata_statement> out;
    wikidata_entity e = wikidata_get(s, id);
    if (!e.found) return out;
    for (auto & st : e.claims) {
        if (st.pid == pid) out.push_back(st);
    }
    return out;
}
