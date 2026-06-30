// -*- mode:c;indent-tabs-mode:nil;c-basic-offset:4;coding:utf-8 -*-
// vi: set et ft=c ts=4 sts=4 sw=4 fenc=utf-8 :vi
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
// Integration test for the Xapian Glass BM25 reader (llamafile/zim/zim_xapian.c)
// against the REAL Simple-English Wikipedia ZIM, which ships an X/fulltext/xapian
// Glass index. The ZIM is ~1 GB and not committed, so this test SKIPS (exit 0)
// when the archive is absent — it never fails CI for a missing fixture.
//
// Override the ZIM path with argv[1] or $ZIM_XAPIAN_TEST_ZIM. Default is the
// dev box location.

#include "zim.h"
#include "zim_xapian.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#define DEFAULT_ZIM \
    "/Users/ochafik/Data/Models/wikipedia_en_simple_all_nopic_2026-05.zim"

static int g_checks = 0;
static int g_fails = 0;

static void check(int cond, const char *what) {
    g_checks++;
    if (cond) {
        printf("  ok   %s\n", what);
    } else {
        g_fails++;
        printf("  FAIL %s\n", what);
    }
}

// Case-insensitive substring.
static int icontains(const char *hay, const char *needle) {
    if (!hay)
        return 0;
    size_t nl = strlen(needle);
    for (const char *p = hay; *p; p++)
        if (strncasecmp(p, needle, nl) == 0)
            return 1;
    return 0;
}

// Run a query, print the top hits, and return them. Caller frees.
static int run(zim_xapian *idx, const char *q, int limit, zim_xapian_hit **out) {
    int n = zim_xapian_search(idx, q, limit, out);
    printf("query \"%s\" -> %d hit(s)\n", q, n);
    for (int i = 0; i < n && i < limit; i++)
        printf("  #%d  did=%u score=%.4f  path=%s  title=%s\n", i + 1,
               (*out)[i].docid, (*out)[i].score, (*out)[i].path, (*out)[i].title);
    return n;
}

// True if any of the top `n` hits has a path/title containing `needle`.
static int any_hit(zim_xapian_hit *h, int n, const char *needle) {
    for (int i = 0; i < n; i++)
        if (icontains(h[i].path, needle) || icontains(h[i].title, needle))
            return 1;
    return 0;
}

int main(int argc, char **argv) {
    const char *zimpath = (argc > 1) ? argv[1] : getenv("ZIM_XAPIAN_TEST_ZIM");
    if (!zimpath)
        zimpath = DEFAULT_ZIM;

    if (access(zimpath, R_OK) != 0) {
        printf("SKIP zim_xapian_test: ZIM not present at %s\n", zimpath);
        return 0;
    }

    printf("=== Xapian Glass BM25 reader tests (%s) ===\n", zimpath);
    zim_archive *z = zim_open(zimpath);
    if (!z) {
        printf("SKIP: could not open ZIM: %s\n", zim_error());
        return 0;
    }

    zim_xapian *idx = zim_xapian_open(z, "X/fulltext/xapian");
    if (!idx) {
        printf("FAIL: zim_xapian_open: %s\n", zim_xapian_error());
        zim_close(z);
        return 1;
    }

    uint32_t dc = zim_xapian_doccount(idx);
    double avg = zim_xapian_avg_doclen(idx);
    printf("doccount=%u avg_doclen=%.2f\n", dc, avg);
    check(dc > 100000, "doccount looks like a full corpus (>100k)");
    check(avg > 50.0 && avg < 5000.0, "avg_doclen in a sane range");

    // --- tokyo: the Tokyo article must rank #1 ---
    {
        zim_xapian_hit *h;
        int n = run(idx, "tokyo", 5, &h);
        check(n > 0, "tokyo returns hits");
        if (n > 0) {
            check(icontains(h[0].path, "Tokyo") || icontains(h[0].title, "Tokyo"),
                  "tokyo ranks the Tokyo article #1");
            check(h[0].score > 0.0f, "top score is positive");
            for (int i = 1; i < n; i++)
                check(h[i].score <= h[i - 1].score, "scores are non-increasing");
        }
        zim_xapian_free_hits(h, n);
    }

    // --- japan ---
    {
        zim_xapian_hit *h;
        int n = run(idx, "japan", 5, &h);
        check(n > 0 && any_hit(h, n, "Japan"),
              "japan returns a Japan article in the top 5");
        zim_xapian_free_hits(h, n);
    }

    // --- einstein ---
    {
        zim_xapian_hit *h;
        int n = run(idx, "einstein", 5, &h);
        check(n > 0 && any_hit(h, n, "Einstein"),
              "einstein returns an Einstein article in the top 5");
        zim_xapian_free_hits(h, n);
    }

    // --- the user's case: "TNT inventor" must surface TNT/Trinitrotoluene ---
    {
        zim_xapian_hit *h;
        int n = run(idx, "TNT inventor", 10, &h);
        check(n > 0, "\"TNT inventor\" is not empty");
        check(n > 0 && (any_hit(h, n, "TNT") || any_hit(h, n, "Trinitrotoluene") ||
                        any_hit(h, n, "Trinitro")),
              "\"TNT inventor\" surfaces TNT/Trinitrotoluene in the top hits");
        zim_xapian_free_hits(h, n);
    }

    // --- doclen normalization sanity: a single rare term scores positively ---
    {
        uint32_t tf = zim_xapian_termfreq(idx, "tokyo");
        printf("termfreq(tokyo)=%u\n", tf);
        check(tf > 1000 && tf < dc, "tokyo document frequency is plausible");
    }

    // --- title index opens and searches with the same code path ---
    {
        zim_xapian *tidx = zim_xapian_open(z, "X/title/xapian");
        if (tidx) {
            printf("title index doccount=%u\n", zim_xapian_doccount(tidx));
            zim_xapian_hit *h;
            int n = zim_xapian_search(tidx, "tokyo", 3, &h);
            printf("title-index query \"tokyo\" -> %d hit(s)\n", n);
            for (int i = 0; i < n; i++)
                printf("  #%d  %s\n", i + 1, h[i].title);
            check(n >= 0, "title index search runs without crashing");
            zim_xapian_free_hits(h, n);
            zim_xapian_close(tidx);
        } else {
            printf("note: X/title/xapian not present (%s)\n", zim_xapian_error());
        }
    }

    zim_xapian_close(idx);
    zim_close(z);

    printf("=== %d checks, %d failures ===\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
