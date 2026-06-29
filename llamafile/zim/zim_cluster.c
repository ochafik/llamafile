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

#include <stdlib.h>
#include <string.h>
#include <third_party/zlib/zlib.h>

// Include zstd single-file decoder
#define ZSTD_STATIC_LINKING_ONLY
#include "zstddeclib.c"

// Maximum decompressed cluster size (16 MB)
#define ZIM_MAX_CLUSTER_SIZE (16 * 1024 * 1024)

// -----------------------------------------------------------------
// Decompression
// -----------------------------------------------------------------

static bool zim_decompress_zlib(const uint8_t *src, size_t src_size,
                                uint8_t **dst, size_t *dst_size) {
    // Initial output buffer size
    size_t out_size = src_size * 4;
    if (out_size < 4096) out_size = 4096;
    if (out_size > ZIM_MAX_CLUSTER_SIZE) out_size = ZIM_MAX_CLUSTER_SIZE;

    uint8_t *out = malloc(out_size);
    if (!out) {
        zim_set_error("out of memory for zlib decompression");
        return false;
    }

    z_stream stream;
    memset(&stream, 0, sizeof(stream));
    stream.next_in = (Bytef *)src;
    stream.avail_in = src_size;
    stream.next_out = out;
    stream.avail_out = out_size;

    // Use raw deflate (no header)
    if (inflateInit2(&stream, -15) != Z_OK) {
        zim_set_error("inflateInit2 failed");
        free(out);
        return false;
    }

    int ret;
    while ((ret = inflate(&stream, Z_NO_FLUSH)) != Z_STREAM_END) {
        if (ret == Z_BUF_ERROR && stream.avail_out == 0) {
            // Need more output space
            size_t new_size = out_size * 2;
            if (new_size > ZIM_MAX_CLUSTER_SIZE) {
                zim_set_error("cluster exceeds maximum size");
                inflateEnd(&stream);
                free(out);
                return false;
            }
            uint8_t *new_out = realloc(out, new_size);
            if (!new_out) {
                zim_set_error("out of memory expanding zlib buffer");
                inflateEnd(&stream);
                free(out);
                return false;
            }
            stream.next_out = new_out + out_size;
            stream.avail_out = new_size - out_size;
            out = new_out;
            out_size = new_size;
        } else if (ret != Z_OK) {
            zim_set_error("inflate failed: %d", ret);
            inflateEnd(&stream);
            free(out);
            return false;
        }
    }

    inflateEnd(&stream);

    *dst = out;
    *dst_size = stream.total_out;
    return true;
}

static bool zim_decompress_zstd(const uint8_t *src, size_t src_size,
                                uint8_t **dst, size_t *dst_size) {
    unsigned long long frame_size = ZSTD_getFrameContentSize(src, src_size);
    if (frame_size == ZSTD_CONTENTSIZE_ERROR) {
        zim_set_error("invalid zstd frame");
        return false;
    }

    // Fast path: the frame embeds a known, sane decompressed size -> one shot.
    if (frame_size != ZSTD_CONTENTSIZE_UNKNOWN) {
        if (frame_size > ZIM_MAX_CLUSTER_SIZE) {
            zim_set_error("cluster exceeds maximum size");
            return false;
        }
        uint8_t *out = malloc(frame_size ? (size_t)frame_size : 1);
        if (!out) {
            zim_set_error("out of memory for zstd decompression");
            return false;
        }
        size_t result = ZSTD_decompress(out, (size_t)frame_size, src, src_size);
        if (ZSTD_isError(result)) {
            zim_set_error("zstd decompress failed: %s", ZSTD_getErrorName(result));
            free(out);
            return false;
        }
        *dst = out;
        *dst_size = result;
        return true;
    }

    // Real Wikipedia ZIMs write clusters with an UNKNOWN frame content size
    // (the writer streams blobs into the frame), so a fixed size estimate is
    // unreliable and a too-small estimate makes ZSTD_decompress fail outright.
    // Stream-decompress instead, growing the output buffer as needed.
    ZSTD_DStream *ds = ZSTD_createDStream();
    if (!ds) {
        zim_set_error("out of memory for zstd stream");
        return false;
    }
    ZSTD_initDStream(ds);

    size_t cap = src_size * 4;
    if (cap < 65536) cap = 65536;
    if (cap > ZIM_MAX_CLUSTER_SIZE) cap = ZIM_MAX_CLUSTER_SIZE;
    uint8_t *out = malloc(cap);
    if (!out) {
        ZSTD_freeDStream(ds);
        zim_set_error("out of memory for zstd decompression");
        return false;
    }

    ZSTD_inBuffer in = { src, src_size, 0 };
    size_t produced = 0;
    for (;;) {
        if (produced == cap) {
            size_t new_cap = cap * 2;
            if (new_cap > ZIM_MAX_CLUSTER_SIZE) new_cap = ZIM_MAX_CLUSTER_SIZE;
            if (new_cap == cap) {
                zim_set_error("cluster exceeds maximum size");
                free(out);
                ZSTD_freeDStream(ds);
                return false;
            }
            uint8_t *new_out = realloc(out, new_cap);
            if (!new_out) {
                zim_set_error("out of memory expanding zstd buffer");
                free(out);
                ZSTD_freeDStream(ds);
                return false;
            }
            out = new_out;
            cap = new_cap;
        }
        ZSTD_outBuffer ob = { out, cap, produced };
        size_t ret = ZSTD_decompressStream(ds, &ob, &in);
        if (ZSTD_isError(ret)) {
            zim_set_error("zstd decompress failed: %s", ZSTD_getErrorName(ret));
            free(out);
            ZSTD_freeDStream(ds);
            return false;
        }
        produced = ob.pos;
        if (ret == 0) break;               // frame fully decoded
        if (in.pos == in.size && ob.pos < cap) break;  // input drained
    }
    ZSTD_freeDStream(ds);

    *dst = out;
    *dst_size = produced;
    return true;
}

