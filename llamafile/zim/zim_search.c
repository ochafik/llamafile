// -*- mode:c;indent-tabs-mode:nil;c-basic-offset:4;coding:utf-8 -*-
// vi: set et ft=c ts=4 sts=4 sw=4 fenc=utf-8 :vi
//
// Copyright 2024 Mozilla Foundation
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

#include "zim_internal.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

// -----------------------------------------------------------------
// String Utilities
// -----------------------------------------------------------------

// Case-insensitive string prefix match
static bool str_prefix_icase(const char *str, const char *prefix) {
    while (*prefix) {
        if (tolower((unsigned char)*str) != tolower((unsigned char)*prefix)) {
            return false;
        }
        str++;
        prefix++;
    }
    return true;
}

// Case-insensitive substring search
static const char *str_find_icase(const char *haystack, const char *needle) {
    if (!*needle) return haystack;

    for (; *haystack; haystack++) {
        const char *h = haystack;
        const char *n = needle;
        while (*h && *n && tolower((unsigned char)*h) == tolower((unsigned char)*n)) {
            h++;
            n++;
        }
        if (!*n) return haystack;
    }
    return NULL;
}

// Calculate simple relevance score
static float calculate_score(const char *title, const char *query) {
    size_t title_len = strlen(title);
    size_t query_len = strlen(query);

    // Exact match (case insensitive)
    if (title_len == query_len && str_prefix_icase(title, query)) {
        return 1.0f;
    }

    // Prefix match
    if (str_prefix_icase(title, query)) {
        return 0.9f - (float)(title_len - query_len) / 1000.0f;
    }

    // Contains query
    if (str_find_icase(title, query)) {
        // Score based on position and length ratio
        const char *pos = str_find_icase(title, query);
        float position_score = 1.0f - (float)(pos - title) / (float)title_len;
        float length_score = (float)query_len / (float)title_len;
        return 0.5f * position_score + 0.3f * length_score;
    }

    // Word match (any word in title starts with query)
    const char *p = title;
    while (*p) {
        // Skip to word start
        while (*p && !isalnum((unsigned char)*p)) p++;
        if (!*p) break;

        if (str_prefix_icase(p, query)) {
            return 0.4f;
        }

        // Skip to word end
        while (*p && isalnum((unsigned char)*p)) p++;
    }

    return 0.0f;
}

// -----------------------------------------------------------------
// Title index navigation (P1 fix + O(log n) search)
// -----------------------------------------------------------------
//
// The ZIM header title-pointer list is ordered by (namespace, title), NOT
// globally by title; within a single namespace the entries are sorted by title
// using a plain byte-wise (case-sensitive) comparison. So the search is two
// binary searches:
//
//   1. find the content-namespace sub-range [start, end) of `title_ptrs`
//      (single-byte namespace comparison — collation-independent, reliable);
//   2. binary-search titles within that sub-range to land on a prefix, then
//      walk forward over the (few) matching titles.
//
// Both are O(log n). Content lives in namespace 'C' (new/"nons" ZIMs) or 'A'
// (old/"withns" ZIMs); metadata ('M'), well-known ('W') and the Xapian index
// ('X') are excluded by construction.
//
// Note on case: the on-disk order is case-sensitive byte order, but queries
// should match case-insensitively. We bridge this by probing a small set of
// first-character case variants of the prefix (as-typed, upper-first,
// lower-first) — this covers the dominant Wikipedia title convention
// ("eiffel" -> "Eiffel"). A query whose case differs from the title past the
// first character (rare) may be missed by the binary path; for fully
// case/locale-insensitive search the robust source is the ZIM's
// X/listing/titleOrdered/v1 article index or the Xapian 'X' full-text index
// (not used here to avoid the libxapian/ICU dependency).

// Namespace byte of the entry referenced by title_ptrs[i] (0 on failure,
// which sorts before any real namespace so it never widens a match range).
static uint8_t title_ptr_ns(zim_archive *archive, uint32_t i) {
    zim_entry e;
    if (!zim_get_entry_by_index(archive, archive->title_ptrs[i], &e)) {
        return 0;
    }
    return e.namespace_;
}

// First index in title_ptrs whose namespace byte is >= ns.
static uint32_t title_ns_lower_bound(zim_archive *archive, uint8_t ns) {
    uint32_t lo = 0, hi = archive->header.entry_count;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        if (title_ptr_ns(archive, mid) < ns) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return lo;
}

