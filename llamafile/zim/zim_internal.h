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

#ifndef LLAMAFILE_ZIM_ZIM_INTERNAL_H_
#define LLAMAFILE_ZIM_ZIM_INTERNAL_H_

#include "zim.h"
#include <stdio.h>

// -----------------------------------------------------------------
// ZIM File Header (80 bytes, little-endian)
// -----------------------------------------------------------------

#pragma pack(push, 1)

typedef struct zim_header {
    uint32_t magic;           // 0x044D495A ("ZIM\x04")
    uint16_t major_version;   // Major version (currently 5 or 6)
    uint16_t minor_version;   // Minor version
    uint8_t  uuid[16];        // Unique identifier
    uint32_t entry_count;     // Number of entries
    uint32_t cluster_count;   // Number of clusters
    uint64_t path_ptr_pos;    // Position of path pointer list
    uint64_t title_ptr_pos;   // Position of title pointer list
    uint64_t cluster_ptr_pos; // Position of cluster pointer list
    uint64_t mime_list_pos;   // Position of MIME type list
    uint32_t main_page;       // Main page entry index (or 0xFFFFFFFF)
    uint32_t layout_page;     // Layout page entry index (or 0xFFFFFFFF)
    uint64_t checksum_pos;    // Position of MD5 checksum
} zim_header;

// Directory entry for content (variable length)
typedef struct zim_dirent_content {
    uint16_t mimetype_idx;    // MIME type index
    uint8_t  param_len;       // Length of extra parameters (usually 0)
    uint8_t  namespace_;      // Namespace character
    uint32_t revision;        // Revision number
    uint32_t cluster_idx;     // Cluster number
    uint32_t blob_idx;        // Blob number within cluster
    // Followed by: path (null-terminated), title (null-terminated)
} zim_dirent_content;

// Directory entry for redirect (variable length)
typedef struct zim_dirent_redirect {
    uint16_t mimetype_idx;    // Always 0xFFFF for redirects
    uint8_t  param_len;       // Length of extra parameters
    uint8_t  namespace_;      // Namespace character
    uint32_t revision;        // Revision number
    uint32_t redirect_idx;    // Target entry index
    // Followed by: path (null-terminated), title (null-terminated)
} zim_dirent_redirect;

#pragma pack(pop)

// -----------------------------------------------------------------
// Cluster Cache Entry
// -----------------------------------------------------------------

#define ZIM_CLUSTER_CACHE_SIZE 16

typedef struct zim_cluster_cache_entry {
    uint32_t cluster_idx;     // Cluster index (or UINT32_MAX if empty)
    uint8_t *data;            // Decompressed cluster data
    size_t   size;            // Size of decompressed data
    uint32_t blob_count;      // Number of blobs in cluster
    uint64_t *blob_offsets;   // Offsets to each blob within data
    uint64_t last_access;     // For LRU eviction
} zim_cluster_cache_entry;

// -----------------------------------------------------------------
// ZIM Archive Structure
// -----------------------------------------------------------------

struct zim_archive {
    // File handle
    int fd;                           // File descriptor
    uint64_t file_offset;             // Offset within file (for embedded ZIM)
    uint64_t file_size;               // Size of ZIM data
    bool owns_fd;                     // True if we should close fd

    // Parsed header
    zim_header header;
    char uuid_str[33];                // UUID as hex string

    // MIME types (null-separated string list)
    char *mime_list;
    size_t mime_list_size;
    const char **mime_types;          // Pointers into mime_list
    int mime_type_count;

    // Cluster cache (LRU)
    zim_cluster_cache_entry cluster_cache[ZIM_CLUSTER_CACHE_SIZE];
    uint64_t access_counter;

    // Path pointer cache (mmap'd or loaded)
    uint64_t *path_ptrs;              // Array of entry offsets
    bool path_ptrs_mmaped;

    // Title pointer cache: array of entry indices in title order. Sourced from
    // the header title-pointer list (old/withns ZIMs) or, when that list is
    // absent (modern nons Wikipedia ZIMs), from X/listing/titleOrdered/v1.
    uint32_t *title_ptrs;             // Array of entry indices (sorted by title)
    uint32_t title_ptr_count;        // Number of valid entries in title_ptrs
    bool title_ptrs_mmaped;

    // Cluster pointer cache
    uint64_t *cluster_ptrs;           // Array of cluster offsets
    bool cluster_ptrs_mmaped;

    // MD5 checksum
    uint8_t checksum[16];
    bool checksum_loaded;

    // String buffers for entry data
    char *entry_path_buf;
    size_t entry_path_buf_size;
    char *entry_title_buf;
    size_t entry_title_buf_size;
};

// -----------------------------------------------------------------
// Internal Functions
// -----------------------------------------------------------------

// Set error message (thread-local)
void zim_set_error(const char *fmt, ...);

// Read bytes from archive at given offset
bool zim_read_at(zim_archive *archive, uint64_t offset, void *buf, size_t size);

// Parse MIME type list
bool zim_parse_mime_list(zim_archive *archive);

// Load pointer lists
bool zim_load_path_ptrs(zim_archive *archive);
bool zim_load_title_ptrs(zim_archive *archive);
bool zim_load_cluster_ptrs(zim_archive *archive);

// Read directory entry at given offset
bool zim_read_dirent(zim_archive *archive, uint64_t offset, zim_entry *entry);

// Get decompressed cluster data
// Returns pointer to cached data (do not free)
bool zim_get_cluster(zim_archive *archive, uint32_t cluster_idx,
                     uint8_t **data, size_t *size,
                     uint64_t **blob_offsets, uint32_t *blob_count);

// Decompress a cluster
bool zim_decompress_cluster(const uint8_t *compressed, size_t compressed_size,
                            uint8_t **decompressed, size_t *decompressed_size);

// Binary search in path pointer list
// Returns entry index or UINT32_MAX if not found
uint32_t zim_find_path(zim_archive *archive, const char *path);

// Binary search in title pointer list for prefix matching
// Returns starting index for prefix matches
uint32_t zim_find_title_prefix(zim_archive *archive, const char *prefix);

// Read from file descriptor at offset (handles partial reads)
ssize_t zim_pread(int fd, void *buf, size_t count, off_t offset);

#endif // LLAMAFILE_ZIM_ZIM_INTERNAL_H_
