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

// `llamafile wikidata ...` — query an offline Wikidata structured-fact store
// (SQLite+FTS5) from the command line, with no model, no server and no MCP.
// Reuses the same in-process reader as the wikidata_* server tools.
//
//   llamafile wikidata search <query> [--wikidata PATH] [--limit N]
//   llamafile wikidata get    <Qid>   [--wikidata PATH]
//   llamafile wikidata prop   <Qid> <Pid> [--wikidata PATH]

#include "wikidata.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace {

// Tried when --wikidata is not given and $LLAMAFILE_WIKIDATA is unset.
const char * const WIKIDATA_DEFAULT = "/zip/wikidata.sqlite";

void wikidata_cli_usage(FILE * f) {
    fprintf(f,
        "llamafile wikidata - query an offline Wikidata fact store from the CLI\n"
        "\n"
        "usage:\n"
        "  llamafile wikidata search <query> [--wikidata PATH] [--limit N]\n"
        "  llamafile wikidata get    <Qid>   [--wikidata PATH]\n"
        "  llamafile wikidata prop   <Qid> <Pid> [--wikidata PATH]\n"
        "\n"
        "options:\n"
        "  --wikidata PATH   path to a wikidata .sqlite store (default:\n"
        "                    $LLAMAFILE_WIKIDATA or a bundled /zip/wikidata.sqlite)\n"
        "  --limit N         max search results (default 10)\n"
        "\n"
        "examples:\n"
        "  llamafile wikidata search \"Eiffel Tower\" --wikidata wikidata.sqlite\n"
        "  llamafile wikidata get Q243 --wikidata wikidata.sqlite\n"
        "  llamafile wikidata prop Q243 P2048 --wikidata wikidata.sqlite\n");
}

void print_value(const wikidata_value & v) {
    if (v.type == "entity" && !v.id.empty() && v.text != v.id) {
        printf("%s (%s)", v.text.c_str(), v.id.c_str());
    } else {
        printf("%s", v.text.c_str());
    }
}

void print_statement(const wikidata_statement & st, bool with_qualifiers) {
    printf("  %s (%s): ",
           st.prop_label.empty() ? st.pid.c_str() : st.prop_label.c_str(),
           st.pid.c_str());
    print_value(st.value);
    if (with_qualifiers && !st.qualifiers.empty()) {
        printf("  [");
        bool first = true;
        for (const auto & q : st.qualifiers) {
            if (!first) printf("; ");
            first = false;
            printf("%s=", q.prop_label.empty() ? q.pid.c_str() : q.prop_label.c_str());
            print_value(q.value);
        }
        printf("]");
    }
    printf("\n");
}

} // namespace

// Returns a process exit code.
int wikidata_cli_main(int argc, char ** argv) {
    // argv: [0]=llamafile [1]=wikidata [2]=subcommand [...]=args
    const char * sub = (argc > 2) ? argv[2] : nullptr;
    if (!sub || !strcmp(sub, "--help") || !strcmp(sub, "-h") || !strcmp(sub, "help")) {
        wikidata_cli_usage(sub ? stdout : stderr);
        return sub ? 0 : 2;
    }
    if (strcmp(sub, "search") && strcmp(sub, "get") && strcmp(sub, "prop")) {
        fprintf(stderr, "error: unknown subcommand '%s'\n\n", sub);
        wikidata_cli_usage(stderr);
        return 2;
    }

    const char * path = nullptr;
    int limit = 10;
    std::string a1, a2;  // positional args

    for (int i = 3; i < argc; i++) {
        if (!strcmp(argv[i], "--wikidata") && i + 1 < argc) {
            path = argv[++i];
        } else if (!strcmp(argv[i], "--limit") && i + 1 < argc) {
            limit = atoi(argv[++i]);
        } else if (a1.empty()) {
            a1 = argv[i];
        } else if (!strcmp(sub, "search")) {
            // search treats trailing words as part of the query
            a1 += ' ';
            a1 += argv[i];
        } else if (a2.empty()) {
            a2 = argv[i];
        }
    }

    if (a1.empty()) {
        fprintf(stderr, "error: missing argument\n\n");
        wikidata_cli_usage(stderr);
        return 2;
    }
    if (!strcmp(sub, "prop") && a2.empty()) {
        fprintf(stderr, "error: prop needs <Qid> <Pid>\n\n");
        wikidata_cli_usage(stderr);
        return 2;
    }
    if (limit < 1) limit = 1;

    if (!path) path = getenv("LLAMAFILE_WIKIDATA");
    bool used_default = false;
    if (!path) { path = WIKIDATA_DEFAULT; used_default = true; }

    wikidata_store * s = wikidata_open(path);
    if (!s) {
        if (used_default) {
            fprintf(stderr, "error: no Wikidata store specified. Pass --wikidata PATH "
                            "(or set $LLAMAFILE_WIKIDATA, or bundle one at %s).\n",
                    WIKIDATA_DEFAULT);
        } else {
            fprintf(stderr, "error: %s\n", wikidata_error());
        }
        return 1;
    }

    int rc = 0;

    if (!strcmp(sub, "search")) {
        std::vector<wikidata_hit> hits = wikidata_search(s, a1, limit);
        if (hits.empty()) {
            fprintf(stderr, "no results for \"%s\"\n", a1.c_str());
            rc = 1;
        } else {
            for (const auto & h : hits) {
                printf("%-12s %s", h.id.c_str(), h.label.c_str());
                if (!h.description.empty()) printf("  |  %s", h.description.c_str());
                printf("\n");
            }
        }
    } else if (!strcmp(sub, "get")) {
        wikidata_entity e = wikidata_get(s, a1);
        if (!e.found) {
            fprintf(stderr, "error: entity not found: %s\n", a1.c_str());
            rc = 1;
        } else {
            printf("%s  %s\n", e.id.c_str(), e.label.c_str());
            if (!e.description.empty()) printf("%s\n", e.description.c_str());
            if (!e.aliases.empty()) {
                printf("aliases:");
                for (const auto & al : e.aliases) printf(" %s;", al.c_str());
                printf("\n");
            }
            if (!e.claims.empty()) {
                printf("claims:\n");
                for (const auto & st : e.claims) print_statement(st, true);
            }
        }
    } else { // prop
        std::vector<wikidata_statement> vals = wikidata_property(s, a1, a2);
        if (vals.empty()) {
            fprintf(stderr, "no values for %s %s\n", a1.c_str(), a2.c_str());
            rc = 1;
        } else {
            for (const auto & st : vals) print_statement(st, true);
        }
    }

    wikidata_close(s);
    return rc;
}
