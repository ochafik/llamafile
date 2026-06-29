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
// Binary Search for Title Prefix
// -----------------------------------------------------------------

// PROTOTYPE FIX (P1): ZIM's header title-pointer list is ordered by
// (namespace, title), NOT globally by title. The old global binary search
// therefore failed on real ZIMs. For content search we only care about the
// content namespace ('C' in new/nons ZIMs, 'A' in old/withns). Linear scan
// here just PROVES findability; the production version should binary-search
// within the content-namespace sub-range (or use X/listing/titleOrdered/v1).
uint32_t zim_find_title_prefix(zim_archive *archive, const char *prefix) {
    if (!zim_load_title_ptrs(archive) || !zim_load_path_ptrs(archive)) {
        return UINT32_MAX;
    }
    zim_entry entry;
    for (uint32_t i = 0; i < archive->header.entry_count; i++) {
        if (!zim_get_entry_by_index(archive, archive->title_ptrs[i], &entry))
            continue;
        if (entry.namespace_ != ZIM_NS_CONTENT && entry.namespace_ != 'A')
            continue;  // only content articles
        const char *t = entry.title, *p = prefix;
        while (*p && *t && tolower((unsigned char)*t) == tolower((unsigned char)*p)) { t++; p++; }
        if (!*p) return i;  // prefix fully matched -> first hit
    }
    return archive->header.entry_count;  // not found -> empty range
}

// -----------------------------------------------------------------
// Search Implementation
// -----------------------------------------------------------------

int zim_search(zim_archive *archive, const char *query,
               zim_search_result *results, int max_results) {
    if (!query || !*query || max_results <= 0) {
        return 0;
    }

    if (!zim_load_title_ptrs(archive) || !zim_load_path_ptrs(archive)) {
        return 0;
    }

    // Find starting position for prefix search
    uint32_t start = zim_find_title_prefix(archive, query);
    if (start == UINT32_MAX) {
        return 0;
    }

    // Collect matching entries
    int count = 0;
    zim_entry entry;

    // Temporary storage for results before sorting
    typedef struct {
        uint32_t index;
        char path[256];
        char title[256];
        float score;
    } temp_result;

    temp_result *temp = malloc(max_results * 2 * sizeof(temp_result));
    if (!temp) {
        return 0;
    }
    int temp_count = 0;

    // Scan from the prefix position
    for (uint32_t i = start; i < archive->header.entry_count && temp_count < max_results * 2; i++) {
        uint32_t entry_idx = archive->title_ptrs[i];

        if (!zim_get_entry_by_index(archive, entry_idx, &entry)) {
            continue;
        }

        // Only include content entries (namespace 'C') that are not redirects
        if (entry.namespace_ != 'C') {
            continue;
        }

        // Check if title matches query
        float score = calculate_score(entry.title, query);
        if (score < 0.1f) {
            // If we're past the prefix match region and score is low, stop
            if (!str_prefix_icase(entry.title, query)) {
                break;
            }
            continue;
        }

        // Skip redirects for main results
        if (entry.is_redirect) {
            score *= 0.5f;  // Reduce score for redirects
        }

        // Store result
        temp[temp_count].index = entry.index;
        strncpy(temp[temp_count].path, entry.path, sizeof(temp[temp_count].path) - 1);
        temp[temp_count].path[sizeof(temp[temp_count].path) - 1] = '\0';
        strncpy(temp[temp_count].title, entry.title, sizeof(temp[temp_count].title) - 1);
        temp[temp_count].title[sizeof(temp[temp_count].title) - 1] = '\0';
        temp[temp_count].score = score;
        temp_count++;
    }

    // Sort by score (simple bubble sort for small arrays)
    for (int i = 0; i < temp_count - 1; i++) {
        for (int j = 0; j < temp_count - i - 1; j++) {
            if (temp[j].score < temp[j + 1].score) {
                temp_result t = temp[j];
                temp[j] = temp[j + 1];
                temp[j + 1] = t;
            }
        }
    }

    // Copy top results
    count = temp_count < max_results ? temp_count : max_results;
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
    // For suggestions, we want quick prefix matches
    if (!prefix || !*prefix || max_results <= 0) {
        return 0;
    }

    if (!zim_load_title_ptrs(archive) || !zim_load_path_ptrs(archive)) {
        return 0;
    }

    uint32_t start = zim_find_title_prefix(archive, prefix);
    if (start == UINT32_MAX || start >= archive->header.entry_count) {
        return 0;
    }

    int count = 0;
    zim_entry entry;

    for (uint32_t i = start; i < archive->header.entry_count && count < max_results; i++) {
        uint32_t entry_idx = archive->title_ptrs[i];

        if (!zim_get_entry_by_index(archive, entry_idx, &entry)) {
            continue;
        }

        // Only include content entries
        if (entry.namespace_ != 'C') {
            continue;
        }

        // Check if still a prefix match
        if (!str_prefix_icase(entry.title, prefix)) {
            break;
        }

        // Skip redirects for suggestions
        if (entry.is_redirect) {
            continue;
        }

        results[count].index = entry.index;
        results[count].path = strdup(entry.path);
        results[count].title = strdup(entry.title);
        results[count].score = 1.0f - (float)count / (float)max_results;
        count++;
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
