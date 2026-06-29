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

#include "wiki_fts.h"

#include "sqlite/sqlite3.h"
#include "zim/zim.h"

#include <cctype>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>

// ---------------------------------------------------------------------------
// Error reporting
// ---------------------------------------------------------------------------
namespace {
thread_local std::string g_fts_error;

// Cap a single article's indexed body so a pathological page can't blow up the
// sidecar (the lead/body is plenty for full-text recall + snippets).
const size_t MAX_BODY_BYTES = 256 * 1024;
}  // namespace

const char * wiki_fts_error() {
    return g_fts_error.c_str();
}

// ---------------------------------------------------------------------------
// Store
// ---------------------------------------------------------------------------
struct wiki_fts_store {
    sqlite3 * db = nullptr;
    sqlite3_stmt * st_search = nullptr;
};

namespace {

// Collapse whitespace runs to single spaces; trim. (Shared shape with wiki_cli.)
std::string collapse_ws(const char * s, size_t n) {
    std::string out;
    out.reserve(n);
    bool in_ws = true;
    for (size_t i = 0; i < n && s[i]; i++) {
        unsigned char c = (unsigned char) s[i];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v') {
            if (!in_ws) { out.push_back(' '); in_ws = true; }
        } else {
            out.push_back((char) c);
            in_ws = false;
        }
    }
    while (!out.empty() && out.back() == ' ') out.pop_back();
    return out;
}

