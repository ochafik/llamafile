// -*- mode:c++;indent-tabs-mode:nil;c-basic-offset:4;coding:utf-8 -*-
// vi: set et ft=cpp ts=4 sts=4 sw=4 fenc=utf-8 :vi
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

// llamafile WIKI BROWSER — `GET /wiki/<path>`: stream an article's ORIGINAL
// HTML straight from the ZIM, rewriting internal links so you can click through
// the offline encyclopedia inside the binary. See wiki_route.h.
//
// The route runs in the MAIN --server process. `--zim PATH` opens the ZIM
// in-process here (in ADDITION to the mcp-server subprocess that backs the
// wiki_* tools), because the subprocess has the only other ZIM handle and it
// speaks stdio JSON-RPC, not HTTP. The bundle path /zip/<name>.zim reads fine
// in-process — the zim reader already works from the APE /zip VFS.

#include "wiki_route.h"

#include "zim/zim.h"

#include "server-http.h"

#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace {

zim_archive * g_wiki_zim = nullptr;
std::string   g_wiki_zim_path;
bool          g_wiki_enabled = false;
bool          g_wiki_open_attempted = false;
std::mutex    g_wiki_mu;  // the zim reader's cluster cache is not thread-safe

// Open the in-process ZIM on first use (idempotent). Caller holds g_wiki_mu.
bool ensure_zim_open() {
    if (g_wiki_zim) return true;
    if (g_wiki_open_attempted) return false;
    g_wiki_open_attempted = true;
    if (g_wiki_zim_path.empty()) return false;
    g_wiki_zim = zim_open(g_wiki_zim_path.c_str());
    if (!g_wiki_zim) {
        fprintf(stderr, "wiki: failed to open ZIM '%s' in-process: %s\n",
                g_wiki_zim_path.c_str(), zim_error());
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// HTTP response helpers
// ---------------------------------------------------------------------------
server_http_res_ptr serve_html(int status, std::string body) {
    auto r = std::make_unique<server_http_res>();
    r->status = status;
    r->content_type = "text/html; charset=utf-8";
    r->data = std::move(body);
    return r;
}

server_http_res_ptr serve_raw(std::string body, const char * mime) {
    auto r = std::make_unique<server_http_res>();
    r->status = 200;
    r->content_type = (mime && *mime) ? mime : "application/octet-stream";
    r->data = std::move(body);
    return r;
}

std::string html_escape(const std::string & s) {
    std::string o;
    o.reserve(s.size());
    for (char c : s) {
        switch (c) {
            case '&': o += "&amp;"; break;
            case '<': o += "&lt;"; break;
            case '>': o += "&gt;"; break;
            case '"': o += "&quot;"; break;
            default: o.push_back(c);
        }
    }
    return o;
}

// ---------------------------------------------------------------------------
// Rewrite href/src attribute values: internal ZIM links -> /wiki/<path>, so the
// browser navigates within this route. External/anchor/absolute targets pass
// through untouched (zim_rewrite_link decides). Attribute-aware so "data-src"
// etc. are not mistaken for "src".
// ---------------------------------------------------------------------------
std::string rewrite_html_links(const std::string & html) {
    std::string out;
    out.reserve(html.size() + 512);
    size_t n = html.size();
    size_t i = 0;
    bool in_tag = false;
    while (i < n) {
        char c = html[i];
        if (!in_tag) {
            out.push_back(c);
            if (c == '<') in_tag = true;
            i++;
            continue;
        }
        if (c == '>') { in_tag = false; out.push_back(c); i++; continue; }
        if (isspace((unsigned char) c)) { out.push_back(c); i++; continue; }
        if (isalpha((unsigned char) c)) {
            // attribute name
            size_t start = i;
            std::string name;
            while (i < n && (isalnum((unsigned char) html[i]) || html[i] == '-' || html[i] == ':')) {
                name.push_back((char) tolower((unsigned char) html[i]));
                i++;
            }
            out.append(html, start, i - start);
            while (i < n && isspace((unsigned char) html[i])) { out.push_back(html[i]); i++; }
            if (i < n && html[i] == '=') {
                out.push_back('=');
                i++;
                while (i < n && isspace((unsigned char) html[i])) { out.push_back(html[i]); i++; }
                char q = 0;
                if (i < n && (html[i] == '"' || html[i] == '\'')) { q = html[i]; out.push_back(q); i++; }
                std::string val;
                while (i < n) {
                    char vc = html[i];
                    if (q) { if (vc == q) break; }
                    else { if (isspace((unsigned char) vc) || vc == '>') break; }
                    val.push_back(vc);
                    i++;
                }
                if (name == "href" || name == "src") {
                    std::vector<char> rew(val.size() + 16);
                    zim_rewrite_link(val.c_str(), rew.data(), rew.size());
                    out += rew.data();
                } else {
                    out += val;
                }
                if (q && i < n && html[i] == q) { out.push_back(q); i++; }
            }
            continue;
        }
        out.push_back(c);
        i++;
    }
    return out;
}

// Inject a tiny header bar (back-to-chat link + title) right after <body ...>.
void inject_header_bar(std::string & html, const std::string & title) {
    std::string bar =
        "<div style=\"position:sticky;top:0;z-index:99999;background:#0b0d10;"
        "color:#e6e8eb;padding:8px 14px;font:14px/1.4 system-ui,-apple-system,"
        "sans-serif;border-bottom:1px solid #222831;display:flex;gap:10px;"
        "align-items:center\">"
        "<a href=\"/\" style=\"color:#7fd1b9;text-decoration:none;font-weight:600\">"
        "&larr; back to chat</a>"
        "<span style=\"opacity:.4\">&middot;</span>"
        "<b style=\"overflow:hidden;text-overflow:ellipsis;white-space:nowrap\">" +
        html_escape(title) + "</b>"
        "<span style=\"margin-left:auto;opacity:.5;font-size:12px\">offline Wikipedia</span>"
        "</div>";

    // find <body ...> and insert after its '>'
    size_t b = html.find("<body");
    if (b == std::string::npos) {
        // no <body>: prepend the bar
        html.insert(0, bar);
        return;
    }
    size_t gt = html.find('>', b);
    if (gt == std::string::npos) { html.insert(0, bar); return; }
    html.insert(gt + 1, bar);
}

// ---------------------------------------------------------------------------
// Resolve a /wiki path to a content entry (redirects followed). Tries the bare
// path, then the namespace-qualified path ('C' new / 'A' old). Empty -> main.
// ---------------------------------------------------------------------------
bool resolve_path(const std::string & rel, zim_entry * e) {
    bool found = false;
    if (rel.empty()) {
        found = zim_get_main_entry(g_wiki_zim, e);
    } else if (zim_get_entry_by_path(g_wiki_zim, rel.c_str(), e)) {
        found = true;
    } else {
        std::string c = "C/" + rel, a = "A/" + rel;
        if (zim_get_entry_by_path(g_wiki_zim, c.c_str(), e) ||
            zim_get_entry_by_path(g_wiki_zim, a.c_str(), e)) {
            found = true;
        }
    }
    if (found && e->is_redirect) zim_resolve_redirect(g_wiki_zim, e);
    return found;
}

server_http_res_ptr not_found(const std::string & rel) {
    std::string body =
        "<!doctype html><meta charset=utf-8><title>Not found</title>"
        "<body style=\"font:15px system-ui,sans-serif;background:#0b0d10;color:#e6e8eb;"
        "padding:40px\"><a href=\"/\" style=\"color:#7fd1b9\">&larr; back to chat</a>"
        "<h1>404 — article not found</h1><p>No ZIM entry for <code>" +
        html_escape(rel) + "</code>.</p></body>";
    return serve_html(404, std::move(body));
}

server_http_res_ptr handle_wiki(const server_http_req & req) {
    // req.path is already URL-decoded by the HTTP layer. Everything after the
    // first "/wiki/" is the ZIM path (may itself contain '/').
    std::string rel;
    size_t pos = req.path.find("/wiki/");
    if (pos != std::string::npos) rel = req.path.substr(pos + 6);

    std::lock_guard<std::mutex> lk(g_wiki_mu);
    if (!ensure_zim_open()) {
        return serve_html(503, "<!doctype html><meta charset=utf-8>"
                               "<body>wiki browser: no ZIM available.</body>");
    }

    zim_entry e;
    if (!resolve_path(rel, &e)) {
        return not_found(rel);
    }

    size_t n = 0;
    void * raw = zim_get_content(g_wiki_zim, &e, &n);
    if (!raw) {
        return not_found(rel);
    }
    const char * mime = zim_get_mimetype(g_wiki_zim, &e);
    bool is_html = mime && (strstr(mime, "text/html") || strstr(mime, "application/xhtml"));

    if (is_html) {
        std::string html((const char *) raw, n);
        zim_free(raw);
        std::string out = rewrite_html_links(html);
        inject_header_bar(out, e.title && *e.title ? e.title : rel);
        return serve_html(200, std::move(out));
    }

    std::string bytes((const char *) raw, n);
    zim_free(raw);
    return serve_raw(std::move(bytes), mime);
}

}  // namespace

// ---------------------------------------------------------------------------
// public API
// ---------------------------------------------------------------------------
void llamafile_wiki_enable(const char * zim_path) {
    if (zim_path && *zim_path) g_wiki_zim_path = zim_path;
    g_wiki_enabled = true;
}

bool llamafile_wiki_enabled() {
    return g_wiki_enabled;
}

void llamafile_wiki_register_routes(server_http_context & http) {
    {
        std::lock_guard<std::mutex> lk(g_wiki_mu);
        ensure_zim_open();  // open now so a bad path is reported at startup
    }
    // No "/:" -> registered as a regex route; "(.*)" captures sub-paths with '/'.
    http.get("/wiki/(.*)", handle_wiki);
}
