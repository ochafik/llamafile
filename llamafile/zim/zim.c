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

#include "zim.h"
#include "zim_internal.h"

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

// Thread-local error message
static _Thread_local char zim_error_buf[256];

void zim_set_error(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(zim_error_buf, sizeof(zim_error_buf), fmt, ap);
    va_end(ap);
}

const char *zim_error(void) {
    return zim_error_buf;
}

// -----------------------------------------------------------------
// File I/O
// -----------------------------------------------------------------

ssize_t zim_pread(int fd, void *buf, size_t count, off_t offset) {
    size_t total = 0;
    while (total < count) {
        ssize_t n = pread(fd, (char *)buf + total, count - total, offset + total);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (n == 0) break;  // EOF
        total += n;
    }
    return total;
}

bool zim_read_at(zim_archive *archive, uint64_t offset, void *buf, size_t size) {
    uint64_t abs_offset = archive->file_offset + offset;
    ssize_t n = zim_pread(archive->fd, buf, size, abs_offset);
    if (n < 0) {
        zim_set_error("read error at offset %llu: %s",
                      (unsigned long long)offset, strerror(errno));
        return false;
    }
    if ((size_t)n < size) {
        zim_set_error("unexpected EOF at offset %llu", (unsigned long long)offset);
        return false;
    }
    return true;
}

// -----------------------------------------------------------------
// Archive Opening/Closing
// -----------------------------------------------------------------

static void zim_init_archive(zim_archive *archive) {
    memset(archive, 0, sizeof(*archive));
    archive->fd = -1;
    for (int i = 0; i < ZIM_CLUSTER_CACHE_SIZE; i++) {
        archive->cluster_cache[i].cluster_idx = UINT32_MAX;
    }
}

static void zim_free_cluster_cache(zim_archive *archive) {
    for (int i = 0; i < ZIM_CLUSTER_CACHE_SIZE; i++) {
        if (archive->cluster_cache[i].data) {
            free(archive->cluster_cache[i].data);
            free(archive->cluster_cache[i].blob_offsets);
        }
    }
}

zim_archive *zim_open(const char *path) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        zim_set_error("cannot open %s: %s", path, strerror(errno));
        return NULL;
    }

    struct stat st;
    if (fstat(fd, &st) < 0) {
        zim_set_error("cannot stat %s: %s", path, strerror(errno));
        close(fd);
        return NULL;
    }

    zim_archive *archive = zim_open_fd(fd, 0, st.st_size);
    if (archive) {
        archive->owns_fd = true;
    } else {
        close(fd);
    }
    return archive;
}

zim_archive *zim_open_fd(int fd, uint64_t offset, uint64_t size) {
    zim_archive *archive = malloc(sizeof(zim_archive));
    if (!archive) {
        zim_set_error("out of memory");
        return NULL;
    }
    zim_init_archive(archive);

    archive->fd = fd;
    archive->file_offset = offset;
    archive->file_size = size;
    archive->owns_fd = false;

    // Read and validate header
    if (!zim_read_at(archive, 0, &archive->header, sizeof(archive->header))) {
        free(archive);
        return NULL;
    }

    if (archive->header.magic != ZIM_MAGIC) {
        zim_set_error("invalid ZIM magic: 0x%08X (expected 0x%08X)",
                      archive->header.magic, ZIM_MAGIC);
        free(archive);
        return NULL;
    }

    // Convert UUID to hex string
    for (int i = 0; i < 16; i++) {
        snprintf(archive->uuid_str + i * 2, 3, "%02x", archive->header.uuid[i]);
    }

    // Parse MIME type list
    if (!zim_parse_mime_list(archive)) {
        zim_close(archive);
        return NULL;
    }

    return archive;
}

void zim_close(zim_archive *archive) {
    if (!archive) return;

    zim_free_cluster_cache(archive);

    if (archive->path_ptrs) {
        if (archive->path_ptrs_mmaped) {
            // munmap
        } else {
            free(archive->path_ptrs);
        }
    }

    if (archive->title_ptrs) {
        if (archive->title_ptrs_mmaped) {
            // munmap
        } else {
            free(archive->title_ptrs);
        }
    }

    if (archive->cluster_ptrs) {
        if (archive->cluster_ptrs_mmaped) {
            // munmap
        } else {
            free(archive->cluster_ptrs);
        }
    }

    free(archive->mime_list);
    free(archive->mime_types);
    free(archive->entry_path_buf);
    free(archive->entry_title_buf);

    if (archive->owns_fd && archive->fd >= 0) {
        close(archive->fd);
    }

    free(archive);
}

