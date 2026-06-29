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

// Offline Wikipedia FULL-TEXT search — a read-only SQLite (+FTS5) sidecar built
// from a Wikipedia ZIM archive that indexes article BODIES (not just titles).
// The built-in ZIM title index only matches titles; this sidecar adds a body
// index so a query can find articles by words that appear only inside the prose.
//
// It mirrors the Wikidata fact store (wikidata.{h,cpp}) exactly: a builder that
// produces a sidecar SQLite from the source dump (here, the ZIM) and a pure-C++
// reader over the vendored third_party/sqlite that backs three surfaces, like
// the ZIM reader ("one handler, three surfaces"):
//
//   * the `llamafile wikipedia index|fulltext` CLI (wiki_cli.cpp)
//   * the wiki_fulltext_search MCP tool (mcp_server.cpp)
//   * (via mcp-server bridging) the --server /tools registry.
//
// The sidecar schema (see wiki_fts_build) is:
//   article(rowid INTEGER PRIMARY KEY, path TEXT, title TEXT)   -- side table
//   fts USING fts5(title, body, tokenize='unicode61')           -- title+body index
// with article.rowid == fts.rowid. The body text is stored inside the fts5
// content shadow so snippet()/highlight() can return a context excerpt straight
// from the sidecar (a `content=''` contentless table cannot — it keeps no text).

#include <string>
#include <vector>

struct wiki_fts_store;  // opaque handle (owns the sqlite connection)

struct wiki_fts_hit {
    std::string path;     // ZIM entry path (e.g. "Eiffel_Tower")
    std::string title;    // article title
    std::string snippet;  // context excerpt around the match (FTS5 snippet())
};

// Open a Wikipedia FTS sidecar read-only. Returns nullptr on failure
// (see wiki_fts_error()).
wiki_fts_store * wiki_fts_open(const char * path);
void             wiki_fts_close(wiki_fts_store *);

// Last error message (thread-unsafe, best-effort; for diagnostics).
const char * wiki_fts_error();

// FTS5 MATCH over title+body. `query` may be plain words (treated as an
// implicit-AND of quoted terms) or an explicit FTS5 expression (boolean OR/AND/
// NOT, phrases, prefixes, column filters like "body:foo") — the latter is
// detected and passed through verbatim, so a model can issue OR'd query
// expansions. Results are ranked by bm25 and carry a snippet() excerpt.
std::vector<wiki_fts_hit> wiki_fulltext_search(wiki_fts_store *,
                                               const std::string & query, int limit);

// Build a Wikipedia FTS sidecar at `out_path` from the ZIM at `zim_path`:
// iterate the content namespace, extract each article's plain-text body
// (HTML stripped), and index title+body into FTS5. Drops+recreates the sidecar
// (idempotent). Prints progress to stderr. Returns 0 on success, non-zero on
// error (message via wiki_fts_error()).
int wiki_fts_build(const char * zim_path, const char * out_path);
