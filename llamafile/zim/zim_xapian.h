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

#ifndef LLAMAFILE_ZIM_ZIM_XAPIAN_H_
#define LLAMAFILE_ZIM_ZIM_XAPIAN_H_

// Read-only BM25 reader for the Xapian *Glass* full-text index that Wikipedia
// ZIM archives embed at the X/fulltext/xapian (and X/title/xapian) entries.
//
// Pure C, no libxapian / no ICU. It navigates the Glass single-file B-tree
// in-place (the index blob is pulled out of the ZIM via the normal zim reader
// and kept in memory), decodes the POSTLIST + DOCDATA tables, and ranks docs
// with Okapi BM25 (k1=1.2, b=0.75). Document lengths come from the index's own
// doclen postlist; the average length from the version-header collection stats.
//
// This is the search *backend* only. Tokenization is a simple ASCII/UTF-8 word
// split with ASCII lowercasing (no stemming) — matching the unstemmed terms the
// ZIM indexer stores. Multi-term queries are scored as an OR with per-term IDF,
// so frequent words naturally contribute little (e.g. "TNT inventor" ranks the
// TNT article, not every page mentioning "inventor").

#include "zim.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct zim_xapian zim_xapian;

typedef struct zim_xapian_hit {
    char *path;      // stored document path, e.g. "C/Tokyo" (caller frees via
                     // zim_xapian_free_hits)
    char *title;     // ZIM entry title for that path (or a copy of path if the
                     // entry can't be resolved)
    float score;     // BM25 score (>0)
    uint32_t docid;  // internal Xapian document id (useful for debugging)
} zim_xapian_hit;

// Open the Glass index stored at `entry_path` (e.g. "X/fulltext/xapian" or
// "X/title/xapian") inside the already-open ZIM archive `z`. The index blob is
// fetched through the normal zim reader and kept resident for the lifetime of
// the handle. Returns NULL and sets zim_xapian_error() if the entry is missing
// or is not a valid Xapian Glass index. `z` must outlive the returned handle.
zim_xapian *zim_xapian_open(zim_archive *z, const char *entry_path);

// Run a BM25 query. `query` is tokenized into lowercased terms; each term's
// postlist (including continuation chunks) contributes IDF-weighted BM25 mass
// to the documents it occurs in. The top `limit` documents are returned via
// `*hits_out` (newly allocated, sorted by descending score). Returns the number
// of hits (0..limit), or -1 on error. The caller must release the result with
// zim_xapian_free_hits().
int zim_xapian_search(zim_xapian *idx, const char *query, int limit,
                      zim_xapian_hit **hits_out);

// Free a hit array returned by zim_xapian_search.
void zim_xapian_free_hits(zim_xapian_hit *hits, int count);

// Close and free a handle (releases the resident index blob).
void zim_xapian_close(zim_xapian *idx);

// Last error message (thread-local). Reuses the zim reader's error channel.
const char *zim_xapian_error(void);

// --- Introspection (mostly for tests) -----------------------------------
uint32_t zim_xapian_doccount(const zim_xapian *idx);
double zim_xapian_avg_doclen(const zim_xapian *idx);
// Document frequency (number of documents) for a single already-lowercased
// term, or 0 if the term is absent. Does not score.
uint32_t zim_xapian_termfreq(zim_xapian *idx, const char *term);

#ifdef __cplusplus
}
#endif

#endif // LLAMAFILE_ZIM_ZIM_XAPIAN_H_