// Resolve the [start, end) content-namespace sub-range of title_ptrs.
// Returns the namespace byte actually used (0 if none found).
static uint8_t zim_content_ns_range(zim_archive *archive,
                                    uint32_t *start, uint32_t *end) {
    static const uint8_t content_ns[] = { ZIM_NS_CONTENT, 'A' };
    for (size_t k = 0; k < sizeof(content_ns); k++) {
        uint8_t ns = content_ns[k];
        uint32_t s = title_ns_lower_bound(archive, ns);
        uint32_t e = title_ns_lower_bound(archive, (uint8_t)(ns + 1));
        if (s < e) {
            *start = s;
            *end = e;
            return ns;
        }
    }
    *start = *end = archive->header.entry_count;
    return 0;
}

// Title of the entry referenced by title_ptrs[i] (NULL on failure). The pointer
// is only valid until the next zim_get_entry_by_index() call, so callers copy
// what they need before another lookup.
static const char *title_ptr_title(zim_archive *archive, uint32_t i, zim_entry *e) {
    if (!zim_get_entry_by_index(archive, archive->title_ptrs[i], e)) {
        return NULL;
    }
    return e->title;
}

// First index in [lo, hi) whose title is byte-wise >= key (case-sensitive,
// matching the ZIM's on-disk title ordering).
static uint32_t title_lower_bound(zim_archive *archive, uint32_t lo, uint32_t hi,
                                  const char *key) {
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        zim_entry e;
        const char *t = title_ptr_title(archive, mid, &e);
        if (t && strcmp(t, key) < 0) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return lo;
}

// -----------------------------------------------------------------
// Title Prefix Lookup (compat shim)
// -----------------------------------------------------------------

// Returns the title_ptrs index at which a case-sensitive scan for `prefix`
// should begin, within the content namespace. Kept for API compatibility.
uint32_t zim_find_title_prefix(zim_archive *archive, const char *prefix) {
    if (!zim_load_title_ptrs(archive) || !zim_load_path_ptrs(archive)) {
        return UINT32_MAX;
    }
    uint32_t start, end;
    zim_content_ns_range(archive, &start, &end);
    if (start >= end) {
        return archive->header.entry_count;  // no content entries
    }
    return title_lower_bound(archive, start, end, prefix);
}

// -----------------------------------------------------------------
// Search Implementation
// -----------------------------------------------------------------

typedef struct {
    uint32_t index;
    char path[256];
    char title[256];
    float score;
} zim_temp_result;

// Build up to 3 first-character case variants of `query` (as-typed,
// upper-first, lower-first), deduplicated. Returns the count; fills probes[]
// with pointers into the caller-provided buffers buf[] (each >= len+1).
static int build_case_probes(const char *query, char buf[3][256], const char *probes[3]) {
    size_t n = strlen(query);
    if (n >= 256) n = 255;
    int count = 0;

    memcpy(buf[count], query, n);
    buf[count][n] = '\0';
    probes[count] = buf[count];
    count++;

    if (n > 0 && isalpha((unsigned char)query[0])) {
        char up = (char)toupper((unsigned char)query[0]);
        char lo = (char)tolower((unsigned char)query[0]);
        for (int which = 0; which < 2; which++) {
            char first = which == 0 ? up : lo;
            if (first == query[0]) continue; // same as as-typed
            // skip duplicate variant
            bool dup = false;
            for (int j = 0; j < count; j++) {
                if (buf[j][0] == first) { dup = true; break; }
            }
            if (dup) continue;
            memcpy(buf[count], query, n);
            buf[count][0] = first;
            buf[count][n] = '\0';
            probes[count] = buf[count];
            count++;
            if (count == 3) break;
        }
    }
    return count;
}

