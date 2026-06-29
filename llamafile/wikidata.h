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

// Offline Wikidata structured-fact store — a read-only SQLite (+FTS5) entity
// store built from the Wikidata JSON dump (see ddocs/07). This pure-C++ reader
// (over the vendored third_party/sqlite) backs three surfaces, exactly like the
// ZIM reader (ddocs/06 "one handler, three surfaces"):
//
//   * the `llamafile wikidata search|get|prop` CLI (wikidata_cli.cpp)
//   * the wikidata_* MCP tools (mcp_server.cpp)
//   * (via mcp-server bridging) the --server /tools registry.
//
// The store schema is:
//   entity(id TEXT, label, description, aliases JSON, claims JSON)
//   + UNIQUE INDEX on id
//   + fts5(label, aliases) contentless, rowid == entity.rowid
// claims is the DuckDB-derived nested JSON: an array of statements, each a list
// of [property, value] pairs where the first pair is the main snak and the rest
// are qualifiers; values are the describe_snak() strings (entity ids, quantities
// "+330 x <unit-uri>", times, "@ lat, lon", quoted strings).

#include <string>
#include <vector>

struct wikidata_store;  // opaque handle (owns the sqlite connection)

struct wikidata_hit {
    std::string id;           // Qid / Pid
    std::string label;
    std::string description;
};

// A single decoded claim value.
struct wikidata_value {
    std::string type;  // "entity" | "quantity" | "time" | "coordinate" | "string" | "other"
    std::string text;  // human display, e.g. "330 metre", "Paris", "1889-03-31"
    std::string id;    // for type=="entity": the referenced Qid/Pid (else empty)
};

struct wikidata_qualifier {
    std::string pid;          // qualifier property id (e.g. P580)
    std::string prop_label;   // resolved property label (e.g. "start time")
    wikidata_value value;
};

struct wikidata_statement {
    std::string pid;          // main property id (e.g. P2048)
    std::string prop_label;   // resolved property label (e.g. "height")
    wikidata_value value;
    std::vector<wikidata_qualifier> qualifiers;
};

struct wikidata_entity {
    bool found = false;
    std::string id;
    std::string label;
    std::string description;
    std::vector<std::string> aliases;
    std::vector<wikidata_statement> claims;
    std::string i18n;  // raw JSON {labels,descriptions,aliases} per lang (en/fr/es/ar);
                       // empty when the store predates the multilingual column.
};

// Open a Wikidata SQLite store read-only. Returns nullptr on failure (see
// wikidata_error()).
wikidata_store * wikidata_open(const char * path);
void             wikidata_close(wikidata_store *);

// Last error message (thread-unsafe, best-effort; for diagnostics).
const char * wikidata_error();

// FTS5 search over label+aliases. `query` may be plain words (treated as an
// implicit-AND of quoted terms) or an explicit FTS5 expression (boolean OR/AND/
// NOT, phrases, prefixes) — the latter is detected and passed through verbatim,
// so a model can issue OR'd query expansions (ddocs/08). An exact label match is
// boosted to the top; remaining results are ranked by bm25.
std::vector<wikidata_hit> wikidata_search(wikidata_store *, const std::string & query, int limit);

// Fetch one entity by id (Qid/Pid), with claims decoded and P/Q ids resolved to
// labels where present in the store. `.found` is false if the id is absent.
wikidata_entity wikidata_get(wikidata_store *, const std::string & id);

// Convenience: just the (decoded, resolved) statements for one property on one
// entity (e.g. id="Q243", pid="P2048" -> the height values).
std::vector<wikidata_statement> wikidata_property(wikidata_store *,
                                                  const std::string & id,
                                                  const std::string & pid);