bool zim_decompress_cluster(const uint8_t *compressed, size_t compressed_size,
                            uint8_t **decompressed, size_t *decompressed_size) {
    if (compressed_size < 1) {
        zim_set_error("cluster too small");
        return false;
    }

    uint8_t compression_type = compressed[0] & 0x0F;
    bool extended = (compressed[0] & 0x10) != 0;

    // Skip compression byte
    const uint8_t *data = compressed + 1;
    size_t data_size = compressed_size - 1;

    switch (compression_type) {
    case ZIM_COMPRESSION_NONE:
        // Uncompressed - just copy
        *decompressed = malloc(data_size);
        if (!*decompressed) {
            zim_set_error("out of memory");
            return false;
        }
        memcpy(*decompressed, data, data_size);
        *decompressed_size = data_size;
        return true;

    case ZIM_COMPRESSION_ZLIB:
        return zim_decompress_zlib(data, data_size, decompressed, decompressed_size);

    case ZIM_COMPRESSION_ZSTD:
        return zim_decompress_zstd(data, data_size, decompressed, decompressed_size);

    default:
        zim_set_error("unsupported compression type: %d", compression_type);
        return false;
    }

    (void)extended;  // TODO: Handle extended clusters if needed
}

// -----------------------------------------------------------------
// Cluster Cache
// -----------------------------------------------------------------

static zim_cluster_cache_entry *zim_find_cache_entry(zim_archive *archive,
                                                      uint32_t cluster_idx) {
    // Check if already cached
    for (int i = 0; i < ZIM_CLUSTER_CACHE_SIZE; i++) {
        if (archive->cluster_cache[i].cluster_idx == cluster_idx) {
            archive->cluster_cache[i].last_access = ++archive->access_counter;
            return &archive->cluster_cache[i];
        }
    }
    return NULL;
}

static zim_cluster_cache_entry *zim_get_free_cache_entry(zim_archive *archive) {
    // Find empty or LRU entry
    zim_cluster_cache_entry *oldest = &archive->cluster_cache[0];

    for (int i = 0; i < ZIM_CLUSTER_CACHE_SIZE; i++) {
        if (archive->cluster_cache[i].cluster_idx == UINT32_MAX) {
            return &archive->cluster_cache[i];
        }
        if (archive->cluster_cache[i].last_access < oldest->last_access) {
            oldest = &archive->cluster_cache[i];
        }
    }

    // Evict oldest entry
    free(oldest->data);
    free(oldest->blob_offsets);
    oldest->data = NULL;
    oldest->blob_offsets = NULL;
    oldest->cluster_idx = UINT32_MAX;

    return oldest;
}

static bool zim_parse_blob_offsets(zim_cluster_cache_entry *entry, bool extended) {
    // First offset tells us how many blobs there are
    // Offset size is 4 bytes (or 8 if extended)
    size_t offset_size = extended ? 8 : 4;

    if (entry->size < offset_size) {
        zim_set_error("cluster too small for blob offsets");
        return false;
    }

    // Read first offset to determine blob count
    uint64_t first_offset;
    if (extended) {
        memcpy(&first_offset, entry->data, 8);
    } else {
        uint32_t offset32;
        memcpy(&offset32, entry->data, 4);
        first_offset = offset32;
    }

    // Number of blobs = first_offset / offset_size - 1
    // (because there's one extra offset at the end)
    if (first_offset < offset_size || first_offset > entry->size) {
        zim_set_error("invalid first blob offset");
        return false;
    }

    entry->blob_count = first_offset / offset_size - 1;
    entry->blob_offsets = malloc((entry->blob_count + 1) * sizeof(uint64_t));
    if (!entry->blob_offsets) {
        zim_set_error("out of memory for blob offsets");
        return false;
    }

    // Read all offsets
    for (uint32_t i = 0; i <= entry->blob_count; i++) {
        if (extended) {
            memcpy(&entry->blob_offsets[i], entry->data + i * 8, 8);
        } else {
            uint32_t offset32;
            memcpy(&offset32, entry->data + i * 4, 4);
            entry->blob_offsets[i] = offset32;
        }
    }

    return true;
}