// -----------------------------------------------------------------
// Archive Metadata
// -----------------------------------------------------------------

const char *zim_get_uuid(zim_archive *archive) {
    return archive->uuid_str;
}

uint32_t zim_get_entry_count(zim_archive *archive) {
    return archive->header.entry_count;
}

uint32_t zim_get_cluster_count(zim_archive *archive) {
    return archive->header.cluster_count;
}

uint32_t zim_get_main_page(zim_archive *archive) {
    return archive->header.main_page;
}

const uint8_t *zim_get_checksum(zim_archive *archive) {
    if (!archive->checksum_loaded) {
        if (archive->header.checksum_pos != 0 &&
            archive->header.checksum_pos < archive->file_size) {
            zim_read_at(archive, archive->header.checksum_pos,
                        archive->checksum, 16);
            archive->checksum_loaded = true;
        }
    }
    return archive->checksum_loaded ? archive->checksum : NULL;
}

// -----------------------------------------------------------------
// MIME Type List Parsing
// -----------------------------------------------------------------

bool zim_parse_mime_list(zim_archive *archive) {
    // MIME list starts right after the header (at mime_list_pos)
    // and ends at the first pointer list
    uint64_t mime_start = archive->header.mime_list_pos;
    uint64_t mime_end = archive->header.path_ptr_pos;

    if (mime_end <= mime_start) {
        zim_set_error("invalid MIME list bounds");
        return false;
    }

    size_t mime_size = mime_end - mime_start;
    archive->mime_list = malloc(mime_size);
    if (!archive->mime_list) {
        zim_set_error("out of memory for MIME list");
        return false;
    }

    if (!zim_read_at(archive, mime_start, archive->mime_list, mime_size)) {
        free(archive->mime_list);
        archive->mime_list = NULL;
        return false;
    }
    archive->mime_list_size = mime_size;

    // Count MIME types (null-separated, double-null terminated)
    int count = 0;
    for (size_t i = 0; i < mime_size; i++) {
        if (archive->mime_list[i] == '\0') {
            count++;
            if (i + 1 < mime_size && archive->mime_list[i + 1] == '\0') {
                break;  // Double null = end of list
            }
        }
    }

    archive->mime_types = malloc(count * sizeof(char *));
    if (!archive->mime_types) {
        zim_set_error("out of memory for MIME type pointers");
        return false;
    }

    // Build pointers
    int idx = 0;
    archive->mime_types[idx++] = archive->mime_list;
    for (size_t i = 0; i < mime_size && idx < count; i++) {
        if (archive->mime_list[i] == '\0') {
            if (i + 1 < mime_size && archive->mime_list[i + 1] != '\0') {
                archive->mime_types[idx++] = archive->mime_list + i + 1;
            }
        }
    }
    archive->mime_type_count = idx;

    return true;
}

const char *zim_get_mimetype(zim_archive *archive, const zim_entry *entry) {
    if (entry->is_redirect) {
        return "text/html";  // Redirects are conceptually HTML
    }
    if (entry->mimetype_idx >= archive->mime_type_count) {
        return "application/octet-stream";
    }
    return archive->mime_types[entry->mimetype_idx];
}

// -----------------------------------------------------------------
// Pointer List Loading
// -----------------------------------------------------------------

bool zim_load_path_ptrs(zim_archive *archive) {
    if (archive->path_ptrs) return true;

    size_t count = archive->header.entry_count;
    size_t size = count * sizeof(uint64_t);

    archive->path_ptrs = malloc(size);
    if (!archive->path_ptrs) {
        zim_set_error("out of memory for path pointers");
        return false;
    }

    if (!zim_read_at(archive, archive->header.path_ptr_pos,
                     archive->path_ptrs, size)) {
        free(archive->path_ptrs);
        archive->path_ptrs = NULL;
        return false;
    }

    return true;
}

