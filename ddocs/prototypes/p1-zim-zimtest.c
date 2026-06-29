// Standalone cosmocc prototype: validate the wikifile ZIM reader (P1).
#include "zim.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s file.zim [query]\n", argv[0]); return 2; }
    const char *query = argc > 2 ? argv[2] : "a";

    zim_archive *z = zim_open(argv[1]);
    if (!z) { fprintf(stderr, "zim_open failed: %s\n", zim_error()); return 1; }

    printf("== metadata ==\n");
    printf("uuid=%s entries=%u clusters=%u main_page=%u\n",
           zim_get_uuid(z), zim_get_entry_count(z), zim_get_cluster_count(z), zim_get_main_page(z));

    // main page article -> text
    zim_entry e;
    if (zim_get_main_entry(z, &e)) {
        if (e.is_redirect) zim_resolve_redirect(z, &e);
        size_t n = 0;
        char *txt = zim_get_content_text(z, &e, &n);
        printf("== main entry: path=%s title=%s ns=%c (%zu bytes text) ==\n",
               e.path ? e.path : "?", e.title ? e.title : "?", e.namespace_, n);
        if (txt) { printf("%.240s%s\n", txt, n > 240 ? "..." : ""); zim_free(txt); }
    } else {
        printf("(no main entry: %s)\n", zim_error());
    }

    // search
    printf("== search '%s' ==\n", query);
    zim_search_result res[10];
    int nr = zim_search(z, query, res, 10);
    printf("%d results\n", nr);
    for (int i = 0; i < nr; i++)
        printf("  [%u] score=%.2f title=%s path=%s\n", res[i].index, res[i].score,
               res[i].title ? res[i].title : "?", res[i].path ? res[i].path : "?");

    // fetch the top search hit's article text (exercise cluster decompress)
    if (nr > 0) {
        zim_entry he;
        if (zim_get_entry_by_index(z, res[0].index, &he)) {
            if (he.is_redirect) zim_resolve_redirect(z, &he);
            size_t n = 0;
            char *txt = zim_get_content_text(z, &he, &n);
            printf("== top hit article (%zu bytes) ==\n", n);
            if (txt) { printf("%.200s%s\n", txt, n > 200 ? "..." : ""); zim_free(txt); }
        }
    }
    if (nr) zim_search_free(res, nr);
    zim_close(z);
    printf("== OK ==\n");
    return 0;
}