// Does the query look like an explicit FTS5 expression we should pass verbatim?
// (boolean operators, phrases, prefixes, column filters, parens). Otherwise we
// treat it as plain words and quote each token (implicit AND) so arbitrary
// punctuation is safe. Mirrors wikidata.cpp.
bool looks_like_fts_expr(const std::string & q) {
    if (q.find('"') != std::string::npos) return true;
    if (q.find('*') != std::string::npos) return true;
    if (q.find('(') != std::string::npos) return true;
    if (q.find(':') != std::string::npos) return true;
    if (q.find('^') != std::string::npos) return true;
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
// indexable characters. Mirrors wikidata.cpp.
std::string quote_terms(const std::string & q) {
    std::string out;
    size_t i = 0, n = q.size();
    while (i < n) {
        while (i < n && isspace((unsigned char) q[i])) i++;
        size_t j = i;
        while (j < n && !isspace((unsigned char) q[j])) j++;
        if (j > i) {
            std::string tok = q.substr(i, j - i);
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

bool exec(sqlite3 * db, const char * sql) {
    char * err = nullptr;
    if (sqlite3_exec(db, sql, nullptr, nullptr, &err) != SQLITE_OK) {
        g_fts_error = std::string("sqlite: ") + (err ? err : "?");
        if (err) sqlite3_free(err);
        return false;
    }
    return true;
}

}  // namespace

// ---------------------------------------------------------------------------
// Builder
// ---------------------------------------------------------------------------

int wiki_fts_build(const char * zim_path, const char * out_path) {
    g_fts_error.clear();
    if (!zim_path || !*zim_path || !out_path || !*out_path) {
        g_fts_error = "wiki_fts_build: need <zim> and <out.sqlite>";
        return 1;
    }

    zim_archive * z = zim_open(zim_path);
    if (!z) {
        g_fts_error = std::string("cannot open ZIM '") + zim_path + "': " + zim_error();
        return 1;
    }

    // Drop+recreate the sidecar from scratch (idempotent).
    remove(out_path);
    sqlite3 * db = nullptr;
    if (sqlite3_open(out_path, &db) != SQLITE_OK) {
        g_fts_error = std::string("cannot create '") + out_path + "': " +
                      (db ? sqlite3_errmsg(db) : "?");
        if (db) sqlite3_close(db);
        zim_close(z);
        return 1;
    }

    // Bulk-load pragmas: this is a build-once artifact, durability not required.
    exec(db, "PRAGMA journal_mode=OFF;");
    exec(db, "PRAGMA synchronous=OFF;");
    exec(db, "PRAGMA cache_size=-65536;");  // ~64 MiB page cache

    if (!exec(db,
            "DROP TABLE IF EXISTS article;"
            "DROP TABLE IF EXISTS fts;"
            "CREATE TABLE article(rowid INTEGER PRIMARY KEY, path TEXT, title TEXT);"
            "CREATE VIRTUAL TABLE fts USING fts5(title, body, tokenize='unicode61');")) {
        sqlite3_close(db);
        zim_close(z);
        return 1;
    }

    sqlite3_stmt * st_art = nullptr;
    sqlite3_stmt * st_fts = nullptr;
    if (sqlite3_prepare_v2(db,
            "INSERT INTO article(rowid,path,title) VALUES(?1,?2,?3)", -1,
            &st_art, nullptr) != SQLITE_OK ||
        sqlite3_prepare_v2(db,
            "INSERT INTO fts(rowid,title,body) VALUES(?1,?2,?3)", -1,
            &st_fts, nullptr) != SQLITE_OK) {
        g_fts_error = std::string("prepare failed: ") + sqlite3_errmsg(db);
        if (st_art) sqlite3_finalize(st_art);
        if (st_fts) sqlite3_finalize(st_fts);
        sqlite3_close(db);
        zim_close(z);
        return 1;
    }

    exec(db, "BEGIN");

    zim_iterator * it = zim_iterate(z, 0);  // all namespaces; we filter below
    sqlite3_int64 rowid = 0;
    uint64_t scanned = 0, skipped_redirect = 0, skipped_nonhtml = 0, skipped_empty = 0;
    time_t t0 = time(nullptr);

    zim_entry e;
    while (it && zim_iterator_next(it, &e)) {
        scanned++;
        if (scanned % 20000 == 0) {
            fprintf(stderr, "\r  indexed %lld / scanned %llu ...",
                    (long long) rowid, (unsigned long long) scanned);
            fflush(stderr);
        }

        // Skip redirects (they have no body of their own).
        if (e.is_redirect) { skipped_redirect++; continue; }

        // Articles are HTML in the content namespace; this filter naturally
        // drops images / css / js / metadata across both v5 ('A') and v6 ('C').
        const char * mime = zim_get_mimetype(z, &e);
        if (!mime || strncmp(mime, "text/html", 9) != 0) { skipped_nonhtml++; continue; }

        size_t n = 0;
        char * txt = zim_get_content_text(z, &e, &n);
        if (!txt || n == 0) {
            if (txt) zim_free(txt);
            skipped_empty++;
            continue;
        }
        std::string body = collapse_ws(txt, n);
        zim_free(txt);
        if (body.empty()) { skipped_empty++; continue; }
        if (body.size() > MAX_BODY_BYTES) body.resize(MAX_BODY_BYTES);

        const char * title = e.title ? e.title : (e.path ? e.path : "");
        const char * path  = e.path ? e.path : "";

        rowid++;
        sqlite3_reset(st_art);
        sqlite3_bind_int64(st_art, 1, rowid);
        sqlite3_bind_text(st_art, 2, path, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st_art, 3, title, -1, SQLITE_TRANSIENT);
        if (sqlite3_step(st_art) != SQLITE_DONE) {
            g_fts_error = std::string("insert article failed: ") + sqlite3_errmsg(db);
            rowid--;
            continue;
        }
        sqlite3_reset(st_fts);
        sqlite3_bind_int64(st_fts, 1, rowid);
        sqlite3_bind_text(st_fts, 2, title, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st_fts, 3, body.c_str(), (int) body.size(), SQLITE_TRANSIENT);
        if (sqlite3_step(st_fts) != SQLITE_DONE) {
            g_fts_error = std::string("insert fts failed: ") + sqlite3_errmsg(db);
        }

        // Periodically flush the transaction so memory stays bounded on big ZIMs.
        if (rowid % 5000 == 0) {
            exec(db, "COMMIT");
            exec(db, "BEGIN");
        }
    }
    if (it) zim_iterator_free(it);

    exec(db, "COMMIT");
    sqlite3_finalize(st_art);
    sqlite3_finalize(st_fts);

    fprintf(stderr,
            "\r  indexed %lld articles (scanned %llu: %llu redirects, "
            "%llu non-html, %llu empty) in %llds\n",
            (long long) rowid, (unsigned long long) scanned,
            (unsigned long long) skipped_redirect,
            (unsigned long long) skipped_nonhtml,
            (unsigned long long) skipped_empty,
            (long long) (time(nullptr) - t0));

    // Compact + optimize the FTS index for query-time speed/size.
    exec(db, "INSERT INTO fts(fts) VALUES('optimize');");
    exec(db, "PRAGMA journal_mode=DELETE;");

    sqlite3_close(db);
    zim_close(z);

    if (rowid == 0) {
        g_fts_error = "no articles indexed (is this a content ZIM?)";
        return 1;
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Reader
// ---------------------------------------------------------------------------

wiki_fts_store * wiki_fts_open(const char * path) {
    g_fts_error.clear();
    if (!path || !*path) {
        g_fts_error = "no wiki-fts sidecar path";
        return nullptr;
    }
    sqlite3 * db = nullptr;
    int rc = sqlite3_open_v2(path, &db, SQLITE_OPEN_READONLY, nullptr);
    if (rc != SQLITE_OK) {
        g_fts_error = std::string("cannot open '") + path + "': " +
                      (db ? sqlite3_errmsg(db) : sqlite3_errstr(rc));
        if (db) sqlite3_close(db);
        return nullptr;
    }
    sqlite3_busy_timeout(db, 2000);

    auto * s = new wiki_fts_store();
    s->db = db;

    // snippet(f, 1, ...) excerpts the body column (col 0=title, col 1=body),
    // bracketing matches and joining fragments with an ellipsis.
    const char * sql_search =
        "SELECT a.path, a.title, "
        "       snippet(fts, 1, '[', ']', ' … ', 12) "
        "FROM fts JOIN article a ON a.rowid = fts.rowid "
        "WHERE fts MATCH ?1 "
        "ORDER BY bm25(fts) "
        "LIMIT ?2";
    if (sqlite3_prepare_v2(db, sql_search, -1, &s->st_search, nullptr) != SQLITE_OK) {
        g_fts_error = std::string("not a wiki-fts sidecar (schema mismatch): ") +
                      sqlite3_errmsg(db);
        wiki_fts_close(s);
        return nullptr;
    }
    return s;
}

void wiki_fts_close(wiki_fts_store * s) {
    if (!s) return;
    if (s->st_search) sqlite3_finalize(s->st_search);
    if (s->db) sqlite3_close(s->db);
    delete s;
}

static void run_search(wiki_fts_store * s, const std::string & match, int limit,
                       std::vector<wiki_fts_hit> & hits) {
    sqlite3_reset(s->st_search);
    sqlite3_clear_bindings(s->st_search);
    sqlite3_bind_text(s->st_search, 1, match.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(s->st_search, 2, limit);
    while (sqlite3_step(s->st_search) == SQLITE_ROW) {
        wiki_fts_hit h;
        const unsigned char * path = sqlite3_column_text(s->st_search, 0);
        const unsigned char * title = sqlite3_column_text(s->st_search, 1);
        const unsigned char * snip = sqlite3_column_text(s->st_search, 2);
        if (path)  h.path = (const char *) path;
        if (title) h.title = (const char *) title;
        if (snip)  h.snippet = (const char *) snip;
        hits.push_back(std::move(h));
    }
    sqlite3_reset(s->st_search);
}

std::vector<wiki_fts_hit> wiki_fulltext_search(wiki_fts_store * s,
                                               const std::string & query, int limit) {
    std::vector<wiki_fts_hit> hits;
    if (!s || !s->st_search) return hits;
    if (limit < 1) limit = 1;

    std::string match = looks_like_fts_expr(query) ? query : quote_terms(query);
    if (match.empty()) return hits;

    run_search(s, match, limit, hits);

    // If a raw FTS5 expression errored mid-step, retry with a sanitized,
    // quoted-terms version (mirrors wikidata.cpp's robustness).
    if (hits.empty() && looks_like_fts_expr(query)) {
        std::string safe = quote_terms(query);
        if (!safe.empty() && safe != match) {
            run_search(s, safe, limit, hits);
        }
    }
    return hits;
}