// Load the title order from the modern X/listing/titleOrdered/v{1,0} entry.
// This is a blob holding a little-endian uint32 array of entry indices in
// title order. Used when the header title-pointer list is absent (which is the
// case for current "nons" Wikipedia ZIMs, where title_ptr_pos is UINT64_MAX).
static bool zim_load_title_listing(zim_archive *archive) {
    zim_entry e;
    if (!zim_get_entry_by_path(archive, "X/listing/titleOrdered/v1", &e) &&
        !zim_get_entry_by_path(archive, "X/listing/titleOrdered/v0", &e)) {
        zim_set_error("no title index: header title list absent and no "
                      "X/listing/titleOrdered/v{1,0} entry");
        return false;
    }
    if (e.is_redirect && !zim_resolve_redirect(archive, &e)) {
        return false;
    }

    size_t blob_size = 0;
    void *blob = zim_get_content(archive, &e, &blob_size);
    if (!blob) {
        return false;  // error already set
    }

    size_t count = blob_size / sizeof(uint32_t);
    archive->title_ptrs = malloc(count ? count * sizeof(uint32_t) : 1);
    if (!archive->title_ptrs) {
        zim_free(blob);
        zim_set_error("out of memory for title listing");
        return false;
    }
    // The on-disk listing is little-endian uint32, matching the in-memory
    // representation on the platforms cosmocc targets.
    memcpy(archive->title_ptrs, blob, count * sizeof(uint32_t));
    archive->title_ptr_count = (uint32_t)count;
    zim_free(blob);
    return true;
}

bool zim_load_title_ptrs(zim_archive *archive) {
    if (archive->title_ptrs) return true;

    // The header title-pointer list is present and usable only when its
    // position points inside the file and the whole array fits. Modern ZIMs set
    // title_ptr_pos to UINT64_MAX (no header title list) -> use the listing.
    uint64_t pos = archive->header.title_ptr_pos;
    uint64_t need = (uint64_t)archive->header.entry_count * sizeof(uint32_t);
    bool header_ok = pos != 0 && pos != UINT64_MAX &&
                     pos < archive->file_size &&
                     pos + need <= archive->file_size;

    if (!header_ok) {
        return zim_load_title_listing(archive);
    }

    size_t count = archive->header.entry_count;
    size_t size = count * sizeof(uint32_t);

    archive->title_ptrs = malloc(size);
    if (!archive->title_ptrs) {
        zim_set_error("out of memory for title pointers");
        return false;
    }

    if (!zim_read_at(archive, pos, archive->title_ptrs, size)) {
        free(archive->title_ptrs);
        archive->title_ptrs = NULL;
        return false;
    }
    archive->title_ptr_count = (uint32_t)count;

    return true;
}

bool zim_load_cluster_ptrs(zim_archive *archive) {
    if (archive->cluster_ptrs) return true;

    size_t count = archive->header.cluster_count;
    size_t size = count * sizeof(uint64_t);

    archive->cluster_ptrs = malloc(size);
    if (!archive->cluster_ptrs) {
        zim_set_error("out of memory for cluster pointers");
        return false;
    }

    if (!zim_read_at(archive, archive->header.cluster_ptr_pos,
                     archive->cluster_ptrs, size)) {
        free(archive->cluster_ptrs);
        archive->cluster_ptrs = NULL;
        return false;
    }

    return true;
}

// -----------------------------------------------------------------
// Directory Entry Reading
// -----------------------------------------------------------------

