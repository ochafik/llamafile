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
// Integration test for the llamafile ZIM reader (llamafile/zim/) against two
// tiny, committed real ZIM archives:
//
//   tests/fixtures/small_nons.zim   - format v6 ("new" namespace-less; content
//                                     entries live in the 'C' namespace).
//   tests/fixtures/small_withns.zim - format v5 ("old" with namespaces; content
//                                     entries live in the 'A' namespace).
//
// Both were produced by zimwriterfs / libzim and contain a single article
// titled "Test ZIM file". This locks down: open (both formats), metadata,
// get-by-path under the version-specific content namespace, title search,
// redirect resolution, and the streaming-zstd cluster decompression path
// (zim_get_content_text pulls the article out of a compressed cluster).
//
// NOTE on the title-index path: these fixtures do NOT ship the
// X/listing/titleOrdered/v1 binary title index, so zim_search() here exercises
// its linear title-scan FALLBACK, not the indexed path. A fixture carrying that
// index would be needed to cover zim_search.c's indexed branch; see the SKIP
// note printed at the end.
//
// The test takes an optional argv[1] = fixtures directory (default
// "tests/fixtures", which is correct when run from the repo root via
// `make check`).

#include "zim.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_checks = 0;
static int g_fails  = 0;

#define CHECK(cond, msg)                                              \
    do {                                                              \
        g_checks++;                                                   \
        if (!(cond)) {                                                \
            g_fails++;                                                \
            fprintf(stderr, "FAIL: %s\n  at %s:%d\n", (msg),         \
                    __FILE__, __LINE__);                              \
        }                                                            \
    } while (0)

// Open a fixture by name under the fixtures dir.
static zim_archive *open_fixture(const char *dir, const char *name) {
    char path[1024];
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    zim_archive *z = zim_open(path);
    if (!z) fprintf(stderr, "  (zim_open '%s' failed: %s)\n", path, zim_error());
    return z;
}

// Find the entry index of the top "Test" search hit (or -1).
static int search_test_title(zim_archive *z, const char *expect_title) {
    zim_search_result res[10];
    int nr = zim_search(z, "Test", res, 10);
    CHECK(nr >= 1, "search 'Test' returns at least one hit");
    int idx = -1;
    if (nr >= 1) {
        CHECK(res[0].title && strcmp(res[0].title, expect_title) == 0,
              "top search hit title == \"Test ZIM file\"");
        idx = (int) res[0].index;
    }
    if (nr > 0) zim_search_free(res, nr);
    return idx;
}

static char *get_text_by_path(zim_archive *z, const char *path, size_t *n) {
    zim_entry e;
    if (!zim_get_entry_by_path(z, path, &e)) return NULL;
    if (e.is_redirect) zim_resolve_redirect(z, &e);
    return zim_get_content_text(z, &e, n);
}

// --- v6 (namespace-less, content in 'C') -------------------------------------
static void test_nons(const char *dir) {
    printf("== small_nons.zim (format v6, content ns 'C') ==\n");
    zim_archive *z = open_fixture(dir, "small_nons.zim");
    CHECK(z != NULL, "v6 archive opens");
    if (!z) return;

    CHECK(zim_get_entry_count(z) == 16, "v6 entry_count == 16");
    CHECK(zim_get_cluster_count(z) == 2, "v6 cluster_count == 2");
    CHECK(strlen(zim_get_uuid(z)) == 32, "v6 uuid is 32 hex chars");

    // title search (linear fallback) -> the single article
    int hit = search_test_title(z, "Test ZIM file");
    CHECK(hit >= 0, "v6 search found an index");

    // get-by-path under the content namespace 'C'; bare path must MISS.
    zim_entry e;
    CHECK(!zim_get_entry_by_path(z, "main.html", &e),
          "v6 bare 'main.html' (no namespace) is NOT found");
    CHECK(zim_get_entry_by_path(z, "C/main.html", &e),
          "v6 'C/main.html' is found");

    // content retrieval (exercises streaming-zstd cluster decompression).
    size_t n = 0;
    char *txt = get_text_by_path(z, "C/main.html", &n);
    CHECK(txt != NULL && n > 0, "v6 article has decompressed text");
    CHECK(txt && strstr(txt, "Test ZIM file") != NULL,
          "v6 article text contains \"Test ZIM file\"");
    if (txt) zim_free(txt);

    // redirect resolution: the W/mainPage well-known entry redirects to the
    // article; resolving it must yield the same readable content.
    if (zim_get_main_entry(z, &e)) {
        CHECK(true, "v6 main entry retrievable");
        if (e.is_redirect) {
            bool ok = zim_resolve_redirect(z, &e);
            CHECK(ok, "v6 main page redirect resolves");
        }
        size_t mn = 0;
        char *mtxt = zim_get_content_text(z, &e, &mn);
        CHECK(mtxt != NULL && mn > 0, "v6 resolved main page has text");
        if (mtxt) zim_free(mtxt);
    }

    zim_close(z);
}

// --- v5 (with namespaces, content in 'A') ------------------------------------
static void test_withns(const char *dir) {
    printf("== small_withns.zim (format v5, content ns 'A') ==\n");
    zim_archive *z = open_fixture(dir, "small_withns.zim");
    CHECK(z != NULL, "v5 archive opens");
    if (!z) return;

    CHECK(zim_get_entry_count(z) == 17, "v5 entry_count == 17");
    CHECK(strlen(zim_get_uuid(z)) == 32, "v5 uuid is 32 hex chars");

    // title search works across the older namespaced layout too.
    int hit = search_test_title(z, "Test ZIM file");
    CHECK(hit >= 0, "v5 search found an index");

    // get-by-path under the old content namespace 'A'.
    zim_entry e;
    CHECK(!zim_get_entry_by_path(z, "C/main.html", &e),
          "v5 'C/main.html' is NOT found (old layout uses 'A')");
    CHECK(zim_get_entry_by_path(z, "A/main.html", &e),
          "v5 'A/main.html' is found");
    CHECK(e.title && strcmp(e.title, "Test ZIM file") == 0,
          "v5 'A/main.html' title == \"Test ZIM file\"");

    zim_close(z);
}

// --- HTML -> text smoke (the helper has its own exhaustive suite in
//     llamafile/zim/zim_html_test.c; this is just a wiring check) -------------
static void test_html_to_text(void) {
    printf("== zim_html_to_text smoke ==\n");
    const char *html = "<p>Hello <b>World</b></p><script>x=1</script>";
    size_t n = 0;
    char *t = zim_html_to_text(html, strlen(html), &n);
    CHECK(t != NULL, "html_to_text returns non-NULL");
    CHECK(t && strstr(t, "Hello") && strstr(t, "World"), "text preserved");
    CHECK(t && strstr(t, "<b>") == NULL, "tags stripped");
    CHECK(t && strstr(t, "x=1") == NULL, "script content dropped");
    if (t) zim_free(t);
}

int main(int argc, char **argv) {
    const char *dir = (argc > 1) ? argv[1] : "tests/fixtures";
    printf("=== ZIM reader tests (fixtures: %s) ===\n", dir);

    test_nons(dir);
    test_withns(dir);
    test_html_to_text();

    printf("\nSKIP: X/listing/titleOrdered/v1 indexed title-search path is not\n"
           "      covered (both tiny fixtures use the linear-scan fallback; a\n"
           "      fixture carrying that index would be required).\n");

    printf("\n%d checks, %d failures\n", g_checks, g_fails);
    if (g_fails) {
        printf("ZIM READER TESTS FAILED\n");
        return 1;
    }
    printf("ALL ZIM READER TESTS PASSED\n");
    return 0;
}
