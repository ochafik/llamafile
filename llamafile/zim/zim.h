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

#ifndef LLAMAFILE_ZIM_ZIM_H_
#define LLAMAFILE_ZIM_ZIM_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// ZIM file format magic number: "ZIM\x04" in little-endian
#define ZIM_MAGIC 0x044D495A

// Compression types (stored in first byte of cluster)
#define ZIM_COMPRESSION_NONE     1
#define ZIM_COMPRESSION_ZLIB     4   // Also used for LZMA in older files
#define ZIM_COMPRESSION_ZSTD     5

// Special values
#define ZIM_ENTRY_REDIRECT       0xFFFF  // MIME type index for redirects
#define ZIM_NO_MAIN_PAGE         0xFFFFFFFF
#define ZIM_NO_LAYOUT_PAGE       0xFFFFFFFF

// Namespace characters
#define ZIM_NS_CONTENT           'C'  // Article content
#define ZIM_NS_METADATA          'M'  // Metadata entries
#define ZIM_NS_WELLKNOWN         'W'  // Well-known entries (favicon, mainpage)
#define ZIM_NS_SEARCH            'X'  // Search index (Xapian)

// -----------------------------------------------------------------
// ZIM Archive Handle
// -----------------------------------------------------------------

typedef struct zim_archive zim_archive;

// Open a ZIM archive from a file path
zim_archive *zim_open(const char *path);

// Open a ZIM archive from a file descriptor at given offset/size
// Useful for ZIM files embedded in an executable
zim_archive *zim_open_fd(int fd, uint64_t offset, uint64_t size);

// Close and free a ZIM archive
void zim_close(zim_archive *archive);

// Get last error message (thread-local)
const char *zim_error(void);

// -----------------------------------------------------------------
// Archive Metadata
// -----------------------------------------------------------------

// Get archive UUID as a 32-character hex string (plus null terminator)
const char *zim_get_uuid(zim_archive *archive);

// Get total number of entries (articles + redirects + metadata)
uint32_t zim_get_entry_count(zim_archive *archive);

// Get number of clusters
uint32_t zim_get_cluster_count(zim_archive *archive);

// Get main page entry index (or ZIM_NO_MAIN_PAGE if none)
uint32_t zim_get_main_page(zim_archive *archive);

// Get archive checksum (MD5, 16 bytes)
const uint8_t *zim_get_checksum(zim_archive *archive);

// -----------------------------------------------------------------
// Entry Access
// -----------------------------------------------------------------

typedef struct zim_entry {
    uint32_t index;           // Entry index in the archive
    uint16_t mimetype_idx;    // MIME type index (or ZIM_ENTRY_REDIRECT)
    uint8_t  namespace_;      // Namespace character (C, M, W, X, etc.)
    uint32_t cluster_idx;     // Cluster number (for content entries)
    uint32_t blob_idx;        // Blob number within cluster
    uint32_t redirect_idx;    // Redirect target (for redirect entries)
    const char *path;         // Entry path (null-terminated)
    const char *title;        // Entry title (null-terminated, may equal path)
    bool is_redirect;         // True if this is a redirect entry
} zim_entry;

// Get entry by index (0 to entry_count-1)
// Returns true on success, false on error
bool zim_get_entry_by_index(zim_archive *archive, uint32_t index, zim_entry *entry);

// Get entry by path (e.g., "A/Albert_Einstein")
// Returns true on success, false if not found
bool zim_get_entry_by_path(zim_archive *archive, const char *path, zim_entry *entry);

// Get the main page entry
bool zim_get_main_entry(zim_archive *archive, zim_entry *entry);

// Follow redirects to get the final content entry
bool zim_resolve_redirect(zim_archive *archive, zim_entry *entry);

// Get MIME type string for an entry
const char *zim_get_mimetype(zim_archive *archive, const zim_entry *entry);

// -----------------------------------------------------------------
// Content Retrieval
// -----------------------------------------------------------------

// Get raw content of an entry
// Returns allocated buffer (caller must free with zim_free)
// Sets *size to content size, returns NULL on error
void *zim_get_content(zim_archive *archive, const zim_entry *entry, size_t *size);

// Get content as text (strips HTML if content is HTML)
// Returns allocated buffer (caller must free with zim_free)
char *zim_get_content_text(zim_archive *archive, const zim_entry *entry, size_t *size);

// Free content buffer returned by zim_get_content or zim_get_content_text
void zim_free(void *ptr);

// -----------------------------------------------------------------
// Search
// -----------------------------------------------------------------

typedef struct zim_search_result {
    uint32_t index;          // Entry index
    const char *path;        // Entry path
    const char *title;       // Entry title
    float score;             // Relevance score (0.0-1.0)
} zim_search_result;

// Search entries by title prefix
// Returns number of results found (up to max_results)
// Results are sorted by relevance
int zim_search(zim_archive *archive, const char *query,
               zim_search_result *results, int max_results);

// Get title suggestions (autocomplete)
int zim_suggest(zim_archive *archive, const char *prefix,
                zim_search_result *results, int max_results);

// Free search results
void zim_search_free(zim_search_result *results, int count);

// -----------------------------------------------------------------
// Iteration
// -----------------------------------------------------------------

typedef struct zim_iterator zim_iterator;

// Create iterator over all entries in a namespace
// namespace_ can be ZIM_NS_CONTENT, ZIM_NS_METADATA, etc. or 0 for all
zim_iterator *zim_iterate(zim_archive *archive, char namespace_);

// Get next entry from iterator
// Returns true if an entry was returned, false if iteration complete
bool zim_iterator_next(zim_iterator *iter, zim_entry *entry);

// Free iterator
void zim_iterator_free(zim_iterator *iter);

// -----------------------------------------------------------------
// Utility Functions
// -----------------------------------------------------------------

// Convert HTML content to plain text
// Returns allocated buffer (caller must free with zim_free)
char *zim_html_to_text(const char *html, size_t html_size, size_t *text_size);

// URL-decode a path string in place
void zim_url_decode(char *path);

#ifdef __cplusplus
}
#endif

#endif // LLAMAFILE_ZIM_ZIM_H_
