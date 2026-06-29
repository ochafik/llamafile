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

//
// Unit test for the offline Wikipedia FULL-TEXT search builder + reader
// (llamafile/wiki_fts.{h,cpp}). It runs the real builder (wiki_fts_build) over
// the committed tiny ZIM fixture (tests/fixtures/small_nons.zim, format v6),
// producing a sidecar SQLite+FTS5 in a temp file, then drives the reader over
// it. The fixture's single article "Test ZIM file" has body text "Test ZIM file
// Test ZIM file", so we exercise the FTS5 BODY column directly (a `body:`
// column-filtered MATCH that title-only search could not satisfy), assert a
// snippet() excerpt comes back, and that a missing term yields nothing. Fully
// hermetic: the sidecar is created in a temp file and unlinked.

#include "wiki_fts.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unistd.h>

static int g_checks = 0;
static int g_fails  = 0;

#define CHECK(cond, msg)                                              \
    do {                                                              \
        g_checks++;                                                   \
        if (!(cond)) {                                                \
            g_fails++;                                                \
            fprintf(stderr, "FAIL: %s\n  at %s:%d\n", (msg),          \
                    __FILE__, __LINE__);                              \
        }                                                            \
    } while (0)

static bool hits_have_title(const std::vector<wiki_fts_hit> &hits, const char *t) {
    for (const auto &h : hits) if (h.title == t) return true;
    return false;
}

int main(int argc, char **argv) {
    const char *dir = (argc > 1) ? argv[1] : "tests/fixtures";
    std::string zim = std::string(dir) + "/small_nons.zim";

    char db[] = "/tmp/wiki_fts_test_XXXXXX";
    int fd = mkstemp(db);
    if (fd >= 0) close(fd);  // builder reopens it by name

    printf("=== Wikipedia full-text tests (zim: %s, sidecar: %s) ===\n",
           zim.c_str(), db);

    // --- builder: index the fixture's article bodies into an FTS5 sidecar ---
    int rc = wiki_fts_build(zim.c_str(), db);
    CHECK(rc == 0, "wiki_fts_build succeeds on the v6 fixture");
    if (rc != 0) {
        fprintf(stderr, "build error: %s\n", wiki_fts_error());
        unlink(db);
        return 1;
    }

    // --- builder is idempotent: a second run over the same out path is fine ---
    CHECK(wiki_fts_build(zim.c_str(), db) == 0, "wiki_fts_build is idempotent (drop+recreate)");

    wiki_fts_store *s = wiki_fts_open(db);
    CHECK(s != nullptr, "wiki_fts_open succeeds on the built sidecar");
    if (!s) { unlink(db); return 1; }

    // --- full-text search by a BODY word -> finds the article + a snippet ---
    {
        auto hits = wiki_fulltext_search(s, "zim", 10);
        CHECK(!hits.empty(), "search 'zim' (a body word) returns hits");
        CHECK(hits_have_title(hits, "Test ZIM file"),
              "body-word search finds the 'Test ZIM file' article");
        bool snip = false;
        for (const auto &h : hits) if (!h.snippet.empty()) snip = true;
        CHECK(snip, "FTS5 snippet() excerpt is returned");
        bool path = false;
        for (const auto &h : hits) if (!h.path.empty()) path = true;
        CHECK(path, "hit carries the ZIM entry path");
    }

    // --- BODY column is genuinely indexed (column-filtered MATCH the ZIM's
    //     title-only index could never satisfy) ---
    {
        auto hits = wiki_fulltext_search(s, "body:file", 10);
        CHECK(!hits.empty(), "column-filtered 'body:file' matches via the BODY index");
        CHECK(!hits.empty() && hits[0].snippet.find('[') != std::string::npos,
              "snippet brackets the matched body term");
    }

    // --- implicit-AND multi-term query over the body ---
    {
        auto hits = wiki_fulltext_search(s, "test file", 10);
        CHECK(!hits.empty(), "implicit-AND 'test file' matches the body");
    }

    // --- missing term -> empty ---
    {
        auto hits = wiki_fulltext_search(s, "nonexistentwordxyzzy", 10);
        CHECK(hits.empty(), "absent body word -> no results");
    }

    wiki_fts_close(s);

    // --- opening a non-sidecar file fails loudly (schema guard) ---
    {
        wiki_fts_store *bad = wiki_fts_open("/dev/null");
        CHECK(bad == nullptr, "opening a non-sidecar fails (schema mismatch)");
        if (bad) wiki_fts_close(bad);
    }

    unlink(db);

    printf("\n%d checks, %d failures\n", g_checks, g_fails);
    if (g_fails) {
        printf("WIKI FULL-TEXT TESTS FAILED\n");
        return 1;
    }
    printf("ALL WIKI FULL-TEXT TESTS PASSED\n");
    return 0;
}
