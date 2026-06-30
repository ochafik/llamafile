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

// `llamafile wikipedia ...` — query an offline Wikipedia (ZIM archive) straight
// from the command line, with no model, no server and no MCP. Reuses the same
// in-process pure-C ZIM reader as the wiki_* server tools.
//
//   llamafile wikipedia search <query> [--zim PATH] [--limit N]
//   llamafile wikipedia get    <title|path> [--zim PATH]

#include "zim/zim.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <strings.h>

namespace {

// Tried when --zim is not given and $LLAMAFILE_ZIM is unset (in-APE bundle).
const char * const WIKI_DEFAULT_ZIM = "/zip/wikipedia.zim";

void wiki_cli_usage(FILE * f) {
    fprintf(f,
        "llamafile wikipedia - query an offline Wikipedia (ZIM) from the CLI\n"
        "\n"
        "usage:\n"
        "  llamafile wikipedia search   <query> [--zim PATH] [--limit N]\n"
        "  llamafile wikipedia get      <title|path> [--zim PATH]\n"
        "\n"
        "subcommands:\n"
        "  search    ranked full-text search via the ZIM's own embedded Xapian\n"
        "            BM25 index (article bodies), with title-match boosting\n"
        "  get       print one article's full plain text\n"
        "\n"
        "options:\n"
        "  --zim PATH       path to a .zim archive (default: $LLAMAFILE_ZIM or a\n"
        "                   bundled /zip/wikipedia.zim if present)\n"
        "  --limit N        max search results (default 5)\n"
        "\n"
        "examples:\n"
        "  llamafile wikipedia search \"Eiffel Tower\" --zim simplewiki.zim\n"
        "  llamafile wikipedia get \"Eiffel Tower\" --zim simplewiki.zim\n");
}

// Collapse whitespace runs to single spaces; trim. Returns a std::string.
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

std::string entry_snippet(zim_archive * z, zim_entry * e, size_t max_len) {
    if (e->is_redirect) zim_resolve_redirect(z, e);
    size_t n = 0;
    char * txt = zim_get_content_text(z, e, &n);
    if (!txt) return std::string();
    std::string out = collapse_ws(txt, n);
    zim_free(txt);
    if (out.size() > max_len) out.resize(max_len);
    return out;
}

} // namespace

// Returns a process exit code.
int wiki_cli_main(int argc, char ** argv) {
    // argv: [0]=llamafile [1]=wikipedia [2]=subcommand [...]=args
    const char * sub = (argc > 2) ? argv[2] : nullptr;
    if (!sub || !strcmp(sub, "--help") || !strcmp(sub, "-h") || !strcmp(sub, "help")) {
        wiki_cli_usage(sub ? stdout : stderr);
        return sub ? 0 : 2;
    }
    if (strcmp(sub, "search") != 0 && strcmp(sub, "get") != 0) {
        fprintf(stderr, "error: unknown subcommand '%s'\n\n", sub);
        wiki_cli_usage(stderr);
        return 2;
    }

    const char * zim_path = nullptr;
    int limit = 5;
    std::string text; // query (search) or title/path (get)

    for (int i = 3; i < argc; i++) {
        if (!strcmp(argv[i], "--zim") && i + 1 < argc) {
            zim_path = argv[++i];
        } else if (!strcmp(argv[i], "--limit") && i + 1 < argc) {
            limit = atoi(argv[++i]);
        } else {
            if (!text.empty()) text.push_back(' ');
            text += argv[i];
        }
    }

    if (text.empty()) {
        fprintf(stderr, "error: missing %s\n\n", !strcmp(sub, "search") ? "<query>" : "<title|path>");
        wiki_cli_usage(stderr);
        return 2;
    }
    if (limit < 1) limit = 1;

    if (!zim_path) zim_path = getenv("LLAMAFILE_ZIM");
    bool used_default = false;
    if (!zim_path) { zim_path = WIKI_DEFAULT_ZIM; used_default = true; }

    zim_archive * z = zim_open(zim_path);
    if (!z) {
        if (used_default) {
            fprintf(stderr, "error: no ZIM archive specified. Pass --zim PATH "
                            "(or set $LLAMAFILE_ZIM, or bundle one at %s).\n",
                    WIKI_DEFAULT_ZIM);
        } else {
            fprintf(stderr, "error: failed to open ZIM '%s': %s\n", zim_path, zim_error());
        }
        return 1;
    }

    int rc = 0;

    if (!strcmp(sub, "search")) {
        zim_search_result * results = (zim_search_result *) calloc(limit, sizeof(*results));
        if (!results) { zim_close(z); return 1; }
        int nr = zim_search(z, text.c_str(), results, limit);
        if (nr <= 0) {
            fprintf(stderr, "no results for \"%s\"\n", text.c_str());
            rc = 1;
        } else {
            for (int i = 0; i < nr; i++) {
                std::string snippet;
                zim_entry e;
                if (zim_get_entry_by_index(z, results[i].index, &e)) {
                    snippet = entry_snippet(z, &e, 200);
                }
                printf("%d. %s  [%s]\n", i + 1,
                       results[i].title ? results[i].title : "",
                       results[i].path ? results[i].path : "");
                if (!snippet.empty()) {
                    printf("   %s\n", snippet.c_str());
                }
            }
            zim_search_free(results, nr);
        }
        free(results);
    } else { // get
        zim_entry e;
        bool found = false;
        // Try as a (namespace-qualified) path first, then the bare path under the
        // content namespaces ('C' new / 'A' old) so the path shown by `search`
        // (e.g. "main.html") works directly.
        if (zim_get_entry_by_path(z, text.c_str(), &e)) {
            found = true;
        } else {
            std::string c = "C/" + text, a = "A/" + text;
            if (zim_get_entry_by_path(z, c.c_str(), &e) ||
                zim_get_entry_by_path(z, a.c_str(), &e)) {
                found = true;
            }
        }
        if (!found) {
            zim_search_result results[10];
            int nr = zim_search(z, text.c_str(), results, 10);
            int pick = -1;
            for (int i = 0; i < nr; i++) {
                if (results[i].title && strcasecmp(results[i].title, text.c_str()) == 0) {
                    pick = i;
                    break;
                }
            }
            if (pick < 0 && nr > 0) pick = 0;
            if (pick >= 0) found = zim_get_entry_by_index(z, results[pick].index, &e);
            if (nr > 0) zim_search_free(results, nr);
        }

        if (!found) {
            fprintf(stderr, "error: article not found: %s\n", text.c_str());
            rc = 1;
        } else {
            if (e.is_redirect) zim_resolve_redirect(z, &e);
            size_t n = 0;
            char * txt = zim_get_content_text(z, &e, &n);
            if (!txt || n == 0) {
                fprintf(stderr, "error: article has no readable text: %s\n",
                        e.title ? e.title : text.c_str());
                rc = 1;
            } else {
                fwrite(txt, 1, n, stdout);
                if (n && txt[n - 1] != '\n') fputc('\n', stdout);
            }
            if (txt) zim_free(txt);
        }
    }

    zim_close(z);
    return rc;
}