bool zim_get_cluster(zim_archive *archive, uint32_t cluster_idx,
                     uint8_t **data, size_t *size,
                     uint64_t **blob_offsets, uint32_t *blob_count) {
    // Check cache
    zim_cluster_cache_entry *entry = zim_find_cache_entry(archive, cluster_idx);
    if (entry) {
        *data = entry->data;
        *size = entry->size;
        *blob_offsets = entry->blob_offsets;
        *blob_count = entry->blob_count;
        return true;
    }

    // Load cluster pointers if needed
    if (!zim_load_cluster_ptrs(archive)) {
        return false;
    }

    if (cluster_idx >= archive->header.cluster_count) {
        zim_set_error("cluster index out of range");
        return false;
    }

    // Get cluster offset and size
    uint64_t cluster_offset = archive->cluster_ptrs[cluster_idx];
    uint64_t next_offset;
    if (cluster_idx + 1 < archive->header.cluster_count) {
        next_offset = archive->cluster_ptrs[cluster_idx + 1];
    } else {
        // Last cluster extends to checksum position
        next_offset = archive->header.checksum_pos;
    }

    if (next_offset <= cluster_offset) {
        zim_set_error("invalid cluster bounds");
        return false;
    }

    size_t compressed_size = next_offset - cluster_offset;

    // Read compressed cluster
    uint8_t *compressed = malloc(compressed_size);
    if (!compressed) {
        zim_set_error("out of memory for compressed cluster");
        return false;
    }

    if (!zim_read_at(archive, cluster_offset, compressed, compressed_size)) {
        free(compressed);
        return false;
    }

    // Decompress
    uint8_t *decompressed;
    size_t decompressed_size;
    bool extended = (compressed[0] & 0x10) != 0;

    if (!zim_decompress_cluster(compressed, compressed_size,
                                &decompressed, &decompressed_size)) {
        free(compressed);
        return false;
    }
    free(compressed);

    // Get cache entry
    entry = zim_get_free_cache_entry(archive);
    entry->cluster_idx = cluster_idx;
    entry->data = decompressed;
    entry->size = decompressed_size;
    entry->last_access = ++archive->access_counter;

    // Parse blob offsets
    if (!zim_parse_blob_offsets(entry, extended)) {
        free(decompressed);
        entry->data = NULL;
        entry->cluster_idx = UINT32_MAX;
        return false;
    }

    *data = entry->data;
    *size = entry->size;
    *blob_offsets = entry->blob_offsets;
    *blob_count = entry->blob_count;
    return true;
}

// -----------------------------------------------------------------
// Content Retrieval
// -----------------------------------------------------------------

void *zim_get_content(zim_archive *archive, const zim_entry *entry, size_t *out_size) {
    if (entry->is_redirect) {
        zim_set_error("cannot get content of redirect entry");
        return NULL;
    }

    uint8_t *cluster_data;
    size_t cluster_size;
    uint64_t *blob_offsets;
    uint32_t blob_count;

    if (!zim_get_cluster(archive, entry->cluster_idx,
                         &cluster_data, &cluster_size,
                         &blob_offsets, &blob_count)) {
        return NULL;
    }

    if (entry->blob_idx >= blob_count) {
        zim_set_error("blob index out of range");
        return NULL;
    }

    uint64_t blob_start = blob_offsets[entry->blob_idx];
    uint64_t blob_end = blob_offsets[entry->blob_idx + 1];

    if (blob_end < blob_start || blob_end > cluster_size) {
        zim_set_error("invalid blob bounds");
        return NULL;
    }

    size_t blob_size = blob_end - blob_start;
    uint8_t *content = malloc(blob_size + 1);  // +1 for null terminator
    if (!content) {
        zim_set_error("out of memory for blob content");
        return NULL;
    }

    memcpy(content, cluster_data + blob_start, blob_size);
    content[blob_size] = '\0';  // Null terminate for convenience

    *out_size = blob_size;
    return content;
}
