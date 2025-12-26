// -*- mode:c++;indent-tabs-mode:nil;c-basic-offset:4;coding:utf-8 -*-
// vi: set et ft=cpp ts=4 sts=4 sw=4 fenc=utf-8 :vi
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

#include "client.h"

#include "llamafile/json.h"
#include "llamafile/llamafile.h"
#include "llamafile/string.h"
#include "llamafile/zim/zim.h"

#include <string>
#include <cstring>

namespace lf {
namespace server {

// Global ZIM archive (initialized at startup)
static ::zim_archive* g_zim_archive = nullptr;

bool
zim_init(const char* path)
{
    if (g_zim_archive) {
        ::zim_close(g_zim_archive);
    }
    g_zim_archive = ::zim_open(path);
    return g_zim_archive != nullptr;
}

void
zim_shutdown()
{
    if (g_zim_archive) {
        ::zim_close(g_zim_archive);
        g_zim_archive = nullptr;
    }
}

bool
zim_is_loaded()
{
    return g_zim_archive != nullptr;
}

::zim_archive*
zim_get_archive()
{
    return g_zim_archive;
}

// GET /zim/metadata - Get archive metadata
bool
Client::zim_metadata()
{
    if (!g_zim_archive) {
        return send_error(503, "No ZIM archive loaded");
    }

    jt::Json json;
    json["uuid"] = ::zim_get_uuid(g_zim_archive);
    json["entry_count"] = ::zim_get_entry_count(g_zim_archive);
    json["cluster_count"] = ::zim_get_cluster_count(g_zim_archive);
    json["main_page"] = ::zim_get_main_page(g_zim_archive);

    dump_ = json.toStringPretty();
    dump_ += '\n';
    char* p = append_http_response_message(obuf_.p, 200);
    p = stpcpy(p, "Content-Type: application/json\r\n");
    return send_response(obuf_.p, p, dump_);
}

// GET /zim/search?q=query&limit=10 - Search articles
bool
Client::zim_search()
{
    if (!g_zim_archive) {
        return send_error(503, "No ZIM archive loaded");
    }

    auto query_opt = param("q");
    if (!query_opt || query_opt->empty()) {
        return send_error(400, "Missing 'q' parameter");
    }
    std::string query(query_opt.value());

    int limit = 10;
    auto limit_opt = param("limit");
    if (limit_opt) {
        limit = std::stoi(std::string(limit_opt.value()));
        if (limit < 1) limit = 1;
        if (limit > 100) limit = 100;
    }

    // Perform search
    ::zim_search_result* results = new ::zim_search_result[limit];
    int count = ::zim_search(g_zim_archive, query.c_str(), results, limit);

    // Build JSON response
    jt::Json json;
    json["query"] = query;
    json["count"] = count;

    for (int i = 0; i < count; i++) {
        jt::Json& result = json["results"][i];
        result["index"] = results[i].index;
        result["path"] = results[i].path ? results[i].path : "";
        result["title"] = results[i].title ? results[i].title : "";
        result["score"] = results[i].score;
    }

    ::zim_search_free(results, count);
    delete[] results;

    dump_ = json.toStringPretty();
    dump_ += '\n';
    char* p = append_http_response_message(obuf_.p, 200);
    p = stpcpy(p, "Content-Type: application/json\r\n");
    return send_response(obuf_.p, p, dump_);
}

// GET /zim/suggest?q=prefix&limit=10 - Get suggestions
bool
Client::zim_suggest()
{
    if (!g_zim_archive) {
        return send_error(503, "No ZIM archive loaded");
    }

    auto prefix_opt = param("q");
    if (!prefix_opt || prefix_opt->empty()) {
        return send_error(400, "Missing 'q' parameter");
    }
    std::string prefix(prefix_opt.value());

    int limit = 10;
    auto limit_opt = param("limit");
    if (limit_opt) {
        limit = std::stoi(std::string(limit_opt.value()));
        if (limit < 1) limit = 1;
        if (limit > 50) limit = 50;
    }

    // Get suggestions
    ::zim_search_result* results = new ::zim_search_result[limit];
    int count = ::zim_suggest(g_zim_archive, prefix.c_str(), results, limit);

    // Build JSON response
    jt::Json json;
    json["prefix"] = prefix;

    for (int i = 0; i < count; i++) {
        jt::Json& suggestion = json["suggestions"][i];
        suggestion["path"] = results[i].path ? results[i].path : "";
        suggestion["title"] = results[i].title ? results[i].title : "";
    }

    ::zim_search_free(results, count);
    delete[] results;

    dump_ = json.toStringPretty();
    dump_ += '\n';
    char* p = append_http_response_message(obuf_.p, 200);
    p = stpcpy(p, "Content-Type: application/json\r\n");
    return send_response(obuf_.p, p, dump_);
}

// GET /zim/article/PATH - Get article content
bool
Client::zim_article()
{
    if (!g_zim_archive) {
        return send_error(503, "No ZIM archive loaded");
    }

    // Extract path from URL (everything after /zim/article/)
    std::string_view p1 = path();
    if (p1.size() <= 12) {  // "zim/article/"
        return send_error(400, "Missing article path");
    }
    std::string article_path(p1.substr(12));

    // URL decode the path
    char* decoded = strdup(article_path.c_str());
    ::zim_url_decode(decoded);

    // Get entry
    ::zim_entry entry;
    if (!::zim_get_entry_by_path(g_zim_archive, decoded, &entry)) {
        free(decoded);
        return send_error(404, ::zim_error());
    }
    free(decoded);

    // Follow redirects
    if (entry.is_redirect) {
        if (!::zim_resolve_redirect(g_zim_archive, &entry)) {
            return send_error(404, "Redirect loop or invalid redirect");
        }
    }

    // Check format parameter
    auto format_opt = param("format");
    bool as_text = format_opt && format_opt.value() == "text";

    // Get content
    size_t content_size;
    char* content;

    if (as_text) {
        content = ::zim_get_content_text(g_zim_archive, &entry, &content_size);
    } else {
        content = (char*)::zim_get_content(g_zim_archive, &entry, &content_size);
    }

    if (!content) {
        return send_error(500, ::zim_error());
    }

    // Build response
    if (as_text) {
        // Return JSON with text content
        jt::Json json;
        json["path"] = entry.path ? entry.path : "";
        json["title"] = entry.title ? entry.title : "";
        json["content"] = std::string(content, content_size);
        json["format"] = "text";

        ::zim_free(content);

        dump_ = json.toStringPretty();
        dump_ += '\n';
        char* p = append_http_response_message(obuf_.p, 200);
        p = stpcpy(p, "Content-Type: application/json\r\n");
        return send_response(obuf_.p, p, dump_);
    } else {
        // Return raw content with appropriate MIME type
        const char* mime = ::zim_get_mimetype(g_zim_archive, &entry);
        char* p = append_http_response_message(obuf_.p, 200);
        p = stpcpy(p, "Content-Type: ");
        p = stpcpy(p, mime ? mime : "application/octet-stream");
        p = stpcpy(p, "\r\n");

        bool ok = send_response(obuf_.p, p, std::string_view(content, content_size));
        ::zim_free(content);
        return ok;
    }
}

// GET /zim/raw/PATH - Get raw content (images, CSS, etc.)
bool
Client::zim_raw()
{
    if (!g_zim_archive) {
        return send_error(503, "No ZIM archive loaded");
    }

    // Extract path from URL (everything after /zim/raw/)
    std::string_view p1 = path();
    if (p1.size() <= 8) {  // "zim/raw/"
        return send_error(400, "Missing path");
    }
    std::string raw_path(p1.substr(8));

    // URL decode the path
    char* decoded = strdup(raw_path.c_str());
    ::zim_url_decode(decoded);

    // Get entry
    ::zim_entry entry;
    if (!::zim_get_entry_by_path(g_zim_archive, decoded, &entry)) {
        free(decoded);
        return send_error(404, ::zim_error());
    }
    free(decoded);

    // Follow redirects
    if (entry.is_redirect) {
        if (!::zim_resolve_redirect(g_zim_archive, &entry)) {
            return send_error(404, "Redirect loop or invalid redirect");
        }
    }

    // Get raw content
    size_t content_size;
    void* content = ::zim_get_content(g_zim_archive, &entry, &content_size);
    if (!content) {
        return send_error(500, ::zim_error());
    }

    // Return with appropriate MIME type
    const char* mime = ::zim_get_mimetype(g_zim_archive, &entry);
    char* p = append_http_response_message(obuf_.p, 200);
    p = stpcpy(p, "Content-Type: ");
    p = stpcpy(p, mime ? mime : "application/octet-stream");
    p = stpcpy(p, "\r\n");
    p = stpcpy(p, "Cache-Control: public, max-age=86400\r\n");

    bool ok = send_response(obuf_.p, p, std::string_view((char*)content, content_size));
    ::zim_free(content);
    return ok;
}

// GET /zim/main - Get main page
bool
Client::zim_main()
{
    if (!g_zim_archive) {
        return send_error(503, "No ZIM archive loaded");
    }

    ::zim_entry entry;
    if (!::zim_get_main_entry(g_zim_archive, &entry)) {
        return send_error(404, "No main page defined");
    }

    // Follow redirects
    if (entry.is_redirect) {
        if (!::zim_resolve_redirect(g_zim_archive, &entry)) {
            return send_error(404, "Main page redirect failed");
        }
    }

    // Get content
    size_t content_size;
    void* content = ::zim_get_content(g_zim_archive, &entry, &content_size);
    if (!content) {
        return send_error(500, ::zim_error());
    }

    const char* mime = ::zim_get_mimetype(g_zim_archive, &entry);
    char* p = append_http_response_message(obuf_.p, 200);
    p = stpcpy(p, "Content-Type: ");
    p = stpcpy(p, mime ? mime : "text/html");
    p = stpcpy(p, "\r\n");

    bool ok = send_response(obuf_.p, p, std::string_view((char*)content, content_size));
    ::zim_free(content);
    return ok;
}

} // namespace server
} // namespace lf