bool zim_read_dirent(zim_archive *archive, uint64_t offset, zim_entry *entry) {
    // Read fixed part of directory entry
    uint8_t buf[256];
    if (!zim_read_at(archive, offset, buf, 12)) {
        return false;
    }

    entry->mimetype_idx = buf[0] | (buf[1] << 8);
    uint8_t param_len = buf[2];
    entry->namespace_ = buf[3];
    // revision at buf[4..7] - we skip it
    entry->is_redirect = (entry->mimetype_idx == ZIM_ENTRY_REDIRECT);

    if (entry->is_redirect) {
        entry->redirect_idx = buf[8] | (buf[9] << 8) | (buf[10] << 16) | (buf[11] << 24);
        entry->cluster_idx = 0;
        entry->blob_idx = 0;
        offset += 12;
    } else {
        entry->cluster_idx = buf[8] | (buf[9] << 8) | (buf[10] << 16) | (buf[11] << 24);
        // Need to read 4 more bytes for blob_idx
        if (!zim_read_at(archive, offset + 12, buf + 12, 4)) {
            return false;
        }
        entry->blob_idx = buf[12] | (buf[13] << 8) | (buf[14] << 16) | (buf[15] << 24);
        offset += 16;
    }

    // Skip extra parameters
    offset += param_len;

    // Read path and title (null-terminated strings)
    // We read in chunks to find the null terminators
    size_t path_len = 0;
    size_t title_len = 0;
    size_t buf_size = 256;

    // Ensure we have enough buffer
    if (archive->entry_path_buf_size < buf_size) {
        free(archive->entry_path_buf);
        archive->entry_path_buf = malloc(buf_size);
        archive->entry_path_buf_size = buf_size;
    }
    if (archive->entry_title_buf_size < buf_size) {
        free(archive->entry_title_buf);
        archive->entry_title_buf = malloc(buf_size);
        archive->entry_title_buf_size = buf_size;
    }

    // Read path
    if (!zim_read_at(archive, offset, archive->entry_path_buf, buf_size)) {
        return false;
    }
    path_len = strnlen(archive->entry_path_buf, buf_size);
    if (path_len == buf_size) {
        zim_set_error("path too long");
        return false;
    }

    // Read title (comes after path's null terminator)
    offset += path_len + 1;
    if (!zim_read_at(archive, offset, archive->entry_title_buf, buf_size)) {
        return false;
    }
    title_len = strnlen(archive->entry_title_buf, buf_size);

    entry->path = archive->entry_path_buf;
    entry->title = archive->entry_title_buf;
    if (title_len == 0) {
        entry->title = entry->path;  // Title defaults to path if empty
    }

    return true;
}

// -----------------------------------------------------------------
// Entry Access
// -----------------------------------------------------------------

bool zim_get_entry_by_index(zim_archive *archive, uint32_t index, zim_entry *entry) {
    if (index >= archive->header.entry_count) {
        zim_set_error("entry index %u out of range", index);
        return false;
    }

    if (!zim_load_path_ptrs(archive)) {
        return false;
    }

    entry->index = index;
    return zim_read_dirent(archive, archive->path_ptrs[index], entry);
}

uint32_t zim_find_path(zim_archive *archive, const char *path) {
    if (!zim_load_path_ptrs(archive)) {
        return UINT32_MAX;
    }

    // Binary search for path
    uint32_t lo = 0;
    uint32_t hi = archive->header.entry_count;
    zim_entry entry;

    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        if (!zim_read_dirent(archive, archive->path_ptrs[mid], &entry)) {
            return UINT32_MAX;
        }

        // Build full path with namespace for comparison
        char full_path[512];
        snprintf(full_path, sizeof(full_path), "%c/%s", entry.namespace_, entry.path);

        int cmp = strcmp(path, full_path);
        if (cmp == 0) {
            return mid;
        } else if (cmp < 0) {
            hi = mid;
        } else {
            lo = mid + 1;
        }
    }

    return UINT32_MAX;
}

bool zim_get_entry_by_path(zim_archive *archive, const char *path, zim_entry *entry) {
    uint32_t index = zim_find_path(archive, path);
    if (index == UINT32_MAX) {
        zim_set_error("path not found: %s", path);
        return false;
    }
    return zim_get_entry_by_index(archive, index, entry);
}

bool zim_get_main_entry(zim_archive *archive, zim_entry *entry) {
    if (archive->header.main_page == ZIM_NO_MAIN_PAGE) {
        zim_set_error("archive has no main page");
        return false;
    }
    return zim_get_entry_by_index(archive, archive->header.main_page, entry);
}

bool zim_resolve_redirect(zim_archive *archive, zim_entry *entry) {
    int max_redirects = 10;
    while (entry->is_redirect && max_redirects-- > 0) {
        if (!zim_get_entry_by_index(archive, entry->redirect_idx, entry)) {
            return false;
        }
    }
    if (entry->is_redirect) {
        zim_set_error("too many redirects");
        return false;
    }
    return true;
}

// -----------------------------------------------------------------
// Memory Management
// -----------------------------------------------------------------

void zim_free(void *ptr) {
    free(ptr);
}

// -----------------------------------------------------------------
// URL Decoding
// -----------------------------------------------------------------

static int hex_digit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

void zim_url_decode(char *path) {
    char *src = path;
    char *dst = path;
    while (*src) {
        if (*src == '%' && src[1] && src[2]) {
            int h1 = hex_digit(src[1]);
            int h2 = hex_digit(src[2]);
            if (h1 >= 0 && h2 >= 0) {
                *dst++ = (h1 << 4) | h2;
                src += 3;
                continue;
            }
        }
        *dst++ = *src++;
    }
    *dst = '\0';
}