int zim_search(zim_archive *archive, const char *query,
               zim_search_result *results, int max_results) {
    if (!query || !*query || max_results <= 0) {
        return 0;
    }

    if (!zim_load_title_ptrs(archive) || !zim_load_path_ptrs(archive)) {
        return 0;
    }

    // 1. Binary-search the content-namespace sub-range to scan.
    uint32_t start, end;
    uint8_t content_ns = zim_content_ns_range(archive, &start, &end);
    if (start >= end) {
        return 0;
    }

    int cap = max_results * 2;
    zim_temp_result *temp = malloc(cap * sizeof(zim_temp_result));
    if (!temp) {
        return 0;
    }
    int temp_count = 0;

    // 2. Binary-search to each case-variant prefix, then walk forward over the
    //    matching titles (O(log n + k)).
    char probe_buf[3][256];
    const char *probes[3];
    int nprobes = build_case_probes(query, probe_buf, probes);

    for (int p = 0; p < nprobes && temp_count < cap; p++) {
        const char *prefix = probes[p];
        uint32_t i = title_lower_bound(archive, start, end, prefix);
        for (; i < end && temp_count < cap; i++) {
            zim_entry entry;
            if (!zim_get_entry_by_index(archive, archive->title_ptrs[i], &entry)) {
                continue;
            }
            if (entry.namespace_ != content_ns) {
                break; // left the content namespace
            }
            // Stop once titles no longer share this (case-sensitive) prefix:
            // the sub-range is sorted, so there can be no further matches.
            if (strncmp(entry.title, prefix, strlen(prefix)) != 0) {
                break;
            }

            float score = calculate_score(entry.title, query);
            if (score < 0.1f) {
                continue;
            }
            if (entry.is_redirect) {
                score *= 0.5f;
            }

            // Deduplicate (variants can overlap).
            bool dup = false;
            for (int j = 0; j < temp_count; j++) {
                if (temp[j].index == entry.index) { dup = true; break; }
            }
            if (dup) {
                continue;
            }

            temp[temp_count].index = entry.index;
            strncpy(temp[temp_count].path, entry.path, sizeof(temp[temp_count].path) - 1);
            temp[temp_count].path[sizeof(temp[temp_count].path) - 1] = '\0';
            strncpy(temp[temp_count].title, entry.title, sizeof(temp[temp_count].title) - 1);
            temp[temp_count].title[sizeof(temp[temp_count].title) - 1] = '\0';
            temp[temp_count].score = score;
            temp_count++;
        }
    }

    // Sort by score (simple insertion-friendly bubble sort for small arrays).
    for (int i = 0; i < temp_count - 1; i++) {
        for (int j = 0; j < temp_count - i - 1; j++) {
            if (temp[j].score < temp[j + 1].score) {
                zim_temp_result t = temp[j];
                temp[j] = temp[j + 1];
                temp[j + 1] = t;
            }
        }
    }

    int count = temp_count < max_results ? temp_count : max_results;
    for (int i = 0; i < count; i++) {
        results[i].index = temp[i].index;
        results[i].path = strdup(temp[i].path);
        results[i].title = strdup(temp[i].title);
        results[i].score = temp[i].score;
    }

    free(temp);
    return count;
}

int zim_suggest(zim_archive *archive, const char *prefix,
                zim_search_result *results, int max_results) {
    if (!prefix || !*prefix || max_results <= 0) {
        return 0;
    }

    if (!zim_load_title_ptrs(archive) || !zim_load_path_ptrs(archive)) {
        return 0;
    }

    uint32_t start, end;
    uint8_t content_ns = zim_content_ns_range(archive, &start, &end);
    if (start >= end) {
        return 0;
    }

    char probe_buf[3][256];
    const char *probes[3];
    int nprobes = build_case_probes(prefix, probe_buf, probes);

    int count = 0;
    for (int p = 0; p < nprobes && count < max_results; p++) {
        const char *pfx = probes[p];
        uint32_t i = title_lower_bound(archive, start, end, pfx);
        for (; i < end && count < max_results; i++) {
            zim_entry entry;
            if (!zim_get_entry_by_index(archive, archive->title_ptrs[i], &entry)) {
                continue;
            }
            if (entry.namespace_ != content_ns) {
                break;
            }
            if (strncmp(entry.title, pfx, strlen(pfx)) != 0) {
                break;
            }
            if (entry.is_redirect) {
                continue;
            }
            // Deduplicate across variants.
            bool dup = false;
            for (int j = 0; j < count; j++) {
                if (results[j].index == entry.index) { dup = true; break; }
            }
            if (dup) {
                continue;
            }
            results[count].index = entry.index;
            results[count].path = strdup(entry.path);
            results[count].title = strdup(entry.title);
            results[count].score = 1.0f - (float)count / (float)max_results;
            count++;
        }
    }

    return count;
}

void zim_search_free(zim_search_result *results, int count) {
    for (int i = 0; i < count; i++) {
        free((void *)results[i].path);
        free((void *)results[i].title);
    }
}

// -----------------------------------------------------------------
// Iterator Implementation
// -----------------------------------------------------------------

struct zim_iterator {
    zim_archive *archive;
    uint32_t current;
    char namespace_;
};

zim_iterator *zim_iterate(zim_archive *archive, char namespace_) {
    zim_iterator *iter = malloc(sizeof(zim_iterator));
    if (!iter) {
        return NULL;
    }
    iter->archive = archive;
    iter->current = 0;
    iter->namespace_ = namespace_;
    return iter;
}

bool zim_iterator_next(zim_iterator *iter, zim_entry *entry) {
    while (iter->current < iter->archive->header.entry_count) {
        if (!zim_get_entry_by_index(iter->archive, iter->current++, entry)) {
            continue;
        }

        // Filter by namespace if specified
        if (iter->namespace_ != 0 && entry->namespace_ != iter->namespace_) {
            continue;
        }

        return true;
    }
    return false;
}

void zim_iterator_free(zim_iterator *iter) {
    free(iter);
}
